#include "voice.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace kerf
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;
    }

    void Voice::start (const Slice& slice, int sliceIndexIn, int midiNoteIn,
                       const AudioBuffer* sampleBufferIn, double sourceSampleRateIn,
                       double outputSampleRateIn, float pitchRatio)
    {
        sampleBuffer = sampleBufferIn;
        sourceSampleRate = sourceSampleRateIn;
        outputSampleRate = outputSampleRateIn;

        sliceIndex = sliceIndexIn;
        midiNote = midiNoteIn;
        mode = slice.mode;

        startSample = (double) slice.start * sourceSampleRate;
        endSample = (double) slice.end * sourceSampleRate;
        readPosition = startSample;

        pitchRatioPerOutputSample = (double) pitchRatio * (sourceSampleRate / outputSampleRate);

        volume = slice.volume;
        pan = slice.pan;
        gain = 0.0f;

        fadeInSamples = std::max (1.0, (double) slice.fadeInMs * 0.001 * outputSampleRate);
        fadeOutSamples = std::max (1.0, (double) slice.fadeOutMs * 0.001 * outputSampleRate);
        samplesSinceStart = 0.0;

        state = State::playing;
    }

    void Voice::release()
    {
        if (state == State::idle)
            return;

        state = State::releasing;
        releaseGainStart = gain;
        releaseSamplesDone = 0.0;
    }

    void Voice::stopImmediately()
    {
        state = State::idle;
        sampleBuffer = nullptr;
    }

    float Voice::readSample (int channel, double position) const noexcept
    {
        const auto numSamples = sampleBuffer->numFrames();
        const auto i0 = (int) position;
        const auto i1 = i0 + 1;

        if (position < 0.0 || i0 >= numSamples)
            return 0.0f;

        const auto frac = (float) (position - (double) i0);
        const float* data = sampleBuffer->channel (channel);
        const auto s0 = data[i0];
        const auto s1 = i1 < numSamples ? data[i1] : s0;

        return s0 + frac * (s1 - s0);
    }

    void Voice::renderNextBlock (AudioBuffer& output, int startSampleInBlock, int numSamples)
    {
        if (state == State::idle || sampleBuffer == nullptr)
            return;

        const auto numSourceChannels = sampleBuffer->numChannels();
        if (numSourceChannels == 0 || output.numChannels() == 0)
            return;

        const auto leftSourceChannel = 0;
        const auto rightSourceChannel = std::min (1, numSourceChannels - 1);

        const auto panAngle = (double) (pan + 1.0f) * kPi / 4.0;
        const auto leftPanGain = (float) std::cos (panAngle);
        const auto rightPanGain = (float) std::sin (panAngle);

        auto* outL = output.channel (0) + startSampleInBlock;
        auto* outR = output.numChannels() > 1 ? output.channel (1) + startSampleInBlock : outL;

        for (int i = 0; i < numSamples; ++i)
        {
            if (state == State::idle)
                break;

            // Envelope: linear fade-in at the start, linear fade-out either on release or (shot
            // mode only) automatically over the last fadeOutSamples before the natural end -
            // matching the original's playSlice(), which schedules that fade-out regardless of
            // an explicit stop.
            if (state == State::releasing)
            {
                const auto t = (float) std::min (1.0, releaseSamplesDone / fadeOutSamples);
                gain = (float) releaseGainStart * (1.0f - t);
                releaseSamplesDone += 1.0;

                if (t >= 1.0f)
                    state = State::idle;
            }
            else
            {
                const auto remainingToEnd = mode == Slice::Mode::shot
                                              ? (endSample - readPosition) / std::max (1e-9, pitchRatioPerOutputSample)
                                              : std::numeric_limits<double>::max();

                if (samplesSinceStart < fadeInSamples)
                    gain = (float) (samplesSinceStart / fadeInSamples) * volume;
                else if (mode == Slice::Mode::shot && remainingToEnd < fadeOutSamples)
                    gain = (float) (remainingToEnd / fadeOutSamples) * volume;
                else
                    gain = volume;

                samplesSinceStart += 1.0;
            }

            const auto sL = readSample (leftSourceChannel, readPosition);
            const auto sR = readSample (rightSourceChannel, readPosition);

            outL[i] += sL * gain * leftPanGain;
            outR[i] += sR * gain * rightPanGain;

            readPosition += pitchRatioPerOutputSample;

            if (mode == Slice::Mode::loop)
            {
                const auto loopLength = endSample - startSample;

                if (loopLength > 0.0 && readPosition >= endSample)
                    readPosition -= loopLength * std::floor ((readPosition - startSample) / loopLength);
            }
            else if (readPosition >= endSample)
            {
                state = State::idle;
                break;
            }
        }
    }
}
