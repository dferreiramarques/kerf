#include "recorder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace kerf::ui
{
    std::vector<ChannelChoice> channelChoices (int streamChannels)
    {
        std::vector<ChannelChoice> out;
        const int n = std::clamp (streamChannels, 0, Recorder::kMaxChannels);

        if (n <= 0)
            return out;

        const auto number = [] (int channel) { return std::to_string (channel + 1); };

        if (n >= 2)
        {
            for (int c = 0; c + 1 < n; c += 2)
                out.push_back ({ c, c + 1, false, number (c) + "+" + number (c + 1) + "  stereo" });
        }

        for (int c = 0; c < n; ++c)
            out.push_back ({ c, -1, false, number (c) + "  mono" });

        if (n == 2)
            out.push_back ({ 0, 1, true, "1+2  mono mix" });

        return out;
    }

    // ------------------------------------------------------------------------------ the take

    void Recorder::reset()
    {
        channels_ = 0;
        rate_ = 0;
        recording_ = false;
        full_ = false;
        choice_ = ChannelChoice {};
        takeChannels_ = 1;
        std::vector<float>().swap (takeLeft_);
        std::vector<float>().swap (takeRight_);
        level_.clear();
        hold_.clear();
        holdTimer_.clear();
        clipTimer_ = 0.0;
        clippedEver_ = false;
    }

    void Recorder::configure (int channels, int sampleRate, double maxSeconds)
    {
        reset();
        channels_ = std::clamp (channels, 0, kMaxChannels);
        rate_ = std::max (sampleRate, 0);
        maxSeconds_ = std::clamp (maxSeconds, 1.0, 3600.0);
        maxFrames_ = (int64_t) std::floor (maxSeconds_ * (double) rate_);

        level_.assign ((size_t) channels_, 0.0f);
        hold_.assign ((size_t) channels_, 0.0f);
        holdTimer_.assign ((size_t) channels_, 0.0f);

        const auto choices = channelChoices (channels_);

        if (! choices.empty())
            choice_ = choices.front();

        takeChannels_ = choice_.takeChannels();
    }

    void Recorder::setChoice (const ChannelChoice& choice)
    {
        if (recording_ || channels_ <= 0)
            return;

        if (choice.left < 0 || choice.left >= channels_ || choice.right >= channels_)
            return;

        choice_ = choice;
    }

    void Recorder::setGainDb (int db)
    {
        gainDb_ = std::clamp (db, -60, 60);
        gain_ = std::pow (10.0f, (float) gainDb_ / 20.0f);
    }

    bool Recorder::start()
    {
        if (channels_ <= 0 || rate_ <= 0 || recording_)
            return false;

        discard();
        takeChannels_ = choice_.takeChannels();
        takeLeft_.reserve ((size_t) maxFrames_);

        if (takeChannels_ == 2)
            takeRight_.reserve ((size_t) maxFrames_);

        recording_ = true;
        return true;
    }

    void Recorder::stop()
    {
        recording_ = false;
    }

    void Recorder::discard()
    {
        recording_ = false;
        full_ = false;
        clippedEver_ = false;
        std::vector<float>().swap (takeLeft_);
        std::vector<float>().swap (takeRight_);
    }

    void Recorder::tick (double seconds)
    {
        const float dt = (float) std::clamp (seconds, 0.0, 1.0);
        const float fall = std::pow (10.0f, -1.2f * dt);        // 24 dB a second

        for (size_t c = 0; c < level_.size(); ++c)
        {
            level_[c] *= fall;
            holdTimer_[c] -= dt;

            if (holdTimer_[c] <= 0.0f)
                hold_[c] *= fall;
        }

        clipTimer_ = std::max (0.0, clipTimer_ - (double) dt);
    }

    void Recorder::feed (const float* interleaved, int frames)
    {
        if (channels_ <= 0 || frames <= 0 || interleaved == nullptr)
            return;

        const int ch = channels_;
        const ChannelChoice pick = choice_;

        // The meters, and whether the channels that are being recorded went over full scale.
        bool over = false;

        for (int i = 0; i < frames; ++i)
        {
            const float* f = interleaved + (size_t) i * (size_t) ch;

            for (int c = 0; c < ch; ++c)
            {
                const float v = std::fabs (f[c] * gain_);

                if (v > level_[(size_t) c])
                    level_[(size_t) c] = v;

                if (v > hold_[(size_t) c])
                {
                    hold_[(size_t) c] = v;
                    holdTimer_[(size_t) c] = (float) kHoldSeconds;
                }

                if (v > 0.9995f && (c == pick.left || c == pick.right))
                    over = true;
            }
        }

        if (over)
        {
            clipTimer_ = kClipHoldSeconds;

            if (recording_)
                clippedEver_ = true;
        }

        if (! recording_)
            return;

        const int64_t room = maxFrames_ - takeFrames();
        const int n = (int) std::min<int64_t> (frames, std::max<int64_t> (room, 0));

        const auto clamp1 = [] (float v) { return std::max (-1.0f, std::min (1.0f, v)); };

        for (int i = 0; i < n; ++i)
        {
            const float* f = interleaved + (size_t) i * (size_t) ch;
            const float l = f[pick.left] * gain_;

            if (takeChannels_ == 2)
            {
                takeLeft_.push_back (clamp1 (l));
                takeRight_.push_back (clamp1 (f[pick.right] * gain_));
            }
            else if (pick.mix && pick.right >= 0)
            {
                takeLeft_.push_back (clamp1 ((l + f[pick.right] * gain_) * 0.5f));
            }
            else
            {
                takeLeft_.push_back (clamp1 (l));
            }
        }

        if (takeFrames() >= maxFrames_)
        {
            recording_ = false;
            full_ = true;
        }
    }

    float Recorder::level (int channel) const noexcept
    {
        return (channel >= 0 && channel < (int) level_.size()) ? level_[(size_t) channel] : 0.0f;
    }

    float Recorder::hold (int channel) const noexcept
    {
        return (channel >= 0 && channel < (int) hold_.size()) ? hold_[(size_t) channel] : 0.0f;
    }

    // --------------------------------------------------------------------------------- files

    bool Recorder::saveWav16 (const fs::path& file, std::string& error) const
    {
        if (! hasTake())
        {
            error = "nothing has been recorded";
            return false;
        }

        const int channels = takeChannels_;
        const int64_t frames = takeFrames();
        const uint64_t dataBytes = (uint64_t) frames * (uint64_t) channels * 2u;

        if (dataBytes > 0xFFFFFFF0ull - 36u)
        {
            error = "the take is too long for a WAV file";
            return false;
        }

        fs::path part = file;
        part += ".part";
        const std::string name = file.filename().string();

        std::ofstream out (part, std::ios::binary | std::ios::trunc);

        if (! out)
        {
            error = "cannot create " + name;
            return false;
        }

        const auto put32 = [] (char* at, uint32_t v) { for (int i = 0; i < 4; ++i) at[i] = (char) ((v >> (8 * i)) & 0xFF); };
        const auto put16 = [] (char* at, uint32_t v) { for (int i = 0; i < 2; ++i) at[i] = (char) ((v >> (8 * i)) & 0xFF); };

        char header[44];
        std::memcpy (header, "RIFF", 4);
        put32 (header + 4, 36u + (uint32_t) dataBytes);
        std::memcpy (header + 8, "WAVEfmt ", 8);
        put32 (header + 16, 16u);
        put16 (header + 20, 1u);                                                   // PCM
        put16 (header + 22, (uint32_t) channels);
        put32 (header + 24, (uint32_t) rate_);
        put32 (header + 28, (uint32_t) rate_ * (uint32_t) channels * 2u);
        put16 (header + 32, (uint32_t) channels * 2u);
        put16 (header + 34, 16u);
        std::memcpy (header + 36, "data", 4);
        put32 (header + 40, (uint32_t) dataBytes);
        out.write (header, sizeof header);

        constexpr int64_t kChunkFrames = 16384;
        std::vector<char> chunk ((size_t) (kChunkFrames * channels * 2));

        const auto to16 = [] (float v)
        {
            const float c = std::max (-1.0f, std::min (1.0f, v));
            return (int16_t) std::lround (c * 32767.0f);
        };

        for (int64_t at = 0; at < frames && out; at += kChunkFrames)
        {
            const int64_t n = std::min (kChunkFrames, frames - at);
            char* p = chunk.data();

            for (int64_t i = 0; i < n; ++i)
            {
                put16 (p, (uint16_t) to16 (takeLeft_[(size_t) (at + i)]));
                p += 2;

                if (channels == 2)
                {
                    put16 (p, (uint16_t) to16 (takeRight_[(size_t) (at + i)]));
                    p += 2;
                }
            }

            out.write (chunk.data(), (std::streamsize) (n * channels * 2));
        }

        out.flush();
        const bool written = static_cast<bool> (out);
        out.close();
        std::error_code ec;

        if (! written)
        {
            fs::remove (part, ec);
            error = "cannot write " + name + " (is the disk full?)";
            return false;
        }

        fs::rename (part, file, ec);

        if (ec)
        {
            fs::remove (part, ec);
            error = "cannot create " + name;
            return false;
        }

        return true;
    }

    fs::path Recorder::nextTakePath (const fs::path& dir)
    {
        int highest = 0;
        std::error_code ec;

        for (const auto& entry : fs::directory_iterator (dir, ec))
        {
            std::string name = entry.path().filename().string();
            std::transform (name.begin(), name.end(), name.begin(), [] (unsigned char ch) { return (char) std::tolower (ch); });

            if (name.size() < 9 || name.compare (0, 4, "rec-") != 0 || name.compare (name.size() - 4, 4, ".wav") != 0)
                continue;

            const std::string digits = name.substr (4, name.size() - 8);

            if (digits.empty() || digits.size() > 6 || ! std::all_of (digits.begin(), digits.end(), [] (unsigned char ch) { return std::isdigit (ch) != 0; }))
                continue;

            highest = std::max (highest, std::atoi (digits.c_str()));
        }

        char buf[32];
        std::snprintf (buf, sizeof buf, "rec-%03d.wav", highest + 1);
        return dir / buf;
    }
}
