#pragma once

#include <string>
#include <vector>

namespace kerf::ui
{
    // The handheld's controls. Everything the UI does is expressed with these, so the keyboard,
    // a USB/Bluetooth pad, the RG35XX H's own buttons and the test scripts all drive the same code.
    enum class Btn : int { Up, Down, Left, Right, A, B, X, Y, L1, R1, L2, R2, Start, Select, Count };
    constexpr int kBtnCount = (int) Btn::Count;

    const char* btnName (Btn b) noexcept;                 // "A", "L1", "Start", ...
    bool btnFromName (const std::string& name, Btn& out); // case-insensitive, for scripts

    // A computer-keyboard "piano" key pressed or released; `slice` is the slice it plays.
    struct PianoEvent
    {
        int slice = 0;
        bool down = false;
    };

    // Keyboard-only shortcuts that have no button on the handheld (there they are chords or menu items).
    enum class KeyCommand { deleteSlice, undo, help };

    // Everything the UI needs to know about input for one frame.
    struct Pad
    {
        bool down[kBtnCount] = {};
        bool pressed[kBtnCount] = {};   // went down since the previous frame
        bool released[kBtnCount] = {};  // went up since the previous frame
        bool repeated[kBtnCount] = {};  // `pressed`, plus auto-repeat ticks while held (menus, D-pad)
        float held[kBtnCount] = {};     // seconds the button has been down

        // Sticks, -1..1, +x right and +y down (the SDL convention). Raw: no dead zone applied yet.
        float lx = 0.0f, ly = 0.0f, rx = 0.0f, ry = 0.0f;

        bool usingKeyboard = true;      // which device was touched last (decides the on-screen hints)
        bool anyInput = false;          // something happened this frame (wakes a dimmed screen, etc.)

        std::vector<PianoEvent> piano;
        std::vector<KeyCommand> commands;
        std::vector<std::string> droppedFiles; // UTF-8 paths
        bool quit = false;
    };

    // Accumulates input from several sources and turns it into per-frame Pad snapshots with
    // reliable edges (a tap shorter than one frame still produces `pressed` and `released`) and
    // auto-repeat. Pure logic with no SDL in it, so it is unit-tested.
    class PadTracker
    {
    public:
        enum class Source { keyboard, gamepad, script };

        void setButton (Btn b, bool down, Source src);
        void setSticks (float lx, float ly, float rx, float ry, Source src);

        // Keyboard stick emulation: axis 0=LX 1=LY 2=RX 3=RY, dir -1/0/+1. Held keys ramp up from
        // a gentle start to full deflection so a keyboard can still make fine adjustments.
        void setKeyAxis (int axis, int dir);

        void pianoKey (int slice, bool down);
        void command (KeyCommand c);
        void dropFile (std::string utf8Path);
        void requestQuit() { quit_ = true; }

        // Chooses which device's names the hints show before anything has been touched (a handheld
        // or a plugged-in pad at start-up: gamepad; otherwise the keyboard). Not counted as input.
        void preferDevice (Source src) { usingKeyboard_ = (src != Source::gamepad); }

        // Releases everything (window lost focus, controller unplugged, ...).
        void releaseAll();

        Pad frame (float dt);

        static constexpr float repeatDelay = 0.38f;
        static constexpr float repeatInterval = 0.075f;

    private:
        static constexpr int kSources = 3;

        bool srcDown_[kSources][kBtnCount] = {};
        bool prev_[kBtnCount] = {};
        bool sawPress_[kBtnCount] = {};
        bool sawRelease_[kBtnCount] = {};
        float held_[kBtnCount] = {};
        float nextRepeat_[kBtnCount] = {};

        float padSticks_[kSources][4] = {};
        int keyDir_[4] = {};
        float keyRamp_[4] = {};

        std::vector<PianoEvent> piano_;
        std::vector<KeyCommand> commands_;
        std::vector<std::string> dropped_;
        bool quit_ = false;
        bool usingKeyboard_ = true;
        bool activity_ = false;
    };
}
