// Tests for SdlInput: how SDL events (keyboard, game controllers, plain joysticks) turn into the
// handheld's buttons and sticks. Game pads are simulated with SDL's virtual joysticks, so the
// real SDL event path is exercised without any hardware.

// The program has its own main(): keep SDL from renaming it (matters on Windows).
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include <cstring>

#include "config.h"
#include "input.h"
#include "minitest.h"
#include "sdl_input.h"

using namespace kerf::ui;

namespace
{
    constexpr float kDt = 1.0f / 60.0f;

    SDL_Event keyEvent (SDL_Scancode sc, bool down, bool repeat = false)
    {
        SDL_Event e;
        std::memset (&e, 0, sizeof e);
        e.type = down ? SDL_KEYDOWN : SDL_KEYUP;
        e.key.keysym.scancode = sc;
        e.key.repeat = repeat ? 1 : 0;
        return e;
    }

    void pump (SdlInput& in)
    {
        SDL_PumpEvents();
        SDL_Event e;

        while (SDL_PollEvent (&e))
            in.handleEvent (e);
    }

    // A simulated pad. `type` decides whether SDL gives it a standard game-controller mapping.
    struct VirtualPad
    {
        int index = -1;
        SDL_Joystick* joy = nullptr;

        VirtualPad (SdlInput& in, SDL_JoystickType type, int axes, int buttons, int hats)
        {
            index = SDL_JoystickAttachVirtual (type, axes, buttons, hats);
            pump (in); // SDL_JOYDEVICEADDED -> the input layer opens it
            joy = SDL_JoystickFromInstanceID (SDL_JoystickGetDeviceInstanceID (index));
        }

        void detach (SdlInput& in)
        {
            SDL_JoystickDetachVirtual (index);
            pump (in);
            joy = nullptr;
        }

        void button (SdlInput& in, int b, bool down) { SDL_JoystickSetVirtualButton (joy, b, down ? 1 : 0); pump (in); }
        void axis (SdlInput& in, int a, int value) { SDL_JoystickSetVirtualAxis (joy, a, (Sint16) value); pump (in); }
        void hat (SdlInput& in, Uint8 value) { SDL_JoystickSetVirtualHat (joy, 0, value); pump (in); }
    };

    bool down (const Pad& p, Btn b) { return p.down[(int) b]; }
}

KTEST (SdlInput, keyboardButtons)
{
    PadTracker t;
    Config cfg;
    SdlInput in (t, cfg);

    in.handleEvent (keyEvent (SDL_SCANCODE_SPACE, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_ESCAPE, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_1, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_2, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_3, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_TAB, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_Q, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_R, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_LCTRL, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_RSHIFT, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_UP, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_RIGHT, true));

    Pad p = t.frame (kDt);
    EXPECT (down (p, Btn::A));
    EXPECT (down (p, Btn::B));
    EXPECT (down (p, Btn::X));
    EXPECT (down (p, Btn::Y));
    EXPECT (down (p, Btn::Select));
    EXPECT (down (p, Btn::Start));
    EXPECT (down (p, Btn::L1));
    EXPECT (down (p, Btn::R1));
    EXPECT (down (p, Btn::L2));
    EXPECT (down (p, Btn::R2));
    EXPECT (down (p, Btn::Up));
    EXPECT (down (p, Btn::Right));
    EXPECT (! down (p, Btn::Down));
    EXPECT (p.usingKeyboard);

    in.handleEvent (keyEvent (SDL_SCANCODE_SPACE, false));
    p = t.frame (kDt);
    EXPECT (! down (p, Btn::A));
    EXPECT (p.released[(int) Btn::A]);

    // Enter and keypad Enter are A as well; page keys are the shoulders.
    in.handleEvent (keyEvent (SDL_SCANCODE_RETURN, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_PAGEUP, true));
    p = t.frame (kDt);
    EXPECT (down (p, Btn::A));
    EXPECT (down (p, Btn::L1));
}

KTEST (SdlInput, keyboardRepeatIsIgnored)
{
    PadTracker t;
    Config cfg;
    SdlInput in (t, cfg);

    in.handleEvent (keyEvent (SDL_SCANCODE_A, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_A, true, true)); // OS auto-repeat
    in.handleEvent (keyEvent (SDL_SCANCODE_A, true, true));

    Pad p = t.frame (kDt);
    EXPECT (p.piano.size() == 1);
}

KTEST (SdlInput, pianoKeysMapToSlices)
{
    PadTracker t;
    Config cfg;
    SdlInput in (t, cfg);

    const SDL_Scancode keys[16] = { SDL_SCANCODE_A, SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_E, SDL_SCANCODE_D, SDL_SCANCODE_F,
                                    SDL_SCANCODE_T, SDL_SCANCODE_G, SDL_SCANCODE_Y, SDL_SCANCODE_H, SDL_SCANCODE_U, SDL_SCANCODE_J,
                                    SDL_SCANCODE_K, SDL_SCANCODE_O, SDL_SCANCODE_L, SDL_SCANCODE_P };

    for (int i = 0; i < 16; ++i)
        in.handleEvent (keyEvent (keys[i], true));

    Pad p = t.frame (kDt);
    EXPECT (p.piano.size() == 16);

    for (int i = 0; i < 16 && i < (int) p.piano.size(); ++i)
    {
        EXPECT (p.piano[(size_t) i].slice == i);
        EXPECT (p.piano[(size_t) i].down);
    }

    in.handleEvent (keyEvent (SDL_SCANCODE_P, false));
    p = t.frame (kDt);
    EXPECT (p.piano.size() == 1);
    EXPECT (p.piano.size() == 1 && p.piano[0].slice == 15 && ! p.piano[0].down);

    // None of the piano letters is also a button.
    EXPECT (! down (p, Btn::A) && ! down (p, Btn::L1) && ! down (p, Btn::R1));
}

KTEST (SdlInput, keyboardCommands)
{
    PadTracker t;
    Config cfg;
    SdlInput in (t, cfg);

    in.handleEvent (keyEvent (SDL_SCANCODE_DELETE, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_BACKSPACE, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_F1, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_DELETE, false)); // releases do nothing

    Pad p = t.frame (kDt);
    EXPECT (p.commands.size() == 3);

    if (p.commands.size() == 3)
    {
        EXPECT (p.commands[0] == KeyCommand::deleteSlice);
        EXPECT (p.commands[1] == KeyCommand::undo);
        EXPECT (p.commands[2] == KeyCommand::help);
    }
}

KTEST (SdlInput, keyboardStickKeys)
{
    PadTracker t;
    Config cfg;
    SdlInput in (t, cfg);

    in.handleEvent (keyEvent (SDL_SCANCODE_Z, true)); // START handle left
    Pad p = t.frame (kDt);
    EXPECT (p.lx < 0.0f && p.rx == 0.0f);

    in.handleEvent (keyEvent (SDL_SCANCODE_X, true)); // both keys: they cancel out
    p = t.frame (kDt);
    EXPECT (p.lx == 0.0f);

    in.handleEvent (keyEvent (SDL_SCANCODE_Z, false)); // X alone remains
    p = t.frame (kDt);
    EXPECT (p.lx > 0.0f);

    in.handleEvent (keyEvent (SDL_SCANCODE_X, false));
    p = t.frame (kDt);
    EXPECT (p.lx == 0.0f);

    in.handleEvent (keyEvent (SDL_SCANCODE_N, true));        // END handle left
    in.handleEvent (keyEvent (SDL_SCANCODE_C, true));        // zoom in around START (stick up)
    in.handleEvent (keyEvent (SDL_SCANCODE_PERIOD, true));   // zoom out around END (stick down)
    p = t.frame (kDt);
    EXPECT (p.rx < 0.0f);
    EXPECT (p.ly < 0.0f);
    EXPECT (p.ry > 0.0f);

    // Keys ramp up from a gentle start to full deflection.
    const float first = std::fabs (p.rx);
    for (int i = 0; i < 60; ++i) p = t.frame (kDt);
    EXPECT (std::fabs (p.rx) > first);
    EXPECT (std::fabs (p.rx) <= 1.0f);
}

KTEST (SdlInput, focusLossReleasesEverything)
{
    PadTracker t;
    Config cfg;
    SdlInput in (t, cfg);

    in.handleEvent (keyEvent (SDL_SCANCODE_SPACE, true));
    in.handleEvent (keyEvent (SDL_SCANCODE_Z, true));
    Pad p = t.frame (kDt);
    EXPECT (down (p, Btn::A) && p.lx < 0.0f);

    SDL_Event e;
    std::memset (&e, 0, sizeof e);
    e.type = SDL_WINDOWEVENT;
    e.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    in.handleEvent (e);

    p = t.frame (kDt);
    EXPECT (! down (p, Btn::A));
    EXPECT (p.lx == 0.0f);

    // And a key that was "held" before the focus loss does not leave the axis latched.
    in.handleEvent (keyEvent (SDL_SCANCODE_X, true));
    p = t.frame (kDt);
    EXPECT (p.lx > 0.0f); // not cancelled by a forgotten Z
}

KTEST (SdlInput, dropAndQuit)
{
    PadTracker t;
    Config cfg;
    SdlInput in (t, cfg);

    SDL_Event drop;
    std::memset (&drop, 0, sizeof drop);
    drop.type = SDL_DROPFILE;
    drop.drop.file = SDL_strdup ("/tmp/Bateria \xC3\xA9 A\xC3\xA7\xC3\xA3o.wav"); // freed by the input layer
    in.handleEvent (drop);

    SDL_Event quit;
    std::memset (&quit, 0, sizeof quit);
    quit.type = SDL_QUIT;
    in.handleEvent (quit);

    Pad p = t.frame (kDt);
    EXPECT (p.droppedFiles.size() == 1);
    EXPECT (p.droppedFiles.size() == 1 && p.droppedFiles[0] == "/tmp/Bateria \xC3\xA9 A\xC3\xA7\xC3\xA3o.wav");
    EXPECT (p.quit);
}

// SDL gives a virtual game controller its standard layout: a b x y = buttons 0..3, back = 4,
// start = 6, shoulders = 9 / 10, D-pad = buttons 11..14; sticks are axes 0..3 and the triggers
// axes 4 and 5, which rest at the bottom of their range.
namespace
{
    constexpr int vA = 0, vB = 1, vX = 2, vY = 3, vBack = 4, vStart = 6, vLeftShoulder = 9, vRightShoulder = 10;
    constexpr int vUp = 11, vDown = 12, vLeft = 13, vRight = 14;
    constexpr int triggerRest = -32768;
}

KTEST (SdlInput, gameControllerButtonsSticksAndTriggers)
{
    PadTracker t;
    Config cfg;
    SdlInput in (t, cfg);
    VirtualPad pad (in, SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 1);

    EXPECT (pad.joy != nullptr);
    EXPECT (in.hasPad());

    in.updateDebug();
    EXPECT (in.debug().isController);
    EXPECT (in.debug().axes == 6);
    EXPECT (! in.debug().deviceName.empty());

    pad.axis (in, 4, triggerRest);
    pad.axis (in, 5, triggerRest);

    for (int b : { vA, vB, vX, vY, vBack, vStart, vLeftShoulder, vRightShoulder })
        pad.button (in, b, true);

    Pad p = t.frame (kDt);
    EXPECT (down (p, Btn::A) && down (p, Btn::B) && down (p, Btn::X) && down (p, Btn::Y));
    EXPECT (down (p, Btn::L1) && down (p, Btn::R1));
    EXPECT (down (p, Btn::Select) && down (p, Btn::Start));
    EXPECT (! down (p, Btn::L2) && ! down (p, Btn::R2));
    EXPECT (! p.usingKeyboard); // pressing a pad button switches the on-screen hints to the pad

    pad.button (in, vA, false);
    p = t.frame (kDt);
    EXPECT (! down (p, Btn::A) && down (p, Btn::B));

    // D-pad
    pad.button (in, vUp, true);
    p = t.frame (kDt);
    EXPECT (down (p, Btn::Up) && ! down (p, Btn::Down));
    pad.button (in, vUp, false);
    pad.button (in, vLeft, true);
    pad.button (in, vDown, true);
    p = t.frame (kDt);
    EXPECT (down (p, Btn::Left) && down (p, Btn::Down) && ! down (p, Btn::Up));
    pad.button (in, vLeft, false);
    pad.button (in, vDown, false);
    pad.button (in, vRight, true);
    p = t.frame (kDt);
    EXPECT (down (p, Btn::Right) && ! down (p, Btn::Left) && ! down (p, Btn::Down));
    pad.button (in, vRight, false);

    // Sticks: left x/y = axes 0/1, right x/y = axes 2/3; +y is down.
    pad.axis (in, 0, 32767);
    pad.axis (in, 1, -16384);
    pad.axis (in, 2, -32768);
    pad.axis (in, 3, 8192);
    p = t.frame (kDt);
    EXPECT (std::fabs (p.lx - 1.0f) < 0.001f);
    EXPECT (std::fabs (p.ly + 0.5f) < 0.001f);
    EXPECT (std::fabs (p.rx + 1.0f) < 0.001f);
    EXPECT (std::fabs (p.ry - 0.25f) < 0.001f);

    pad.axis (in, 0, 0);
    pad.axis (in, 2, 0);
    p = t.frame (kDt);
    EXPECT (p.lx == 0.0f && p.rx == 0.0f);

    // Triggers act as buttons, with a little hysteresis (down at 50 %, up again below 30 %).
    pad.axis (in, 4, 32767);
    pad.axis (in, 5, 20000);
    p = t.frame (kDt);
    EXPECT (down (p, Btn::L2) && down (p, Btn::R2));
    pad.axis (in, 4, 0); // about 50 %: right at the threshold, still down
    p = t.frame (kDt);
    EXPECT (down (p, Btn::L2));
    pad.axis (in, 4, -20000); // about 20 %: released
    pad.axis (in, 5, triggerRest);
    p = t.frame (kDt);
    EXPECT (! down (p, Btn::L2) && ! down (p, Btn::R2));

    // Unplugging lets everything go.
    pad.button (in, vRightShoulder, false);
    pad.button (in, vRightShoulder, true);
    pad.detach (in);
    p = t.frame (kDt);
    EXPECT (! in.hasPad());
    EXPECT (! down (p, Btn::R1) && ! down (p, Btn::B));
}

KTEST (SdlInput, swapButtonsSetting)
{
    PadTracker t;
    Config cfg;
    cfg.swapAB = true;
    SdlInput in (t, cfg);
    VirtualPad pad (in, SDL_JOYSTICK_TYPE_GAMECONTROLLER, 6, 15, 1);

    pad.button (in, 0, true); // physical "a"
    Pad p = t.frame (kDt);
    EXPECT (down (p, Btn::B) && ! down (p, Btn::A));
    pad.button (in, 0, false);

    cfg.swapAB = false;
    cfg.swapXY = true;
    pad.button (in, 2, true); // physical "x"
    p = t.frame (kDt);
    EXPECT (down (p, Btn::Y) && ! down (p, Btn::X));
    pad.button (in, 0, true); // a is back to A
    p = t.frame (kDt);
    EXPECT (down (p, Btn::A));

    pad.detach (in);
}

KTEST (SdlInput, rawJoystickUsesConfigNumbers)
{
    PadTracker t;
    Config cfg; // defaults: A=0 B=1 X=2 Y=3 L1=4 R1=5 L2=8 R2=9 Select=6 Start=7, axes 0..3, hat for the D-pad
    SdlInput in (t, cfg);
    VirtualPad pad (in, SDL_JOYSTICK_TYPE_UNKNOWN, 4, 12, 1);

    EXPECT (pad.joy != nullptr);
    in.updateDebug();
    EXPECT (! in.debug().isController); // no mapping: read raw

    pad.button (in, 0, true);
    pad.button (in, 7, true);
    pad.button (in, 8, true);
    Pad p = t.frame (kDt);
    EXPECT (down (p, Btn::A) && down (p, Btn::Start) && down (p, Btn::L2));
    EXPECT (! down (p, Btn::B));
    EXPECT (in.debug().lastButton == 8);

    pad.hat (in, SDL_HAT_RIGHT);
    p = t.frame (kDt);
    EXPECT (down (p, Btn::Right) && ! down (p, Btn::Left));
    pad.hat (in, SDL_HAT_CENTERED);
    p = t.frame (kDt);
    EXPECT (! down (p, Btn::Right));

    pad.axis (in, 2, 16384); // right stick x
    pad.axis (in, 1, 32767); // left stick y
    p = t.frame (kDt);
    EXPECT (std::fabs (p.rx - 0.5f) < 0.001f);
    EXPECT (std::fabs (p.ly - 1.0f) < 0.001f);
    EXPECT (p.lx == 0.0f && p.ry == 0.0f);

    // A different wiring in the settings file changes what a button means.
    cfg.joyButton[(int) Btn::A] = 5;
    cfg.joyButton[(int) Btn::R1] = 0;
    pad.button (in, 0, false);
    pad.button (in, 0, true);
    p = t.frame (kDt);
    EXPECT (down (p, Btn::R1));

    pad.detach (in);
}

int main()
{
    SDL_SetHint (SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");

    if (SDL_Init (SDL_INIT_EVENTS | SDL_INIT_JOYSTICK | SDL_INIT_GAMECONTROLLER) != 0)
    {
        std::printf ("SDL_Init failed: %s\n", SDL_GetError());
        return 2;
    }

    const int result = minitest::runAll();
    SDL_Quit();
    return result;
}
