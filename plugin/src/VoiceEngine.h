#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <array>
#include <cmath>
#include "Voice.h"
#include "SliceBank.h"

// Owns the voice pool and all MIDI-note-to-slice dispatch. Two distinct keying schemes, both
// ported directly from kerf.html:
//  - Trigger mode (and preview/retrigger): one voice per slice index. Starting a new voice for a
//    slice index that's already sounding releases the old one first - not an instant cut, this
//    matches playSlice()'s own stopSlice()-then-play sequence, which crossfades rather than cuts.
//  - Sampler mode: one voice per (slice index, MIDI note) pair, so multiple notes can sound the
//    single selected slice at different pitches simultaneously (matches the original's
//    `sampler_${note}` keying).
class VoiceEngine
{
public:
    enum class MidiMode { trigger, sampler };

    void prepare (double outputSampleRateIn)
    {
        outputSampleRate = outputSampleRateIn;
        for (auto& v : voices)
            v.stopImmediately();
    }

    void setSampleSource (const juce::AudioBuffer<float>* buffer, double sourceRate)
    {
        sampleBuffer = buffer;
        sourceSampleRate = sourceRate;
    }

    void allNotesOff()
    {
        for (auto& v : voices)
            v.stopImmediately();
    }

    // midiNote is the raw incoming MIDI note number (0-127).
    void noteOn (MidiMode mode, int noteOffset, int selectedSliceIndex, int midiNote, const SliceBank::Snapshot& slices)
    {
        if (sampleBuffer == nullptr)
            return;

        if (mode == MidiMode::trigger)
        {
            const auto relativeNote = midiNote - noteOffset;
            if (relativeNote < 0 || relativeNote >= (int) slices->size())
                return;

            const auto& slice = (*slices)[(size_t) relativeNote];
            if (! slice.enabled)
                return;

            triggerMonophonicPerSlice (relativeNote, midiNote, slice, slice.pitch);
        }
        else
        {
            if (selectedSliceIndex < 0 || selectedSliceIndex >= (int) slices->size())
                return;

            const auto& slice = (*slices)[(size_t) selectedSliceIndex];
            if (! slice.enabled)
                return;

            const auto pitchRatio = slice.pitch * (float) std::pow (2.0, (midiNote - 60) / 12.0);
            triggerPolyphonicPerNote (selectedSliceIndex, midiNote, slice, pitchRatio);
        }
    }

    void noteOff (MidiMode mode, int noteOffset, int midiNote)
    {
        if (mode == MidiMode::trigger)
        {
            const auto relativeNote = midiNote - noteOffset;

            for (auto& v : voices)
                if (v.isActive() && v.getSliceIndex() == relativeNote)
                    v.release();
        }
        else
        {
            for (auto& v : voices)
                if (v.isActive() && v.getMidiNote() == midiNote)
                    v.release();
        }
    }

    // Mouse-triggered audition (on-screen piano, slice list Play button) - not tied to a MIDI
    // note, always plays the slice at its own stored pitch.
    void triggerPreview (int sliceIndex, const Slice& slice)
    {
        triggerMonophonicPerSlice (sliceIndex, previewNoteMarker, slice, slice.pitch);
    }

    void stopPreview (int sliceIndex)
    {
        for (auto& v : voices)
            if (v.isActive() && v.getSliceIndex() == sliceIndex)
                v.release();
    }

    // Drag-to-resize live retrigger: same crossfading stop+play as any other retrigger in the
    // original (updatePlayingSliceAudio() -> stopSlice()+playSlice()), just called from the
    // audio thread's command queue instead of directly from a UI callback.
    void retrigger (int sliceIndex, const Slice& slice)
    {
        triggerMonophonicPerSlice (sliceIndex, previewNoteMarker, slice, slice.pitch);
    }

    void renderNextBlock (juce::AudioBuffer<float>& output, int startSample, int numSamples)
    {
        for (auto& v : voices)
            if (v.isActive())
                v.renderNextBlock (output, startSample, numSamples);
    }

    int getNumActiveVoices() const noexcept
    {
        int count = 0;
        for (auto& v : voices)
            if (v.isActive())
                ++count;
        return count;
    }

    bool isSliceActive (int sliceIndex) const noexcept
    {
        for (auto& v : voices)
            if (v.isActive() && v.getSliceIndex() == sliceIndex)
                return true;
        return false;
    }

    bool isNoteActive (int midiNote) const noexcept
    {
        for (auto& v : voices)
            if (v.isActive() && v.getMidiNote() == midiNote)
                return true;
        return false;
    }

private:
    static constexpr int maxVoices = 32;
    static constexpr int previewNoteMarker = -1;

    void triggerMonophonicPerSlice (int sliceIndex, int midiNote, const Slice& slice, float pitchRatio)
    {
        for (auto& v : voices)
            if (v.isActive() && v.getSliceIndex() == sliceIndex)
                v.release();

        startVoice (sliceIndex, midiNote, slice, pitchRatio);
    }

    void triggerPolyphonicPerNote (int sliceIndex, int midiNote, const Slice& slice, float pitchRatio)
    {
        for (auto& v : voices)
            if (v.isActive() && v.getSliceIndex() == sliceIndex && v.getMidiNote() == midiNote)
                v.release();

        startVoice (sliceIndex, midiNote, slice, pitchRatio);
    }

    void startVoice (int sliceIndex, int midiNote, const Slice& slice, float pitchRatio)
    {
        auto* voice = allocateVoice();
        if (voice != nullptr)
            voice->start (slice, sliceIndex, midiNote, sampleBuffer, sourceSampleRate, outputSampleRate, pitchRatio);
    }

    Voice* allocateVoice()
    {
        for (auto& v : voices)
            if (! v.isActive())
                return &v;

        // Pool exhausted: steal the first (arbitrary, not oldest-tracked - acceptable for the
        // realistic polyphony this plugin sees) voice rather than dropping the new note.
        voices[0].stopImmediately();
        return &voices[0];
    }

    std::array<Voice, maxVoices> voices;
    const juce::AudioBuffer<float>* sampleBuffer = nullptr;
    double sourceSampleRate = 44100.0;
    double outputSampleRate = 44100.0;
};
