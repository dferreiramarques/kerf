#pragma once

#include "canvas.h"

namespace kerf::ui::theme
{
    // Dark theme built on Kerf's indigo (#6366f1) and its slice colours (kerf.html).
    constexpr uint32_t bg        = rgb (13, 14, 24);
    constexpr uint32_t panel     = rgb (22, 24, 40);
    constexpr uint32_t panelHi   = rgb (33, 36, 62);
    constexpr uint32_t border    = rgb (50, 54, 88);
    constexpr uint32_t text      = rgb (232, 233, 246);
    constexpr uint32_t textDim   = rgb (134, 139, 170);
    constexpr uint32_t textFaint = rgb (84, 88, 120);
    constexpr uint32_t brand     = rgb (99, 102, 241);   // --f-brand
    constexpr uint32_t brandHi   = rgb (129, 140, 248);  // --f-brand-h
    constexpr uint32_t brandDeep = rgb (79, 70, 229);    // --f-brand-a
    constexpr uint32_t ok        = rgb (34, 197, 94);    // MIDI "connected" green used by the web app
    constexpr uint32_t warn      = rgb (202, 80, 16);    // --f-warning
    constexpr uint32_t danger    = rgb (239, 68, 68);

    // The two handles: left stick = START, right stick = END.
    constexpr uint32_t startCol  = rgb (45, 212, 191);
    constexpr uint32_t endCol    = rgb (251, 146, 60);

    constexpr uint32_t waveDim   = rgb (88, 92, 190);
    constexpr uint32_t waveHot   = rgb (214, 220, 255);

    // Slice colours, in the order kerf.html uses them (state.colors).
    inline uint32_t sliceColour (int index) noexcept
    {
        static const uint32_t c[8] = {
            rgb (255, 0, 110), rgb (0, 245, 255), rgb (255, 190, 11), rgb (131, 56, 236),
            rgb (251, 86, 7),  rgb (58, 134, 255), rgb (6, 255, 165), rgb (255, 0, 110) };
        return c[((index % 8) + 8) % 8];
    }
}
