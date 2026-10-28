#pragma once

// The take: what is recorded from a stream of sound, and how it is written to a WAV file.
//
// The stream can have many channels (a mixer sends 12), but a take is mono or stereo: the person
// chooses which channels it comes from (ChannelChoice), and only those are kept, so a long take
// does not need the memory of twelve. Everything here runs on the UI thread; the sound reaches it
// through the CaptureRing (audio_input.h).

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kerf::ui
{
    // Which channels of the stream become the take.
    struct ChannelChoice
    {
        int left = 0;               // 0-based channel of the stream
        int right = -1;             // the second channel of a stereo take; -1 = mono
        bool mix = false;           // mono made from the average of `left` and `right`
        std::string label;          // for the screen: "1+2", "3", "mix 1+2"

        int takeChannels() const noexcept { return (right >= 0 && ! mix) ? 2 : 1; }
        bool operator== (const ChannelChoice& o) const noexcept { return left == o.left && right == o.right && mix == o.mix; }
    };

    // The choices for a stream of `streamChannels` channels: the pairs first (1+2, 3+4, ...), then
    // each channel on its own, and for a stereo stream also the two mixed to mono.
    std::vector<ChannelChoice> channelChoices (int streamChannels);

    class Recorder
    {
    public:
        static constexpr int kMaxChannels = 32;
        static constexpr double kClipHoldSeconds = 1.5;
        static constexpr double kHoldSeconds = 1.0;

        // A new stream: sets the number of channels and the sample rate, and forgets any take (the
        // gain stays).
        void configure (int channels, int sampleRate, double maxSeconds);

        // No stream any more: forgets the stream and the take. The gain is a setting and stays.
        void reset();

        void setChoice (const ChannelChoice& choice);   // not while recording
        void setGainDb (int db);                        // added to what is recorded and metered

        // Begins a take; whatever was recorded before is forgotten. False if there is no stream.
        bool start();
        void stop();
        void discard();                                 // forgets the take

        // The sound of a stretch of time, `channels()` wide, interleaved. Updates the meters and,
        // while recording, the take. Recording stops by itself when the take is as long as allowed.
        void feed (const float* interleaved, int frames);

        // Lets `seconds` pass for the meters (they fall back at a steady pace).
        void tick (double seconds);

        bool configured() const noexcept { return channels_ > 0; }
        bool recording() const noexcept { return recording_; }
        bool hasTake() const noexcept { return takeFrames() > 0; }
        bool full() const noexcept { return full_; }
        int channels() const noexcept { return channels_; }
        int sampleRate() const noexcept { return rate_; }
        int gainDb() const noexcept { return gainDb_; }
        const ChannelChoice& choice() const noexcept { return choice_; }

        int64_t takeFrames() const noexcept { return takeLeft_.size(); }
        int takeChannels() const noexcept { return takeChannels_; }
        double takeSeconds() const noexcept { return rate_ > 0 ? (double) takeFrames() / (double) rate_ : 0.0; }
        double maxSeconds() const noexcept { return maxSeconds_; }

        // 0..1 (linear, after the gain): the peak of each channel, falling back after a hit, and the
        // highest peak of the last second.
        float level (int channel) const noexcept;
        float hold (int channel) const noexcept;

        // The channels that are being recorded went over full scale during the last second and a half.
        bool clipping() const noexcept { return clipTimer_ > 0.0; }
        bool clipped() const noexcept { return clippedEver_; }      // ... at any time during this take

        // Writes the take as a 16-bit WAV file, through a temporary file that is renamed when it is
        // complete. On failure returns false and says why in `error`.
        bool saveWav16 (const std::filesystem::path& file, std::string& error) const;

        // rec-001.wav, rec-002.wav ...: the first number after the highest one that exists in `dir`.
        static std::filesystem::path nextTakePath (const std::filesystem::path& dir);

    private:
        int channels_ = 0;
        int rate_ = 0;
        double maxSeconds_ = 180.0;
        int64_t maxFrames_ = 0;
        int gainDb_ = 0;
        float gain_ = 1.0f;
        ChannelChoice choice_;
        int takeChannels_ = 1;
        bool recording_ = false;
        bool full_ = false;

        std::vector<float> takeLeft_, takeRight_;

        std::vector<float> level_, hold_, holdTimer_;
        double clipTimer_ = 0.0;
        bool clippedEver_ = false;
    };
}
