#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>

#include "transient_detector.h"

namespace kerf
{
    Engine::Engine() : scratch_ (2, kMaxChunk)
    {
        for (int i = 0; i < maxPlayheads; ++i)
        {
            playheadSlice_[i].store (-1);
            playheadSeconds_[i].store (0.0f);
        }

        midiScratch_.reserve (256);
        voices_.prepare (sampleRate_);
    }

    // ------------------------------------------------------------------------------------------
    // audio thread

    void Engine::prepare (double outputSampleRate)
    {
        sampleRate_ = outputSampleRate;
        voices_.prepare (outputSampleRate);
        std::fill (std::begin (pressed_), std::end (pressed_), false);
        std::fill (std::begin (seqPrev_), std::end (seqPrev_), false);
        std::fill (std::begin (seqUsed_), std::end (seqUsed_), false);
    }

    void Engine::render (float* out, int numFrames)
    {
        while (numFrames > 0)
        {
            const auto n = std::min (numFrames, kMaxChunk);
            renderChunk (out, n);
            out += (size_t) n * 2;
            numFrames -= n;
        }
    }

    void Engine::applyCommand (Command& c, const SliceBank::Snapshot& slices)
    {
        const auto count = (int) slices->size();

        switch (c.type)
        {
            case Command::Type::setSample:
                voices_.allNotesOff();
                std::fill (std::begin (pressed_), std::end (pressed_), false);
                std::fill (std::begin (seqPrev_), std::end (seqPrev_), false);
                std::fill (std::begin (seqUsed_), std::end (seqUsed_), false);

                // Hand the old sample back to the UI thread to free: deleting a big buffer
                // here would be a (rare, but real) real-time hazard.
                if (audioSample_ != nullptr)
                    trash_.push (std::move (audioSample_));

                audioSample_ = std::move (c.sample);
                voices_.setSampleSource (audioSample_ != nullptr ? &audioSample_->data : nullptr,
                                         audioSample_ != nullptr ? audioSample_->sampleRate : 44100.0);
                break;

            case Command::Type::triggerPreview:
                if (c.a >= 0 && c.a < count)
                    voices_.triggerPreview (c.a, (*slices)[(size_t) c.a]);
                break;

            case Command::Type::stopPreview:
                voices_.stopPreview (c.a);
                break;

            case Command::Type::retriggerIfPlaying:
                // Mirrors kerf.html's updatePlayingSliceAudio(): only a slice that is already
                // sounding is retriggered, so dragging a boundary on a silent slice stays silent.
                if (c.a >= 0 && c.a < count && voices_.isSliceActive (c.a))
                    voices_.retrigger (c.a, (*slices)[(size_t) c.a]);
                break;

            case Command::Type::noteOn:
                handleNoteOn (c.a, slices);
                break;

            case Command::Type::noteOff:
                handleNoteOff (c.a);
                break;

            case Command::Type::allNotesOff:
                voices_.allNotesOff();
                std::fill (std::begin (pressed_), std::end (pressed_), false);
                std::fill (std::begin (seqPrev_), std::end (seqPrev_), false);   // a held loop is gone: let the next lit step restart it
                std::fill (std::begin (seqUsed_), std::end (seqUsed_), false);
                break;

            case Command::Type::none:
                break;
        }
    }

    void Engine::handleNoteOn (int note, const SliceBank::Snapshot& slices)
    {
        if (note < 0 || note > 127)
            return;

        // The original ignores a note-on for a key that is still held (pressedKeys[...]).
        if (pressed_[note])
            return;

        pressed_[note] = true;
        voices_.noteOn (midiMode(), noteOffset_.load(), selectedSlice_.load(), note, slices);
    }

    void Engine::handleNoteOff (int note)
    {
        if (note < 0 || note > 127)
            return;

        pressed_[note] = false;
        voices_.noteOff (midiMode(), noteOffset_.load(), note);
    }

    // A step of the pattern starts: row N of the active scene plays slice N.
    //  - A one-shot slice starts again on every lit step.
    //  - A loop slice is gated: it starts on the first lit step of a run and is released on the
    //    first unlit step after it, so a run of lit steps is one held note.
    void Engine::fireStep (const StepSequencer::Event& e, const SliceBank::Snapshot& slices)
    {
        if (audioSample_ == nullptr)
            return;

        const int rows = std::min ((int) slices->size(), StepSequencer::kRows);

        for (int r = 0; r < StepSequencer::kRows; ++r)
        {
            const bool on = r < rows && ((seq_.rowBits (e.scene, r) >> e.step) & 1u) != 0;
            const bool was = seqPrev_[r];
            seqPrev_[r] = on;

            if (r >= rows)
            {
                if (seqUsed_[r])            // the slice was deleted while it was sounding
                {
                    voices_.stopPreview (r);
                    seqUsed_[r] = false;
                }

                continue;
            }

            const auto& slice = (*slices)[(size_t) r];

            if (slice.mode == Slice::Mode::loop)
            {
                if (on && ! was && slice.enabled)
                {
                    voices_.triggerPreview (r, slice);
                    seqUsed_[r] = true;
                }
                else if (was && ! on)
                {
                    voices_.stopPreview (r);
                }
            }
            else if (on && slice.enabled)
            {
                voices_.triggerPreview (r, slice);
                seqUsed_[r] = true;
            }
        }
    }

    // The sequencer stopped: let go of everything it started (with each slice's own fade-out).
    void Engine::releaseSequencerVoices()
    {
        for (int r = 0; r < StepSequencer::kRows; ++r)
        {
            if (seqUsed_[r])
                voices_.stopPreview (r);

            seqUsed_[r] = false;
            seqPrev_[r] = false;
        }
    }

    void Engine::renderChunk (float* out, int numFrames)
    {
        const auto slices = bank_.getCurrent();

        Command cmd;
        while (commands_.pop (cmd))
            applyCommand (cmd, slices);

        if (midiInbox_.tryDrain (midiScratch_))
        {
            // MIDI is applied at the top of the block (up to one block of timing imprecision),
            // the same deliberate simplification the plugin makes.
            for (const auto& m : midiScratch_)
            {
                if (m.isNoteOn())
                    handleNoteOn (m.data1, slices);
                else if (m.isNoteOff())
                    handleNoteOff (m.data1);
            }
        }

        // The step sequencer: the block is rendered in pieces, and each step that starts inside it
        // is fired on its exact frame (the voices simply carry on from where the last piece ended).
        StepSequencer::Event events[StepSequencer::kMaxEvents];
        bool seqStopped = false;
        const int numEvents = seq_.process (numFrames, sampleRate_, events, StepSequencer::kMaxEvents, seqStopped);

        if (seqStopped)
            releaseSequencerVoices();

        scratch_.clear (0, numFrames);

        int done = 0;

        for (int i = 0; i < numEvents; ++i)
        {
            const int at = std::clamp (events[i].frame, done, numFrames);

            if (at > done)
            {
                voices_.renderNextBlock (scratch_, done, at - done);
                done = at;
            }

            fireStep (events[i], slices);
        }

        if (done < numFrames)
            voices_.renderNextBlock (scratch_, done, numFrames - done);

        const auto gain = masterVolume_.load();
        const float* l = scratch_.channel (0);
        const float* r = scratch_.channel (1);
        float blockPeak = 0.0f;

        for (int i = 0; i < numFrames; ++i)
        {
            const auto sl = std::clamp (l[i] * gain, -1.0f, 1.0f);
            const auto sr = std::clamp (r[i] * gain, -1.0f, 1.0f);
            out[2 * i] = sl;
            out[2 * i + 1] = sr;
            blockPeak = std::max (blockPeak, std::max (std::abs (sl), std::abs (sr)));
        }

        // Peak meter with a simple per-block fall-off.
        const auto previous = peak_.load (std::memory_order_relaxed);
        const auto fall = (float) std::pow (0.5, (double) numFrames / (sampleRate_ * 0.25));
        peak_.store (std::max (blockPeak, previous * fall), std::memory_order_relaxed);

        // Publish playheads for the UI.
        int slicesOut[maxPlayheads];
        float posOut[maxPlayheads];
        const auto n = voices_.collectPlayheads (slicesOut, posOut, maxPlayheads);

        for (int i = 0; i < n; ++i)
        {
            playheadSlice_[i].store (slicesOut[i], std::memory_order_relaxed);
            playheadSeconds_[i].store (posOut[i], std::memory_order_relaxed);
        }

        playheadCount_.store (n, std::memory_order_release);
    }

    // ------------------------------------------------------------------------------------------
    // UI thread

    bool Engine::pushCommand (Command&& c, bool mustSucceed)
    {
        if (! mustSucceed)
            return commands_.push (std::move (c));

        // Critical commands (sample swaps) wait briefly for the audio thread to make room.
        for (int attempt = 0; attempt < 500; ++attempt)
        {
            Command copy = c; // push() consumes its argument only on success
            if (commands_.push (std::move (copy)))
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (1));
        }

        return false;
    }

    bool Engine::loadSample (SamplePtr sample)
    {
        bank_.clear();
        seq_.reset();   // the patterns belong to the old slices
        selectedSlice_.store (0);

        Command c;
        c.type = Command::Type::setSample;
        c.sample = sample;

        if (! pushCommand (std::move (c), true))
            return false;

        uiSample_ = std::move (sample);
        return true;
    }

    int Engine::detectTransients (float slider)
    {
        if (uiSample_ == nullptr || uiSample_->data.empty())
            return 0;

        auto found = TransientDetector::detect (uiSample_->data.channel (0), uiSample_->data.numFrames(),
                                                uiSample_->sampleRate, 1.0f - slider);
        const auto count = (int) found.size();
        bank_.replaceAll (std::move (found));
        selectedSlice_.store (0);
        return count;
    }

    void Engine::triggerPreview (int sliceIndex)
    {
        Command c; c.type = Command::Type::triggerPreview; c.a = sliceIndex;
        pushCommand (std::move (c), false);
    }

    void Engine::stopPreview (int sliceIndex)
    {
        Command c; c.type = Command::Type::stopPreview; c.a = sliceIndex;
        pushCommand (std::move (c), false);
    }

    void Engine::retriggerIfPlaying (int sliceIndex)
    {
        Command c; c.type = Command::Type::retriggerIfPlaying; c.a = sliceIndex;
        pushCommand (std::move (c), false);
    }

    void Engine::noteOn (int midiNote)
    {
        Command c; c.type = Command::Type::noteOn; c.a = midiNote;
        pushCommand (std::move (c), false);
    }

    void Engine::noteOff (int midiNote)
    {
        Command c; c.type = Command::Type::noteOff; c.a = midiNote;
        pushCommand (std::move (c), false);
    }

    void Engine::allNotesOff()
    {
        Command c; c.type = Command::Type::allNotesOff;
        pushCommand (std::move (c), true);
    }

    void Engine::setMidiMode (VoiceEngine::MidiMode mode)
    {
        midiMode_.store (mode == VoiceEngine::MidiMode::trigger ? 0 : 1);
        allNotesOff(); // held notes belong to the old mode's keying scheme
    }

    int Engine::copyPlayheads (Playhead* out, int maxOut) const noexcept
    {
        const auto n = std::min (playheadCount_.load (std::memory_order_acquire), maxOut);

        for (int i = 0; i < n; ++i)
        {
            out[i].slice = playheadSlice_[i].load (std::memory_order_relaxed);
            out[i].seconds = playheadSeconds_[i].load (std::memory_order_relaxed);
        }

        return n;
    }

    bool Engine::isSliceSounding (int sliceIndex) const noexcept
    {
        Playhead heads[maxPlayheads];
        const auto n = copyPlayheads (heads, maxPlayheads);

        for (int i = 0; i < n; ++i)
            if (heads[i].slice == sliceIndex)
                return true;

        return false;
    }

    void Engine::collectGarbage()
    {
        SamplePtr dead;
        while (trash_.pop (dead))
            dead.reset();
    }
}
