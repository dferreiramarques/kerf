// The second page of Kerf: a 16-step pattern sequencer for the slices. Row N is slice N, the 16
// columns are sixteenth notes, and there are eight scenes (patterns). The engine plays it
// (kerf::StepSequencer); this file is the cursor, the editing and the drawing.

#include "app.h"

#include <algorithm>
#include <cmath>

#include "draw_util.h"
#include "format.h"
#include "stick_shape.h"
#include "theme.h"
#include "utf8.h"

namespace kerf::ui
{
    namespace
    {
        using namespace theme;
        using Seq = kerf::StepSequencer;

        // ---- layout (640 x 480) ----
        constexpr int kMargin = 8;
        constexpr int kStripY = 32, kStripH = 40;              // tempo, swing, length, scenes
        constexpr int kGridY = 76, kGridH = 358;               // the pattern
        constexpr int kHintsY = 438, kHintsH = 42;
        constexpr int kHeadH = 20;                             // step numbers above the rows
        constexpr int kRowH = 22;
        constexpr int kLabelW = 108;                           // slice number + mode, left of the cells
        constexpr int kCellW = 30, kGroupGap = 6;              // a gap after every four steps
        constexpr int kGridW = kLabelW + Seq::kSteps * kCellW + 3 * kGroupGap;
        constexpr int kVisibleRows = (kGridH - kHeadH - 6) / kRowH;

        constexpr float kAuditionSeconds = 0.35f;

        int cellX (int left, int step) { return left + kLabelW + step * kCellW + (step / 4) * kGroupGap; }

        const uint32_t kWhite = rgb (255, 255, 255);
        const uint32_t kCellA = rgb (31, 34, 60), kCellB = rgb (40, 44, 76);   // two shades so the beats can be told apart
        const uint32_t kCellOut = rgb (20, 22, 36);                            // steps beyond the pattern length
    }

    // ------------------------------------------------------------------------------ pages

    void App::enterSequencer()
    {
        seqPainting_ = false;
        seqHearing_ = false;
        auditionTimer_ = 0.0f;

        const int count = engine_.slices().size();
        seqRow_ = count > 0 ? std::clamp (selected_, 0, count - 1) : 0;   // the slice you were looking at
        clampSeqCursor();

        tempoJog_ = engine_.sequencer().tempo();
        swingJog_ = engine_.sequencer().swing();
    }

    void App::leaveSequencer()
    {
        seqPainting_ = false;

        if (seqHearing_ || auditionTimer_ > 0.0f)
            seqAudition (auditionRow_, false);

        seqHearing_ = false;
        auditionTimer_ = 0.0f;
    }

    void App::clampSeqCursor()
    {
        const int count = engine_.slices().size();

        seqRow_ = count > 0 ? std::clamp (seqRow_, 0, count - 1) : 0;
        seqCol_ = std::clamp (seqCol_, 0, Seq::kSteps - 1);

        seqScroll_ = std::clamp (seqScroll_, 0, std::max (0, count - kVisibleRows));

        if (seqRow_ < seqScroll_)
            seqScroll_ = seqRow_;
        else if (seqRow_ >= seqScroll_ + kVisibleRows)
            seqScroll_ = seqRow_ - kVisibleRows + 1;
    }

    // B, on either page.
    void App::toggleTransport()
    {
        auto& seq = engine_.sequencer();

        if (seq.isPlaying())
        {
            seq.stop();
            return;
        }

        seq.play();

        if (seq.cellCount (seq.scene(), engine_.slices().size()) == 0)
            showToast (mainScreen_ == Screen::sequencer ? "This scene is empty - A puts steps in it"
                                                        : "Playing - the pattern is empty (Select: sequencer page)", 3.0f);
    }

    // ----------------------------------------------------------------------- editing helpers

    void App::seqPushUndo (int scene)
    {
        const auto& seq = engine_.sequencer();

        SeqUndo u;
        u.scene = scene;
        u.rows.resize (Seq::kRows);

        for (int r = 0; r < Seq::kRows; ++r)
            u.rows[(size_t) r] = seq.rowBits (scene, r);

        seqUndo_.push_back (std::move (u));

        if (seqUndo_.size() > 64)
            seqUndo_.erase (seqUndo_.begin());
    }

    void App::seqUndo()
    {
        if (seqUndo_.empty())
        {
            showToast ("Nothing to undo");
            return;
        }

        auto u = std::move (seqUndo_.back());
        seqUndo_.pop_back();

        auto& seq = engine_.sequencer();

        for (int r = 0; r < Seq::kRows; ++r)
            seq.setRowBits (u.scene, r, u.rows[(size_t) r]);

        seq.setScene (u.scene);   // show what came back
        showToast ("Undone");
    }

    void App::seqSetCell (int row, int col, bool on)
    {
        auto& seq = engine_.sequencer();
        seq.setCell (seq.scene(), row, col, on);
    }

    void App::seqClearRow()
    {
        auto& seq = engine_.sequencer();
        const int count = engine_.slices().size();

        if (count == 0 || seqRow_ < 0 || seqRow_ >= count)
            return;

        if (seq.rowBits (seq.scene(), seqRow_) == 0)
        {
            showToast ("Row " + integer (seqRow_ + 1) + " is already empty");
            return;
        }

        seqPushUndo (seq.scene());
        seq.setRowBits (seq.scene(), seqRow_, 0);
        showToast ("Row " + integer (seqRow_ + 1) + " cleared - " + (usingKeyboard_ ? "Backspace" : "Select+B") + " undoes");
    }

    void App::seqClearScene()
    {
        auto& seq = engine_.sequencer();

        if (seq.cellCount (seq.scene()) == 0)
        {
            showToast ("Scene " + integer (seq.scene() + 1) + " is already empty");
            return;
        }

        seqPushUndo (seq.scene());
        seq.clearScene (seq.scene());
        showToast ("Scene " + integer (seq.scene() + 1) + " cleared - " + (usingKeyboard_ ? "Backspace" : "Select+B") + " undoes");
    }

    void App::seqCopySceneToNext()
    {
        auto& seq = engine_.sequencer();
        const int from = seq.scene();

        if (from >= Seq::kScenes - 1)
        {
            showToast ("This is the last scene");
            return;
        }

        seqPushUndo (from + 1);
        seq.copyScene (from, from + 1);
        seq.setScene (from + 1);   // carry on editing the copy
        showToast ("Scene " + integer (from + 1) + " copied to scene " + integer (from + 2));
    }

    void App::seqChangeScene (int direction)
    {
        auto& seq = engine_.sequencer();
        const int next = std::clamp (seq.scene() + direction, 0, Seq::kScenes - 1);

        if (next == seq.scene())
            return;

        seq.setScene (next);

        if (seq.isPlaying() && seq.activeScene() != next)
            showToast ("Scene " + integer (next + 1) + " starts with the next loop", 1.8f);
    }

    void App::seqChangeLength (int direction)
    {
        auto& seq = engine_.sequencer();
        seq.setLength (seq.length() + direction);
    }

    void App::seqChangeTempo (int direction)
    {
        auto& seq = engine_.sequencer();
        seq.setTempo (std::round (seq.tempo()) + (float) direction);   // whole BPM from the menu
        tempoJog_ = seq.tempo();
    }

    void App::seqChangeSwing (int direction)
    {
        auto& seq = engine_.sequencer();
        seq.setSwing (std::round (seq.swing() * 100.0f) / 100.0f + 0.01f * (float) direction);
        swingJog_ = seq.swing();
    }

    // Row N plays slice N: hold the row's slice (Y) or hear it briefly (a step switched on).
    void App::seqAudition (int row, bool start)
    {
        if (start)
        {
            if (row < 0 || row >= engine_.slices().size())
                return;

            if (auditionRow_ >= 0 && auditionRow_ != row)
                engine_.stopPreview (auditionRow_);

            auditionRow_ = row;
            engine_.triggerPreview (row);
        }
        else
        {
            if (auditionRow_ >= 0)
                engine_.stopPreview (auditionRow_);

            auditionRow_ = -1;
        }
    }

    void App::seqMoveCursor (int dRow, int dCol, bool paint)
    {
        const int count = engine_.slices().size();

        if (count == 0)
            return;

        const int oldRow = seqRow_;
        seqRow_ = std::clamp (seqRow_ + dRow, 0, count - 1);
        seqCol_ = std::clamp (seqCol_ + dCol, 0, Seq::kSteps - 1);
        clampSeqCursor();

        // The slicer page follows the cursor: switch pages and the slice you were on is selected there.
        if (seqRow_ != oldRow)
            selectSlice (seqRow_);

        if (paint)
            seqSetCell (seqRow_, seqCol_, seqPaintValue_);
    }

    // Tempo and swing are on the sticks, like the slice handles are on the other page: left stick
    // up/down = tempo, right stick up/down = swing. L2 / R2 slow them down / speed them up.
    void App::seqJog (const Pad& pad, float dt)
    {
        auto& seq = engine_.sequencer();
        const float dz = cfg_.deadzone;

        float lx = pad.lx, ly = pad.ly, rx = pad.rx, ry = pad.ry;
        lockAxis (lx, ly);
        lockAxis (rx, ry);

        const double mult = (pad.down[(int) Btn::L2] ? 0.1 : 1.0) * (pad.down[(int) Btn::R2] ? 4.0 : 1.0);
        const float tempoStick = shapeStick (ly, dz);
        const float swingStick = shapeStick (ry, dz);

        if (tempoStick != 0.0f)
        {
            // Up is negative (the SDL convention): up = faster. The jog value keeps the fraction that
            // the 0.1 BPM rounding would otherwise swallow when moving slowly.
            tempoJog_ = std::clamp (tempoJog_ - (float) ((double) tempoStick * 40.0 * mult * (double) dt), Seq::kMinTempo, Seq::kMaxTempo);
            seq.setTempo (std::round (tempoJog_ * 10.0f) / 10.0f);
        }
        else
        {
            tempoJog_ = seq.tempo();
        }

        if (swingStick != 0.0f)
        {
            swingJog_ = std::clamp (swingJog_ - (float) ((double) swingStick * 0.12 * mult * (double) dt), Seq::kMinSwing, Seq::kMaxSwing);
            seq.setSwing (std::round (swingJog_ * 100.0f) / 100.0f);
        }
        else
        {
            swingJog_ = seq.swing();
        }
    }

    // ---------------------------------------------------------------------------- update

    void App::updateSequencer (const Pad& pad, float dt)
    {
        auto& seq = engine_.sequencer();
        clampSeqCursor();

        if (auditionTimer_ > 0.0f)
        {
            auditionTimer_ -= dt;

            if (auditionTimer_ <= 0.0f && ! seqHearing_)
                seqAudition (auditionRow_, false);
        }

        // ---- keyboard-only shortcuts: Delete clears the row, Backspace undoes
        for (auto c : pad.commands)
        {
            if (c == KeyCommand::deleteSlice) seqClearRow();
            else if (c == KeyCommand::undo) seqUndo();
        }

        // ---- Start: menu
        if (pad.pressed[(int) Btn::Start])
        {
            openMenu();
            return;
        }

        // ---- Select: tap = the next page (the slicer); chords: + X clears the row, + B undoes,
        // + Left/Right sets the pattern length. The page changes when Select is let go.
        const bool selectHeld = pad.down[(int) Btn::Select];

        if (pad.pressed[(int) Btn::Select])
            selectChord_ = false;

        if (selectHeld)
        {
            if (pad.pressed[(int) Btn::X]) { seqClearRow(); selectChord_ = true; }
            else if (pad.pressed[(int) Btn::B]) { seqUndo(); selectChord_ = true; }

            if (pad.repeated[(int) Btn::Left])  { seqChangeLength (-1); selectChord_ = true; }
            if (pad.repeated[(int) Btn::Right]) { seqChangeLength (+1); selectChord_ = true; }
        }

        if (pad.released[(int) Btn::Select] && ! selectChord_)
        {
            cyclePage (+1);
            return;
        }

        if (screen_ != Screen::sequencer)
            return;

        const int count = engine_.slices().size();

        // ---- the transport and the scenes
        if (! selectHeld)
        {
            if (pad.pressed[(int) Btn::B])  toggleTransport();
            if (pad.pressed[(int) Btn::L1]) seqChangeScene (-1);
            if (pad.pressed[(int) Btn::R1]) seqChangeScene (+1);
        }

        // ---- Y: hear the slice of the row under the cursor, while held
        if (pad.pressed[(int) Btn::Y] && ! selectHeld && count > 0)
        {
            seqHearing_ = true;
            seqAudition (seqRow_, true);
        }

        if (pad.released[(int) Btn::Y] && seqHearing_)
        {
            seqHearing_ = false;
            seqAudition (auditionRow_, false);
        }

        // ---- A: switch the step under the cursor on / off, X: switch it off. While one is held, the
        // cursor paints what it just did (so a whole row is one press and a sweep of the D-pad).
        if (count > 0 && ! selectHeld)
        {
            if (pad.pressed[(int) Btn::A])
            {
                seqPushUndo (seq.scene());
                const bool now = ! seq.cell (seq.scene(), seqRow_, seqCol_);
                seqSetCell (seqRow_, seqCol_, now);
                seqPainting_ = true;
                seqPaintValue_ = now;

                if (now && ! seq.isPlaying() && ! seqHearing_)   // a step you just put in: hear what it is
                {
                    seqAudition (seqRow_, true);
                    auditionTimer_ = kAuditionSeconds;
                }
            }
            else if (pad.pressed[(int) Btn::X])
            {
                seqPushUndo (seq.scene());
                seqSetCell (seqRow_, seqCol_, false);
                seqPainting_ = true;
                seqPaintValue_ = false;
            }
        }

        if (! pad.down[(int) Btn::A] && ! pad.down[(int) Btn::X])
            seqPainting_ = false;

        // ---- D-pad: the cursor
        if (! selectHeld)
        {
            int dRow = 0, dCol = 0;
            if (pad.repeated[(int) Btn::Up])    dRow -= 1;
            if (pad.repeated[(int) Btn::Down])  dRow += 1;
            if (pad.repeated[(int) Btn::Left])  dCol -= 1;
            if (pad.repeated[(int) Btn::Right]) dCol += 1;

            if (dRow != 0 || dCol != 0)
                seqMoveCursor (dRow, dCol, seqPainting_);
        }

        // ---- sticks: tempo and swing
        seqJog (pad, dt);
    }

    // ------------------------------------------------------------------------------ drawing

    void App::drawSequencer (Canvas& c)
    {
        c.clear (bg);
        drawHeader (c);
        drawSeqStrip (c);
        drawSeqGrid (c);
        drawSeqHints (c);
    }

    void App::drawSeqStrip (Canvas& c)
    {
        const auto& seq = engine_.sequencer();
        const int y0 = kStripY;

        drawPanel (c, kMargin, y0, kScreenW - 2 * kMargin, kStripH);

        // transport: a triangle while it plays, a square while it is stopped
        const bool playing = seq.isPlaying();
        const int tx = kMargin + 12;

        if (playing) c.fillTriangle (tx, y0 + 11, tx, y0 + 29, tx + 15, y0 + 20, ok);
        else         c.fillRect (tx + 1, y0 + 12, 14, 16, textDim);

        c.drawText (tx + 26, y0 + 12, playing ? "PLAY" : "STOP", playing ? ok : textDim, 2);

        auto field = [&] (int x, const char* label, const std::string& value, uint32_t col)
        {
            c.drawText (x, y0 + 6, label, textFaint, 1);
            c.drawText (x, y0 + 18, value, col, 2);
        };

        field (126, "TEMPO", fixed ((double) seq.tempo(), 1), startCol);
        field (222, "SWING", integer (std::lround (seq.swing() * 100.0f)) + "%", endCol);
        field (306, "LENGTH", integer (seq.length()), text);

        // scenes: the one being edited is lit; a green frame marks the one that sounds, when it is another
        c.drawText (388, y0 + 6, "SCENE", textFaint, 1);
        const int editing = seq.scene(), sounding = seq.activeScene();

        for (int s = 0; s < Seq::kScenes; ++s)
        {
            const int x = 388 + s * 29, y = y0 + 17;
            const bool isEditing = s == editing;
            const bool isSounding = playing && s == sounding;
            const bool hasSteps = seq.cellCount (s, engine_.slices().size()) > 0;

            c.fillRect (x, y, 26, 18, isEditing ? brand : bg);
            c.strokeRect (x, y, 26, 18, isEditing ? brandHi : isSounding ? ok : border);

            if (isSounding && ! isEditing)
                c.strokeRect (x + 1, y + 1, 24, 16, ok);

            const std::string label = integer (s + 1);
            c.drawText (x + (26 - Canvas::textWidth (label, 2)) / 2, y + 1, label,
                        isEditing ? kWhite : hasSteps ? text : textFaint, 2);
        }
    }

    void App::drawSeqGrid (Canvas& c)
    {
        const auto& seq = engine_.sequencer();
        const int gx = kMargin, gw = kScreenW - 2 * kMargin;

        drawPanel (c, gx, kGridY, gw, kGridH);

        const auto slices = engine_.slices().getCurrent();
        const int count = (int) slices->size();

        if (count == 0)
        {
            const std::string a = "No slices yet";
            c.drawText (gx + (gw - Canvas::textWidth (a, 2)) / 2, kGridY + 120, a, textDim, 2);

            const std::string b = usingKeyboard_ ? "3: back to the slicer, then 2 to auto-detect" : "Select: back to the slicer, then Y to auto-detect";
            c.drawText (gx + (gw - Canvas::textWidth (b, 1)) / 2, kGridY + 150, b, textFaint, 1);

            const std::string d = "Every slice becomes a row here.";
            c.drawText (gx + (gw - Canvas::textWidth (d, 1)) / 2, kGridY + 166, d, textFaint, 1);
            return;
        }

        const int left = gx + (gw - kGridW) / 2;
        const int scene = seq.scene();
        const int length = seq.length();
        const int current = seq.currentStep();
        const bool showHead = seq.isPlaying() && current >= 0 && seq.activeScene() == scene;

        // step numbers
        for (int s = 0; s < Seq::kSteps; ++s)
        {
            const int x = cellX (left, s);
            const std::string n = integer (s + 1);
            const bool isHead = showHead && s == current;
            const uint32_t col = isHead ? kWhite : s >= length ? withAlpha (textFaint, 130) : (s % 4 == 0 ? text : textDim);

            c.drawText (x + (kCellW - 2 - Canvas::textWidth (n, 1)) / 2, kGridY + 5, n, col, 1);

            if (isHead)
                c.fillRect (x, kGridY + kHeadH - 5, kCellW - 2, 2, brandHi);
        }

        const int rows = std::min (kVisibleRows, count - seqScroll_);
        c.setClip (gx + 1, kGridY + kHeadH, gw - 2, kGridH - kHeadH - 1);

        for (int k = 0; k < rows; ++k)
        {
            const int r = seqScroll_ + k;
            const auto& slice = (*slices)[(size_t) r];
            const int y = kGridY + kHeadH + k * kRowH;
            const bool isCursorRow = r == seqRow_;
            const uint32_t sliceCol = sliceColour (r);

            // the row's label: colour, number, and L(oop) / OFF
            if (isCursorRow)
            {
                c.fillRect (left, y, kLabelW - 6, kRowH - 2, panelHi);
                c.fillRect (left, y, 3, kRowH - 2, brandHi);
            }

            c.fillRect (left + 7, y + 3, 5, kRowH - 8, withAlpha (sliceCol, slice.enabled ? 255 : 90));
            c.drawText (left + 17, y + 2, integer (r + 1), slice.enabled ? (isCursorRow ? text : textDim) : textFaint, 2);

            if (! slice.enabled)
                c.drawText (left + 17 + 50, y + 7, "OFF", danger, 1);
            else if (slice.mode == kerf::Slice::Mode::loop)
                c.drawText (left + 17 + 50, y + 7, "LOOP", brandHi, 1);

            // the cells
            for (int s = 0; s < Seq::kSteps; ++s)
            {
                const int x = cellX (left, s);
                const bool inside = s < length;
                const bool on = seq.cell (scene, r, s);
                const bool isHead = showHead && s == current;
                const int w = kCellW - 2, h = kRowH - 3;

                c.fillRect (x, y, w, h, inside ? (((s / 4) & 1) ? kCellB : kCellA) : kCellOut);

                if (on)
                {
                    c.fillRect (x, y, w, h, inside ? sliceCol : withAlpha (sliceCol, 70));

                    if (inside)
                        c.fillRect (x, y, w, 2, withAlpha (kWhite, 80));   // a little light on top
                }

                if (isHead)
                    c.fillRect (x, y, w, h, withAlpha (kWhite, on ? 90 : 34));
            }
        }

        // the cursor, drawn last so it sits on top of everything
        if (seqRow_ >= seqScroll_ && seqRow_ < seqScroll_ + rows)
        {
            const int x = cellX (left, seqCol_), y = kGridY + kHeadH + (seqRow_ - seqScroll_) * kRowH;
            const uint32_t col = seqPainting_ ? (seqPaintValue_ ? ok : danger) : kWhite;
            c.strokeRect (x - 1, y - 1, kCellW, kRowH - 1, col);
            c.strokeRect (x, y, kCellW - 2, kRowH - 3, col);
        }

        c.resetClip();

        // scroll bar
        if (count > kVisibleRows)
        {
            const int trackY = kGridY + kHeadH, trackH = kVisibleRows * kRowH;
            const int thumbH = std::max (10, trackH * kVisibleRows / count);
            const int thumbY = trackY + (trackH - thumbH) * seqScroll_ / std::max (1, count - kVisibleRows);
            c.fillRect (gx + gw - 5, trackY, 2, trackH, border);
            c.fillRect (gx + gw - 5, thumbY, 2, thumbH, brandHi);
        }
    }

    void App::drawSeqHints (Canvas& c)
    {
        c.fillRect (0, kHintsY, kScreenW, kHintsH, panel);
        c.hline (0, kHintsY, kScreenW, border);

        std::vector<Seg> line1, line2;

        if (usingKeyboard_)
        {
            line1 = { { "Space", brandHi }, { " step  ", textDim }, { "1", brandHi }, { " erase  ", textDim }, { "Esc", brandHi }, { " play  ", textDim },
                      { "2", brandHi }, { " hear", textDim } };
            line2 = { { "Q/R", textDim }, { " scene  ", textFaint }, { "C/V", startCol }, { " tempo  ", textDim }, { ",/.", endCol }, { " swing  ", textDim },
                      { "3+L/R", textDim }, { " length  ", textFaint }, { "3", textDim }, { " page  ", textFaint }, { "Tab", textDim }, { " menu  ", textFaint },
                      { "F1", textDim }, { " help", textFaint } };
        }
        else
        {
            line1 = { { "A", brandHi }, { " step  ", textDim }, { "X", brandHi }, { " erase  ", textDim }, { "B", brandHi }, { " play  ", textDim },
                      { "Y", brandHi }, { " hear", textDim } };
            line2 = { { "L1/R1", textDim }, { " scene  ", textFaint }, { "L-STICK", startCol }, { " tempo  ", textDim }, { "R-STICK", endCol }, { " swing  ", textDim },
                      { "Sel+L/R", textDim }, { " length  ", textFaint }, { "Select", textDim }, { " page", textFaint } };
        }

        drawSegments (c, kMargin + 2, kHintsY + 5, line1, 2);
        drawSegments (c, kMargin + 2, kHintsY + 27, line2, 1);
    }
}
