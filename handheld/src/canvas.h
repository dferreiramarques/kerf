#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace kerf::ui
{
    // Colours are packed 0xAARRGGBB, the layout of SDL's ARGB8888 texture format.
    constexpr uint32_t argb (int a, int r, int g, int b) noexcept
    {
        return ((uint32_t) (a & 255) << 24) | ((uint32_t) (r & 255) << 16) | ((uint32_t) (g & 255) << 8) | (uint32_t) (b & 255);
    }

    constexpr uint32_t rgb (int r, int g, int b) noexcept { return argb (255, r, g, b); }

    constexpr uint32_t withAlpha (uint32_t colour, int a) noexcept
    {
        return ((uint32_t) (a & 255) << 24) | (colour & 0x00FFFFFFu);
    }

    // A small software drawing surface. The whole UI is drawn here and the host just uploads the
    // pixels, which keeps the look identical on every platform and lets the tests render and
    // inspect screens without a window.
    class Canvas
    {
    public:
        Canvas (int width, int height);

        int width() const noexcept { return w_; }
        int height() const noexcept { return h_; }
        uint32_t* pixels() noexcept { return px_.data(); }
        const uint32_t* pixels() const noexcept { return px_.data(); }
        int pitchBytes() const noexcept { return w_ * 4; }

        uint32_t pixelAt (int x, int y) const noexcept;

        void clear (uint32_t colour);

        // Everything below is clipped to the clip rectangle (default: the whole canvas).
        void setClip (int x, int y, int w, int h);
        void resetClip();

        void plot (int x, int y, uint32_t colour);
        void fillRect (int x, int y, int w, int h, uint32_t colour);
        void strokeRect (int x, int y, int w, int h, uint32_t colour);
        void hline (int x, int y, int length, uint32_t colour);
        void vline (int x, int y, int length, uint32_t colour);
        void line (int x0, int y0, int x1, int y1, uint32_t colour);
        void fillTriangle (int x0, int y0, int x1, int y1, int x2, int y2, uint32_t colour);

        // Text uses a monospaced 8x8 bitmap font (ASCII + Latin-1, so Portuguese file names
        // render properly), enlarged by an integer scale. Strings are UTF-8. Returns the x
        // position just after the last glyph.
        int drawText (int x, int y, const std::string& utf8, uint32_t colour, int scale = 1);
        static int textWidth (const std::string& utf8, int scale = 1);
        static int glyphCount (const std::string& utf8);

        // Writes the canvas as an RGB PNG file. Returns false if the file could not be written.
        bool savePng (const std::string& utf8Path) const;

    private:
        void blend (int x, int y, uint32_t colour) noexcept;

        int w_, h_;
        int clipX0_ = 0, clipY0_ = 0, clipX1_, clipY1_;
        std::vector<uint32_t> px_;
    };
}
