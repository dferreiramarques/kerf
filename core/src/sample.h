#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>

#include "audio_buffer.h"

namespace kerf
{
    // An immutable decoded sample. Shared between the UI thread and the audio thread through a
    // shared_ptr<const Sample>, so loading a new file while playing can never free audio that a
    // voice is still reading.
    struct Sample
    {
        AudioBuffer data; // 1 or 2 channels, planar float
        double sampleRate = 44100.0;
        std::string name;

        double durationSeconds() const noexcept
        {
            return sampleRate > 0.0 ? (double) data.numFrames() / sampleRate : 0.0;
        }
    };

    using SamplePtr = std::shared_ptr<const Sample>;

    struct LoadResult
    {
        SamplePtr sample;   // null on failure
        std::string error;  // human-readable reason when sample is null
    };

    // Decodes WAV / MP3 / FLAC from a file or memory (format detected from the bytes, not the
    // extension). Channels beyond the second are dropped. Files longer than `maxSeconds` are
    // refused up front, before any large allocation, to protect low-memory devices.
    // Decoding is chunked straight into the planar buffer, so peak memory is one copy of the
    // audio (plus the compressed file), not two.
    LoadResult decodeAudioMemory (const void* data, size_t size, const std::string& name, double maxSeconds = 360.0);

    // decodeAudioFile takes a real file-system path (Unicode-safe on Windows); decodeAudioFileUtf8
    // takes the UTF-8 strings the UI passes around.
    LoadResult decodeAudioFile (const std::filesystem::path& path, double maxSeconds = 360.0);
    LoadResult decodeAudioFileUtf8 (const std::string& utf8Path, double maxSeconds = 360.0);

    // Writes a 16-bit PCM WAV (1 or 2 channels). Used by tests and later by export.
    std::string encodeWav16 (const AudioBuffer& buffer, double sampleRate);
}
