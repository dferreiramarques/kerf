#include "input.h"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace kerf::ui
{
    namespace
    {
        const char* const kNames[kBtnCount] = { "Up", "Down", "Left", "Right", "A", "B", "X", "Y", "L1", "R1", "L2", "R2", "Start", "Select" };

        int sourceIndex (PadTracker::Source s) { return (int) s; }
    }

    const char* btnName (Btn b) noexcept
    {
        const int i = (int) b;
        return (i >= 0 && i < kBtnCount) ? kNames[i] : "?";
    }

    bool btnFromName (const std::string& name, Btn& out)
    {
        std::string lower;
        for (char c : name) lower.push_back ((char) std::tolower ((unsigned char) c));

        for (int i = 0; i < kBtnCount; ++i)
        {
            std::string n = kNames[i];
            for (auto& c : n) c = (char) std::tolower ((unsigned char) c);

            if (n == lower)
            {
                out = (Btn) i;
                return true;
            }
        }

        return false;
    }

    void PadTracker::setButton (Btn b, bool down, Source src)
    {
        const int i = (int) b;
        if (i < 0 || i >= kBtnCount)
            return;

        auto& slot = srcDown_[sourceIndex (src)][i];
        if (slot == down)
            return;

        slot = down;
        activity_ = true;

        if (src == Source::keyboard) usingKeyboard_ = true;
        else if (src == Source::gamepad) usingKeyboard_ = false;

        if (down) sawPress_[i] = true;
        else sawRelease_[i] = true;
    }

    void PadTracker::setSticks (float lx, float ly, float rx, float ry, Source src)
    {
        auto& s = padSticks_[sourceIndex (src)];
        const float v[4] = { lx, ly, rx, ry };

        for (int k = 0; k < 4; ++k)
        {
            // Only a real movement counts as "the user touched the gamepad".
            if (std::fabs (v[k] - s[k]) > 0.25f && src == Source::gamepad)
                usingKeyboard_ = false;

            s[k] = std::clamp (v[k], -1.0f, 1.0f);
        }
    }

    void PadTracker::setKeyAxis (int axis, int dir)
    {
        if (axis < 0 || axis > 3)
            return;

        dir = std::clamp (dir, -1, 1);

        if (dir != keyDir_[axis])
        {
            keyDir_[axis] = dir;
            keyRamp_[axis] = 0.0f;
        }

        if (dir != 0)
        {
            activity_ = true;
            usingKeyboard_ = true;
        }
    }

    void PadTracker::pianoKey (int slice, bool down)
    {
        piano_.push_back ({ slice, down });
        activity_ = true;
        usingKeyboard_ = true;
    }

    void PadTracker::command (KeyCommand c)
    {
        commands_.push_back (c);
        activity_ = true;
        usingKeyboard_ = true;
    }

    void PadTracker::dropFile (std::string utf8Path)
    {
        dropped_.push_back (std::move (utf8Path));
        activity_ = true;
    }

    void PadTracker::releaseAll()
    {
        for (int s = 0; s < kSources; ++s)
        {
            for (int i = 0; i < kBtnCount; ++i)
            {
                if (srcDown_[s][i])
                {
                    srcDown_[s][i] = false;
                    sawRelease_[i] = true;
                }
            }

            std::fill (std::begin (padSticks_[s]), std::end (padSticks_[s]), 0.0f);
        }

        std::fill (std::begin (keyDir_), std::end (keyDir_), 0);
        std::fill (std::begin (keyRamp_), std::end (keyRamp_), 0.0f);
    }

    Pad PadTracker::frame (float dt)
    {
        Pad pad;
        pad.usingKeyboard = usingKeyboard_;
        pad.anyInput = activity_;
        activity_ = false;

        for (int i = 0; i < kBtnCount; ++i)
        {
            bool now = false;
            for (int s = 0; s < kSources; ++s)
                now = now || srcDown_[s][i];

            pad.down[i] = now;
            pad.pressed[i] = sawPress_[i] || (now && ! prev_[i]);
            pad.released[i] = sawRelease_[i] || (! now && prev_[i]);

            if (pad.pressed[i])
            {
                held_[i] = 0.0f;
                nextRepeat_[i] = repeatDelay;
                pad.repeated[i] = true;
            }
            else if (now)
            {
                held_[i] += dt;

                if (held_[i] >= nextRepeat_[i])
                {
                    pad.repeated[i] = true;
                    nextRepeat_[i] += repeatInterval;
                }
            }

            if (! now && ! pad.pressed[i])
                held_[i] = 0.0f;

            pad.held[i] = held_[i];
            prev_[i] = now;
            sawPress_[i] = false;
            sawRelease_[i] = false;
        }

        // Sticks: the gamepad's value or the emulated keyboard one, whichever is bigger.
        float out[4];

        for (int k = 0; k < 4; ++k)
        {
            if (keyDir_[k] != 0)
                keyRamp_[k] = keyRamp_[k] <= 0.0f ? 0.3f : std::min (1.0f, keyRamp_[k] + dt * 1.1f);

            const float key = (float) keyDir_[k] * keyRamp_[k];
            float v = key;

            for (int s = 0; s < kSources; ++s)
                if (std::fabs (padSticks_[s][k]) > std::fabs (v))
                    v = padSticks_[s][k];

            out[k] = v;
        }

        pad.lx = out[0]; pad.ly = out[1]; pad.rx = out[2]; pad.ry = out[3];

        pad.piano = std::move (piano_);
        piano_.clear();
        pad.commands = std::move (commands_);
        commands_.clear();
        pad.droppedFiles = std::move (dropped_);
        dropped_.clear();
        pad.quit = quit_;

        return pad;
    }
}
