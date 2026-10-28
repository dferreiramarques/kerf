#pragma once

#include <string>
#include <vector>

#include "canvas.h"
#include "theme.h"

namespace kerf::ui
{
    // Small drawing helpers shared by the pages (the slicer in app_draw.cpp, the sequencer in
    // app_sequencer.cpp, the Record page in app_record.cpp), so all look like the same program.

    inline int textRightX (int right, const std::string& s, int scale) { return right - Canvas::textWidth (s, scale); }

    inline void drawPanel (Canvas& c, int x, int y, int w, int h, uint32_t fill = theme::panel, uint32_t edge = theme::border)
    {
        c.fillRect (x, y, w, h, fill);
        c.strokeRect (x, y, w, h, edge);
    }

    // A page tab in the header: the page you are on is lit. Select flips between the two of a pair.
    inline void drawPill (Canvas& c, int x, int y, int w, int h, const char* label, bool active)
    {
        c.fillRect (x, y, w, h, active ? theme::brand : theme::bg);
        c.strokeRect (x, y, w, h, active ? theme::brandHi : theme::border);
        c.drawText (x + 3, y + (h - 8) / 2, label, active ? rgb (255, 255, 255) : theme::textDim, 1);
    }

    // Filled left/right pointing triangle used as an "adjust me" arrow.
    inline void arrowLeft (Canvas& c, int x, int y, uint32_t col)  { c.fillTriangle (x + 5, y, x + 5, y + 10, x, y + 5, col); }
    inline void arrowRight (Canvas& c, int x, int y, uint32_t col) { c.fillTriangle (x, y, x, y + 10, x + 5, y + 5, col); }

    // A piece of text with its own colour: a hint line is a row of these.
    struct Seg { std::string text; uint32_t colour; };

    inline int drawSegments (Canvas& c, int x, int y, const std::vector<Seg>& segs, int scale = 1)
    {
        for (const auto& s : segs)
            x = c.drawText (x, y, s.text, s.colour, scale);

        return x;
    }
}
