#pragma once

#include <algorithm>
#include <vector>

namespace kerf
{
    // Planar float audio buffer (one contiguous block per channel). This is the JUCE-free
    // stand-in for juce::AudioBuffer<float> used by the plugin's engine.
    class AudioBuffer
    {
    public:
        AudioBuffer() = default;

        AudioBuffer (int numChannels, int numFrames)
        {
            resize (numChannels, numFrames);
        }

        // Resizes and zero-fills.
        void resize (int numChannels, int numFrames)
        {
            channels_ = std::max (0, numChannels);
            frames_ = std::max (0, numFrames);
            data_.assign ((size_t) channels_ * (size_t) frames_, 0.0f);
        }

        // Keeps the first `newFrames` frames of every channel and releases the rest (never grows).
        void shrinkTo (int newFrames)
        {
            newFrames = std::clamp (newFrames, 0, frames_);

            if (newFrames == frames_)
                return;

            for (int ch = 1; ch < channels_; ++ch)
                std::copy_n (channel (ch), newFrames, data_.data() + (size_t) ch * (size_t) newFrames);

            frames_ = newFrames;
            data_.resize ((size_t) channels_ * (size_t) frames_);
            data_.shrink_to_fit();
        }

        int numChannels() const noexcept { return channels_; }
        int numFrames() const noexcept { return frames_; }
        bool empty() const noexcept { return channels_ == 0 || frames_ == 0; }

        float* channel (int ch) noexcept { return data_.data() + (size_t) ch * (size_t) frames_; }
        const float* channel (int ch) const noexcept { return data_.data() + (size_t) ch * (size_t) frames_; }

        float get (int ch, int frame) const noexcept { return channel (ch)[frame]; }
        void set (int ch, int frame, float value) noexcept { channel (ch)[frame] = value; }

        void clear() noexcept { std::fill (data_.begin(), data_.end(), 0.0f); }

        void clear (int startFrame, int numFrames) noexcept
        {
            for (int ch = 0; ch < channels_; ++ch)
                std::fill_n (channel (ch) + startFrame, numFrames, 0.0f);
        }

        void applyGain (float gain) noexcept
        {
            for (auto& v : data_)
                v *= gain;
        }

    private:
        std::vector<float> data_;
        int channels_ = 0;
        int frames_ = 0;
    };
}
