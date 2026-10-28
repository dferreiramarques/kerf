#pragma once

#include <cmath>

namespace kerf::ui
{
    // Turns a raw stick deflection (-1..1) into the value the interface works with: nothing inside
    // the dead zone, then a quadratic ramp so there is fine control near the centre and speed at
    // the edge. Used by everything a stick drives (the slice handles, the tempo and the swing).
    inline float shapeStick (float v, float deadzone) noexcept
    {
        float a = std::fabs (v);

        if (a <= deadzone)
            return 0.0f;

        a = (a - deadzone) / (1.0f - deadzone);
        return std::copysign (a * a, v);
    }

    // A stick pushed mostly sideways should not zoom, and one pushed mostly up/down should not move.
    inline void lockAxis (float& x, float& y) noexcept
    {
        if (std::fabs (x) > std::fabs (y) * 1.5f) y = 0.0f;
        else if (std::fabs (y) > std::fabs (x) * 1.5f) x = 0.0f;
    }
}
