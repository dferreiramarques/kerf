// The Record page (REC): makes a sample from a microphone or a line input, saves it as rec-001.wav,
// rec-002.wav ... in the samples folder and loads it into the slicer. It is the page Kerf starts on;
// Select flips it with the library (LOAD), which is the page behind it.
//
// What happens to the sound (which input is open, which of its channels are kept, the take, the
// file) is RecordSession's business (record_session.h). This file reads the buttons and draws.

#include "app.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "draw_util.h"
#include "format.h"
#include "path_util.h"
#include "theme.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace kerf::ui
{
    namespace
    {
        using namespace theme;

        // ---- layout (640 x 480) ----
        constexpr int kMargin = 8;
        constexpr int kPanelW = kScreenW - 2 * kMargin;
        constexpr int kHintsY = 438, kHintsH = 42;
        constexpr int kRowsY = 52, kRowsH = 88, kRowH = 26;
        constexpr int kMetersY = 146, kMetersH = 164;
        constexpr int kTakeY = 316, kTakeH = 114;               // the toast covers the bottom 28 pixels: nothing important goes there
        constexpr int kRowCount = 3;                            // input, channels, gain
        constexpr int kMaxMeters = 16;                          // channels drawn at once (a mixer can have more)
        constexpr int kBarX = kMargin + 44, kBarW = 500;

        // Where a level sits on the meters: -60 dB at the left end, full scale at the right end.
        float meterFraction (float level)
        {
            if (! (level > 0.001f))
                return 0.0f;

            return std::clamp ((20.0f * std::log10 (level) + 60.0f) / 60.0f, 0.0f, 1.0f);
        }

        std::string peakText (float level)
        {
            if (! (level > 0.0001f))
                return "--";

            return fixed (20.0 * std::log10 ((double) level), 1);
        }

        std::string gainText (int db) { return (db > 0 ? "+" : "") + integer (db) + " dB"; }

        // "samples" for /mnt/mmc/kerf/samples (and for the same with a slash at the end)
        std::string folderName (const fs::path& dir)
        {
            fs::path name = dir.filename();

            if (name.empty())
                name = dir.parent_path().filename();

            return pathToUtf8 (name);
        }

        // 3:00, 0:30
        std::string minutesText (double seconds)
        {
            const int total = std::max (0, (int) std::lround (seconds));
            return integer (total / 60) + ":" + (total % 60 < 10 ? "0" : "") + integer (total % 60);
        }

        void drawMeter (Canvas& c, int x, int y, int w, int h, float level, float hold, bool kept)
        {
            c.fillRect (x, y, w, h, bg);

            const int filled = (int) std::lround (meterFraction (level) * (float) w);
            const int green = (int) (0.8f * (float) w), amber = (int) (0.95f * (float) w);     // up to -12 dB, up to -3 dB
            const int alpha = kept ? 255 : 90;

            c.fillRect (x, y, std::min (filled, green), h, withAlpha (ok, alpha));

            if (filled > green)
                c.fillRect (x + green, y, std::min (filled, amber) - green, h, withAlpha (rgb (234, 179, 8), alpha));

            if (filled > amber)
                c.fillRect (x + amber, y, filled - amber, h, withAlpha (danger, alpha));

            if (hold > 0.001f)
                c.fillRect (x + std::min (w - 2, (int) std::lround (meterFraction (hold) * (float) w)), y, 2, h, kept ? rgb (255, 255, 255) : textFaint);

            c.strokeRect (x - 1, y - 1, w + 2, h + 2, kept ? border : panelHi);
        }

        void drawScale (Canvas& c, int x, int y, int w)
        {
            static const int marks[] = { -48, -36, -24, -12, -6, 0 };

            for (const int db : marks)
            {
                const int tx = x + (int) std::lround ((float) (db + 60) / 60.0f * (float) w);
                const std::string label = integer (db);
                c.vline (tx, y, 3, textFaint);
                c.drawText (tx - Canvas::textWidth (label, 1) / 2, y + 5, label, textFaint, 1);
            }
        }
    }

    // ----------------------------------------------------------------------- coming and going

    void App::setInputHub (InputHub* hub)
    {
        inputs_ = hub;
        session_.attach (hub);

        // Kerf starts on this page, before the host has a hub to give it: the input is opened now.
        if (screen_ == Screen::record)
            enterRecord();
    }

    void App::openRecord()
    {
        setScreen (Screen::record);
    }

    void App::enterRecord()
    {
        recordRow_ = 0;
        recordSaving_ = 0;
        session_.begin (cfg_.recordInput, cfg_.recordGainDb, (double) cfg_.recordLimit, cfg_.recordChannels);
    }

    // What the person chose is kept for the next time; the input is closed so that nothing else is held up.
    void App::leaveRecord()
    {
        recordSaving_ = 0;

        if (! session_.active())
            return;

        cfg_.recordInput = session_.sourceId();         // the one that was open, or the one wanted while it is unplugged
        cfg_.recordGainDb = session_.gainDb();
        cfg_.recordChannels = session_.chosenChannels();
        session_.end();
        saveSettings();
    }

    // Nothing may leave the Record page while this is true: the take would be thrown away by accident.
    bool App::takeInHand() const
    {
        return screen_ == Screen::record && session_.phase() != RecordSession::Phase::idle;
    }

    // ----------------------------------------------------------------------------------- update

    void App::updateRecord (const Pad& pad, float dt)
    {
        using Phase = RecordSession::Phase;

        // The box that says "saving" has been on the screen for a frame: now the file is written
        // (on a slow card that can take a moment).
        if (recordSaving_ != 0)
        {
            const bool thenLoad = recordSaving_ == 1;
            recordSaving_ = 0;
            saveTake (thenLoad);
            return;
        }

        session_.update ((double) dt);

        const std::string notice = session_.takeNotice();

        if (! notice.empty())
            showToast (notice, 3.5f);

        const bool selectTapped = selectTap (pad);

        switch (session_.phase())
        {
            case Phase::idle:
            {
                // Select: the other page of the pair, the library. A tap has no other button in it.
                if (selectTapped)
                {
                    setScreen (Screen::browser);
                    return;
                }

                if (pad.repeated[(int) Btn::Up])   recordRow_ = (recordRow_ + kRowCount - 1) % kRowCount;
                if (pad.repeated[(int) Btn::Down]) recordRow_ = (recordRow_ + 1) % kRowCount;

                // Choosing an input opens it, so a held button does not run through them.
                const int tap = (pad.pressed[(int) Btn::Right] ? 1 : 0) - (pad.pressed[(int) Btn::Left] ? 1 : 0);
                const int step = (pad.repeated[(int) Btn::Right] ? 1 : 0) - (pad.repeated[(int) Btn::Left] ? 1 : 0);

                if (recordRow_ == 0 && tap != 0)      session_.chooseSource (tap);
                else if (recordRow_ == 1 && step != 0) session_.chooseChannels (step);
                else if (recordRow_ == 2 && step != 0) session_.adjustGain (step);

                if (pad.pressed[(int) Btn::L1]) session_.chooseSource (-1);
                if (pad.pressed[(int) Btn::R1]) session_.chooseSource (+1);

                if (pad.pressed[(int) Btn::A])
                {
                    if (! session_.toggleRecording())
                        showToast (session_.ready() ? "Cannot record now" : "No input is ready yet", 2.5f);
                }
                else if (pad.pressed[(int) Btn::Y])
                {
                    session_.reopen();
                }
                else if (pad.pressed[(int) Btn::B])
                {
                    // Back to the sample being worked on, if there is one: before that there is nowhere to go back to.
                    if (wave_.valid())
                        setScreen (mainScreen_);
                }
                else if (pad.pressed[(int) Btn::Start])
                {
                    // The same, and before there is a sample the menu: settings, help, the way out.
                    if (wave_.valid()) setScreen (mainScreen_);
                    else               openMenu();
                }

                break;
            }

            case Phase::recording:
                if (pad.pressed[(int) Btn::A] || pad.pressed[(int) Btn::B])
                    session_.toggleRecording();
                else if (selectTapped)
                    showToast ("Stop the take first", 2.0f);

                break;

            case Phase::stopped:
                // B throws the take away; Start and Select do nothing here, so that a take is never lost by accident.
                if (selectTapped)                   showToast ("Save or discard the take first", 2.5f);
                else if (pad.pressed[(int) Btn::A]) { if (confirmDiscard()) recordSaving_ = 1; }   // save+load replaces the work in hand
                else if (pad.pressed[(int) Btn::X]) recordSaving_ = 2;
                else if (pad.pressed[(int) Btn::B]) session_.newTake();

                break;
        }
    }

    // Writes the take as the next rec-NNN.wav in the samples folder; then loads it, or stays for another take.
    void App::saveTake (bool thenLoad)
    {
        fs::path saved;
        std::string error;

        if (! session_.save (startDir_, saved, error))
        {
            showToast ("Could not save: " + error, 5.0f);
            return;
        }

        lastSaved_ = saved;
        const std::string name = pathToUtf8 (saved.filename());

        // A listing of that folder, if the file browser has one up, is out of date now.
        if (! browser_.atRootsList() && ! browser_.directory().empty())
            browser_.open (browser_.directory());

        if (thenLoad)
        {
            setScreen (workOrLibrary());   // the input is closed; the loading box comes up over the page it was started from
            loadFile (saved, "Saved " + name + ". Press " + (usingKeyboard_ ? "2" : "Y") + " to auto-detect");
        }
        else
        {
            session_.newTake();
            showToast ("Saved " + name, 3.0f);
        }
    }

    // ------------------------------------------------------------------------------------ draw

    void App::drawRecord (Canvas& c)
    {
        c.clear (bg);
        drawHomeHeader (c, Screen::record);

        drawRecordRows (c);
        drawRecordMeters (c);
        drawRecordTake (c);
        drawRecordHints (c);

        if (recordSaving_ != 0)
        {
            const Recorder& rec = session_.recorder();
            const long long bytes = (long long) rec.takeFrames() * rec.takeChannels() * 2 + 44;
            const int w = 440, h = 96, x = (kScreenW - w) / 2, y = 190;
            drawDialog (c, x, y, w, h, "SAVING");
            c.drawText (x + 14, y + 40, "Writing " + sizeText ((uintmax_t) bytes), text, 2);
            c.drawText (x + 14, y + 66, "to the " + elide (folderName (startDir_), 24) + " folder", textDim, 2);
        }
    }

    void App::drawRecordRows (Canvas& c)
    {
        drawPanel (c, kMargin, kRowsY, kPanelW, kRowsH);

        const bool canChange = session_.canChange();
        const auto& sources = session_.sources();
        const auto& choices = session_.choices();
        const Recorder& rec = session_.recorder();

        struct Row
        {
            const char* label;
            std::string value;
            std::string note;
            bool many;          // there is more than one to choose from
            bool dim;           // nothing to show
        };

        std::vector<Row> rows;

        {
            Row r { "Input", "none found", "", false, true };

            if (! sources.empty() || ! session_.sourceName().empty())
            {
                r.value = elide (session_.sourceName().empty() ? sources.front().name : session_.sourceName(), 24);
                r.dim = false;
            }

            if (sources.size() > 1)
            {
                r.many = true;
                r.note = integer (session_.sourceIndex() + 1) + "/" + integer ((long long) sources.size());
            }

            rows.push_back (std::move (r));
        }

        {
            Row r { "Channels", "-", "", false, true };

            if (! choices.empty())
            {
                r.value = choices[(size_t) std::clamp (session_.choiceIndex(), 0, (int) choices.size() - 1)].label;
                r.dim = false;
                r.many = choices.size() > 1;

                if (rec.channels() > 2)
                    r.note = "of " + integer (rec.channels());
            }

            rows.push_back (std::move (r));
        }

        rows.push_back ({ "Gain", gainText (session_.gainDb()), "", true, false });

        for (int i = 0; i < kRowCount; ++i)
        {
            const Row& r = rows[(size_t) i];
            const int y = kRowsY + 5 + i * kRowH;
            const bool selected = canChange && i == recordRow_;

            if (selected)
            {
                c.fillRect (kMargin + 2, y, kPanelW - 4, kRowH - 2, panelHi);
                c.fillRect (kMargin + 2, y, 3, kRowH - 2, brandHi);
            }

            const uint32_t labelCol = ! canChange ? textFaint : selected ? text : textDim;
            const uint32_t valueCol = ! canChange || r.dim ? textFaint : selected ? text : textDim;
            c.drawText (kMargin + 16, y + 4, r.label, labelCol, 2);

            const int vx = kMargin + 160;
            const int endX = c.drawText (vx, y + 4, r.value, valueCol, 2);

            if (selected && r.many)
            {
                arrowLeft (c, vx - 14, y + 7, brandHi);
                arrowRight (c, endX + 8, y + 7, brandHi);
            }

            if (! r.note.empty())
                c.drawText (textRightX (kMargin + kPanelW - 12, r.note, 1), y + 9, r.note, textFaint, 1);
        }
    }

    void App::drawRecordMeters (Canvas& c)
    {
        drawPanel (c, kMargin, kMetersY, kPanelW, kMetersH);
        c.drawText (kMargin + 10, kMetersY + 6, "LEVELS", textDim, 1);

        const Recorder& rec = session_.recorder();
        const InputStatus& in = session_.input();
        const bool broken = in.state == InputState::failed || in.state == InputState::ended;
        const int midX = kScreenW / 2;

        // The input is not running: say why, in the middle of the panel.
        if (! rec.configured())
        {
            const int y = kMetersY + 44;
            auto centred = [&] (int dy, const std::string& s, uint32_t col, int scale)
            {
                c.drawText (midX - Canvas::textWidth (s, scale) / 2, y + dy, s, col, scale);
            };

            if (session_.sources().empty() && ! broken)
            {
                centred (0, "No audio input found", textDim, 2);
                centred (30, "Plug in a USB microphone, receiver or audio interface.", textFaint, 1);
                centred (46, "On the handheld use the USB-C port, with an OTG adapter or a hub.", textFaint, 1);
                centred (62, "Kerf looks again by itself. kerf-log.txt lists the USB devices it saw.", textFaint, 1);
            }
            else if (broken)
            {
                centred (0, "The input is not working", danger, 2);
                centred (30, elide (session_.statusText(), 76), textDim, 1);
                centred (46, usingKeyboard_ ? "Press 2 to try again, or pick another input." : "Press Y to try again, or pick another input.", textFaint, 1);
            }
            else
            {
                centred (0, elide (session_.statusText(), 36), textDim, 2);
            }

            return;
        }

        // How the input is doing, at the top right.
        {
            const std::string status = elide (session_.statusText(), 66);
            const uint32_t col = broken ? danger : session_.quiet() ? warn : textDim;
            c.drawText (textRightX (kMargin + kPanelW - 10, status, 1), kMetersY + 6, status, col, 1);
        }

        const ChannelChoice& pick = rec.choice();
        const int n = rec.channels();
        const int shown = std::min (n, kMaxMeters);
        const int first = n <= kMaxMeters ? 0 : std::clamp (pick.left - kMaxMeters / 2, 0, n - kMaxMeters);

        const int areaY = kMetersY + 22, areaH = kMetersH - 44;
        const int pitch = n <= 2 ? 44 : std::min (24, areaH / shown);
        const int barH = n <= 2 ? 28 : std::max (3, pitch - 3);
        const int top = areaY + (areaH - pitch * shown) / 2 + (pitch - barH) / 2;

        for (int i = 0; i < shown; ++i)
        {
            const int ch = first + i;
            const int y = top + i * pitch;
            const bool kept = ch == pick.left || ch == pick.right;

            if (n <= 2)
                c.drawText (kMargin + 16, y + (barH - 16) / 2, integer (ch + 1), kept ? brandHi : textFaint, 2);
            else if (pitch >= 9 || kept)
                c.drawText (kMargin + 16, y + (barH - 8) / 2, integer (ch + 1), kept ? brandHi : textFaint, 1);

            drawMeter (c, kBarX, y, kBarW, barH, rec.level (ch), rec.hold (ch), kept);

            const float hold = rec.hold (ch);
            const std::string peak = peakText (hold);

            if (barH >= 7)
                c.drawText (kBarX + kBarW + 10, y + (barH - 8) / 2, peak, kept ? textDim : textFaint, 1);

            if (hold > 0.9995f)
                c.fillRect (kBarX + kBarW + 54, y, 8, barH, withAlpha (danger, kept ? 255 : 100));
        }

        drawScale (c, kBarX, kMetersY + kMetersH - 18, kBarW);

        if (n > kMaxMeters)
            c.drawText (kMargin + 10, kMetersY + kMetersH - 14, integer (first + 1) + "-" + integer (first + shown), textFaint, 1);
    }

    void App::drawRecordTake (Canvas& c)
    {
        using Phase = RecordSession::Phase;

        drawPanel (c, kMargin, kTakeY, kPanelW, kTakeH);

        const Recorder& rec = session_.recorder();
        const Phase phase = session_.phase();
        const bool blink = std::fmod (time_, 1.0) < 0.6;
        const int x0 = kMargin + 16;

        // The time of the take, and how full the take is.
        c.drawText (x0, kTakeY + 10, clockTime (rec.takeSeconds()), phase == Phase::idle ? textFaint : text, 4);

        const double limit = rec.configured() ? rec.maxSeconds() : (double) cfg_.recordLimit;
        const int barW = 300, barY = kTakeY + 52;
        const double used = limit > 0.0 ? std::clamp (rec.takeSeconds() / limit, 0.0, 1.0) : 0.0;
        c.fillRect (x0, barY, barW, 8, bg);
        c.strokeRect (x0, barY, barW, 8, border);
        c.fillRect (x0 + 2, barY + 2, (int) std::lround (used * (barW - 4)), 4, phase == Phase::recording ? (used > 0.9 ? warn : danger) : brand);

        const long long bytes = (long long) rec.takeFrames() * rec.takeChannels() * 2 + (rec.hasTake() ? 44 : 0);
        c.drawText (x0, barY + 14, "limit " + minutesText (limit), textFaint, 1);
        const std::string size = rec.hasTake() ? sizeText ((uintmax_t) bytes) : std::string();
        c.drawText (x0 + barW - Canvas::textWidth (size, 1), barY + 14, size, textDim, 1);

        // What the person can do next, in a word.
        const int bx = 350;
        const InputStatus& in = session_.input();
        std::string badge;
        uint32_t badgeCol = textFaint;

        switch (phase)
        {
            case Phase::recording:
                badge = "REC";
                badgeCol = danger;

                if (blink)
                    c.fillRect (bx, kTakeY + 14, 18, 18, danger);

                break;

            case Phase::stopped:
                badge = "STOPPED";
                badgeCol = brandHi;
                break;

            case Phase::idle:
                if (session_.ready() && session_.quiet())                                  { badge = "NO SOUND"; badgeCol = warn; }
                else if (session_.ready())                                                 { badge = "READY"; badgeCol = ok; }
                else if (in.state == InputState::failed || in.state == InputState::ended) { badge = "NOT READY"; badgeCol = danger; }
                else if (session_.sources().empty())                                       { badge = "NO INPUT"; badgeCol = textFaint; }
                else                                                                       { badge = "OPENING"; badgeCol = textDim; }

                break;
        }

        c.drawText (phase == Phase::recording ? bx + 28 : bx, kTakeY + 12, badge, badgeCol, 3);

        // The format of the take, and a warning if the sound is too loud.
        if (rec.configured())
        {
            const int takeChannels = phase == Phase::idle ? rec.choice().takeChannels() : rec.takeChannels();
            c.drawText (bx, kTakeY + 48, std::string (takeChannels == 2 ? "stereo" : "mono") + ", " + integer (rec.sampleRate()) + " Hz, 16 bit WAV", textDim, 1);

            if (rec.clipping() && phase != Phase::stopped)
                c.drawText (bx, kTakeY + 62, "Too loud - lower the gain", blink ? danger : warn, 1);
            else if (rec.clipped() && phase == Phase::stopped)
                c.drawText (bx, kTakeY + 62, "This take clipped - try again with less gain", warn, 1);
        }
    }

    void App::drawRecordHints (Canvas& c)
    {
        using Phase = RecordSession::Phase;

        c.fillRect (0, kHintsY, kScreenW, kHintsH, panel);
        c.hline (0, kHintsY, kScreenW, border);

        const bool kb = usingKeyboard_;
        const char* keyA = kb ? "Space" : "A";
        const char* keyB = kb ? "Esc" : "B";
        const char* keyX = kb ? "1" : "X";
        const char* keyY = kb ? "2" : "Y";
        const char* keySelect = kb ? "3" : "Select";
        const char* keyStart = kb ? "Tab" : "Start";
        const char* keyInput = kb ? "Q/R" : "L1/R1";

        std::vector<Seg> line1, line2;
        const std::string folder = folderName (startDir_);

        switch (session_.phase())
        {
            case Phase::idle:
                // Select leads to the library; B leads back to the sample being worked on, once there is one.
                if (session_.ready())
                    line1 = { { keyA, brandHi }, { " record  ", textDim }, { keySelect, brandHi }, { " load", textDim } };
                else
                    line1 = { { keyY, brandHi }, { " try again  ", textDim }, { keySelect, brandHi }, { " load", textDim } };

                if (wave_.valid())
                {
                    line1.push_back ({ "  ", textDim });
                    line1.push_back ({ keyB, brandHi });
                    line1.push_back ({ " back", textDim });
                }

                line2 = { { "Up/Down", textDim }, { " row  ", textFaint }, { "Left/Right", textDim }, { " change  ", textFaint },
                          { keyInput, textDim }, { " input", textFaint } };

                if (! wave_.valid())
                {
                    line2.push_back ({ "  ", textFaint });
                    line2.push_back ({ keyStart, textDim });
                    line2.push_back ({ " menu", textFaint });
                }

                break;

            case Phase::recording:
                line1 = { { keyA, brandHi }, { "/", textDim }, { keyB, brandHi }, { " stop the take", textDim } };
                line2 = { { "The take stops by itself after " + minutesText (session_.recorder().maxSeconds()) + ".", textFaint } };
                break;

            case Phase::stopped:
                line1 = { { keyA, brandHi }, { " save+load  ", textDim }, { keyX, brandHi }, { " save  ", textDim }, { keyB, brandHi }, { " discard", textDim } };
                line2 = { { "rec-NNN.wav goes in the " + elide (folder, 10) + " folder; save+load opens it in the slicer.", textFaint } };
                break;
        }

        drawSegments (c, kMargin + 2, kHintsY + 5, line1, 2);
        drawSegments (c, kMargin + 2, kHintsY + 27, line2, 1);
    }
}
