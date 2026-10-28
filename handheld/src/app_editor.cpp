#include "app.h"

#include <algorithm>
#include <cmath>

#include "format.h"
#include "stick_shape.h"

namespace kerf::ui
{
    namespace
    {
        constexpr double kMinSliceLength = 0.001; // seconds
        constexpr double kMinViewSpan = 0.004;

    }

    // --------------------------------------------------------------------------- bookkeeping

    int App::pianoNote (int slice) const
    {
        return cfg_.midiMode == 1 ? 60 + slice : cfg_.noteOffset + slice; // same keying as kerf.html
    }

    void App::syncHandles()
    {
        const auto slices = engine_.slices().getCurrent();

        if (selected_ < 0 || selected_ >= (int) slices->size())
        {
            handleSlice_ = -1;
            return;
        }

        const auto& s = (*slices)[(size_t) selected_];

        // Our double-precision copies only drift from the slice when something else changed it
        // (undo, a different slice selected): then the slice's values win.
        if (handleSlice_ != selected_ || (float) handleStart_ != s.start) handleStart_ = s.start;
        if (handleSlice_ != selected_ || (float) handleEnd_ != s.end) handleEnd_ = s.end;
        handleSlice_ = selected_;
    }

    void App::pushUndo (int kind)
    {
        // Continuous edits (a stick held down, a button auto-repeating) merge into one undo step
        // as long as they follow each other closely; discrete actions (kind >= 100) never merge.
        const bool merge = kind < 100 && kind == lastEditKind_ && (time_ - lastEditTime_) < 0.8;
        lastEditTime_ = time_;
        lastEditKind_ = kind;

        if (merge)
            return;

        Undo u;
        u.slices = *engine_.slices().getCurrent();
        u.selected = selected_;

        // Detect and clear-all replace every slice, and take every pattern with them (add and
        // delete note the one row they move: see addSlice / deleteSelected).
        if (kind == kUndoDetect || kind == kUndoClear)
            u.grid = std::make_shared<const kerf::StepSequencer::Grid> (engine_.sequencer().snapshot());

        undo_.push_back (std::move (u));

        if (undo_.size() > 64)
            undo_.erase (undo_.begin());
    }

    void App::undo()
    {
        if (undo_.empty())
        {
            showToast ("Nothing to undo");
            return;
        }

        auto u = std::move (undo_.back());
        undo_.pop_back();
        lastEditKind_ = -1;

        stopPlay();
        engine_.slices().replaceAll (std::move (u.slices));

        // The patterns go back with their slices (the sequencer page's own undo steps were
        // recorded against the old row numbers, so they go too).
        auto& seq = engine_.sequencer();

        if (u.grid != nullptr)
        {
            seq.restore (*u.grid);
            seqUndo_.clear();
        }
        else if (u.addedRow >= 0)
        {
            seq.removeRow (u.addedRow);
            seqUndo_.clear();
        }
        else if (u.removedRow >= 0)
        {
            seq.insertRow (u.removedRow);

            for (int s = 0; s < kerf::StepSequencer::kScenes; ++s)
                seq.setRowBits (s, u.removedRow, u.removedBits[s]);

            seqUndo_.clear();
        }

        const int count = engine_.slices().size();
        selected_ = std::clamp (u.selected, 0, std::max (0, count - 1));
        engine_.setSelectedSlice (selected_);
        handleSlice_ = -1;
        showToast ("Undone");
    }

    void App::mutateSelected (const std::function<void (kerf::Slice&)>& fn)
    {
        engine_.slices().updateAt (selected_, fn);
    }

    // ----------------------------------------------------------------------- view / zoom

    void App::clampView()
    {
        const double dur = wave_.duration();

        if (dur <= 0.0)
        {
            viewStart_ = 0.0;
            viewEnd_ = 1.0;
            return;
        }

        double span = std::clamp (viewEnd_ - viewStart_, std::min (kMinViewSpan, dur), dur);
        viewStart_ = std::clamp (viewStart_, 0.0, dur - span);
        viewEnd_ = viewStart_ + span;
    }

    void App::fitAll()
    {
        viewStart_ = 0.0;
        viewEnd_ = std::max (wave_.duration(), 0.001);
    }

    void App::fitSelected()
    {
        syncHandles();

        if (handleSlice_ < 0)
        {
            fitAll();
            return;
        }

        const double length = std::max (handleEnd_ - handleStart_, 0.02);
        double s = handleStart_ - length * 0.3;
        double e = handleEnd_ + length * 0.3;

        if (e - s < 0.05)
        {
            const double centre = 0.5 * (s + e);
            s = centre - 0.025;
            e = centre + 0.025;
        }

        viewStart_ = s;
        viewEnd_ = e;
        clampView();
    }

    bool App::viewShowsAll() const
    {
        return wave_.valid() && (viewEnd_ - viewStart_) >= 0.98 * wave_.duration();
    }

    void App::toggleView()
    {
        if (! wave_.valid())
            return;

        if (viewShowsAll()) fitSelected();
        else fitAll();
    }

    void App::ensureVisible (double t)
    {
        const double span = viewEnd_ - viewStart_;
        const double margin = span * 0.04;

        if (t < viewStart_ + margin)
        {
            viewStart_ = t - margin;
            viewEnd_ = viewStart_ + span;
        }
        else if (t > viewEnd_ - margin)
        {
            viewEnd_ = t + margin;
            viewStart_ = viewEnd_ - span;
        }

        clampView();
    }

    void App::zoomAbout (double anchor, double factor)
    {
        const double dur = wave_.duration();
        const double span = viewEnd_ - viewStart_;

        if (dur <= 0.0 || span <= 0.0)
            return;

        const double newSpan = std::clamp (span * factor, std::min (kMinViewSpan, dur), dur);
        const double rel = std::clamp ((anchor - viewStart_) / span, 0.0, 1.0);

        viewStart_ = anchor - rel * newSpan;
        viewEnd_ = viewStart_ + newSpan;
        clampView();
    }

    // ------------------------------------------------------------------------ slice edits

    void App::selectSlice (int index)
    {
        const int count = engine_.slices().size();

        if (count == 0)
            return;

        index = std::clamp (index, 0, count - 1);

        if (index == selected_ && handleSlice_ == selected_)
            return;

        const bool wasPlaying = playing_;
        stopPlay();
        selected_ = index;
        engine_.setSelectedSlice (selected_);
        syncHandles();

        // Keep the new slice in view: when zoomed in, jump to it.
        const bool nearlyAll = (viewEnd_ - viewStart_) >= 0.98 * wave_.duration();
        if (! nearlyAll && (handleEnd_ < viewStart_ || handleStart_ > viewEnd_))
            fitSelected();

        if (wasPlaying)
            startPlay(); // holding Play while stepping through slices auditions each one
    }

    void App::addSlice()
    {
        if (! wave_.valid())
        {
            showToast ("Load a sample first");
            return;
        }

        const double dur = wave_.duration();
        double t = 0.5 * (viewStart_ + viewEnd_); // new slices start at the middle of what you are looking at
        const auto current = engine_.slices().getCurrent();

        double limit = dur;
        for (const auto& s : *current)
            if (s.start > t + 1e-4)
                limit = std::min (limit, (double) s.start);

        double end = std::min ({ t + 0.5, limit, dur });

        if (end - t < 0.01)
        {
            t = std::max (0.0, end - 0.1);
            end = std::max (end, t + 0.01);
        }

        pushUndo (kUndoAdd);

        kerf::Slice n; // defaults mirror kerf.html: pitch 1, volume 1, pan 0, shot, fades 1 / 50 ms
        n.start = (float) t;
        n.end = (float) std::min (end, dur);

        auto copy = *current;
        const auto at = std::upper_bound (copy.begin(), copy.end(), n, [] (const kerf::Slice& a, const kerf::Slice& b) { return a.start < b.start; });
        const int index = (int) (at - copy.begin());
        copy.insert (at, n);
        engine_.sequencer().insertRow (index);   // the slices after the new one move down; so do their patterns
        undo_.back().addedRow = index;
        seqUndo_.clear();
        engine_.slices().replaceAll (std::move (copy));

        handleSlice_ = -1;
        selected_ = -1; // force selectSlice to run its full logic
        selectSlice (index);
        showToast ("Slice " + integer (index + 1) + " added");
    }

    void App::deleteSelected()
    {
        const int count = engine_.slices().size();

        if (count == 0 || selected_ < 0 || selected_ >= count)
        {
            showToast ("No slice to delete");
            return;
        }

        pushUndo (kUndoDelete);
        undo_.back().removedRow = selected_;

        for (int s = 0; s < kerf::StepSequencer::kScenes; ++s)
            undo_.back().removedBits[s] = engine_.sequencer().rowBits (s, selected_);

        stopPlay();
        engine_.stopPreview (selected_);
        engine_.sequencer().removeRow (selected_);   // the pattern goes with the slice
        seqUndo_.clear();
        engine_.slices().removeAt (selected_);

        selected_ = std::clamp (selected_, 0, std::max (0, count - 2));
        engine_.setSelectedSlice (selected_);
        handleSlice_ = -1;
        showToast ("Slice deleted - undo with " + std::string (usingKeyboard_ ? "Backspace" : "Select+B"));
    }

    void App::clearSlices()
    {
        if (engine_.slices().size() == 0)
            return;

        pushUndo (kUndoClear);
        stopPlay();
        engine_.sequencer().clearAll();
        seqUndo_.clear();
        engine_.slices().clear();
        selected_ = 0;
        handleSlice_ = -1;
        engine_.setSelectedSlice (0);
        showToast ("All slices cleared");
    }

    void App::moveHandle (bool isStart, double delta)
    {
        syncHandles();

        if (handleSlice_ < 0 || delta == 0.0)
            return;

        const double dur = wave_.duration();

        if (isStart) handleStart_ = std::clamp (handleStart_ + delta, 0.0, std::max (0.0, handleEnd_ - kMinSliceLength));
        else         handleEnd_ = std::clamp (handleEnd_ + delta, std::min (dur, handleStart_ + kMinSliceLength), dur);

        const float fs = (float) handleStart_, fe = (float) handleEnd_;
        const auto current = engine_.slices().getCurrent();
        const auto& s = (*current)[(size_t) selected_];

        if (s.start != fs || s.end != fe)
        {
            pushUndo (isStart ? 1 : 2);
            engine_.slices().updateAt (selected_, [fs, fe] (kerf::Slice& sl) { sl.start = fs; sl.end = fe; });
            needRetrigger_ = true;
        }

        ensureVisible (isStart ? handleStart_ : handleEnd_);
    }

    // -------------------------------------------------------------------------- playing

    void App::startPlay()
    {
        const int count = engine_.slices().size();

        if (selected_ < 0 || selected_ >= count)
            return;

        engine_.triggerPreview (selected_);
        playing_ = true;
        playingSlice_ = selected_;
    }

    void App::stopPlay()
    {
        if (playing_)
            engine_.stopPreview (playingSlice_);

        playing_ = false;
        playingSlice_ = -1;
    }

    // ---------------------------------------------------------------------------- update

    void App::updateEditor (const Pad& pad, float dt)
    {
        const int count = engine_.slices().size();

        if (count > 0)
            selected_ = std::clamp (selected_, 0, count - 1);

        syncHandles();

        // ---- keyboard-only shortcuts
        for (auto c : pad.commands)
        {
            if (c == KeyCommand::deleteSlice) deleteSelected();
            else if (c == KeyCommand::undo) undo();
        }

        // ---- Start: menu
        if (pad.pressed[(int) Btn::Start])
        {
            openMenu();
            return;
        }

        // ---- Select: tap = the next page (the sequencer); Select+X = delete; Select+B = undo.
        // The page changes when Select is let go, so that Select+X / Select+B can be told apart.
        const bool selectHeld = pad.down[(int) Btn::Select];

        if (pad.pressed[(int) Btn::Select])
            selectChord_ = false;

        if (selectHeld && pad.pressed[(int) Btn::X]) { deleteSelected(); selectChord_ = true; }
        else if (selectHeld && pad.pressed[(int) Btn::B]) { undo(); selectChord_ = true; }
        else if (! selectHeld)
        {
            if (pad.pressed[(int) Btn::X]) addSlice();
            if (pad.pressed[(int) Btn::B]) toggleTransport();   // the sequencer's play / stop, from either page
            if (pad.pressed[(int) Btn::Y])
            {
                if (wave_.valid()) setScreen (Screen::detect);
                else showToast ("Load a sample first");
            }
        }

        if (pad.released[(int) Btn::Select] && ! selectChord_)
            cyclePage (+1);

        if (screen_ != Screen::editor)
            return;

        // ---- A: play the selected slice while held
        if (pad.pressed[(int) Btn::A] && ! selectHeld)
            startPlay();

        if (pad.released[(int) Btn::A])
            stopPlay();

        // ---- L1 / R1: previous / next slice
        if (pad.repeated[(int) Btn::L1]) selectSlice (selected_ - 1);
        if (pad.repeated[(int) Btn::R1]) selectSlice (selected_ + 1);

        // ---- piano keys (computer keyboard)
        for (const auto& e : pad.piano)
        {
            if (e.slice < 0 || e.slice >= 16)
                continue;

            if (e.down && pianoNotes_[e.slice] < 0)
            {
                pianoNotes_[e.slice] = pianoNote (e.slice);
                engine_.noteOn (pianoNotes_[e.slice]);
            }
            else if (! e.down && pianoNotes_[e.slice] >= 0)
            {
                engine_.noteOff (pianoNotes_[e.slice]);
                pianoNotes_[e.slice] = -1;
            }
        }

        handleDpad (pad);
        handleSticks (pad, dt);

        // While a handle moves, a sounding slice is retriggered a few times per second (like
        // dragging a boundary in the web app) so the change can be heard as it happens.
        retriggerTimer_ += dt;

        if (needRetrigger_ && retriggerTimer_ >= 0.09f)
        {
            engine_.retriggerIfPlaying (selected_);
            retriggerTimer_ = 0.0f;
            needRetrigger_ = false;
        }
    }

    void App::handleDpad (const Pad& pad)
    {
        if (pad.repeated[(int) Btn::Up])   paramRow_ = (paramRow_ + paramRows - 1) % paramRows;
        if (pad.repeated[(int) Btn::Down]) paramRow_ = (paramRow_ + 1) % paramRows;

        int dir = 0;
        float held = 0.0f;

        if (pad.repeated[(int) Btn::Left])  { dir -= 1; held = pad.held[(int) Btn::Left]; }
        if (pad.repeated[(int) Btn::Right]) { dir += 1; held = pad.held[(int) Btn::Right]; }

        if (dir != 0)
        {
            // Holding the button speeds a long adjustment up.
            const int steps = held < 1.0f ? 1 : held < 2.0f ? 3 : held < 3.5f ? 8 : 20;
            adjustParam (paramRow_, dir, steps, pad);
        }
    }

    void App::adjustParam (int row, int dir, int steps, const Pad& pad)
    {
        const int count = engine_.slices().size();

        if (count == 0 || selected_ < 0 || selected_ >= count)
            return;

        const bool fine = pad.down[(int) Btn::L2];
        const bool coarse = pad.down[(int) Btn::R2];
        const auto current = (*engine_.slices().getCurrent())[(size_t) selected_];

        switch (row)
        {
            case pStart:
            case pEnd:
            {
                const double step = fine ? 0.0001 : coarse ? 0.010 : 0.001;
                moveHandle (row == pStart, dir * step * steps);
                return;
            }

            case pPitch:
            {
                const int semis = (int) std::lround (std::log2 ((double) current.pitch) * 12.0);
                const int next = std::clamp (semis + dir * (coarse ? 12 : 1), -24, 24); // kerf.html: -24..+24 semitones
                pushUndo (10 + row);
                mutateSelected ([next] (kerf::Slice& s) { s.pitch = (float) std::pow (2.0, next / 12.0); });
                return;
            }

            case pVolume:
            {
                const float next = std::clamp (std::round ((current.volume + 0.05f * (float) dir) * 20.0f) / 20.0f, 0.0f, 1.0f);
                pushUndo (10 + row);
                mutateSelected ([next] (kerf::Slice& s) { s.volume = next; });
                return;
            }

            case pPan:
            {
                const float next = std::clamp (std::round ((current.pan + 0.1f * (float) dir) * 10.0f) / 10.0f, -1.0f, 1.0f);
                pushUndo (10 + row);
                mutateSelected ([next] (kerf::Slice& s) { s.pan = next; });
                return;
            }

            case pMode:
                pushUndo (10 + row);
                mutateSelected ([] (kerf::Slice& s) { s.mode = s.mode == kerf::Slice::Mode::shot ? kerf::Slice::Mode::loop : kerf::Slice::Mode::shot; });
                return;

            case pFadeIn:
            {
                const float next = std::clamp (current.fadeInMs + (float) (dir * steps), 0.0f, 100.0f);
                pushUndo (10 + row);
                mutateSelected ([next] (kerf::Slice& s) { s.fadeInMs = next; });
                return;
            }

            case pFadeOut:
            {
                const float next = std::clamp (current.fadeOutMs + (float) (dir * steps), 0.0f, 200.0f);
                pushUndo (10 + row);
                mutateSelected ([next] (kerf::Slice& s) { s.fadeOutMs = next; });
                return;
            }

            case pEnabled:
                pushUndo (10 + row);
                mutateSelected ([] (kerf::Slice& s) { s.enabled = ! s.enabled; });
                return;

            default:
                return;
        }
    }

    void App::handleSticks (const Pad& pad, float dt)
    {
        if (! wave_.valid() || engine_.slices().size() == 0)
            return;

        const float dz = cfg_.deadzone;
        float lx = pad.lx, ly = pad.ly, rx = pad.rx, ry = pad.ry;
        lockAxis (lx, ly);
        lockAxis (rx, ry);

        // L2 = fine, R2 = coarse (both = a little finer than normal).
        const double mult = (pad.down[(int) Btn::L2] ? 0.1 : 1.0) * (pad.down[(int) Btn::R2] ? 4.0 : 1.0);
        const double span = viewEnd_ - viewStart_;
        const double rate = span * (double) cfg_.stickSpeed * (double) dt * mult;

        // Left stick = START handle, right stick = END handle.
        moveHandle (true,  (double) shapeStick (lx, dz) * rate);
        moveHandle (false, (double) shapeStick (rx, dz) * rate);

        // Up / down on a stick zooms around that stick's handle (up = in).
        const float zl = shapeStick (ly, dz), zr = shapeStick (ry, dz);

        if (zl != 0.0f) zoomAbout (handleStart_, std::exp ((double) zl * 2.2 * (double) dt));
        if (zr != 0.0f) zoomAbout (handleEnd_,   std::exp ((double) zr * 2.2 * (double) dt));
    }
}
