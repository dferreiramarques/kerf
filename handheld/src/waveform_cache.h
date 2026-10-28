#pragma once

#include <vector>

#include "sample.h"

namespace kerf::ui
{
    // Min/max summary of a sample, kept as a pyramid of ever coarser levels so that drawing a
    // waveform costs O(screen width) per frame whatever the zoom - even for a 6-minute file on
    // the handheld's CPU. Stereo samples are summarised as the union of both channels.
    class WaveformCache
    {
    public:
        struct Column
        {
            float lo = 0.0f;
            float hi = 0.0f;
            bool valid = false; // false where the requested time range lies outside the sample
        };

        void build (kerf::SamplePtr sample);
        void clear();

        bool valid() const noexcept { return sample_ != nullptr; }
        double duration() const noexcept { return sample_ ? sample_->durationSeconds() : 0.0; }
        double sampleRate() const noexcept { return sample_ ? sample_->sampleRate : 44100.0; }
        const kerf::SamplePtr& sample() const noexcept { return sample_; }

        // Fills out[0..width) with the min/max of the audio in [t0, t1) seconds, one entry per
        // pixel column. Where a column spans more than one pyramid block the result may extend
        // by up to one block (at most about one pixel) beyond the column, never less.
        void columns (double t0, double t1, int width, Column* out) const;

        static constexpr int baseBlock = 64; // frames summarised by level 0
        static constexpr int fanOut = 8;     // each level summarises this many blocks of the one below

    private:
        struct Level
        {
            int blockFrames = 0;
            std::vector<float> lo, hi;
        };

        void rawRange (long long a, long long b, float& lo, float& hi) const noexcept;

        kerf::SamplePtr sample_;
        std::vector<Level> levels_;
    };
}
