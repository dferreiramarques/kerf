#include "project.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "engine.h"
#include "sample.h"

// The file format, version 1. Plain text, one fact per line, 'name = value'; blank lines and lines
// starting with '#' are ignored. Written by serializeProject() in exactly this order:
//
//   kerf_project = 1                       the format; has to be the first line
//   audio = rec-001.wav                    the sample it belongs to (for people; not checked)
//   frames = 441000                        its length in frames ...
//   rate = 44100                           ... and its sample rate: these two are checked
//   tempo = 120
//   swing = 0.5
//   length = 16                            pattern length, in steps
//   scene = 1                              the scene being edited, 1 to 8
//   selected = 3                           the selected slice, counting from 1 (0 = none)
//   slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode=shot fade_in=1 fade_out=50 enabled=1
//   pattern = 1 3 X...X...X...X...         scene 1 to 8, row (= slice) 1 to 128, 16 steps: X lit, . off
//   end = 12 5                             how many slice and pattern lines the file holds
//
// 'slice' lines come in the order of the slices; 'pattern' lines only for rows with a step lit.
// The last line is there to tell a file that was cut short from a complete one.
//
// Reading is strict about the shape of the file (an unknown name, a missing field, a repeated one,
// a number that is not a number, a missing end line are all "damaged") and forgiving about the
// size of numbers (they are brought into range by Project::sanitize), because a number out of range
// is harmless and a file that does not make sense is not to be half-trusted. A first line that names
// a newer format version is reported as such, and nothing else of the file is looked at.
//
// Numbers are written the shortest way that reads back as the same float, with '.' for the
// decimal point (the program runs in the "C" locale).

namespace kerf
{
    namespace
    {
        constexpr float kMinPitch = 0.25f, kMaxPitch = 4.0f;       // -24 to +24 semitones
        constexpr float kMaxFadeInMs = 100.0f, kMaxFadeOutMs = 200.0f;
        constexpr std::size_t kMaxAudioName = 1024;

        // ---------------------------------------------------------------------------- text

        bool isBlank (char c) noexcept { return c == ' ' || c == '\t' || c == '\r'; }

        std::string trimmed (const std::string& s)
        {
            std::size_t b = 0, e = s.size();

            while (b < e && isBlank (s[b])) ++b;
            while (e > b && isBlank (s[e - 1])) --e;

            return s.substr (b, e - b);
        }

        std::vector<std::string> words (const std::string& s)
        {
            std::vector<std::string> out;
            std::size_t i = 0;

            while (i < s.size())
            {
                while (i < s.size() && isBlank (s[i])) ++i;

                std::size_t j = i;
                while (j < s.size() && ! isBlank (s[j])) ++j;

                if (j > i)
                    out.push_back (s.substr (i, j - i));

                i = j;
            }

            return out;
        }

        // The shortest text that reads back as exactly this float.
        std::string formatFloat (float v)
        {
            if (v == 0.0f)
                v = 0.0f;               // never "-0"

            char text[64];

            for (int decimals = 0; decimals <= 17; ++decimals)
            {
                std::snprintf (text, sizeof text, "%.*f", decimals, (double) v);

                if (std::strtof (text, nullptr) == v)
                    return text;
            }

            std::snprintf (text, sizeof text, "%.9g", (double) v);   // so small that no plain decimal fits
            return text;
        }

        bool parseFloat (const std::string& token, float& out)
        {
            if (token.empty() || token.size() > 40)
                return false;

            // Digits, a point, a sign and an exponent: not "nan", "inf" or a hexadecimal float.
            for (const char ch : token)
                if (! ((ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' || ch == 'e' || ch == 'E'))
                    return false;

            char* end = nullptr;
            const float v = std::strtof (token.c_str(), &end);

            // All of the token has to be the number (it is not empty, so that means one was read).
            if (*end != '\0' || ! std::isfinite (v))
                return false;

            out = v;
            return true;
        }

        bool parseInt (const std::string& token, std::int64_t& out)
        {
            if (token.size() > 18)                  // up to 18 characters always fit
                return false;

            const bool negative = token.starts_with ('-');
            std::size_t i = negative ? 1 : 0;

            if (i >= token.size())                  // nothing at all, or a minus sign alone
                return false;

            std::int64_t v = 0;

            for (; i < token.size(); ++i)
            {
                if (token[i] < '0' || token[i] > '9')
                    return false;

                v = v * 10 + (token[i] - '0');
            }

            out = negative ? -v : v;
            return true;
        }

        // A number from a file that has to fit an int, whatever it says.
        int toInt (std::int64_t v) noexcept
        {
            return (int) std::clamp<std::int64_t> (v, -1000000, 1000000);
        }

        // A file name for the 'audio' line: one line, no control characters.
        std::string oneLine (const std::string& name)
        {
            std::string out = name.substr (0, kMaxAudioName);

            for (auto& ch : out)
                if ((unsigned char) ch < 0x20 || (unsigned char) ch == 0x7F)
                    ch = '?';

            return out;
        }

        bool finite (const Slice& s) noexcept
        {
            return std::isfinite (s.start) && std::isfinite (s.end) && std::isfinite (s.pitch) && std::isfinite (s.volume)
                   && std::isfinite (s.pan) && std::isfinite (s.fadeInMs) && std::isfinite (s.fadeOutMs);
        }

        bool sameSlice (const Slice& a, const Slice& b) noexcept
        {
            return a.start == b.start && a.end == b.end && a.pitch == b.pitch && a.volume == b.volume && a.pan == b.pan
                   && a.enabled == b.enabled && a.mode == b.mode && a.fadeInMs == b.fadeInMs && a.fadeOutMs == b.fadeOutMs;
        }

        const char* const kSliceFields[9] = { "start", "end", "pitch", "volume", "pan", "mode", "fade_in", "fade_out", "enabled" };
    }

    // ------------------------------------------------------------------------------ Project

    bool Project::belongsTo (const Sample& sample) const noexcept
    {
        return frames == (std::int64_t) sample.data.numFrames() && rate == (std::int64_t) std::llround (sample.sampleRate);
    }

    int Project::patternCount() const noexcept
    {
        const int rows = std::min ((int) slices.size(), StepSequencer::kRows);
        int n = 0;

        for (int s = 0; s < StepSequencer::kScenes; ++s)
            for (int r = 0; r < rows; ++r)
                if (grid.bits[s][r] != 0)
                    ++n;

        return n;
    }

    void Project::sanitize (double duration) noexcept
    {
        if (! std::isfinite (duration) || duration < 0.0)
            duration = 0.0;

        const auto fix = [] (float v, float lo, float hi, float fallback) { return std::isfinite (v) ? std::clamp (v, lo, hi) : fallback; };

        for (auto& s : slices)
        {
            const double start = std::clamp (std::isfinite (s.start) ? (double) s.start : 0.0, 0.0, duration);
            const double end = std::clamp (std::isfinite (s.end) ? (double) s.end : duration, start, duration);

            s.start = (float) start;
            s.end = (float) end;
            s.pitch = fix (s.pitch, kMinPitch, kMaxPitch, 1.0f);
            s.volume = fix (s.volume, 0.0f, 1.0f, 1.0f);
            s.pan = fix (s.pan, -1.0f, 1.0f, 0.0f);
            s.fadeInMs = fix (s.fadeInMs, 0.0f, kMaxFadeInMs, 1.0f);
            s.fadeOutMs = fix (s.fadeOutMs, 0.0f, kMaxFadeOutMs, 50.0f);
        }

        tempo = fix (tempo, StepSequencer::kMinTempo, StepSequencer::kMaxTempo, StepSequencer::kDefaultTempo);
        swing = fix (swing, StepSequencer::kMinSwing, StepSequencer::kMaxSwing, StepSequencer::kMinSwing);
        length = std::clamp (length, 1, StepSequencer::kSteps);
        scene = std::clamp (scene, 0, StepSequencer::kScenes - 1);
        selected = slices.empty() ? 0 : std::clamp (selected, 0, (int) slices.size() - 1);

        // A pattern row belongs to a slice: where there is none, there is no pattern.
        for (int s = 0; s < StepSequencer::kScenes; ++s)
            for (int r = std::min ((int) slices.size(), StepSequencer::kRows); r < StepSequencer::kRows; ++r)
                grid.bits[s][r] = 0;
    }

    bool sameWork (const Project& a, const Project& b) noexcept
    {
        if (a.slices.size() != b.slices.size())
            return false;

        for (std::size_t i = 0; i < a.slices.size(); ++i)
            if (! sameSlice (a.slices[i], b.slices[i]))
                return false;

        return a.tempo == b.tempo && a.swing == b.swing && a.length == b.length
               && std::memcmp (&a.grid, &b.grid, sizeof a.grid) == 0;
    }

    // ---------------------------------------------------------------------------- writing

    bool serializeProject (const Project& p, std::string& text)
    {
        text.clear();

        if ((int) p.slices.size() > Project::kMaxSlices || ! std::isfinite (p.tempo) || ! std::isfinite (p.swing))
            return false;

        for (const auto& s : p.slices)
            if (! finite (s))
                return false;

        std::string t;
        t.reserve (512 + p.slices.size() * 112);

        t += "# Kerf project: the slices and patterns made from one sample. Kerf saves it next to the sample\n";
        t += "# (its name is the sample's name plus .kerf) and reads it again when that sample is loaded.\n";
        t += "kerf_project = " + std::to_string (Project::kFormatVersion) + "\n";
        t += "audio = " + oneLine (p.audio) + "\n";
        t += "frames = " + std::to_string (p.frames) + "\n";
        t += "rate = " + std::to_string (p.rate) + "\n";
        t += "tempo = " + formatFloat (p.tempo) + "\n";
        t += "swing = " + formatFloat (p.swing) + "\n";
        t += "length = " + std::to_string (p.length) + "\n";
        t += "scene = " + std::to_string (p.scene + 1) + "\n";
        t += "selected = " + std::to_string (p.slices.empty() ? 0 : p.selected + 1) + "\n";

        for (const auto& s : p.slices)
        {
            t += "slice = start=" + formatFloat (s.start) + " end=" + formatFloat (s.end) + " pitch=" + formatFloat (s.pitch)
                 + " volume=" + formatFloat (s.volume) + " pan=" + formatFloat (s.pan)
                 + " mode=" + (s.mode == Slice::Mode::loop ? "loop" : "shot")
                 + " fade_in=" + formatFloat (s.fadeInMs) + " fade_out=" + formatFloat (s.fadeOutMs)
                 + " enabled=" + (s.enabled ? "1" : "0") + "\n";
        }

        const int rows = std::min ((int) p.slices.size(), StepSequencer::kRows);
        int patterns = 0;

        for (int scene = 0; scene < StepSequencer::kScenes; ++scene)
        {
            for (int row = 0; row < rows; ++row)
            {
                const unsigned bits = p.grid.bits[scene][row];

                if (bits == 0)
                    continue;

                std::string steps (StepSequencer::kSteps, '.');

                for (int step = 0; step < StepSequencer::kSteps; ++step)
                    if ((bits >> step) & 1u)
                        steps[(std::size_t) step] = 'X';

                t += "pattern = " + std::to_string (scene + 1) + " " + std::to_string (row + 1) + " " + steps + "\n";
                ++patterns;
            }
        }

        t += "end = " + std::to_string (p.slices.size()) + " " + std::to_string (patterns) + "\n";

        text = std::move (t);
        return true;
    }

    // ---------------------------------------------------------------------------- reading

    ProjectParse parseProject (const std::string& text)
    {
        ProjectParse out;

        const auto fail = [&out] (int line, const std::string& why)
        {
            out.status = ProjectParse::Status::damaged;
            out.error = line > 0 ? "line " + std::to_string (line) + ": " + why : why;
            out.project = Project {};
            return out;
        };

        if (text.size() > Project::kMaxFileBytes)
            return fail (0, "the file is too big");

        Project& p = out.project;

        enum Once { oAudio, oFrames, oRate, oTempo, oSwing, oLength, oScene, oSelected, oEnd, oCount };
        bool seen[oCount] = {};
        bool patternSeen[StepSequencer::kScenes][StepSequencer::kRows] = {};
        bool haveVersion = false;
        std::int64_t declaredSlices = 0, declaredPatterns = 0;
        int patternLines = 0, highestRow = 0;

        // A byte order mark (an editor on Windows may add one) is not part of the first line.
        std::size_t at = text.compare (0, 3, "\xEF\xBB\xBF") == 0 ? 3 : 0;
        int lineNo = 0;

        while (at < text.size())
        {
            const std::size_t newline = text.find ('\n', at);
            const std::string line = trimmed (text.substr (at, newline == std::string::npos ? std::string::npos : newline - at));
            at = newline == std::string::npos ? text.size() : newline + 1;
            ++lineNo;

            if (line.empty() || line[0] == '#')
                continue;

            if (seen[oEnd])
                return fail (lineNo, "there is more after the end line");

            const std::size_t equals = line.find ('=');

            if (equals == std::string::npos)
                return fail (lineNo, "expected 'name = value'");

            const std::string key = trimmed (line.substr (0, equals));
            const std::string value = trimmed (line.substr (equals + 1));

            // The first thing in the file is its version. A newer one may be written in a way this
            // Kerf cannot read, so nothing else is looked at.
            if (! haveVersion)
            {
                std::int64_t version = 0;

                if (key != "kerf_project")
                    return fail (lineNo, "this is not a Kerf project");

                if (! parseInt (value, version) || version < 1)
                    return fail (lineNo, "the project version is not a number");

                out.version = toInt (version);

                if (version > Project::kFormatVersion)
                {
                    out.status = ProjectParse::Status::newer;
                    out.error = "made by a newer Kerf (project version " + std::to_string (version) + ")";
                    out.project = Project {};
                    return out;
                }

                haveVersion = true;
                continue;
            }

            // One-off lines: each once.
            const auto once = [&] (Once which) { const bool again = seen[which]; seen[which] = true; return ! again; };
            std::int64_t number = 0;

            if (key == "audio")
            {
                if (! once (oAudio)) return fail (lineNo, "'audio' twice");
                p.audio = value.substr (0, kMaxAudioName);
            }
            else if (key == "frames")
            {
                if (! once (oFrames)) return fail (lineNo, "'frames' twice");
                if (! parseInt (value, number) || number < 0) return fail (lineNo, "'frames' is not a number");
                p.frames = number;
            }
            else if (key == "rate")
            {
                if (! once (oRate)) return fail (lineNo, "'rate' twice");
                if (! parseInt (value, number) || number < 0) return fail (lineNo, "'rate' is not a number");
                p.rate = number;
            }
            else if (key == "tempo")
            {
                if (! once (oTempo)) return fail (lineNo, "'tempo' twice");
                if (! parseFloat (value, p.tempo)) return fail (lineNo, "'tempo' is not a number");
            }
            else if (key == "swing")
            {
                if (! once (oSwing)) return fail (lineNo, "'swing' twice");
                if (! parseFloat (value, p.swing)) return fail (lineNo, "'swing' is not a number");
            }
            else if (key == "length")
            {
                if (! once (oLength)) return fail (lineNo, "'length' twice");
                if (! parseInt (value, number)) return fail (lineNo, "'length' is not a number");
                p.length = toInt (number);
            }
            else if (key == "scene")
            {
                if (! once (oScene)) return fail (lineNo, "'scene' twice");
                if (! parseInt (value, number)) return fail (lineNo, "'scene' is not a number");
                p.scene = std::max (0, toInt (number) - 1);
            }
            else if (key == "selected")
            {
                if (! once (oSelected)) return fail (lineNo, "'selected' twice");
                if (! parseInt (value, number)) return fail (lineNo, "'selected' is not a number");
                p.selected = std::max (0, toInt (number) - 1);
            }
            else if (key == "slice")
            {
                if ((int) p.slices.size() >= Project::kMaxSlices)
                    return fail (lineNo, "too many slices");

                Slice s;
                bool got[9] = {};

                for (const auto& token : words (value))
                {
                    const std::size_t eq = token.find ('=');

                    if (eq == std::string::npos)
                        return fail (lineNo, "expected name=value in a slice");

                    const std::string name = token.substr (0, eq), fieldValue = token.substr (eq + 1);
                    int field = -1;

                    for (int i = 0; i < 9; ++i)
                        if (name == kSliceFields[i])
                            field = i;

                    if (field < 0)
                        return fail (lineNo, "unknown slice field '" + name + "'");

                    if (got[field])
                        return fail (lineNo, "slice field '" + name + "' twice");

                    got[field] = true;
                    float* numeric = nullptr;

                    switch (field)
                    {
                        case 0: numeric = &s.start; break;
                        case 1: numeric = &s.end; break;
                        case 2: numeric = &s.pitch; break;
                        case 3: numeric = &s.volume; break;
                        case 4: numeric = &s.pan; break;
                        case 6: numeric = &s.fadeInMs; break;
                        case 7: numeric = &s.fadeOutMs; break;
                        default: break;
                    }

                    if (numeric != nullptr)
                    {
                        if (! parseFloat (fieldValue, *numeric))
                            return fail (lineNo, "'" + name + "' is not a number");
                    }
                    else if (field == 5)
                    {
                        if (fieldValue == "shot")      s.mode = Slice::Mode::shot;
                        else if (fieldValue == "loop") s.mode = Slice::Mode::loop;
                        else return fail (lineNo, "'mode' is neither shot nor loop");
                    }
                    else
                    {
                        if (fieldValue == "1")      s.enabled = true;
                        else if (fieldValue == "0") s.enabled = false;
                        else return fail (lineNo, "'enabled' is neither 1 nor 0");
                    }
                }

                for (int i = 0; i < 9; ++i)
                    if (! got[i])
                        return fail (lineNo, std::string ("a slice is missing '") + kSliceFields[i] + "'");

                p.slices.push_back (s);
            }
            else if (key == "pattern")
            {
                const auto parts = words (value);
                std::int64_t scene = 0, row = 0;

                if (parts.size() != 3 || ! parseInt (parts[0], scene) || ! parseInt (parts[1], row))
                    return fail (lineNo, "expected 'pattern = scene row steps'");

                if (scene < 1 || scene > StepSequencer::kScenes)
                    return fail (lineNo, "a pattern is in a scene from 1 to " + std::to_string (StepSequencer::kScenes));

                if (row < 1 || row > StepSequencer::kRows)
                    return fail (lineNo, "a pattern is in a row from 1 to " + std::to_string (StepSequencer::kRows));

                if ((int) parts[2].size() != StepSequencer::kSteps)
                    return fail (lineNo, "a pattern has " + std::to_string (StepSequencer::kSteps) + " steps");

                unsigned bits = 0;

                for (int step = 0; step < StepSequencer::kSteps; ++step)
                {
                    const char ch = parts[2][(std::size_t) step];

                    if (ch == 'X')       bits |= 1u << step;
                    else if (ch != '.')  return fail (lineNo, "the steps of a pattern are X or .");
                }

                bool& again = patternSeen[scene - 1][row - 1];

                if (again)
                    return fail (lineNo, "a pattern row twice");

                again = true;
                p.grid.bits[scene - 1][row - 1] = (std::uint16_t) bits;
                highestRow = std::max (highestRow, (int) row);
                ++patternLines;
            }
            else if (key == "end")
            {
                const auto parts = words (value);

                if (parts.size() != 2 || ! parseInt (parts[0], declaredSlices) || ! parseInt (parts[1], declaredPatterns))
                    return fail (lineNo, "expected 'end = slices patterns'");

                seen[oEnd] = true;
            }
            else
            {
                return fail (lineNo, "unknown name '" + key + "'");
            }
        }

        if (! haveVersion)
            return fail (0, "the file is empty");

        if (! seen[oEnd])
            return fail (0, "the file is cut short (it has no end line)");

        static const char* const required[] = { "audio", "frames", "rate", "tempo", "swing", "length", "scene", "selected" };

        for (int i = 1; i < 8; ++i)   // 'audio' (0) is for people and may be left out
            if (! seen[i])
                return fail (0, std::string ("'") + required[i] + "' is missing");

        if (declaredSlices != (std::int64_t) p.slices.size() || declaredPatterns != patternLines)
            return fail (0, "the file is not complete (the end line does not match what is in it)");

        if (highestRow > (int) p.slices.size())
            return fail (0, "a pattern is on a row that has no slice");

        out.status = ProjectParse::Status::ok;
        return out;
    }

    // ------------------------------------------------------------------------------ engine

    Project captureProject (Engine& engine)
    {
        Project p;
        const auto sample = engine.sample();

        if (sample != nullptr)
        {
            p.audio = sample->name;
            p.frames = (std::int64_t) sample->data.numFrames();
            p.rate = (std::int64_t) std::llround (sample->sampleRate);
        }

        p.slices = *engine.slices().getCurrent();
        p.selected = engine.selectedSlice();

        const auto& seq = engine.sequencer();
        p.tempo = seq.tempo();
        p.swing = seq.swing();
        p.length = seq.length();
        p.scene = seq.scene();
        p.grid = seq.snapshot();

        p.sanitize (sample != nullptr ? sample->durationSeconds() : 0.0);
        return p;
    }

    bool applyProject (Engine& engine, const Project& project)
    {
        const auto sample = engine.sample();

        if (sample == nullptr || ! project.belongsTo (*sample))
            return false;

        Project p = project;
        p.sanitize (sample->durationSeconds());

        engine.slices().replaceAll (std::move (p.slices));

        auto& seq = engine.sequencer();
        seq.restore (p.grid);
        seq.setTempo (p.tempo);
        seq.setSwing (p.swing);
        seq.setLength (p.length);
        seq.setScene (p.scene);

        engine.setSelectedSlice (p.selected);
        return true;
    }
}
