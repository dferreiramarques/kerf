#include "app.h"

#include <algorithm>
#include <cmath>

#include "draw_util.h"
#include "format.h"
#include "help_text.h"
#include "path_util.h"
#include "theme.h"
#include "utf8.h"

namespace kerf::ui
{
    namespace
    {
        using namespace theme;

        // ---- layout (640 x 480) ----
        constexpr int kMargin = 8;
        constexpr int kHeaderH = 28;
        constexpr int kWaveY = 32, kWaveH = 186;           // waveform panel
        constexpr int kLowerY = 224, kLowerH = 210;        // lists
        constexpr int kListW = 224;
        constexpr int kParamX = 240, kParamW = 392;
        constexpr int kHintsY = 438, kHintsH = 42;
        constexpr int kRowH = 20;

        double niceStep (double raw)
        {
            if (raw <= 0.0) return 1.0;
            const double mag = std::pow (10.0, std::floor (std::log10 (raw)));
            const double f = raw / mag;
            const double nice = f <= 1.0 ? 1.0 : f <= 2.0 ? 2.0 : f <= 5.0 ? 5.0 : 10.0;
            return nice * mag;
        }
    }

    // ------------------------------------------------------------------------------ helpers

    std::string App::paramLabel (int row) const
    {
        static const char* const names[paramRows] = { "Start", "End", "Pitch", "Volume", "Pan", "Mode", "Fade in", "Fade out", "Enabled" };
        return names[std::clamp (row, 0, paramRows - 1)];
    }

    std::string App::paramValue (int row, const kerf::Slice& s) const
    {
        switch (row)
        {
            case pStart:   return seconds3 (s.start) + " s";
            case pEnd:     return seconds3 (s.end) + " s";
            case pPitch:   return semitoneText ((int) std::lround (std::log2 ((double) s.pitch) * 12.0));
            case pVolume:  return integer (std::lround (s.volume * 100.0f)) + "%";
            case pPan:     return panText (s.pan);
            case pMode:    return s.mode == kerf::Slice::Mode::shot ? "SHOT" : "LOOP";
            case pFadeIn:  return integer (std::lround (s.fadeInMs)) + " ms";
            case pFadeOut: return integer (std::lround (s.fadeOutMs)) + " ms";
            case pEnabled: return s.enabled ? "ON" : "OFF";
            default:       return {};
        }
    }

    double App::timeToX (double t, int x0, int width) const
    {
        const double span = std::max (viewEnd_ - viewStart_, 1e-9);
        return x0 + (t - viewStart_) / span * width;
    }

    void App::drawDialog (Canvas& c, int x, int y, int w, int h, const std::string& title)
    {
        c.fillRect (0, 0, kScreenW, kScreenH, argb (165, 4, 5, 12));
        c.fillRect (x + 4, y + 4, w, h, argb (120, 0, 0, 0)); // soft drop shadow
        drawPanel (c, x, y, w, h, panel, brand);
        c.fillRect (x + 1, y + 1, w - 2, 26, brandDeep);
        c.drawText (x + 12, y + 6, title, text, 2);
    }

    // -------------------------------------------------------------------------------- draw

    void App::draw (Canvas& c)
    {
        c.resetClip();

        switch (screen_)
        {
            case Screen::editor:    drawEditor (c); break;
            case Screen::sequencer: drawSequencer (c); break;
            case Screen::browser:   drawBrowser (c); break;
            case Screen::detect:    drawEditor (c); drawDetect (c); break;
            case Screen::menu:      drawPage (c, page_); drawMenu (c, menu_); break;
            case Screen::settings:  drawPage (c, page_); drawMenu (c, settings_); break;
            case Screen::help:      drawHelp (c); break;
            case Screen::inputTest: drawInputTest (c); break;
            case Screen::record:    drawRecord (c); break;
            case Screen::projects:  drawLibrary (c, Screen::projects); break;
        }

        if (isLoading())
            drawLoading (c);

        if (screen_ != Screen::help && screen_ != Screen::inputTest) // full-screen pages need every line
            drawToast (c);
    }

    void App::drawEditor (Canvas& c)
    {
        c.clear (bg);
        drawHeader (c);
        drawWaveform (c);
        drawSliceList (c);
        drawParams (c);
        drawHints (c);
    }

    // A page as it looks under a menu. The Record page has closed its input by then, so it shows only its header.
    void App::drawPage (Canvas& c, Screen page)
    {
        switch (page)
        {
            case Screen::sequencer: drawSequencer (c); break;
            case Screen::browser:   drawBrowser (c); break;
            case Screen::projects:  drawLibrary (c, Screen::projects); break;
            case Screen::record:    c.clear (bg); drawHomeHeader (c, Screen::record); break;
            default:                drawEditor (c); break;
        }
    }

    // ------------------------------------------------------------------------------ header

    void App::drawHeader (Canvas& c)
    {
        c.fillRect (0, 0, kScreenW, kHeaderH, panel);
        c.hline (0, kHeaderH - 1, kScreenW, border);

        c.drawText (kMargin + 2, 6, "KERF", brandHi, 2);

        // The two work pages of Kerf, one above the other; the page you are on is lit. Select flips them.
        // A green triangle on SEQ says the pattern is running (it keeps going on the other page).
        {
            const bool onSequencer = mainScreen_ == Screen::sequencer;
            drawPill (c, 82, 2, 54, 12, "SLICER", ! onSequencer);
            drawPill (c, 82, 14, 54, 12, "SEQ", onSequencer);

            if (engine_.sequencer().isPlaying())
                c.fillTriangle (82 + 40, 14 + 2, 82 + 40, 14 + 9, 82 + 47, 14 + 5, ok);
        }

        const auto sample = engine_.sample();

        if (sample)
        {
            c.drawText (144, 6, elide (sample->name, 15), text, 2);

            const std::string info = clockTime (sample->durationSeconds()) + "  " + rateText (sample->sampleRate) + "  "
                                   + (sample->data.numChannels() > 1 ? "ST" : "MONO");
            c.drawText (396, 5, info, textDim, 1);
        }
        else
        {
            c.drawText (144, 6, "no sample loaded", textFaint, 2);
        }

        // second line of the status area: MIDI + audio state
        const bool midiOn = host_.midiDevices > 0;
        const uint32_t led = midiFlash_ > 0.0f ? brandHi : midiOn ? ok : textFaint;
        c.fillRect (396, 17, 6, 6, led);
        c.drawText (406, 16, midiOn ? "MIDI " + integer (host_.midiDevices) : "MIDI -", midiOn ? textDim : textFaint, 1);
        c.drawText (466, 16, host_.audioOk ? host_.audioInfo : "NO AUDIO", host_.audioOk ? textFaint : danger, 1);

        // output level; above it, how the work stands against its project file (it says OUT when there is nothing to say)
        const int meterX = 556, meterW = 72;

        {
            const char* label = "OUT";
            uint32_t color = textFaint;

            switch (workTag())
            {
                case WorkTag::saved:   label = "SAVED";   color = ok; break;
                case WorkTag::edited:  label = "EDITED";  color = rgb (234, 179, 8); break;
                case WorkTag::unsaved: label = "UNSAVED"; color = rgb (234, 179, 8); break;
                case WorkTag::problem: label = "NO SAVE"; color = danger; break;

                case WorkTag::none:
                    // The autosave, when it is on, has its own word; "off" is the usual state and says nothing.
                    switch (project_.tag())
                    {
                        case ProjectKeeper::Tag::saved:   label = "AUTOSAVED"; color = ok; break;
                        case ProjectKeeper::Tag::edited:  label = "EDITED";     color = rgb (234, 179, 8); break;
                        case ProjectKeeper::Tag::problem: label = "NO SAVE";    color = danger; break;
                        case ProjectKeeper::Tag::off:
                        case ProjectKeeper::Tag::none:    break;
                    }

                    break;
            }

            c.drawText (meterX, 5, label, color, 1);
        }

        c.fillRect (meterX, 15, meterW, 7, bg);
        const float level = std::clamp (peakShown_, 0.0f, 1.0f);
        const int filled = (int) std::lround (level * meterW);
        const int green = (int) (meterW * 0.7f), amber = (int) (meterW * 0.9f);
        c.fillRect (meterX, 15, std::min (filled, green), 7, ok);
        if (filled > green) c.fillRect (meterX + green, 15, std::min (filled, amber) - green, 7, rgb (234, 179, 8));
        if (filled > amber) c.fillRect (meterX + amber, 15, filled - amber, 7, danger);
        c.strokeRect (meterX - 1, 14, meterW + 2, 9, border);
    }

    // The header of REC, LOAD and PROJ, the pages that get a sample or a project: the same family as the
    // header of SLICER and SEQ. KERF, the three pages with the one you are on lit (Select goes round them),
    // and what the page is for.
    void App::drawHomeHeader (Canvas& c, Screen page)
    {
        c.fillRect (0, 0, kScreenW, 44, panel);
        c.hline (0, 43, kScreenW, border);
        c.drawText (kMargin + 2, 6, "KERF", brandHi, 2);

        drawPill (c, 82, 3, 54, 12, "REC", page == Screen::record);
        drawPill (c, 82, 16, 54, 12, "LOAD", page == Screen::browser);
        drawPill (c, 82, 29, 54, 12, "PROJ", page == Screen::projects);

        if (page == Screen::record)
        {
            c.drawText (144, 6, "RECORD SAMPLE", brandHi, 2);
            c.drawText (144, 28, "Takes are saved in " + elideLeftAscii (pathToUtf8 (startDir_), 42), textDim, 1);
        }
        else if (page == Screen::browser)
        {
            c.drawText (144, 6, "LOAD SAMPLE", brandHi, 2);
            c.drawText (144, 28, elideLeftAscii (browser_.directoryUtf8(), 61), textDim, 1);
        }
        else
        {
            c.drawText (144, 6, "LOAD PROJECT", brandHi, 2);
            c.drawText (144, 28, elideLeftAscii (projects_.directoryUtf8(), 61), textDim, 1);
        }
    }

    // ---------------------------------------------------------------------------- waveform

    void App::drawWaveform (Canvas& c)
    {
        const int px = kMargin, py = kWaveY, pw = kScreenW - 2 * kMargin, ph = kWaveH;
        drawPanel (c, px, py, pw, ph);

        const int ix = px + 2, iw = pw - 4;             // inner drawing area
        const int stripTop = py + 3;                    // flags / slice numbers
        const int laneTop = py + 17, laneBottom = py + ph - 19;
        const int rulerY = laneBottom + 2;
        const int cy = (laneTop + laneBottom) / 2;
        const int amp = (laneBottom - laneTop) / 2 - 1;

        if (! wave_.valid())
        {
            c.hline (ix, cy, iw, border);
            c.drawText (px + (pw - Canvas::textWidth ("No sample loaded", 2)) / 2, cy - 22, "No sample loaded", textDim, 2);
            const std::string hint = usingKeyboard_ ? "Tab: menu > Load sample" : "Start: menu > Load sample";
            c.drawText (px + (pw - Canvas::textWidth (hint, 1)) / 2, cy + 6, hint, textFaint, 1);
            return;
        }

        c.setClip (ix, py + 1, iw, ph - 2);

        const auto slices = engine_.slices().getCurrent();
        const int count = (int) slices->size();
        const int sel = (count > 0) ? std::clamp (selected_, 0, count - 1) : -1;

        // slice regions
        for (int i = 0; i < count; ++i)
        {
            const auto& s = (*slices)[(size_t) i];
            const int x0 = (int) std::floor (timeToX (s.start, ix, iw));
            const int x1 = (int) std::ceil (timeToX (s.end, ix, iw));

            if (x1 < ix || x0 > ix + iw)
                continue;

            const int alpha = ! s.enabled ? 12 : (i == sel ? 70 : 32);
            c.fillRect (x0, laneTop, std::max (1, x1 - x0), laneBottom - laneTop, withAlpha (sliceColour (i), alpha));
        }

        // zero line
        c.hline (ix, cy, iw, withAlpha (textFaint, 140));

        // waveform columns
        if ((int) cols_.size() < iw)
            cols_.resize ((size_t) iw);

        wave_.columns (viewStart_, viewEnd_, iw, cols_.data());

        const double selStart = sel >= 0 ? (double) (*slices)[(size_t) sel].start : 0.0;
        const double selEnd = sel >= 0 ? (double) (*slices)[(size_t) sel].end : 0.0;
        const double span = viewEnd_ - viewStart_;

        for (int x = 0; x < iw; ++x)
        {
            const auto& col = cols_[(size_t) x];
            if (! col.valid)
                continue;

            const double t = viewStart_ + (x + 0.5) / iw * span;
            const bool inSel = sel >= 0 && t >= selStart && t <= selEnd;
            const int yTop = cy - (int) std::lround (std::clamp (col.hi, -1.0f, 1.0f) * amp);
            int yBot = cy - (int) std::lround (std::clamp (col.lo, -1.0f, 1.0f) * amp);

            if (yBot <= yTop)
                yBot = yTop + 1;

            c.vline (ix + x, yTop, yBot - yTop + 1, inSel ? waveHot : waveDim);
        }

        // slice boundaries + numbers
        int lastLabelX = -100;

        for (int i = 0; i < count; ++i)
        {
            const auto& s = (*slices)[(size_t) i];
            const int x0 = (int) std::lround (timeToX (s.start, ix, iw));
            const int x1 = (int) std::lround (timeToX (s.end, ix, iw));
            const uint32_t col = sliceColour (i);

            if (x0 >= ix && x0 < ix + iw)
            {
                c.vline (x0, laneTop, laneBottom - laneTop, withAlpha (col, i == sel ? 220 : 130));

                if (x0 - lastLabelX >= 14 && i != sel)
                {
                    c.drawText (x0 + 2, stripTop + 2, integer (i + 1), col, 1);
                    lastLabelX = x0;
                }
            }

            if (x1 >= ix && x1 < ix + iw)
                c.vline (x1, laneTop, laneBottom - laneTop, withAlpha (col, i == sel ? 220 : 70));
        }

        // handles of the selected slice
        if (sel >= 0)
        {
            const int sx = (int) std::lround (timeToX (selStart, ix, iw));
            const int ex = (int) std::lround (timeToX (selEnd, ix, iw));

            c.fillRect (sx - 1, stripTop, 2, laneBottom - stripTop, startCol);
            c.fillRect (sx, stripTop, 11, 11, startCol);
            c.drawText (sx + 2, stripTop + 2, "S", bg, 1);

            c.fillRect (ex - 1, laneTop, 2, laneBottom - laneTop + 12, endCol);
            c.fillRect (ex - 10, laneBottom + 1, 11, 11, endCol);
            c.drawText (ex - 8, laneBottom + 3, "E", bg, 1);

            c.drawText (sx + 14, stripTop + 2, integer (sel + 1), sliceColour (sel), 1);
        }

        // playheads
        Engine::Playhead heads[Engine::maxPlayheads];
        const int nHeads = engine_.copyPlayheads (heads, Engine::maxPlayheads);

        for (int i = 0; i < nHeads; ++i)
        {
            const int x = (int) std::lround (timeToX ((double) heads[i].seconds, ix, iw));
            c.fillRect (x, laneTop, 2, laneBottom - laneTop, rgb (255, 255, 255));
            c.fillTriangle (x - 4, laneTop, x + 5, laneTop, x, laneTop + 6, rgb (255, 255, 255));
        }

        // ruler
        c.hline (ix, rulerY, iw, border);
        const double step = niceStep (span / std::max (1.0, iw / 78.0));
        const int decimals = step >= 1.0 ? 0 : step >= 0.1 ? 1 : step >= 0.01 ? 2 : 3;

        for (double t = std::ceil (viewStart_ / step) * step; t <= viewEnd_ + 1e-9; t += step)
        {
            const int x = (int) std::lround (timeToX (t, ix, iw));
            c.vline (x, rulerY, 5, textDim);
            c.drawText (x + 3, rulerY + 5, fixed (t, decimals) + "s", textFaint, 1);
        }

        // zoom indicator
        const double zoom = wave_.duration() / std::max (span, 1e-9);
        if (zoom > 1.02)
        {
            const std::string z = "x" + fixed (zoom, zoom < 10 ? 1 : 0);
            c.drawText (ix + iw - Canvas::textWidth (z, 1) - 4, stripTop + 2, z, brandHi, 1);
        }

        c.resetClip();
    }

    // ------------------------------------------------------------------------------- lists

    void App::drawSliceList (Canvas& c)
    {
        drawPanel (c, kMargin, kLowerY, kListW, kLowerH);

        const auto slices = engine_.slices().getCurrent();
        const int count = (int) slices->size();

        c.drawText (kMargin + 10, kLowerY + 6, "SLICES", textDim, 2);
        const std::string n = integer (count);
        c.drawText (kMargin + kListW - 10 - Canvas::textWidth (n, 2), kLowerY + 6, n, text, 2);

        const int rowsY = kLowerY + 28;
        const int rows = (kLowerH - 32) / kRowH;

        if (count == 0)
        {
            c.drawText (kMargin + 10, rowsY + 6, "none yet", textFaint, 2);
            c.drawText (kMargin + 10, rowsY + 30, usingKeyboard_ ? "2: auto-detect" : "Y: auto-detect", textFaint, 1);
            c.drawText (kMargin + 10, rowsY + 42, usingKeyboard_ ? "1: add one" : "X: add one", textFaint, 1);
            return;
        }

        const int sel = std::clamp (selected_, 0, count - 1);
        const int first = std::clamp (sel - rows / 2, 0, std::max (0, count - rows));

        c.setClip (kMargin + 1, rowsY, kListW - 2, rows * kRowH);

        for (int r = 0; r < rows && first + r < count; ++r)
        {
            const int i = first + r;
            const auto& s = (*slices)[(size_t) i];
            const int y = rowsY + r * kRowH;
            const bool isSel = i == sel;

            if (isSel)
            {
                c.fillRect (kMargin + 2, y, kListW - 4, kRowH - 2, panelHi);
                c.fillRect (kMargin + 2, y, 3, kRowH - 2, brandHi);
            }

            const uint32_t main = s.enabled ? (isSel ? text : textDim) : textFaint;
            c.fillRect (kMargin + 10, y + 3, 6, kRowH - 8, withAlpha (sliceColour (i), s.enabled ? 255 : 90));
            c.drawText (kMargin + 22, y + 1, integer (i + 1), main, 2);
            c.drawText (kMargin + 22 + 48, y + 1, noteName (cfg_.noteOffset + i), textFaint, 2);

            const double len = (double) s.end - (double) s.start;
            c.drawText (kMargin + 22 + 48 + 56, y + 1, fixed (len, len >= 10.0 ? 1 : 2) + "s", main, 2);

            if (s.mode == kerf::Slice::Mode::loop)
                c.drawText (kMargin + kListW - 16, y + 6, "L", brandHi, 1);
        }

        c.resetClip();

        // scroll bar
        if (count > rows)
        {
            const int trackH = rows * kRowH;
            const int thumbH = std::max (10, trackH * rows / count);
            const int thumbY = rowsY + (trackH - thumbH) * first / std::max (1, count - rows);
            c.fillRect (kMargin + kListW - 5, rowsY, 2, trackH, border);
            c.fillRect (kMargin + kListW - 5, thumbY, 2, thumbH, brandHi);
        }
    }

    void App::drawParams (Canvas& c)
    {
        drawPanel (c, kParamX, kLowerY, kParamW, kLowerH);

        const auto slices = engine_.slices().getCurrent();
        const int count = (int) slices->size();

        if (count == 0)
        {
            c.drawText (kParamX + 10, kLowerY + 6, "SLICE", textDim, 2);
            c.drawText (kParamX + 10, kLowerY + 34, "Detect or add a slice to edit it.", textFaint, 1);
            return;
        }

        const int sel = std::clamp (selected_, 0, count - 1);
        const auto& s = (*slices)[(size_t) sel];

        c.fillRect (kParamX + 10, kLowerY + 8, 8, 14, sliceColour (sel));
        c.drawText (kParamX + 24, kLowerY + 6, "SLICE " + integer (sel + 1), text, 2);
        const std::string key = "key " + noteName (cfg_.noteOffset + sel);
        c.drawText (kParamX + kParamW - 10 - Canvas::textWidth (key, 2), kLowerY + 6, key, textDim, 2);

        const int rowsY = kLowerY + 28;

        for (int r = 0; r < paramRows; ++r)
        {
            const int y = rowsY + r * kRowH;
            const bool isSel = (r == paramRow_);

            if (isSel)
            {
                c.fillRect (kParamX + 2, y, kParamW - 4, kRowH - 2, panelHi);
                c.fillRect (kParamX + 2, y, 3, kRowH - 2, brandHi);
            }

            uint32_t labelCol = isSel ? text : textDim;
            if (r == pStart) labelCol = startCol;
            if (r == pEnd)   labelCol = endCol;

            c.drawText (kParamX + 14, y + 1, paramLabel (r), labelCol, 2);

            const std::string v = paramValue (r, s);
            const uint32_t valueCol = (r == pEnabled && ! s.enabled) ? danger : (isSel ? text : textDim);
            const int vx = kParamX + 164;
            c.drawText (vx, y + 1, v, valueCol, 2);

            if (isSel)
            {
                arrowLeft (c, vx - 12, y + 4, brandHi);
                arrowRight (c, vx + Canvas::textWidth (v, 2) + 6, y + 4, brandHi);
            }

            if (r == pStart || r == pEnd)
            {
                const std::string tag = r == pStart ? (usingKeyboard_ ? "Z / X" : "L-STICK") : (usingKeyboard_ ? "N / M" : "R-STICK");
                c.drawText (kParamX + kParamW - 12 - Canvas::textWidth (tag, 1), y + 6, tag, r == pStart ? startCol : endCol, 1);
            }
        }
    }

    // -------------------------------------------------------------------------------- hints

    void App::drawHints (Canvas& c)
    {
        c.fillRect (0, kHintsY, kScreenW, kHintsH, panel);
        c.hline (0, kHintsY, kScreenW, border);

        std::vector<Seg> line1, line2;

        if (usingKeyboard_)
        {
            line1 = { { "Space", brandHi }, { " play  ", textDim }, { "1", brandHi }, { " add  ", textDim }, { "2", brandHi }, { " detect  ", textDim },
                      { "Tab", brandHi }, { " menu", textDim } };
            line2 = { { "Z/X", startCol }, { " start  ", textDim }, { "N/M", endCol }, { " end  ", textDim }, { "Q/R", textDim }, { " slice  ", textFaint },
                      { "3", textDim }, { " page  ", textFaint }, { "Ctrl", textDim }, { " fine  ", textFaint }, { "Shift", textDim }, { " coarse  ", textFaint },
                      { "F1", textDim }, { " help", textFaint } };
        }
        else
        {
            line1 = { { "A", brandHi }, { " play  ", textDim }, { "X", brandHi }, { " add  ", textDim }, { "Y", brandHi }, { " detect  ", textDim },
                      { "Start", brandHi }, { " menu", textDim } };
            line2 = { { "L-STICK", startCol }, { " start  ", textDim }, { "R-STICK", endCol }, { " end  ", textDim }, { "L1/R1", textDim }, { " slice  ", textFaint },
                      { "Select", textDim }, { " page  ", textFaint }, { "L2", textDim }, { " fine  ", textFaint }, { "R2", textDim }, { " coarse", textFaint } };
        }

        drawSegments (c, kMargin + 2, kHintsY + 5, line1, 2);
        drawSegments (c, kMargin + 2, kHintsY + 27, line2, 1);
    }

    void App::drawToast (Canvas& c)
    {
        if (toastTime_ <= 0.0f || toast_.empty())
            return;

        const int alpha = (int) std::lround (std::clamp (toastTime_ / 0.35f, 0.0f, 1.0f) * 235.0f);
        const int maxGlyphs = (kScreenW - 40) / 16;
        const auto msg = elide (toast_, maxGlyphs);
        const int w = Canvas::textWidth (msg, 2) + 24;
        const int x = (kScreenW - w) / 2, y = kHintsY - 36;

        c.fillRect (x, y, w, 28, argb (alpha, 56, 52, 190));
        c.strokeRect (x, y, w, 28, argb (alpha, 129, 140, 248));
        c.drawText (x + 12, y + 6, msg, argb (alpha, 255, 255, 255), 2);
    }

    // ------------------------------------------------------------------------------ browser

    void App::drawBrowser (Canvas& c)
    {
        drawLibrary (c, Screen::browser);
    }

    // The two libraries look alike: LOAD lists samples, PROJ lists projects.
    void App::drawLibrary (Canvas& c, Screen page)
    {
        const bool projectsPage = page == Screen::projects;
        const FileBrowser& library = projectsPage ? projects_ : browser_;
        const int selection = projectsPage ? projectsSel_ : browserSel_;
        const int scroll = projectsPage ? projectsScroll_ : browserScroll_;

        c.clear (bg);
        drawHomeHeader (c, page);

        const int rowH = 30, listY = 52, rows = 12;
        const auto& entries = library.entries();
        const int n = (int) entries.size();

        c.setClip (0, listY, kScreenW, rows * rowH);

        for (int r = 0; r < rows && scroll + r < n; ++r)
        {
            const int i = scroll + r;
            const auto& e = entries[(size_t) i];
            const int y = listY + r * rowH;
            const bool isSel = i == selection;

            if (isSel)
            {
                c.fillRect (kMargin, y, kScreenW - 2 * kMargin - 8, rowH - 3, panelHi);
                c.fillRect (kMargin, y, 4, rowH - 3, brandHi);
            }

            // icon: folder (tab + body), project (a page with lines) or audio file (note)
            const int ix = kMargin + 14, iy = y + 7;

            if (e.isDir)
            {
                const uint32_t fc = e.isParent ? textDim : rgb (250, 204, 21);
                c.fillRect (ix, iy, 8, 3, fc);
                c.fillRect (ix, iy + 3, 20, 12, fc);
            }
            else if (projectsPage)
            {
                c.fillRect (ix + 3, iy - 1, 14, 17, brandHi);
                c.fillRect (ix + 6, iy + 3, 8, 2, panel);
                c.fillRect (ix + 6, iy + 7, 8, 2, panel);
                c.fillRect (ix + 6, iy + 11, 5, 2, panel);
            }
            else
            {
                c.fillRect (ix + 12, iy, 3, 13, brandHi);
                c.fillRect (ix + 3, iy + 10, 10, 5, brandHi);
                c.fillRect (ix + 12, iy, 8, 3, brandHi);
            }

            // A file that has a project says so, left of its size ("KERF"): the name gives way if it is long.
            const std::string sz = e.isDir ? std::string() : sizeText (e.size);
            const int sizeX = kScreenW - kMargin - 14 - Canvas::textWidth (sz, 2);
            const int tagW = Canvas::textWidth ("KERF", 1) + 8;
            const int tagX = sizeX - 10 - tagW;
            const int nameX = kMargin + 46;
            const int nameChars = e.hasProject ? std::clamp ((tagX - nameX - 8) / 16, 8, 28) : 28;

            const std::string name = e.isParent ? "[ up ]" : e.name;
            c.drawText (nameX, y + 6, elide (name, nameChars), e.isDir ? (isSel ? text : textDim) : (isSel ? rgb (255, 255, 255) : text), 2);

            if (! e.isDir)
                c.drawText (sizeX, y + 6, sz, textFaint, 2);

            if (e.hasProject)
            {
                c.strokeRect (tagX, y + 8, tagW, 12, brandHi);
                c.drawText (tagX + 4, y + 10, "KERF", brandHi, 1);
            }
        }

        c.resetClip();

        if (n > rows)
        {
            const int trackH = rows * rowH;
            const int thumbH = std::max (14, trackH * rows / n);
            const int thumbY = listY + (trackH - thumbH) * scroll / std::max (1, n - rows);
            c.fillRect (kScreenW - 7, listY, 3, trackH, border);
            c.fillRect (kScreenW - 7, thumbY, 3, thumbH, brandHi);
        }

        if (n <= 1)
        {
            if (projectsPage)
            {
                c.drawText (kMargin + 46, listY + 40, "No Kerf projects here", textFaint, 2);
                c.drawText (kMargin + 46, listY + 66, usingKeyboard_ ? "Tab menu > Save project makes one." : "Start menu > Save project makes one.", textFaint, 1);
            }
            else
            {
                c.drawText (kMargin + 46, listY + 40, "No WAV, MP3 or FLAC files here", textFaint, 2);
                c.drawText (kMargin + 46, listY + 66, usingKeyboard_ ? "Press 3 twice to record a sample instead." : "Press Select twice to record a sample instead.", textFaint, 1);
            }
        }

        c.fillRect (0, kHintsY, kScreenW, kHintsH, panel);
        c.hline (0, kHintsY, kScreenW, border);

        const bool haveSample = wave_.valid();
        const char* openText = projectsPage ? " open   " : " open/load   ";
        const std::vector<Seg> hints = usingKeyboard_
            ? std::vector<Seg> { { "Enter", brandHi }, { openText, textDim }, { "Esc", brandHi }, { " up   ", textDim }, { "Up/Down", brandHi }, { " move   ", textDim },
                                 { "PgUp/PgDn", brandHi }, { " page", textDim } }
            : std::vector<Seg> { { "A", brandHi }, { openText, textDim }, { "B", brandHi }, { " up   ", textDim }, { "D-pad", brandHi }, { " move   ", textDim },
                                 { "L1/R1", brandHi }, { " page", textDim } };

        drawSegments (c, kMargin + 2, kHintsY + 6, hints);

        // The next page (LOAD goes on to PROJ, PROJ back to REC), and the way out of these pages: back to the sample, or the menu before there is one.
        std::vector<Seg> tip = { { usingKeyboard_ ? "3" : "Select", brandHi }, { projectsPage ? " record a sample   " : " projects   ", textDim },
                                 { usingKeyboard_ ? "Tab" : "Start", brandHi } };

        if (haveSample)
            tip.push_back ({ mainScreen_ == Screen::sequencer ? " back to the sequencer" : " back to the slicer", textDim });
        else
            tip.push_back ({ " menu", textDim });

        if (usingKeyboard_ && ! projectsPage)
            tip.push_back ({ "   Drop a file here to load it", textFaint });

        drawSegments (c, kMargin + 2, kHintsY + 19, tip);
    }

    // --------------------------------------------------------------------- dialogs + menus

    void App::drawDetect (Canvas& c)
    {
        const int w = 500, h = 200, x = (kScreenW - w) / 2, y = 130;
        drawDialog (c, x, y, w, h, "AUTO-DETECT SLICES");

        const int existing = engine_.slices().size();
        c.drawText (x + 14, y + 40, "Finds the hits in the sample", text, 2);
        c.drawText (x + 14, y + 60,
                    existing > 0 ? "Replaces your " + integer (existing) + " slices." : "and cuts it into slices.",
                    existing > 0 ? warn : textDim, 2);

        // sensitivity slider (kerf.html: 10% .. 100%, default 50%)
        const int sx = x + 14, sy = y + 104, sw = w - 28;
        c.drawText (sx, sy - 22, "Sensitivity", textDim, 2);
        const std::string pct = integer (std::lround (detectSlider_ * 100.0f)) + "%";
        c.drawText (sx + sw - Canvas::textWidth (pct, 2), sy - 22, pct, text, 2);
        c.fillRect (sx, sy, sw, 8, bg);
        c.strokeRect (sx, sy, sw, 8, border);
        const float norm = (detectSlider_ - 0.1f) / 0.9f;
        const int fillW = (int) std::lround (norm * (sw - 4));
        c.fillRect (sx + 2, sy + 2, fillW, 4, brand);
        c.fillRect (sx + 2 + fillW - 3, sy - 4, 6, 16, brandHi);

        c.drawText (sx, sy + 20, "less hits", textFaint, 1);
        c.drawText (sx + sw - Canvas::textWidth ("more hits", 1), sy + 20, "more hits", textFaint, 1);

        const std::vector<Seg> hints = usingKeyboard_
            ? std::vector<Seg> { { "Left/Right", brandHi }, { " adjust   ", textDim }, { "Enter", brandHi }, { " detect   ", textDim }, { "Esc", brandHi }, { " cancel", textDim } }
            : std::vector<Seg> { { "Left/Right", brandHi }, { " adjust   ", textDim }, { "A", brandHi }, { " detect   ", textDim }, { "B", brandHi }, { " cancel", textDim } };
        drawSegments (c, x + 14, y + h - 22, hints);
    }

    void App::drawMenu (Canvas& c, const ListMenu& m)
    {
        const int rowH = 24, rows = std::min ((int) m.items.size(), kMenuRows);
        const int w = 420, h = 36 + rows * rowH + 30;
        const int x = (kScreenW - w) / 2, y = std::max (20, (kScreenH - h) / 2 - 10);

        drawDialog (c, x, y, w, h, m.title);

        const int listY = y + 34;
        c.setClip (x + 1, listY, w - 2, rows * rowH);

        for (int r = 0; r < rows && m.scroll + r < (int) m.items.size(); ++r)
        {
            const int i = m.scroll + r;
            const auto& item = m.items[(size_t) i];
            const int ry = listY + r * rowH;
            const bool isSel = i == m.sel;

            if (isSel)
            {
                c.fillRect (x + 6, ry, w - 12, rowH - 2, panelHi);
                c.fillRect (x + 6, ry, 3, rowH - 2, brandHi);
            }

            c.drawText (x + 18, ry + 3, item.label, isSel ? text : textDim, 2);

            if (item.value)
            {
                const auto v = item.value();
                const int vx = textRightX (x + w - 24, v, 2);
                c.drawText (vx, ry + 3, v, isSel ? brandHi : textDim, 2);

                if (isSel && item.adjust)
                {
                    arrowLeft (c, vx - 14, ry + 7, brandHi);
                    arrowRight (c, x + w - 18, ry + 7, brandHi);
                }
            }
        }

        c.resetClip();

        if ((int) m.items.size() > rows)
        {
            const int trackH = rows * rowH;
            const int thumbH = std::max (10, trackH * rows / (int) m.items.size());
            const int thumbY = listY + (trackH - thumbH) * m.scroll / std::max (1, (int) m.items.size() - rows);
            c.fillRect (x + w - 6, listY, 2, trackH, border);
            c.fillRect (x + w - 6, thumbY, 2, thumbH, brandHi);
        }

        c.drawText (x + 14, y + h - 20, m.footer, textFaint, 1);
    }

    void App::drawLoading (Canvas& c)
    {
        const int w = 400, h = 96, x = (kScreenW - w) / 2, y = 190;
        drawDialog (c, x, y, w, h, "LOADING");
        c.drawText (x + 14, y + 40, elide (loadingName_, 23), text, 2);

        // moving bar
        const int bx = x + 14, by = y + 70, bw = w - 28;
        c.fillRect (bx, by, bw, 8, bg);
        c.strokeRect (bx, by, bw, 8, border);
        const double phase = std::fmod (time_ * 0.9, 1.0);
        const int segW = bw / 4;
        const int segX = bx + (int) ((bw - segW) * (0.5 - 0.5 * std::cos (phase * 6.2831853)));
        c.fillRect (segX, by + 2, segW, 4, brandHi);
    }

    // ------------------------------------------------------------------------ help + test

    void App::drawHelp (Canvas& c)
    {
        c.clear (bg);
        c.fillRect (0, 0, kScreenW, 44, panel);
        c.hline (0, 43, kScreenW, border);

        // 0 gamepad / slicer, 1 gamepad / sequencer, 2 keyboard / slicer, 3 keyboard / sequencer, 4 gamepad / REC + LOAD, 5 keyboard / REC + LOAD
        const int index = std::clamp (helpPage_, 0, kHelpPages - 1);
        const HelpPage& page = helpPage (index);
        c.drawText (kMargin + 2, 6, page.title, brandHi, 2);
        c.drawText (kMargin + 2, 28, "Left/Right: other page      B: close", textFaint, 1);
        c.drawText (kScreenW - kMargin - 2 - Canvas::textWidth (integer (index + 1) + "/" + integer (kHelpPages), 1), 28,
                    integer (index + 1) + "/" + integer (kHelpPages), textFaint, 1);

        auto toneColour = [] (HelpTone t)
        {
            switch (t)
            {
                case HelpTone::start: return startCol;
                case HelpTone::end:   return endCol;
                case HelpTone::dim:   return textDim;
                case HelpTone::page:  return brandHi;
                case HelpTone::normal: break;
            }

            return text;
        };

        const int pitch = 24, whatX = kMargin + 6 + (kHelpKeyChars + 1) * 16;

        for (int i = 0; i < page.rowCount; ++i)
        {
            const int y = 52 + i * pitch;
            const auto& row = page.rows[i];
            c.drawText (kMargin + 6, y, row.keys, toneColour (row.tone), 2);
            c.drawText (whatX, y, row.what, row.tone == HelpTone::dim ? textDim : text, 2);
        }

        c.drawText (kMargin + 6, 52 + page.rowCount * pitch + 2, page.note, textFaint, 1);
    }

    void App::drawInputTest (Canvas& c)
    {
        c.clear (bg);
        c.fillRect (0, 0, kScreenW, 44, panel);
        c.hline (0, 43, kScreenW, border);
        c.drawText (kMargin + 2, 6, "INPUT TEST", brandHi, 2);
        c.drawText (kMargin + 2, 28, "Press every button and move both sticks.  B (or hold Start) exits.", textFaint, 1);

        // device
        const std::string dev = debug_.deviceName.empty() ? "no gamepad - keyboard only" : debug_.deviceName;
        c.drawText (kMargin + 6, 54, "Device", textDim, 2);
        c.drawText (kMargin + 6 + 8 * 16, 54, elide (dev, 30), text, 2);

        if (! debug_.deviceName.empty())
        {
            c.drawText (kMargin + 6, 76, "Mapping", textDim, 2);
            c.drawText (kMargin + 6 + 8 * 16, 76, debug_.isController ? "SDL game controller" : "raw joystick (see kerf.cfg)", debug_.isController ? ok : warn, 2);
            c.drawText (kMargin + 6, 98, std::string ("axes ") + integer (debug_.axes) + "   buttons " + integer (debug_.buttons) + "   hats " + integer (debug_.hats)
                                         + "   last raw button " + integer (debug_.lastButton) + "  " + debug_.lastControllerButton, textFaint, 1);
        }

        // logical buttons
        static const Btn order[] = { Btn::Up, Btn::Down, Btn::Left, Btn::Right, Btn::A, Btn::B, Btn::X, Btn::Y,
                                     Btn::L1, Btn::R1, Btn::L2, Btn::R2, Btn::Select, Btn::Start };

        for (int i = 0; i < 14; ++i)
        {
            const int col = i % 7, row = i / 7;
            const int x = kMargin + 6 + col * 88, y = 122 + row * 40;
            const bool on = lastPad_.down[(int) order[i]];
            c.fillRect (x, y, 80, 32, on ? brand : panel);
            c.strokeRect (x, y, 80, 32, on ? brandHi : border);
            const std::string name = btnName (order[i]);
            const int scale = name.size() > 4 ? 1 : 2; // "Select" does not fit at double size
            c.drawText (x + (80 - Canvas::textWidth (name, scale)) / 2, y + (scale == 2 ? 8 : 12), name, on ? rgb (255, 255, 255) : textDim, scale);
        }

        // sticks
        auto drawStick = [&] (int cx, int cy, float sx, float sy, const char* label, uint32_t col)
        {
            c.fillRect (cx - 50, cy - 50, 100, 100, panel);
            c.strokeRect (cx - 50, cy - 50, 100, 100, border);
            c.hline (cx - 50, cy, 100, border);
            c.vline (cx, cy - 50, 100, border);
            const int dx = (int) std::lround (std::clamp (sx, -1.0f, 1.0f) * 46.0f);
            const int dy = (int) std::lround (std::clamp (sy, -1.0f, 1.0f) * 46.0f);
            c.fillRect (cx + dx - 4, cy + dy - 4, 9, 9, col);
            c.drawText (cx - Canvas::textWidth (label, 1) / 2, cy + 56, label, textDim, 1);
            const std::string vals = fixed (sx, 2) + "  " + fixed (sy, 2);
            c.drawText (cx - Canvas::textWidth (vals, 1) / 2, cy + 68, vals, textFaint, 1);
        };

        drawStick (kMargin + 6 + 90, 262 + 40, lastPad_.lx, lastPad_.ly, "LEFT STICK (start)", startCol);
        drawStick (kMargin + 6 + 330, 262 + 40, lastPad_.rx, lastPad_.ry, "RIGHT STICK (end)", endCol);

        // raw axes
        if (! debug_.deviceName.empty())
        {
            c.drawText (440, 240, "raw axes", textDim, 1);

            for (int i = 0; i < std::min (8, debug_.axes); ++i)
            {
                const int x = 440, y = 256 + i * 16;
                c.drawText (x, y, "ax" + integer (i), textFaint, 1);
                c.fillRect (x + 24, y, 100, 8, panel);
                c.strokeRect (x + 24, y, 100, 8, border);
                c.fillRect (x + 24 + 50 + (int) std::lround (std::clamp (debug_.axis[i], -1.0f, 1.0f) * 48.0f) - 1, y, 3, 8, brandHi);
            }
        }

        // MIDI inputs, and the latest message that came in on any of them
        const int midiY = 392;
        const int inputs = host_.midiDevices;
        c.drawText (kMargin + 6, midiY, "MIDI", textDim, 2);
        c.drawText (kMargin + 6 + 8 * 16, midiY, inputs == 0 ? std::string ("no input found") : integer (inputs) + (inputs == 1 ? " input" : " inputs"),
                    inputs > 0 ? ok : textFaint, 2);
        c.drawText (kMargin + 6, midiY + 24, "Last", textDim, 2);
        c.drawText (kMargin + 6 + 8 * 16, midiY + 28, midiMessageText (host_.midiLast), host_.midiLast != 0 ? text : textFaint, 1);

        if (inputs == 0)
            c.drawText (kMargin + 6, midiY + 54, "Nothing found - kerf-log.txt lists the USB devices Kerf saw.", textFaint, 1);
        else if (host_.midiUsbDirect > 0)
            c.drawText (kMargin + 6, midiY + 54, integer (host_.midiUsbDirect) + " read by Kerf itself (the system has no driver for it).", textFaint, 1);
    }
}
