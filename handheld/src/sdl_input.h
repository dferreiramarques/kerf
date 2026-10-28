#pragma once

#include <SDL.h>

#include <string>
#include <vector>

#include "app.h"
#include "config.h"
#include "input.h"

namespace kerf::ui
{
    // Turns SDL events (keyboard, game controllers, plain joysticks, dropped files) into the
    // handheld's logical buttons and sticks, via a PadTracker.
    //
    // Pads: a device SDL knows a button mapping for is used as a standard game controller. Any
    // other joystick is read raw, using the axis and button numbers in the Config (the "Input
    // test" screen shows the numbers to put there). Handhelds such as the RG35XX H normally get
    // their mapping from SDL_GAMECONTROLLERCONFIG, which the PortMaster launcher sets.
    class SdlInput
    {
    public:
        SdlInput (PadTracker& tracker, const Config& config);
        ~SdlInput();

        SdlInput (const SdlInput&) = delete;
        SdlInput& operator= (const SdlInput&) = delete;

        // Opens the pads that are already plugged in. Call once after SDL_Init.
        void openExisting();

        void handleEvent (const SDL_Event& e);

        // Once per frame: refreshes the information the "Input test" screen shows.
        void updateDebug();

        bool hasPad() const noexcept { return ! devices_.empty(); }
        const InputDebug& debug() const noexcept { return debug_; }

    private:
        struct Device
        {
            SDL_JoystickID id = -1;
            SDL_GameController* controller = nullptr; // null: raw joystick
            SDL_Joystick* joystick = nullptr;
            std::string name;
        };

        void addDevice (int index);
        void removeDevice (SDL_JoystickID id);
        Device* find (SDL_JoystickID id);

        void keyEvent (const SDL_KeyboardEvent& k);
        void controllerButton (const SDL_ControllerButtonEvent& b);
        void controllerAxis (const SDL_ControllerAxisEvent& a);
        void joystickButton (const SDL_JoyButtonEvent& b);
        void joystickAxis (const SDL_JoyAxisEvent& a);
        void joystickHat (const SDL_JoyHatEvent& h);

        void padButton (Btn b, bool down);                  // applies the A/B and X/Y swaps
        void pushSticks();
        void keyAxis (int axis, int sign, bool down);
        void triggerAxis (bool left, float value);

        PadTracker& tracker_;
        const Config& cfg_;
        std::vector<Device> devices_;

        float sticks_[4] = {};              // left x/y, right x/y from the pad
        bool keyAxisDown_[4][2] = {};       // [axis][0 = negative key, 1 = positive key]
        bool triggerDown_[2] = {};
        bool hatDown_[4] = {};              // raw joystick hat as up/down/left/right

        InputDebug debug_;
    };
}
