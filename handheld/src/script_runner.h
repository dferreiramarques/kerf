#pragma once

#include <string>
#include <vector>

#include "app.h"
#include "canvas.h"
#include "input.h"

namespace kerf::ui
{
    // Drives the app from a small text script, one command per line ('#' starts a comment).
    // Used for headless screenshots and end-to-end tests, and available in the real program via
    // --script for reproducing a problem on the desktop. Commands:
    //
    //   load <file>                  load a sample and wait until it is ready
    //   frames <n>  |  wait <secs>   let time pass (60 frames per second)
    //   wait load                    until a sample that is being loaded (a take that was just saved) is in
    //   reload                       load the open sample again from its file, as picking it in the library does
    //   press <button> [frames]      tap a button (default 4 frames)
    //   hold <button> | release <button>
    //   stick l|r <x> <y> [frames]   push a stick (default 30 frames), then centre it
    //   stickset l|r <x> <y>         set a stick and leave it there (centre it with 0 0)
    //   piano <slice> [frames]       computer-keyboard piano key (default 20 frames)
    //   command undo|delete|help     keyboard-only shortcuts
    //   detect <0.1-1>               run auto-detect directly
    //   keyboard | gamepad           pretend the last input came from that device
    //   shot <file.png>              save a screenshot of the frame being drawn
    //   expect <what> <value>        fail the run if it is wrong: screen, slices, selected, loaded, toast, canundo
    //   expect duration <seconds> [tolerance] | sample <text>   the length and the name of the loaded sample
    //   expect start|end <slice> <seconds> [tolerance]   a slice boundary (slices count from 1)
    //   expect page slicer|sequencer          the page of Kerf you are on
    //   expect view all|zoomed                is the whole sample in view?
    //   expect playing yes|no | tempo <bpm> [tol] | swing <percent> | length <n> | scene <n>   the sequencer
    //   expect cursor <row> <step> | cell <row> <step> on|off | cells <n>   its grid (rows and steps count from 1)
    //   expect phase idle|recording|stopped | take <secs> [tol] | gain <dB>   the Record screen's take
    //   expect input <text> | inputs <n> | channels <label start> | status <text> | saved <file>   what it listens to
    //   expect project none|saved|edited|off|problem   what the header says about the project of the sample
    //   expect file <name> yes|no    is there such a file next to the sample (rec-001.wav.kerf, ... .bak, ... .old)?
    //   print <what>                 write a value to stdout: slices, selected, view, screen, sequencer, record, project
    //
    // Every command that takes time blocks the script until it is over; use hold/release for chords.
    class ScriptRunner
    {
    public:
        ScriptRunner (App& app, kerf::Engine& engine);

        // Parses the script. On a syntax error returns false and fills `error`.
        bool parse (const std::string& text, std::string& error);

        bool finished() const noexcept { return index_ >= commands_.size() && frameCountdown_ <= 0; }
        int failures() const noexcept { return failures_; }

        // Call once per frame before PadTracker::frame(): applies any commands that are due.
        void beforeFrame (PadTracker& tracker);

        // Call after App::draw(): saves a pending screenshot.
        void afterFrame (const Canvas& canvas);

    private:
        struct Command
        {
            std::string name;
            std::vector<std::string> args;
            int line = 0;
        };

        struct Pending
        {
            enum class Kind { none, button, stick, piano } kind = Kind::none;
            Btn button = Btn::A;
            bool leftStick = true;
            int slice = 0;
        };

        void execute (const Command& c, PadTracker& tracker);
        void finishPending (PadTracker& tracker);
        void fail (const Command& c, const std::string& why);
        int frameArg (const Command& c, size_t i, int fallback) const;

        App& app_;
        kerf::Engine& engine_;
        std::vector<Command> commands_;
        size_t index_ = 0;
        float sticks_[4] = {};          // left x/y, right x/y as last set by the script
        int frameCountdown_ = 0;
        int failures_ = 0;
        std::string pendingShot_;
        Pending pending_;
    };
}
