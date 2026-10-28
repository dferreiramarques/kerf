#include "canvas.h"

#include <algorithm>
#include <cstdlib>

#include "png_writer.h"
#include "utf8.h"

// The two font tables are public domain (see third_party/README.md). They define plain global
// arrays, so they are included exactly once, in this file, with internal linkage.
namespace
{
#include "font8x8_basic.h"
#include "font8x8_ext_latin.h"

    const unsigned char* glyphFor (uint32_t cp) noexcept
    {
        if (cp < 0x80)
            return (const unsigned char*) font8x8_basic[cp];

        if (cp >= 0xA0 && cp <= 0xFF)
            return (const unsigned char*) font8x8_ext_latin[cp - 0xA0];

        return (const unsigned char*) font8x8_basic[(int) '?'];
    }
}

namespace kerf::ui
{
    Canvas::Canvas (int width, int height)
        : w_ (std::max (1, width)), h_ (std::max (1, height)), clipX1_ (w_), clipY1_ (h_),
          px_ ((size_t) w_ * (size_t) h_, 0xFF000000u)
    {
    }

    uint32_t Canvas::pixelAt (int x, int y) const noexcept
    {
        if (x < 0 || y < 0 || x >= w_ || y >= h_)
            return 0;

        return px_[(size_t) y * (size_t) w_ + (size_t) x];
    }

    void Canvas::clear (uint32_t colour)
    {
        std::fill (px_.begin(), px_.end(), colour | 0xFF000000u);
    }

    void Canvas::setClip (int x, int y, int w, int h)
    {
        clipX0_ = std::clamp (x, 0, w_);
        clipY0_ = std::clamp (y, 0, h_);
        clipX1_ = std::clamp (x + w, clipX0_, w_);
        clipY1_ = std::clamp (y + h, clipY0_, h_);
    }

    void Canvas::resetClip()
    {
        clipX0_ = 0; clipY0_ = 0; clipX1_ = w_; clipY1_ = h_;
    }

    void Canvas::blend (int x, int y, uint32_t c) noexcept
    {
        auto& dst = px_[(size_t) y * (size_t) w_ + (size_t) x];
        const uint32_t a = c >> 24;

        if (a == 255) { dst = c; return; }
        if (a == 0) return;

        const uint32_t ia = 255 - a;
        const uint32_t r = (((c >> 16) & 255) * a + ((dst >> 16) & 255) * ia + 127) / 255;
        const uint32_t g = (((c >> 8) & 255) * a + ((dst >> 8) & 255) * ia + 127) / 255;
        const uint32_t b = ((c & 255) * a + (dst & 255) * ia + 127) / 255;
        dst = 0xFF000000u | (r << 16) | (g << 8) | b;
    }

    void Canvas::plot (int x, int y, uint32_t colour)
    {
        if (x < clipX0_ || y < clipY0_ || x >= clipX1_ || y >= clipY1_)
            return;

        blend (x, y, colour);
    }

    void Canvas::fillRect (int x, int y, int w, int h, uint32_t colour)
    {
        const int x0 = std::max (x, clipX0_), y0 = std::max (y, clipY0_);
        const int x1 = std::min (x + w, clipX1_), y1 = std::min (y + h, clipY1_);

        if (x0 >= x1 || y0 >= y1)
            return;

        if ((colour >> 24) == 255)
        {
            for (int yy = y0; yy < y1; ++yy)
                std::fill (px_.begin() + (std::ptrdiff_t) ((size_t) yy * (size_t) w_ + (size_t) x0),
                           px_.begin() + (std::ptrdiff_t) ((size_t) yy * (size_t) w_ + (size_t) x1), colour);
            return;
        }

        for (int yy = y0; yy < y1; ++yy)
            for (int xx = x0; xx < x1; ++xx)
                blend (xx, yy, colour);
    }

    void Canvas::strokeRect (int x, int y, int w, int h, uint32_t colour)
    {
        if (w <= 0 || h <= 0)
            return;

        hline (x, y, w, colour);
        hline (x, y + h - 1, w, colour);
        vline (x, y + 1, h - 2, colour);
        vline (x + w - 1, y + 1, h - 2, colour);
    }

    void Canvas::hline (int x, int y, int length, uint32_t colour) { fillRect (x, y, length, 1, colour); }
    void Canvas::vline (int x, int y, int length, uint32_t colour) { fillRect (x, y, 1, length, colour); }

    void Canvas::line (int x0, int y0, int x1, int y1, uint32_t colour)
    {
        const int dx = std::abs (x1 - x0), dy = -std::abs (y1 - y0);
        const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
        int err = dx + dy;

        for (;;)
        {
            plot (x0, y0, colour);

            if (x0 == x1 && y0 == y1)
                break;

            const int e2 = 2 * err;
            if (e2 >= dy) { err += dy; x0 += sx; }
            if (e2 <= dx) { err += dx; y0 += sy; }
        }
    }

    void Canvas::fillTriangle (int x0, int y0, int x1, int y1, int x2, int y2, uint32_t colour)
    {
        const int minY = std::min ({ y0, y1, y2 }), maxY = std::max ({ y0, y1, y2 });
        const int minX = std::min ({ x0, x1, x2 }), maxX = std::max ({ x0, x1, x2 });

        auto edge = [] (int ax, int ay, int bx, int by, int px, int py)
        {
            return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
        };

        const int area = edge (x0, y0, x1, y1, x2, y2);
        if (area == 0)
            return;

        for (int y = minY; y <= maxY; ++y)
        {
            for (int x = minX; x <= maxX; ++x)
            {
                int w0 = edge (x1, y1, x2, y2, x, y);
                int w1 = edge (x2, y2, x0, y0, x, y);
                int w2 = edge (x0, y0, x1, y1, x, y);

                if (area < 0) { w0 = -w0; w1 = -w1; w2 = -w2; }
                if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                    plot (x, y, colour);
            }
        }
    }

    int Canvas::drawText (int x, int y, const std::string& utf8, uint32_t colour, int scale)
    {
        scale = std::max (1, scale);

        for (size_t i = 0; i < utf8.size();)
        {
            const auto cp = nextCodePoint (utf8, i);

            // Skip glyphs that are entirely outside the clip rectangle.
            if (x + 8 * scale > clipX0_ && x < clipX1_ && y + 8 * scale > clipY0_ && y < clipY1_)
            {
                const auto* rows = glyphFor (cp);

                for (int row = 0; row < 8; ++row)
                {
                    const unsigned bits = rows[row];
                    if (bits == 0)
                        continue;

                    for (int col = 0; col < 8; ++col)
                        if (bits & (1u << col)) // bit 0 is the left-most pixel
                            fillRect (x + col * scale, y + row * scale, scale, scale, colour);
                }
            }

            x += 8 * scale;
        }

        return x;
    }

    int Canvas::textWidth (const std::string& utf8, int scale)
    {
        return countGlyphs (utf8) * 8 * std::max (1, scale);
    }

    int Canvas::glyphCount (const std::string& utf8) { return countGlyphs (utf8); }

    bool Canvas::savePng (const std::string& utf8Path) const
    {
        std::vector<uint8_t> rgbData ((size_t) w_ * (size_t) h_ * 3);

        for (size_t i = 0; i < px_.size(); ++i)
        {
            rgbData[i * 3 + 0] = (uint8_t) (px_[i] >> 16);
            rgbData[i * 3 + 1] = (uint8_t) (px_[i] >> 8);
            rgbData[i * 3 + 2] = (uint8_t) px_[i];
        }

        return writePngRgb (utf8Path, rgbData.data(), w_, h_);
    }
}
