#pragma once

#include <atomic>
#include <memory>
#include <vector>

#include "audio_buffer.h"
#include "midi.h"
#include "sample.h"
#include "slice_bank.h"
#include "spsc_queue.h"
#include "step_sequencer.h"
#include "voice_engine.h"

namespace kerf
{
    // The portable "processor": everything the VST3 plugin's AudioProcessor does, minus JUCE.
    // A host (SDL on the handheld, anything else later) owns one Engine, calls render() from its
    // audio callback and drives everything else from its UI thread.
    //
    // Threading contract:
    //  - render() / prepare(): audio thread only (prepare() may also be called before audio starts).
    //  - everything else: the single UI thread, except midiInbox().push(), which any thread may call.
    //  - The UI thread never touches the voices directly: it queues commands that the audio thread
    //    applies at the start of the next block.
    //  - The step sequencer (sequencer()) is the one exception to "commands only": its pattern and
    //    transport are lock-free atomics the UI thread edits directly (see step_sequencer.h).
    class Engine
    {
    public:
        static constexpr int maxPlayheads = VoiceEngine::maxVoices;

        struct Playhead
        {
            int slice = -1;
            float seconds = 0.0f;
        };

        Engine();

        // ---- audio thread ----
        void prepare (double outputSampleRate);

        // Overwrites `interleavedStereo` with numFrames of stereo audio.
        void render (float* interleavedStereo, int numFrames);

        // ---- UI thread: sample + slices ----
        // Replaces the sample (stopping all voices) and clears the slice list. Safe while playing.
        bool loadSample (SamplePtr sample);
        SamplePtr sample() const { return uiSample_; }

        SliceBank& slices() noexcept { return bank_; }

        // The 16-step pattern sequencer. Row N of every pattern plays slice N. It starts and stops
        // with sequencer().play() / stop(); loadSample() stops it and clears the patterns.
        StepSequencer& sequencer() noexcept { return seq_; }
        const StepSequencer& sequencer() const noexcept { return seq_; }

        // `slider` is the original app's sensitivity slider value (0.1 .. 1); the detector's
        // threshold is 0.15 * (1 - slider). Replaces the slice list; returns how many were found.
        int detectTransients (float slider);

        // ---- UI thread: playing ----
        void triggerPreview (int sliceIndex);
        void stopPreview (int sliceIndex);
        // Re-triggers the slice only if it is currently sounding (drag-a-boundary behaviour).
        void retriggerIfPlaying (int sliceIndex);
        void noteOn (int midiNote);
        void noteOff (int midiNote);
        void allNotesOff();

        // Any thread: raw MIDI from drivers.
        MidiInbox& midiInbox() noexcept { return midiInbox_; }

        // ---- settings (UI thread writes, audio thread reads) ----
        void setMidiMode (VoiceEngine::MidiMode mode);
        VoiceEngine::MidiMode midiMode() const noexcept { return midiMode_.load() == 0 ? VoiceEngine::MidiMode::trigger : VoiceEngine::MidiMode::sampler; }

        void setNoteOffset (int offset) noexcept { noteOffset_.store (offset); }
        int noteOffset() const noexcept { return noteOffset_.load(); }

        void setMasterVolume (float v) noexcept { masterVolume_.store (v); }
        float masterVolume() const noexcept { return masterVolume_.load(); }

        // The slice sampler mode plays (and the one the editor is working on).
        void setSelectedSlice (int index) noexcept { selectedSlice_.store (index); }
        int selectedSlice() const noexcept { return selectedSlice_.load(); }

        // ---- feedback for the UI ----
        int copyPlayheads (Playhead* out, int maxOut) const noexcept;
        bool isSliceSounding (int sliceIndex) const noexcept;
        float peakLevel() const noexcept { return peak_.load(); }

        // Frees samples retired by the audio thread (call once per UI frame).
        void collectGarbage();

    private:
        struct Command
        {
            enum class Type : uint8_t { none, setSample, triggerPreview, stopPreview, retriggerIfPlaying, noteOn, noteOff, allNotesOff };

            Type type = Type::none;
            int a = 0;
            SamplePtr sample;
        };

        bool pushCommand (Command&& c, bool mustSucceed);
        void renderChunk (float* out, int numFrames);
        void applyCommand (Command& c, const SliceBank::Snapshot& slices);
        void handleNoteOn (int note, const SliceBank::Snapshot& slices);
        void handleNoteOff (int note);
        void fireStep (const StepSequencer::Event& e, const SliceBank::Snapshot& slices);
        void releaseSequencerVoices();

        static constexpr int kMaxChunk = 1024;

        // shared state
        SliceBank bank_;
        StepSequencer seq_;
        SpscQueue<Command> commands_ { 512 };
        SpscQueue<SamplePtr> trash_ { 16 };
        MidiInbox midiInbox_;

        std::atomic<int> midiMode_ { 0 };
        std::atomic<int> noteOffset_ { 60 };
        std::atomic<float> masterVolume_ { 0.7f };
        std::atomic<int> selectedSlice_ { 0 };

        // UI-thread only
        SamplePtr uiSample_;

        // audio-thread only
        VoiceEngine voices_;
        SamplePtr audioSample_;
        AudioBuffer scratch_;
        std::vector<MidiMessage> midiScratch_;
        bool pressed_[128] = {};
        bool seqPrev_[StepSequencer::kRows] = {};   // was this row lit on the step before? (loop slices are gated by it)
        bool seqUsed_[StepSequencer::kRows] = {};   // did the sequencer start a voice on this row? (released on stop)
        double sampleRate_ = 44100.0;

        // audio -> UI feedback
        std::atomic<int> playheadCount_ { 0 };
        std::atomic<int> playheadSlice_[maxPlayheads];
        std::atomic<float> playheadSeconds_[maxPlayheads];
        std::atomic<float> peak_ { 0.0f };
    };
}
