#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace kerf::ui
{
    // Small text-formatting helpers for the UI. ASCII only, locale independent.

    inline std::string fixed (double v, int decimals)
    {
        char buf[64];
        std::snprintf (buf, sizeof buf, "%.*f", decimals, v);
        return buf;
    }

    inline std::string integer (long long v)
    {
        char buf[32];
        std::snprintf (buf, sizeof buf, "%lld", v);
        return buf;
    }

    // Same naming as kerf.html's getNoteName(): MIDI 60 is C4.
    inline std::string noteName (int midi)
    {
        static const char* const names[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
        if (midi < 0 || midi > 127)
            return "--";

        return std::string (names[midi % 12]) + integer (midi / 12 - 1);
    }

    // A MIDI message packed as status << 16 | data1 << 8 | data2, for the "Input test" screen:
    // "90 3C 64  note on C4 vel 100  ch 1". 0 (nothing received yet) gives "-".
    inline std::string midiMessageText (uint32_t packed)
    {
        if (packed == 0)
            return "-";

        const int status = (int) ((packed >> 16) & 0xFF);
        const int d1 = (int) ((packed >> 8) & 0x7F);
        const int d2 = (int) (packed & 0x7F);
        const int kind = status & 0xF0;
        char raw[24];

        if (kind == 0xC0 || kind == 0xD0)
            std::snprintf (raw, sizeof raw, "%02X %02X", status, d1);
        else
            std::snprintf (raw, sizeof raw, "%02X %02X %02X", status, d1, d2);

        std::string what;

        switch (kind)
        {
            case 0x80: what = "note off " + noteName (d1); break;
            case 0x90: what = d2 == 0 ? "note off " + noteName (d1) : "note on " + noteName (d1) + " vel " + integer (d2); break;
            case 0xA0: what = "key pressure " + noteName (d1); break;
            case 0xB0: what = "controller " + integer (d1) + " = " + integer (d2); break;
            case 0xC0: what = "program " + integer (d1); break;
            case 0xD0: what = "channel pressure " + integer (d1); break;
            case 0xE0: what = "pitch bend " + integer ((d2 << 7 | d1) - 8192); break;
            default: what = "?"; break;
        }

        return std::string (raw) + "  " + what + "  ch " + integer ((status & 15) + 1);
    }

    // 0:12.3 / 1:02.3 (minutes only when needed). The time is rounded to a tenth first, so that
    // 59.96 s reads "1:00.0" and not "60.0s", and 119.97 s reads "2:00.0" and not "1:60.0".
    inline std::string clockTime (double seconds)
    {
        if (! (seconds > 0.0))
            seconds = 0.0;

        if (seconds > 1.0e9)
            seconds = 1.0e9;

        const long long tenths = std::llround (seconds * 10.0);
        const long long minutes = tenths / 600;
        const long long rest = tenths % 600;
        char buf[48];

        if (minutes > 0)
            std::snprintf (buf, sizeof buf, "%lld:%02lld.%lld", minutes, rest / 10, rest % 10);
        else
            std::snprintf (buf, sizeof buf, "%lld.%llds", rest / 10, rest % 10);

        return buf;
    }

    // Slice position/length: "0.532" (seconds, three decimals)
    inline std::string seconds3 (double s) { return fixed (s, 3); }

    inline std::string panText (float pan)
    {
        const int p = (int) std::lround (pan * 100.0f);
        if (p == 0) return "C";
        return (p < 0 ? "L" : "R") + integer (std::abs (p));
    }

    inline std::string semitoneText (int st)
    {
        if (st == 0) return "0 st";
        return (st > 0 ? "+" : "") + integer (st) + " st";
    }

    inline std::string sizeText (uintmax_t bytes)
    {
        if (bytes < 1024) return integer ((long long) bytes) + " B";
        if (bytes < 1024ull * 1024ull) return fixed ((double) bytes / 1024.0, 0) + " KB";
        return fixed ((double) bytes / (1024.0 * 1024.0), 1) + " MB";
    }

    // A time limit as the Settings screen shows it: "30 s", "90 s", "3 min".
    inline std::string limitText (int seconds)
    {
        if (seconds >= 60 && seconds % 60 == 0)
            return integer (seconds / 60) + " min";

        return integer (seconds) + " s";
    }

    inline std::string rateText (double hz)
    {
        if (std::fabs (hz - std::round (hz / 1000.0) * 1000.0) < 0.5)
            return fixed (hz / 1000.0, 0) + "k";

        return fixed (hz / 1000.0, 1) + "k";
    }

    // Keeps the END of a long path: "...Studio\Music\Loops"
    inline std::string elideLeftAscii (const std::string& s, int maxChars)
    {
        if ((int) s.size() <= maxChars || maxChars <= 3)
            return s;

        size_t start = s.size() - (size_t) (maxChars - 3);

        // Do not start in the middle of a UTF-8 sequence.
        while (start < s.size() && ((unsigned char) s[start] & 0xC0) == 0x80)
            ++start;

        return "..." + s.substr (start);
    }
}
