#include "script_runner.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <sstream>

#include "format.h"
#include "path_util.h"

namespace kerf::ui
{
    namespace
    {
        const char* tagName (ProjectKeeper::Tag t)
        {
            switch (t)
            {
                case ProjectKeeper::Tag::none:    return "none";
                case ProjectKeeper::Tag::saved:   return "saved";
                case ProjectKeeper::Tag::edited:  return "edited";
                case ProjectKeeper::Tag::off:     return "off";
                case ProjectKeeper::Tag::problem: return "problem";
            }
            return "?";
        }

        const char* screenName (Screen s)
        {
            switch (s)
            {
                case Screen::editor:    return "editor";
                case Screen::browser:   return "browser";
                case Screen::detect:    return "detect";
                case Screen::menu:      return "menu";
                case Screen::settings:  return "settings";
                case Screen::help:      return "help";
                case Screen::inputTest: return "inputTest";
                case Screen::sequencer: return "sequencer";
                case Screen::record:    return "record";
                case Screen::projects:  return "projects";
            }
            return "?";
        }
    }

    ScriptRunner::ScriptRunner (App& app, kerf::Engine& engine) : app_ (app), engine_ (engine) {}

    bool ScriptRunner::parse (const std::string& text, std::string& error)
    {
        commands_.clear();
        index_ = 0;

        std::istringstream in (text);
        std::string line;
        int lineNo = 0;

        while (std::getline (in, line))
        {
            ++lineNo;
            const auto hash = line.find ('#');
            if (hash != std::string::npos)
                line.erase (hash);

            std::istringstream words (line);
            Command c;
            c.line = lineNo;

            if (! (words >> c.name))
                continue;

            // The rest of the line, split on spaces; a path with spaces can be written with quotes.
            std::string rest;
            std::getline (words, rest);
            size_t i = 0;

            while (i < rest.size())
            {
                while (i < rest.size() && rest[i] == ' ') ++i;
                if (i >= rest.size()) break;

                std::string arg;

                if (rest[i] == '"')
                {
                    const auto close = rest.find ('"', i + 1);
                    arg = rest.substr (i + 1, close == std::string::npos ? std::string::npos : close - i - 1);
                    i = close == std::string::npos ? rest.size() : close + 1;
                }
                else
                {
                    const auto stop = rest.find (' ', i);
                    arg = rest.substr (i, stop == std::string::npos ? std::string::npos : stop - i);
                    i = stop == std::string::npos ? rest.size() : stop;
                }

                if (! arg.empty() && arg.back() == '\r') arg.pop_back();
                c.args.push_back (arg);
            }

            static const char* const known[] = { "load", "reload", "frames", "wait", "press", "hold", "release", "stick", "stickset", "piano", "command",
                                                  "detect", "keyboard", "gamepad", "shot", "expect", "print" };
            bool ok = false;
            for (auto k : known) ok = ok || c.name == k;

            if (! ok)
            {
                error = "line " + integer (lineNo) + ": unknown command '" + c.name + "'";
                return false;
            }

            commands_.push_back (std::move (c));
        }

        return true;
    }

    int ScriptRunner::frameArg (const Command& c, size_t i, int fallback) const
    {
        if (i >= c.args.size())
            return fallback;

        return std::max (1, std::atoi (c.args[i].c_str()));
    }

    void ScriptRunner::fail (const Command& c, const std::string& why)
    {
        ++failures_;
        std::fprintf (stderr, "script line %d: FAILED %s %s: %s\n", c.line, c.name.c_str(), c.args.empty() ? "" : c.args[0].c_str(), why.c_str());
    }

    void ScriptRunner::finishPending (PadTracker& tracker)
    {
        switch (pending_.kind)
        {
            case Pending::Kind::button:
                tracker.setButton (pending_.button, false, PadTracker::Source::script);
                break;

            case Pending::Kind::stick:
                sticks_[pending_.leftStick ? 0 : 2] = 0.0f;
                sticks_[pending_.leftStick ? 1 : 3] = 0.0f;
                tracker.setSticks (sticks_[0], sticks_[1], sticks_[2], sticks_[3], PadTracker::Source::script);
                break;

            case Pending::Kind::piano:
                tracker.pianoKey (pending_.slice, false);
                break;

            case Pending::Kind::none:
                break;
        }

        pending_ = Pending {};
    }

    void ScriptRunner::beforeFrame (PadTracker& tracker)
    {
        if (frameCountdown_ > 0)
        {
            if (--frameCountdown_ > 0)
                return;

            finishPending (tracker);
        }

        while (index_ < commands_.size() && frameCountdown_ == 0)
            execute (commands_[index_++], tracker);
    }

    void ScriptRunner::afterFrame (const Canvas& canvas)
    {
        if (! pendingShot_.empty())
        {
            if (! canvas.savePng (pendingShot_))
                std::fprintf (stderr, "could not write screenshot %s\n", pendingShot_.c_str());

            pendingShot_.clear();
        }
    }

    void ScriptRunner::execute (const Command& c, PadTracker& tracker)
    {
        const auto arg = [&] (size_t i) -> std::string { return i < c.args.size() ? c.args[i] : std::string(); };

        if (c.name == "load")
        {
            app_.loadFile (pathFromUtf8 (arg (0)));
            app_.waitForLoad();

            if (! engine_.sample())
                fail (c, "sample did not load");
        }
        else if (c.name == "reload")
        {
            if (app_.samplePath().empty())
            {
                fail (c, "no sample has been loaded");
                return;
            }

            app_.loadFile (app_.samplePath());
            app_.waitForLoad();
        }
        else if (c.name == "frames")
        {
            frameCountdown_ = frameArg (c, 0, 1);
        }
        else if (c.name == "wait")
        {
            if (arg (0) == "load")      // a sample the program is loading (a take that was just saved)
                app_.waitForLoad();
            else
                frameCountdown_ = std::max (1, (int) std::lround (std::atof (arg (0).c_str()) * 60.0));
        }
        else if (c.name == "press" || c.name == "hold" || c.name == "release")
        {
            Btn b;
            if (! btnFromName (arg (0), b)) { fail (c, "unknown button"); return; }

            if (c.name == "hold")
            {
                tracker.setButton (b, true, PadTracker::Source::script);
            }
            else if (c.name == "release")
            {
                tracker.setButton (b, false, PadTracker::Source::script);
            }
            else
            {
                tracker.setButton (b, true, PadTracker::Source::script);
                pending_.kind = Pending::Kind::button;
                pending_.button = b;
                frameCountdown_ = frameArg (c, 1, 4);
            }
        }
        else if (c.name == "stick" || c.name == "stickset")
        {
            const bool left = arg (0) == "l" || arg (0) == "L";
            const float x = (float) std::atof (arg (1).c_str());
            const float y = (float) std::atof (arg (2).c_str());

            sticks_[left ? 0 : 2] = x;
            sticks_[left ? 1 : 3] = y;
            tracker.setSticks (sticks_[0], sticks_[1], sticks_[2], sticks_[3], PadTracker::Source::script);

            if (c.name == "stick")
            {
                pending_.kind = Pending::Kind::stick;
                pending_.leftStick = left;
                frameCountdown_ = frameArg (c, 3, 30);
            }
        }
        else if (c.name == "piano")
        {
            const int slice = std::atoi (arg (0).c_str());
            tracker.pianoKey (slice, true);
            pending_.kind = Pending::Kind::piano;
            pending_.slice = slice;
            frameCountdown_ = frameArg (c, 1, 20);
        }
        else if (c.name == "command")
        {
            if (arg (0) == "undo") tracker.command (KeyCommand::undo);
            else if (arg (0) == "delete") tracker.command (KeyCommand::deleteSlice);
            else if (arg (0) == "help") tracker.command (KeyCommand::help);
            else fail (c, "unknown command");
        }
        else if (c.name == "detect")
        {
            const int n = app_.detectSlices ((float) std::atof (arg (0).c_str()));
            std::printf ("detect: %d slices\n", n);
        }
        else if (c.name == "keyboard")
        {
            tracker.setKeyAxis (0, 0); // marks keyboard as the active device
            tracker.pianoKey (-1, false);
        }
        else if (c.name == "gamepad")
        {
            tracker.setSticks (0.5f, 0, 0, 0, PadTracker::Source::gamepad);
            tracker.setSticks (0, 0, 0, 0, PadTracker::Source::gamepad);
        }
        else if (c.name == "shot")
        {
            // The picture is taken after this frame's update; later commands wait for the next frame.
            pendingShot_ = arg (0);
            frameCountdown_ = 1;
        }
        else if (c.name == "expect")
        {
            const auto what = arg (0), want = arg (1);

            if (what == "screen")
            {
                if (screenName (app_.screen()) != want)
                    fail (c, std::string ("screen is ") + screenName (app_.screen()) + ", wanted " + want);
            }
            else if (what == "slices")
            {
                if (engine_.slices().size() != std::atoi (want.c_str()))
                    fail (c, "slice count is " + integer (engine_.slices().size()) + ", wanted " + want);
            }
            else if (what == "selected")
            {
                if (app_.selectedSlice() != std::atoi (want.c_str()))
                    fail (c, "selected is " + integer (app_.selectedSlice()) + ", wanted " + want);
            }
            else if (what == "loaded")
            {
                const bool loaded = engine_.sample() != nullptr;
                if (loaded != (want == "yes"))
                    fail (c, loaded ? "a sample is loaded" : "no sample is loaded");
            }
            else if (what == "duration")
            {
                // expect duration <seconds> [tolerance, default 0.05]: the length of the sample that is loaded
                const auto sample = engine_.sample();
                const double got = sample ? sample->durationSeconds() : -1.0;
                const double tolerance = c.args.size() > 2 ? std::atof (arg (2).c_str()) : 0.05;

                if (std::fabs (got - std::atof (want.c_str())) > tolerance)
                    fail (c, sample ? "the sample is " + fixed (got, 3) + " s long, wanted " + want : std::string ("no sample is loaded"));
            }
            else if (what == "sample")
            {
                // expect sample <text>: the name of the sample that is loaded contains the text
                const auto sample = engine_.sample();
                const std::string name = sample ? sample->name : std::string();

                if (name.find (want) == std::string::npos)
                    fail (c, "the sample is '" + name + "'");
            }
            else if (what == "toast")
            {
                if (app_.toast().find (want) == std::string::npos)
                    fail (c, "toast is '" + app_.toast() + "'");
            }
            else if (what == "start" || what == "end")
            {
                // expect start|end <slice, counting from 1> <seconds> [tolerance, default 2 ms]
                const int index = std::atoi (arg (1).c_str()) - 1;
                const double wanted = std::atof (arg (2).c_str());
                const double tolerance = c.args.size() > 3 ? std::atof (arg (3).c_str()) : 0.002;
                const auto current = engine_.slices().getCurrent();

                if (index < 0 || index >= (int) current->size())
                {
                    fail (c, "there is no slice " + integer (index + 1));
                }
                else
                {
                    const double got = what == "start" ? (double) (*current)[(size_t) index].start : (double) (*current)[(size_t) index].end;

                    if (std::fabs (got - wanted) > tolerance)
                        fail (c, "slice " + integer (index + 1) + " " + what + " is " + seconds3 (got) + ", wanted " + seconds3 (wanted));
                }
            }
            else if (what == "view")
            {
                // expect view all|zoomed: is the whole sample in view?
                const auto sample = engine_.sample();
                const double duration = sample ? sample->durationSeconds() : 0.0;
                const bool all = (app_.viewEnd() - app_.viewStart()) >= 0.98 * duration;
                if (all != (want == "all"))
                    fail (c, std::string ("the view shows ") + (all ? "the whole sample" : "a part of it"));
            }
            else if (what == "page")
            {
                // expect page slicer|sequencer: the page of Kerf you are on (also behind a menu)
                const char* name = app_.mainScreen() == Screen::sequencer ? "sequencer" : "slicer";
                if (want != name)
                    fail (c, std::string ("page is ") + name + ", wanted " + want);
            }
            else if (what == "playing")
            {
                if (engine_.sequencer().isPlaying() != (want == "yes"))
                    fail (c, std::string ("sequencer playing is ") + (engine_.sequencer().isPlaying() ? "yes" : "no"));
            }
            else if (what == "tempo")
            {
                const double got = (double) engine_.sequencer().tempo();
                const double tolerance = c.args.size() > 2 ? std::atof (arg (2).c_str()) : 0.05;
                if (std::fabs (got - std::atof (want.c_str())) > tolerance)
                    fail (c, "tempo is " + fixed (got, 2) + ", wanted " + want);
            }
            else if (what == "swing")
            {
                // percent: 50 = straight
                const double got = (double) engine_.sequencer().swing() * 100.0;
                if (std::fabs (got - std::atof (want.c_str())) > 0.6)
                    fail (c, "swing is " + fixed (got, 1) + ", wanted " + want);
            }
            else if (what == "length")
            {
                if (engine_.sequencer().length() != std::atoi (want.c_str()))
                    fail (c, "length is " + integer (engine_.sequencer().length()) + ", wanted " + want);
            }
            else if (what == "scene")
            {
                // scenes count from 1
                if (engine_.sequencer().scene() + 1 != std::atoi (want.c_str()))
                    fail (c, "scene is " + integer (engine_.sequencer().scene() + 1) + ", wanted " + want);
            }
            else if (what == "cursor")
            {
                // expect cursor <row> <step>, both counting from 1
                const int row = std::atoi (want.c_str()), step = std::atoi (arg (2).c_str());
                if (app_.seqRow() + 1 != row || app_.seqCol() + 1 != step)
                    fail (c, "cursor is " + integer (app_.seqRow() + 1) + "/" + integer (app_.seqCol() + 1) + ", wanted " + want + "/" + arg (2));
            }
            else if (what == "cell")
            {
                // expect cell <row> <step> on|off  (rows and steps count from 1; the scene being edited)
                const int row = std::atoi (want.c_str()) - 1, step = std::atoi (arg (2).c_str()) - 1;
                const bool on = engine_.sequencer().cell (engine_.sequencer().scene(), row, step);
                if (on != (arg (3) == "on"))
                    fail (c, "cell " + want + "/" + arg (2) + " is " + (on ? "on" : "off"));
            }
            else if (what == "cells")
            {
                // expect cells <n>: how many steps are lit in the scene being edited
                const int n = engine_.sequencer().cellCount (engine_.sequencer().scene());
                if (n != std::atoi (want.c_str()))
                    fail (c, "lit steps: " + integer (n) + ", wanted " + want);
            }
            else if (what == "phase")
            {
                // expect phase idle|recording|stopped: where the Record screen is with its take
                const auto phase = app_.recording().phase();
                const char* name = phase == RecordSession::Phase::recording ? "recording" : phase == RecordSession::Phase::stopped ? "stopped" : "idle";
                if (want != name)
                    fail (c, std::string ("record phase is ") + name + ", wanted " + want);
            }
            else if (what == "take")
            {
                // expect take <seconds> [tolerance, default 0.05]: the length of the take
                const double got = app_.recording().recorder().takeSeconds();
                const double tolerance = c.args.size() > 2 ? std::atof (arg (2).c_str()) : 0.05;
                if (std::fabs (got - std::atof (want.c_str())) > tolerance)
                    fail (c, "take is " + fixed (got, 3) + " s, wanted " + want);
            }
            else if (what == "input")
            {
                // expect input <text>: the name of the input that is open contains the text
                const auto name = app_.recording().sourceName();
                if (name.find (want) == std::string::npos)
                    fail (c, "input is '" + name + "'");
            }
            else if (what == "inputs")
            {
                // expect inputs <n>: how many inputs the Record screen can choose from
                if ((int) app_.recording().sources().size() != std::atoi (want.c_str()))
                    fail (c, "inputs found: " + integer ((long long) app_.recording().sources().size()) + ", wanted " + want);
            }
            else if (what == "channels")
            {
                // expect channels <label start>: "3+4" for "3+4  stereo"
                const auto& rec = app_.recording();
                const std::string label = rec.choices().empty() ? std::string() : rec.choices()[(size_t) rec.choiceIndex()].label;
                if (label.rfind (want, 0) != 0)
                    fail (c, "channels are '" + label + "', wanted " + want);
            }
            else if (what == "gain")
            {
                if (app_.recording().gainDb() != std::atoi (want.c_str()))
                    fail (c, "gain is " + integer (app_.recording().gainDb()) + " dB, wanted " + want);
            }
            else if (what == "status")
            {
                // expect status <text>: the Record screen's line about the input says this
                const auto status = app_.recording().statusText();
                if (status.find (want) == std::string::npos)
                    fail (c, "status is '" + status + "'");
            }
            else if (what == "saved")
            {
                // expect saved <file name>: the latest take written to disk, e.g. rec-001.wav
                const auto name = pathToUtf8 (app_.lastSaved().filename());
                if (name != want)
                    fail (c, "latest take is '" + name + "', wanted " + want);
            }
            else if (what == "project")
            {
                const std::string got = tagName (app_.project().tag());
                if (got != want)
                    fail (c, "the project is '" + got + "', wanted " + want);
            }
            else if (what == "file")
            {
                // expect file <name> yes|no: next to the sample that is open
                std::error_code ec;
                const bool there = ! app_.samplePath().empty() && std::filesystem::exists (app_.samplePath().parent_path() / pathFromUtf8 (want), ec);
                if (there != (arg (2) == "yes"))
                    fail (c, "file " + want + (there ? " exists" : " does not exist"));
            }
            else if (what == "canundo")
            {
                if (app_.canUndo() != (want == "yes"))
                    fail (c, "canUndo is " + std::string (app_.canUndo() ? "yes" : "no"));
            }
            else if (what == "quit")
            {
                if (app_.wantsQuit() != (want == "yes"))
                    fail (c, "wantsQuit is " + std::string (app_.wantsQuit() ? "yes" : "no"));
            }
            else
            {
                fail (c, "unknown expectation");
            }
        }
        else if (c.name == "print")
        {
            const auto what = arg (0);

            if (what == "slices")
            {
                const auto sl = engine_.slices().getCurrent();
                std::printf ("slices: %d\n", (int) sl->size());

                for (size_t i = 0; i < sl->size(); ++i)
                    std::printf ("  %2zu  %.4f - %.4f  pitch %.3f vol %.2f pan %.2f %s\n", i + 1, (double) (*sl)[i].start, (double) (*sl)[i].end,
                                 (double) (*sl)[i].pitch, (double) (*sl)[i].volume, (double) (*sl)[i].pan,
                                 (*sl)[i].mode == kerf::Slice::Mode::loop ? "loop" : "shot");
            }
            else if (what == "sequencer")
            {
                const auto& seq = engine_.sequencer();
                std::printf ("sequencer: %s  %.1f BPM  swing %.0f%%  length %d  scene %d (sounding %d)  step %s\n",
                             seq.isPlaying() ? "playing" : "stopped", (double) seq.tempo(), (double) seq.swing() * 100.0, seq.length(),
                             seq.scene() + 1, seq.activeScene() + 1,
                             seq.currentStep() < 0 ? "-" : integer (seq.currentStep() + 1).c_str());

                const int rows = std::min (engine_.slices().size(), 32);

                for (int r = 0; r < rows; ++r)
                {
                    std::printf ("  %2d ", r + 1);

                    for (int st = 0; st < kerf::StepSequencer::kSteps; ++st)
                        std::printf ("%s%c", st % 4 == 0 && st > 0 ? " " : "", seq.cell (seq.scene(), r, st) ? 'X' : '.');

                    std::printf ("\n");
                }
            }
            else if (what == "record")
            {
                const auto& rec = app_.recording();
                std::printf ("record: %s  input '%s' (%d found)  %s  gain %d dB  take %.3f s  %s\n",
                             screenName (app_.screen()), rec.sourceName().c_str(), (int) rec.sources().size(),
                             rec.choices().empty() ? "-" : rec.choices()[(size_t) rec.choiceIndex()].label.c_str(),
                             rec.gainDb(), rec.recorder().takeSeconds(), rec.statusText().c_str());
            }
            else if (what == "project")
            {
                std::printf ("project: %s  file '%s'\n", tagName (app_.project().tag()), pathToUtf8 (app_.project().file().filename()).c_str());
            }
            else if (what == "selected") std::printf ("selected: %d\n", app_.selectedSlice() + 1);
            else if (what == "view")     std::printf ("view: %.4f - %.4f\n", app_.viewStart(), app_.viewEnd());
            else if (what == "screen")   std::printf ("screen: %s\n", screenName (app_.screen()));
            else                         fail (c, "unknown value");
        }
    }
}
