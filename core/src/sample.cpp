#include "sample.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <new>
#include <vector>

#include "path_util.h"

#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

namespace kerf
{
    namespace
    {
        enum class Format { unknown, wav, mp3, flac };

        constexpr uint64_t kChunkFrames = 4096;
        constexpr uint64_t kMaxFileBytes = 512ull * 1024ull * 1024ull;

        Format detectFormat (const unsigned char* d, size_t n)
        {
            if (n >= 12 && (std::memcmp (d, "RIFF", 4) == 0 || std::memcmp (d, "RIFX", 4) == 0 || std::memcmp (d, "RF64", 4) == 0))
                return Format::wav;

            if (n >= 4 && std::memcmp (d, "fLaC", 4) == 0)
                return Format::flac;

            if (n >= 3 && std::memcmp (d, "ID3", 3) == 0)
                return Format::mp3;

            if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0)
                return Format::mp3;

            return Format::unknown;
        }

        LoadResult fail (const std::string& why)
        {
            LoadResult r;
            r.error = why;
            return r;
        }

        // Pulls interleaved float frames from `readFrames` in small chunks and de-interleaves them
        // into the final planar buffer, keeping at most two channels.
        template <typename ReadFrames>
        LoadResult decodePlanar (uint64_t totalFrames, unsigned channels, unsigned sampleRate,
                                 const std::string& name, ReadFrames&& readFrames)
        {
            if (channels == 0 || sampleRate == 0 || totalFrames == 0)
                return fail ("The file contains no audio");

            if (totalFrames > (uint64_t) INT_MAX)
                return fail ("Sample is too long for this device");

            auto s = std::make_shared<Sample>();
            const int outChannels = channels >= 2 ? 2 : 1;
            s->data.resize (outChannels, (int) totalFrames);
            s->sampleRate = (double) sampleRate;
            s->name = name;

            std::vector<float> chunk ((size_t) (kChunkFrames * channels));
            uint64_t done = 0;

            while (done < totalFrames)
            {
                const auto want = std::min<uint64_t> (kChunkFrames, totalFrames - done);
                const auto got = (uint64_t) readFrames (chunk.data(), want);

                if (got == 0)
                    break;

                for (int ch = 0; ch < outChannels; ++ch)
                {
                    float* dst = s->data.channel (ch) + done;

                    for (uint64_t i = 0; i < got; ++i)
                        dst[i] = chunk[(size_t) (i * channels + (uint64_t) ch)];
                }

                done += got;
            }

            if (done == 0)
                return fail ("Could not decode the audio data");

            // A damaged file may end early: keep what decoded.
            s->data.shrinkTo ((int) done);

            LoadResult r;
            r.sample = s;
            return r;
        }

        LoadResult decodeWav (const void* data, size_t size, const std::string& name, double maxSeconds)
        {
            drwav wav;
            if (! drwav_init_memory (&wav, data, size, nullptr))
                return fail ("Not a valid WAV file");

            LoadResult r;

            if (wav.channels == 0 || wav.sampleRate == 0 || wav.totalPCMFrameCount == 0)
                r = fail ("Empty WAV file");
            else if ((double) wav.totalPCMFrameCount / (double) wav.sampleRate > maxSeconds)
                r = fail ("Sample is too long for this device");
            else
                r = decodePlanar (wav.totalPCMFrameCount, wav.channels, wav.sampleRate, name,
                                  [&] (float* out, uint64_t n) { return drwav_read_pcm_frames_f32 (&wav, n, out); });

            drwav_uninit (&wav);
            return r;
        }

        LoadResult decodeMp3 (const void* data, size_t size, const std::string& name, double maxSeconds)
        {
            drmp3 mp3;
            if (! drmp3_init_memory (&mp3, data, size, nullptr))
                return fail ("Not a valid MP3 file");

            LoadResult r;
            const unsigned rate = mp3.sampleRate;
            const unsigned channels = mp3.channels;
            const drmp3_uint64 frames = drmp3_get_pcm_frame_count (&mp3);

            if (rate == 0 || channels == 0 || frames == 0)
                r = fail ("Empty MP3 file");
            else if ((double) frames / (double) rate > maxSeconds)
                r = fail ("Sample is too long for this device");
            else if (! drmp3_seek_to_pcm_frame (&mp3, 0))
                r = fail ("Could not decode MP3 data");
            else
                r = decodePlanar (frames, channels, rate, name,
                                  [&] (float* out, uint64_t n) { return drmp3_read_pcm_frames_f32 (&mp3, n, out); });

            drmp3_uninit (&mp3);
            return r;
        }

        LoadResult decodeFlac (const void* data, size_t size, const std::string& name, double maxSeconds)
        {
            drflac* flac = drflac_open_memory (data, size, nullptr);
            if (flac == nullptr)
                return fail ("Not a valid FLAC file");

            LoadResult r;

            if (flac->channels == 0 || flac->sampleRate == 0 || flac->totalPCMFrameCount == 0)
                r = fail ("Empty FLAC file");
            else if ((double) flac->totalPCMFrameCount / (double) flac->sampleRate > maxSeconds)
                r = fail ("Sample is too long for this device");
            else
                r = decodePlanar (flac->totalPCMFrameCount, flac->channels, flac->sampleRate, name,
                                  [&] (float* out, uint64_t n) { return drflac_read_pcm_frames_f32 (flac, n, out); });

            drflac_close (flac);
            return r;
        }
    }

    LoadResult decodeAudioMemory (const void* data, size_t size, const std::string& name, double maxSeconds)
    {
        if (data == nullptr || size < 4)
            return fail ("File is empty");

        try
        {
            switch (detectFormat ((const unsigned char*) data, size))
            {
                case Format::wav:  return decodeWav (data, size, name, maxSeconds);
                case Format::mp3:  return decodeMp3 (data, size, name, maxSeconds);
                case Format::flac: return decodeFlac (data, size, name, maxSeconds);
                default:           return fail ("Unsupported audio format (use WAV, MP3 or FLAC)");
            }
        }
        catch (const std::bad_alloc&)
        {
            return fail ("Not enough memory to load this file");
        }
    }

    LoadResult decodeAudioFile (const std::filesystem::path& path, double maxSeconds)
    {
        std::ifstream in (path, std::ios::binary | std::ios::ate);
        if (! in)
            return fail ("Could not open file");

        const auto end = in.tellg();
        if (end <= 0)
            return fail ("File is empty");

        if ((uint64_t) end > kMaxFileBytes)
            return fail ("File is too large for this device");

        std::vector<char> bytes;

        try { bytes.resize ((size_t) end); }
        catch (const std::bad_alloc&) { return fail ("Not enough memory to load this file"); }

        in.seekg (0);
        if (! in.read (bytes.data(), (std::streamsize) bytes.size()))
            return fail ("Could not read file");

        return decodeAudioMemory (bytes.data(), bytes.size(), pathToUtf8 (path.filename()), maxSeconds);
    }

    LoadResult decodeAudioFileUtf8 (const std::string& utf8Path, double maxSeconds)
    {
        return decodeAudioFile (pathFromUtf8 (utf8Path), maxSeconds);
    }

    std::string encodeWav16 (const AudioBuffer& buffer, double sampleRate)
    {
        const int channels = std::max (1, std::min (2, buffer.numChannels()));
        const int frames = buffer.numFrames();
        const uint32_t dataBytes = (uint32_t) frames * (uint32_t) channels * 2u;

        std::string out;
        out.reserve (44 + dataBytes);

        auto put32 = [&] (uint32_t v) { for (int i = 0; i < 4; ++i) out.push_back ((char) ((v >> (8 * i)) & 0xFF)); };
        auto put16 = [&] (uint16_t v) { for (int i = 0; i < 2; ++i) out.push_back ((char) ((v >> (8 * i)) & 0xFF)); };

        out += "RIFF";
        put32 (36u + dataBytes);
        out += "WAVEfmt ";
        put32 (16);
        put16 (1); // PCM
        put16 ((uint16_t) channels);
        put32 ((uint32_t) std::lround (sampleRate));
        put32 ((uint32_t) std::lround (sampleRate) * (uint32_t) channels * 2u);
        put16 ((uint16_t) (channels * 2));
        put16 (16);
        out += "data";
        put32 (dataBytes);

        for (int i = 0; i < frames; ++i)
        {
            for (int ch = 0; ch < channels; ++ch)
            {
                const float v = std::max (-1.0f, std::min (1.0f, buffer.get (ch, i)));
                put16 ((uint16_t) (int16_t) std::lround (v * 32767.0f));
            }
        }

        return out;
    }
}
