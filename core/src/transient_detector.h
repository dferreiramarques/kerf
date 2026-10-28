#pragma once

#include <cmath>
#include <vector>

#include "slice.h"

namespace kerf
{
    // Faithful port of kerf.html's detectTransients() (single-pass delta-threshold scan over
    // channel-0 raw samples). It is a one-shot analysis over the whole loaded sample, not
    // real-time DSP, so call it from the UI thread (or a worker), never the audio thread.
    // Same simplification as the original: if the sample ends while still mid-slice, that
    // trailing slice is silently dropped rather than closed at EOF.
    namespace TransientDetector
    {
        // `sensitivity` matches the original's own (confusingly-named, kept for fidelity) local
        // variable: it's already `1 - sliderValue`, not the raw slider value itself.
        inline std::vector<Slice> detect (const float* channelData, int numSamples, double sampleRate, float sensitivity)
        {
            std::vector<Slice> result;

            const auto threshold = 0.15f * sensitivity;
            const auto minDuration = sampleRate * 0.2;
            const auto minSliceLength = sampleRate * 0.15;

            bool inSlice = false;
            int sliceStart = 0;

            for (int i = 1; i < numSamples; ++i)
            {
                const auto delta = std::abs (channelData[i] - channelData[i - 1]);
                const auto isTransient = delta > threshold;

                if (isTransient && ! inSlice)
                {
                    sliceStart = i;
                    inSlice = true;
                }
                else if (! isTransient && inSlice && (double) (i - sliceStart) > minDuration)
                {
                    if ((double) (i - sliceStart) > minSliceLength)
                    {
                        Slice s;
                        s.start = (float) ((double) sliceStart / sampleRate);
                        s.end = (float) ((double) i / sampleRate);
                        result.push_back (s);
                    }

                    inSlice = false;
                }
            }

            return result;
        }
    }
}
