#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include "Slice.h"

// Energy-onset detector. Runs on a background/UI thread, never the audio thread - it's a one-shot
// analysis over the whole loaded sample, not real-time DSP.
//
// Frame RMS (dB) is compared against the average of the preceding frames; a frame that jumps by
// more than a sensitivity-dependent number of dB, and is a local maximum of that jump, is an onset.
// Slices run from each onset to the next one (the last runs to the end of the sample), like a
// beat-slicer, so the whole loop is covered.
namespace TransientDetector
{
    // `sensitivity` is the slider value, 0..1: higher finds more (softer) transients.
    inline std::vector<Slice> detect (const float* channelData, int numSamples, double sampleRate, float sensitivity)
    {
        std::vector<Slice> result;

        const int frameSize = std::max (16, (int) (sampleRate * 0.010));
        const int hop = std::max (4, frameSize / 4);

        if (channelData == nullptr || numSamples < frameSize * 2)
            return result;

        const int numFrames = (numSamples - frameSize) / hop + 1;
        std::vector<float> db ((size_t) numFrames);
        float peakDb = -120.0f;

        for (int f = 0; f < numFrames; ++f)
        {
            double sum = 0.0;
            const float* p = channelData + (size_t) f * (size_t) hop;
            for (int i = 0; i < frameSize; ++i)
                sum += (double) p[i] * p[i];

            const auto rms = std::sqrt (sum / frameSize);
            db[(size_t) f] = (float) std::max (-120.0, 20.0 * std::log10 (rms + 1.0e-9));
            peakDb = std::max (peakDb, db[(size_t) f]);
        }

        if (peakDb < -80.0f)
            return result; // effectively silence

        const auto s = std::clamp (sensitivity, 0.0f, 1.0f);
        const float jumpThreshold = 14.0f - 11.0f * s;           // dB rise needed: 14 (low) .. 3 (high)
        const float floorDb = peakDb - (30.0f + 30.0f * s);      // ignore onsets far below the loudest part
        const int history = 8;                                    // frames averaged as the "before" level
        const int minGapFrames = std::max (1, (int) (sampleRate * 0.060 / hop));

        std::vector<float> flux ((size_t) numFrames, 0.0f);
        for (int f = 1; f < numFrames; ++f)
        {
            const int from = std::max (0, f - history);
            float avg = 0.0f;
            for (int k = from; k < f; ++k)
                avg += db[(size_t) k];
            avg /= (float) (f - from);
            flux[(size_t) f] = std::max (0.0f, db[(size_t) f] - avg);
        }

        std::vector<double> onsets;
        onsets.push_back (0.0); // slices always start at the top of the sample
        int lastFrame = -minGapFrames;

        for (int f = 1; f < numFrames; ++f)
        {
            if (flux[(size_t) f] < jumpThreshold || db[(size_t) f] < floorDb)
                continue;

            // local peak of flux within the neighbourhood
            bool isPeak = true;
            for (int k = std::max (1, f - 2); k <= std::min (numFrames - 1, f + 2); ++k)
                if (flux[(size_t) k] > flux[(size_t) f])
                    isPeak = false;

            if (! isPeak || f - lastFrame < minGapFrames)
                continue;

            lastFrame = f;
            onsets.push_back (std::max (0.0, (double) (f * hop - hop) / sampleRate)); // back off so the attack isn't clipped
        }

        const auto total = (double) numSamples / sampleRate;

        for (size_t i = 0; i < onsets.size(); ++i)
        {
            const auto start = onsets[i];
            const auto end = i + 1 < onsets.size() ? onsets[i + 1] : total;

            if (end - start < 0.01)
                continue;

            Slice sl;
            sl.start = (float) start;
            sl.end = (float) end;
            result.push_back (sl);
        }

        return result;
    }
}
