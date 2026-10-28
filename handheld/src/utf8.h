#pragma once

#include <cstdint>
#include <string>

namespace kerf::ui
{
    // Reads the code point starting at s[i] and advances i past it. Malformed input never
    // loops or reads out of range: it yields U+FFFD and consumes one byte.
    inline uint32_t nextCodePoint (const std::string& s, size_t& i) noexcept
    {
        const auto n = s.size();
        const auto b0 = (unsigned char) s[i];

        if (b0 < 0x80) { ++i; return b0; }

        auto cont = [&] (size_t k) -> int
        {
            return (i + k < n && ((unsigned char) s[i + k] & 0xC0) == 0x80) ? ((unsigned char) s[i + k] & 0x3F) : -1;
        };

        if ((b0 & 0xE0) == 0xC0)
        {
            const auto c1 = cont (1);
            if (c1 >= 0) { i += 2; return ((uint32_t) (b0 & 0x1F) << 6) | (uint32_t) c1; }
        }
        else if ((b0 & 0xF0) == 0xE0)
        {
            const auto c1 = cont (1), c2 = cont (2);
            if (c1 >= 0 && c2 >= 0) { i += 3; return ((uint32_t) (b0 & 0x0F) << 12) | ((uint32_t) c1 << 6) | (uint32_t) c2; }
        }
        else if ((b0 & 0xF8) == 0xF0)
        {
            const auto c1 = cont (1), c2 = cont (2), c3 = cont (3);
            if (c1 >= 0 && c2 >= 0 && c3 >= 0)
            {
                i += 4;
                return ((uint32_t) (b0 & 0x07) << 18) | ((uint32_t) c1 << 12) | ((uint32_t) c2 << 6) | (uint32_t) c3;
            }
        }

        ++i;
        return 0xFFFD;
    }

    inline void appendUtf8 (std::string& out, uint32_t cp)
    {
        if (cp < 0x80) out.push_back ((char) cp);
        else if (cp < 0x800) { out.push_back ((char) (0xC0 | (cp >> 6))); out.push_back ((char) (0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { out.push_back ((char) (0xE0 | (cp >> 12))); out.push_back ((char) (0x80 | ((cp >> 6) & 0x3F))); out.push_back ((char) (0x80 | (cp & 0x3F))); }
        else { out.push_back ((char) (0xF0 | (cp >> 18))); out.push_back ((char) (0x80 | ((cp >> 12) & 0x3F))); out.push_back ((char) (0x80 | ((cp >> 6) & 0x3F))); out.push_back ((char) (0x80 | (cp & 0x3F))); }
    }

    // Number of characters (code points) the font will draw for this string.
    inline int countGlyphs (const std::string& s) noexcept
    {
        int n = 0;
        for (size_t i = 0; i < s.size();) { nextCodePoint (s, i); ++n; }
        return n;
    }

    // Shortens `s` to at most `maxGlyphs` characters, ending in "..." when it had to cut.
    inline std::string elide (const std::string& s, int maxGlyphs)
    {
        if (maxGlyphs <= 0) return {};
        if (countGlyphs (s) <= maxGlyphs) return s;
        if (maxGlyphs <= 3) return std::string ((size_t) maxGlyphs, '.');

        std::string out;
        size_t i = 0;
        for (int n = 0; n < maxGlyphs - 3 && i < s.size(); ++n)
        {
            const auto start = i;
            nextCodePoint (s, i);
            out.append (s, start, i - start);
        }
        return out + "...";
    }
}
