#include "Voice.h"
#include <cmath>
#include <limits>

void Voice::start (const Slice& slice, int sliceIndexIn, int midiNoteIn,
                   const juce::AudioBuffer<float>* sampleBufferIn, double sourceSampleRateIn,
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

    fadeInSamples = juce::jmax (1.0, (double) slice.fadeInMs * 0.001 * outputSampleRate);
    fadeOutSamples = juce::jmax (1.0, (double) slice.fadeOutMs * 0.001 * outputSampleRate);
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
    const auto numSamples = sampleBuffer->getNumSamples();
    const auto i0 = (int) position;
    const auto i1 = i0 + 1;

    if (i0 < 0 || i0 >= numSamples)
        return 0.0f;

    const auto frac = (float) (position - (double) i0);
    const auto s0 = sampleBuffer->getSample (channel, i0);
    const auto s1 = i1 < numSamples ? sampleBuffer->getSample (channel, i1) : s0;

    return s0 + frac * (s1 - s0);
}

void Voice::renderNextBlock (juce::AudioBuffer<float>& output, int startSampleInBlock, int numSamples)
{
    if (state == State::idle || sampleBuffer == nullptr)
        return;

    const auto numSourceChannels = sampleBuffer->getNumChannels();
    if (numSourceChannels == 0)
        return;

    const auto leftSourceChannel = 0;
    const auto rightSourceChannel = juce::jmin (1, numSourceChannels - 1);

    const auto panAngle = (double) (pan + 1.0f) * juce::MathConstants<double>::pi / 4.0;
    const auto leftPanGain = (float) std::cos (panAngle);
    const auto rightPanGain = (float) std::sin (panAngle);

    auto* outL = output.getWritePointer (0, startSampleInBlock);
    auto* outR = output.getNumChannels() > 1 ? output.getWritePointer (1, startSampleInBlock) : outL;

    for (int i = 0; i < numSamples; ++i)
    {
        if (state == State::idle)
            break;

        // Envelope: linear fade-in at the start, linear fade-out either on release or (shot mode
        // only) automatically over the last fadeOutSamples before the natural end - matching the
        // original's playSlice(), which schedules that fade-out regardless of an explicit stop.
        if (state == State::releasing)
        {
            const auto t = (float) juce::jmin (1.0, releaseSamplesDone / fadeOutSamples);
            gain = (float) releaseGainStart * (1.0f - t);
            releaseSamplesDone += 1.0;

            if (t >= 1.0f)
                state = State::idle;
        }
        else
        {
            const auto remainingToEnd = mode == Slice::Mode::shot ? (endSample - readPosition) / juce::jmax (1e-9, pitchRatioPerOutputSample)
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
