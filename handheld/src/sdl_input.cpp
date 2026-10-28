#include "sdl_input.h"

#include <algorithm>
#include <cmath>

namespace kerf::ui
{
    namespace
    {
        float axisValue (Sint16 v) noexcept
        {
            return v < 0 ? (float) v / 32768.0f : (float) v / 32767.0f;
        }

        // The computer-keyboard piano: two rows of keys, white keys on the home row.
        int pianoSlice (SDL_Scancode sc) noexcept
        {
            switch (sc)
            {
                case SDL_SCANCODE_A: return 0;
                case SDL_SCANCODE_W: return 1;
                case SDL_SCANCODE_S: return 2;
                case SDL_SCANCODE_E: return 3;
                case SDL_SCANCODE_D: return 4;
                case SDL_SCANCODE_F: return 5;
                case SDL_SCANCODE_T: return 6;
                case SDL_SCANCODE_G: return 7;
                case SDL_SCANCODE_Y: return 8;
                case SDL_SCANCODE_H: return 9;
                case SDL_SCANCODE_U: return 10;
                case SDL_SCANCODE_J: return 11;
                case SDL_SCANCODE_K: return 12;
                case SDL_SCANCODE_O: return 13;
                case SDL_SCANCODE_L: return 14;
                case SDL_SCANCODE_P: return 15;
                default:             return -1;
            }
        }

        bool controllerToLogical (Uint8 button, Btn& out) noexcept
        {
            switch (button)
            {
                case SDL_CONTROLLER_BUTTON_DPAD_UP:       out = Btn::Up;     return true;
                case SDL_CONTROLLER_BUTTON_DPAD_DOWN:     out = Btn::Down;   return true;
                case SDL_CONTROLLER_BUTTON_DPAD_LEFT:     out = Btn::Left;   return true;
                case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:    out = Btn::Right;  return true;
                case SDL_CONTROLLER_BUTTON_A:             out = Btn::A;      return true;
                case SDL_CONTROLLER_BUTTON_B:             out = Btn::B;      return true;
                case SDL_CONTROLLER_BUTTON_X:             out = Btn::X;      return true;
                case SDL_CONTROLLER_BUTTON_Y:             out = Btn::Y;      return true;
                case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:  out = Btn::L1;     return true;
                case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: out = Btn::R1;     return true;
                case SDL_CONTROLLER_BUTTON_START:         out = Btn::Start;  return true;
                case SDL_CONTROLLER_BUTTON_BACK:          out = Btn::Select; return true;
                default:                                  return false;
            }
        }
    }

    SdlInput::SdlInput (PadTracker& tracker, const Config& config) : tracker_ (tracker), cfg_ (config) {}

    SdlInput::~SdlInput()
    {
        while (! devices_.empty())
            removeDevice (devices_.back().id);
    }

    // ------------------------------------------------------------------------------- devices

    SdlInput::Device* SdlInput::find (SDL_JoystickID id)
    {
        for (auto& d : devices_)
            if (d.id == id)
                return &d;

        return nullptr;
    }

    void SdlInput::openExisting()
    {
        for (int i = 0; i < SDL_NumJoysticks(); ++i)
            addDevice (i);
    }

    void SdlInput::addDevice (int index)
    {
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID (index);

        if (id < 0 || find (id) != nullptr)
            return;

        Device d;
        d.id = id;

        if (SDL_IsGameController (index))
        {
            d.controller = SDL_GameControllerOpen (index);

            if (d.controller != nullptr)
            {
                d.joystick = SDL_GameControllerGetJoystick (d.controller);
                const char* n = SDL_GameControllerName (d.controller);
                d.name = n != nullptr ? n : "game controller";
            }
        }

        if (d.controller == nullptr)
        {
            d.joystick = SDL_JoystickOpen (index);

            if (d.joystick == nullptr)
                return;

            const char* n = SDL_JoystickName (d.joystick);
            d.name = n != nullptr ? n : "joystick";
        }

        devices_.push_back (std::move (d));
    }

    void SdlInput::removeDevice (SDL_JoystickID id)
    {
        for (size_t i = 0; i < devices_.size(); ++i)
        {
            if (devices_[i].id != id)
                continue;

            if (devices_[i].controller != nullptr)
                SDL_GameControllerClose (devices_[i].controller);
            else if (devices_[i].joystick != nullptr)
                SDL_JoystickClose (devices_[i].joystick);

            devices_.erase (devices_.begin() + (std::ptrdiff_t) i);

            // Whatever the pad was holding down is let go, so nothing can stay stuck.
            std::fill (std::begin (sticks_), std::end (sticks_), 0.0f);
            std::fill (std::begin (triggerDown_), std::end (triggerDown_), false);
            std::fill (std::begin (hatDown_), std::end (hatDown_), false);
            tracker_.releaseAll();
            return;
        }
    }

    void SdlInput::updateDebug()
    {
        if (devices_.empty())
        {
            debug_.deviceName.clear();
            debug_.isController = false;
            debug_.axes = debug_.buttons = debug_.hats = 0;
            return;
        }

        // Prefer a pad SDL has a mapping for; otherwise the first one found.
        const Device* shown = &devices_.front();

        for (const auto& d : devices_)
        {
            if (d.controller != nullptr)
            {
                shown = &d;
                break;
            }
        }

        debug_.deviceName = shown->name;
        debug_.isController = shown->controller != nullptr;

        if (shown->joystick == nullptr)
            return;

        debug_.axes = std::max (0, SDL_JoystickNumAxes (shown->joystick));
        debug_.buttons = std::max (0, SDL_JoystickNumButtons (shown->joystick));
        debug_.hats = std::max (0, SDL_JoystickNumHats (shown->joystick));

        for (int i = 0; i < 8; ++i)
            debug_.axis[i] = i < debug_.axes ? axisValue (SDL_JoystickGetAxis (shown->joystick, i)) : 0.0f;
    }

    // ------------------------------------------------------------------------------- events

    void SdlInput::handleEvent (const SDL_Event& e)
    {
        switch (e.type)
        {
            case SDL_KEYDOWN:
            case SDL_KEYUP:                keyEvent (e.key); break;

            case SDL_JOYDEVICEADDED:       addDevice (e.jdevice.which); break;
            case SDL_JOYDEVICEREMOVED:     removeDevice (e.jdevice.which); break;

            case SDL_CONTROLLERBUTTONDOWN:
            case SDL_CONTROLLERBUTTONUP:   controllerButton (e.cbutton); break;
            case SDL_CONTROLLERAXISMOTION: controllerAxis (e.caxis); break;

            case SDL_JOYBUTTONDOWN:
            case SDL_JOYBUTTONUP:          joystickButton (e.jbutton); break;
            case SDL_JOYAXISMOTION:        joystickAxis (e.jaxis); break;
            case SDL_JOYHATMOTION:         joystickHat (e.jhat); break;

            case SDL_DROPFILE:
                if (e.drop.file != nullptr)
                {
                    tracker_.dropFile (e.drop.file); // UTF-8
                    SDL_free (e.drop.file);
                }
                break;

            case SDL_WINDOWEVENT:
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)
                {
                    std::fill (&keyAxisDown_[0][0], &keyAxisDown_[0][0] + 8, false);
                    tracker_.releaseAll();
                }
                break;

            case SDL_QUIT:
                tracker_.requestQuit();
                break;

            default:
                break;
        }
    }

    void SdlInput::padButton (Btn b, bool down)
    {
        if (cfg_.swapAB)
        {
            if (b == Btn::A) b = Btn::B;
            else if (b == Btn::B) b = Btn::A;
        }

        if (cfg_.swapXY)
        {
            if (b == Btn::X) b = Btn::Y;
            else if (b == Btn::Y) b = Btn::X;
        }

        tracker_.setButton (b, down, PadTracker::Source::gamepad);
    }

    void SdlInput::pushSticks()
    {
        tracker_.setSticks (sticks_[0], sticks_[1], sticks_[2], sticks_[3], PadTracker::Source::gamepad);
    }

    void SdlInput::triggerAxis (bool left, float value)
    {
        bool& state = triggerDown_[left ? 0 : 1];

        if (! state && value >= 0.5f) state = true;
        else if (state && value <= 0.3f) state = false;
        else return;

        padButton (left ? Btn::L2 : Btn::R2, state);
    }

    // Standard game controllers ----------------------------------------------------------------

    void SdlInput::controllerButton (const SDL_ControllerButtonEvent& b)
    {
        if (b.type == SDL_CONTROLLERBUTTONDOWN)
        {
            const char* name = SDL_GameControllerGetStringForButton ((SDL_GameControllerButton) b.button);
            debug_.lastControllerButton = name != nullptr ? name : "?";
        }

        Btn logical;

        if (controllerToLogical (b.button, logical))
            padButton (logical, b.type == SDL_CONTROLLERBUTTONDOWN);
    }

    void SdlInput::controllerAxis (const SDL_ControllerAxisEvent& a)
    {
        const float v = axisValue (a.value);

        switch (a.axis)
        {
            case SDL_CONTROLLER_AXIS_LEFTX:        sticks_[0] = v; pushSticks(); break;
            case SDL_CONTROLLER_AXIS_LEFTY:        sticks_[1] = v; pushSticks(); break;
            case SDL_CONTROLLER_AXIS_RIGHTX:       sticks_[2] = v; pushSticks(); break;
            case SDL_CONTROLLER_AXIS_RIGHTY:       sticks_[3] = v; pushSticks(); break;
            case SDL_CONTROLLER_AXIS_TRIGGERLEFT:  triggerAxis (true, v); break;
            case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: triggerAxis (false, v); break;
            default: break;
        }
    }

    // Raw joysticks (no mapping known) ---------------------------------------------------------

    void SdlInput::joystickButton (const SDL_JoyButtonEvent& b)
    {
        const bool down = b.type == SDL_JOYBUTTONDOWN;

        if (down)
            debug_.lastButton = b.button;

        const Device* d = find (b.which);

        if (d == nullptr || d->controller != nullptr)
            return; // controllers are handled through their own events

        for (int i = 0; i < kBtnCount; ++i)
            if (cfg_.joyButton[i] == (int) b.button)
                padButton ((Btn) i, down);
    }

    void SdlInput::joystickAxis (const SDL_JoyAxisEvent& a)
    {
        const Device* d = find (a.which);

        if (d == nullptr || d->controller != nullptr)
            return;

        const float v = axisValue (a.value);

        for (int k = 0; k < 4; ++k)
            if (cfg_.joyAxis[k] == (int) a.axis)
                sticks_[k] = v;

        pushSticks();
    }

    void SdlInput::joystickHat (const SDL_JoyHatEvent& h)
    {
        const Device* d = find (h.which);

        if (d == nullptr || d->controller != nullptr || h.hat != 0)
            return;

        const bool want[4] = { (h.value & SDL_HAT_UP) != 0, (h.value & SDL_HAT_DOWN) != 0,
                               (h.value & SDL_HAT_LEFT) != 0, (h.value & SDL_HAT_RIGHT) != 0 };

        // Up, Down, Left, Right are the first four buttons; -1 in the config means "use the hat".
        for (int k = 0; k < 4; ++k)
        {
            if (cfg_.joyButton[k] == -1 && hatDown_[k] != want[k])
            {
                hatDown_[k] = want[k];
                padButton ((Btn) k, want[k]);
            }
        }
    }

    // Keyboard ---------------------------------------------------------------------------------

    void SdlInput::keyAxis (int axis, int sign, bool down)
    {
        keyAxisDown_[axis][sign > 0 ? 1 : 0] = down;
        const int dir = (keyAxisDown_[axis][1] ? 1 : 0) - (keyAxisDown_[axis][0] ? 1 : 0);
        tracker_.setKeyAxis (axis, dir);
    }

    void SdlInput::keyEvent (const SDL_KeyboardEvent& k)
    {
        if (k.repeat != 0)
            return; // the tracker makes its own auto-repeat

        const bool down = k.type == SDL_KEYDOWN;
        const SDL_Scancode sc = k.keysym.scancode;

        if (const int slice = pianoSlice (sc); slice >= 0)
        {
            tracker_.pianoKey (slice, down);
            return;
        }

        // Keys that stand in for the two analogue sticks (left = START handle, right = END handle).
        switch (sc)
        {
            case SDL_SCANCODE_Z:      keyAxis (0, -1, down); return;
            case SDL_SCANCODE_X:      keyAxis (0, +1, down); return;
            case SDL_SCANCODE_C:      keyAxis (1, -1, down); return; // up = zoom in
            case SDL_SCANCODE_V:      keyAxis (1, +1, down); return;
            case SDL_SCANCODE_N:      keyAxis (2, -1, down); return;
            case SDL_SCANCODE_M:      keyAxis (2, +1, down); return;
            case SDL_SCANCODE_COMMA:  keyAxis (3, -1, down); return;
            case SDL_SCANCODE_PERIOD: keyAxis (3, +1, down); return;
            default: break;
        }

        // Shortcuts that have no button on the handheld.
        if (down)
        {
            if (sc == SDL_SCANCODE_DELETE)    { tracker_.command (KeyCommand::deleteSlice); return; }
            if (sc == SDL_SCANCODE_BACKSPACE) { tracker_.command (KeyCommand::undo); return; }
            if (sc == SDL_SCANCODE_F1)        { tracker_.command (KeyCommand::help); return; }
        }

        Btn b;

        switch (sc)
        {
            case SDL_SCANCODE_UP:       b = Btn::Up; break;
            case SDL_SCANCODE_DOWN:     b = Btn::Down; break;
            case SDL_SCANCODE_LEFT:     b = Btn::Left; break;
            case SDL_SCANCODE_RIGHT:    b = Btn::Right; break;
            case SDL_SCANCODE_SPACE:
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_KP_ENTER: b = Btn::A; break;
            case SDL_SCANCODE_ESCAPE:   b = Btn::B; break;
            case SDL_SCANCODE_1:        b = Btn::X; break;
            case SDL_SCANCODE_2:        b = Btn::Y; break;
            case SDL_SCANCODE_3:        b = Btn::Select; break;
            case SDL_SCANCODE_TAB:      b = Btn::Start; break;
            case SDL_SCANCODE_Q:
            case SDL_SCANCODE_PAGEUP:   b = Btn::L1; break;
            case SDL_SCANCODE_R:
            case SDL_SCANCODE_PAGEDOWN: b = Btn::R1; break;
            case SDL_SCANCODE_LCTRL:
            case SDL_SCANCODE_RCTRL:    b = Btn::L2; break;
            case SDL_SCANCODE_LSHIFT:
            case SDL_SCANCODE_RSHIFT:   b = Btn::R2; break;
            default: return;
        }

        tracker_.setButton (b, down, PadTracker::Source::keyboard);
    }
}
