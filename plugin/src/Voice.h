#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "Slice.h"

// One currently-sounding slice instance. Reads from a shared, externally-owned sample buffer
// (never copies it) via simple linear interpolation for pitch/resampling - deliberately not
// juce::LagrangeInterpolator, which is built for continuous streaming resampling and doesn't fit
// this class's need for random-access seeking into an arbitrary region of a shared buffer plus
// looping. Linear interpolation is also a reasonably faithful match for the original's Web Audio
// AudioBufferSourceNode.playbackRate approach, which isn't higher quality than this either.
class Voice
{
public:
    // Captures its own copy of the slice's start/end/fade/mode at trigger time, so a later UI
    // edit to the slice doesn't retroactively mutate an already-playing voice's boundaries.
    void start (const Slice& slice, int sliceIndexIn, int midiNoteIn,
                const juce::AudioBuffer<float>* sampleBufferIn, double sourceSampleRateIn,
                double outputSampleRateIn, float pitchRatio);

    // Note-off / explicit stop: begins (or restarts) a release fade over the slice's fadeOutMs
    // from whatever gain the voice is currently at, then goes idle. Matches the original's
    // stopSlice(), which does the same regardless of shot/loop mode.
    void release();

    // Immediate cut, no fade - used only for the drag-to-resize retrigger path, where the voice
    // is about to be replaced by a freshly retriggered one anyway.
    void stopImmediately();

    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples);

    bool isActive() const noexcept { return state != State::idle; }
    int getSliceIndex() const noexcept { return sliceIndex; }
    int getMidiNote() const noexcept { return midiNote; }

private:
    enum class State { idle, playing, releasing };

    float readSample (int channel, double position) const noexcept;

    const juce::AudioBuffer<float>* sampleBuffer = nullptr;
    double sourceSampleRate = 44100.0;
    double outputSampleRate = 44100.0;

    State state = State::idle;
    int sliceIndex = -1;
    int midiNote = -1;

    Slice::Mode mode = Slice::Mode::shot;
    double startSample = 0.0, endSample = 0.0;
    double readPosition = 0.0;
    double pitchRatioPerOutputSample = 1.0;

    float volume = 1.0f, pan = 0.0f;
    float gain = 0.0f;
    double fadeInSamples = 0.0, fadeOutSamples = 0.0;
    double samplesSinceStart = 0.0;
    double releaseGainStart = 0.0;
    double releaseSamplesDone = 0.0;
};
