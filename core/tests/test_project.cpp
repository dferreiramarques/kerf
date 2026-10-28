// Tests for projects: the text file (what is written, what is accepted, what is refused), the
// ranges numbers are brought into, what counts as "the same work", and the two calls that move a
// project between the Engine and a value.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine.h"
#include "minitest.h"
#include "project.h"

using namespace kerf;

namespace
{
    // ------------------------------------------------------------------------------ helpers

    Slice makeSlice (float start, float end)
    {
        Slice s;
        s.start = start;
        s.end = end;
        return s;
    }

    // A project that uses every kind of value: all the parameters off their defaults, awkward floats,
    // lit steps in several scenes and rows, a disabled slice, a loop slice.
    Project makeProject()
    {
        Project p;
        p.audio = "rec-001.wav";
        p.frames = 441000;
        p.rate = 44100;
        p.tempo = 133.5f;
        p.swing = 0.6f;
        p.length = 12;
        p.scene = 2;
        p.selected = 1;

        Slice a = makeSlice (0.25f, 0.5f);

        Slice b = makeSlice (0.5f, 1.1f);
        b.pitch = (float) std::pow (2.0, 7.0 / 12.0);
        b.volume = 0.65f;
        b.pan = -0.3f;
        b.mode = Slice::Mode::loop;
        b.fadeInMs = 12.0f;
        b.fadeOutMs = 120.0f;
        b.enabled = false;

        Slice c = makeSlice (1.0f / 3.0f, 9.99999f);
        c.pitch = 0.25f;
        c.pan = 1.0f;
        c.volume = 0.0f;
        c.fadeInMs = 0.0f;
        c.fadeOutMs = 200.0f;

        p.slices = { a, b, c };

        p.grid.bits[0][0] = 0x1111;
        p.grid.bits[0][2] = 0x8001;
        p.grid.bits[2][1] = 0xFFFF;
        p.grid.bits[7][2] = 0x0002;
        return p;
    }

    std::string write (const Project& p)
    {
        std::string text;
        EXPECT (serializeProject (p, text));
        return text;
    }

    std::vector<std::string> lines (const std::string& text)
    {
        std::vector<std::string> out;
        std::size_t at = 0;

        while (at < text.size())
        {
            const auto nl = text.find ('\n', at);
            out.push_back (text.substr (at, nl == std::string::npos ? std::string::npos : nl - at));
            at = nl == std::string::npos ? text.size() : nl + 1;
        }

        return out;
    }

    std::string join (const std::vector<std::string>& v)
    {
        std::string out;

        for (const auto& l : v)
            out += l + "\n";

        return out;
    }

    // The lines that carry data (not the comments at the top).
    std::vector<std::string> dataLines (const std::string& text)
    {
        std::vector<std::string> out;

        for (auto& l : lines (text))
            if (! l.empty() && l[0] != '#')
                out.push_back (l);

        return out;
    }

    bool startsWith (const std::string& s, const std::string& prefix)
    {
        return s.compare (0, prefix.size(), prefix) == 0;
    }

    // The text without the lines that start with `prefix`.
    std::string without (const std::string& text, const std::string& prefix)
    {
        std::vector<std::string> kept;

        for (auto& l : lines (text))
            if (! startsWith (l, prefix))
                kept.push_back (l);

        return join (kept);
    }

    // The first line that starts with `prefix` is replaced by `replacement`.
    std::string replacing (const std::string& text, const std::string& prefix, const std::string& replacement)
    {
        auto v = lines (text);

        for (auto& l : v)
            if (startsWith (l, prefix))
            {
                l = replacement;
                break;
            }

        return join (v);
    }

    // The first line that starts with `prefix` is written twice.
    std::string doubling (const std::string& text, const std::string& prefix)
    {
        std::vector<std::string> v;
        bool done = false;

        for (auto& l : lines (text))
        {
            v.push_back (l);

            if (! done && startsWith (l, prefix))
            {
                v.push_back (l);
                done = true;
            }
        }

        return join (v);
    }

    bool isDamaged (const std::string& text)
    {
        return parseProject (text).status == ProjectParse::Status::damaged;
    }

    bool sameBits (float a, float b)
    {
        return std::memcmp (&a, &b, sizeof a) == 0;
    }

    bool identical (const Project& a, const Project& b)
    {
        if (! sameWork (a, b) || a.selected != b.selected || a.scene != b.scene || a.audio != b.audio || a.frames != b.frames || a.rate != b.rate)
            return false;

        for (std::size_t i = 0; i < a.slices.size(); ++i)
            if (! sameBits (a.slices[i].start, b.slices[i].start) || ! sameBits (a.slices[i].end, b.slices[i].end)
                || ! sameBits (a.slices[i].pitch, b.slices[i].pitch) || ! sameBits (a.slices[i].volume, b.slices[i].volume)
                || ! sameBits (a.slices[i].pan, b.slices[i].pan) || ! sameBits (a.slices[i].fadeInMs, b.slices[i].fadeInMs)
                || ! sameBits (a.slices[i].fadeOutMs, b.slices[i].fadeOutMs))
                return false;

        return sameBits (a.tempo, b.tempo) && sameBits (a.swing, b.swing);
    }

    const std::string kSliceLine = "slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode=shot fade_in=1 fade_out=50 enabled=1";

    // The smallest complete file: one slice, one lit row.
    std::string smallFile()
    {
        return "kerf_project = 1\n"
               "audio = a.wav\n"
               "frames = 44100\n"
               "rate = 44100\n"
               "tempo = 120\n"
               "swing = 0.5\n"
               "length = 16\n"
               "scene = 1\n"
               "selected = 1\n"
               + kSliceLine + "\n"
               "pattern = 1 1 X...X...X...X...\n"
               "end = 1 1\n";
    }
}

// =============================================================================== the file

KTEST (project_file, round_trip_is_exact)
{
    const Project p = makeProject();
    const auto parsed = parseProject (write (p));

    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT (parsed.error.empty());
    EXPECT_EQ (parsed.version, 1);
    EXPECT (identical (parsed.project, p));
    EXPECT_EQ (parsed.project.slices.size(), 3u);
    EXPECT_EQ (parsed.project.patternCount(), 4);
}

KTEST (project_file, an_empty_project_round_trips)
{
    Project p;
    p.audio = "x.wav";
    p.frames = 10;
    p.rate = 8000;

    const auto text = write (p);
    EXPECT (dataLines (text).back() == "end = 0 0");
    EXPECT (std::find (dataLines (text).begin(), dataLines (text).end(), "selected = 0") != dataLines (text).end());

    const auto parsed = parseProject (text);
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT (identical (parsed.project, p));
}

KTEST (project_file, the_text_is_canonical)
{
    const Project p = makeProject();
    const auto text = write (p);

    EXPECT (write (p) == text);
    EXPECT (write (parseProject (text).project) == text);
}

KTEST (project_file, the_layout_is_the_documented_one)
{
    Project p;
    p.audio = "a.wav";
    p.frames = 44100;
    p.rate = 44100;
    p.slices = { makeSlice (0.25f, 0.5f) };
    p.grid.bits[0][0] = 0x1111;

    const auto text = write (p);
    EXPECT (startsWith (text, "#"));                         // a few comment lines, then the data

    const std::vector<std::string> expected = {
        "kerf_project = 1", "audio = a.wav", "frames = 44100", "rate = 44100", "tempo = 120", "swing = 0.5", "length = 16",
        "scene = 1", "selected = 1", kSliceLine, "pattern = 1 1 X...X...X...X...", "end = 1 1" };
    EXPECT (dataLines (text) == expected);
    EXPECT (text.back() == '\n');
    EXPECT (text.find ('\r') == std::string::npos);
}

KTEST (project_file, indices_are_written_counting_from_one)
{
    Project p;
    p.frames = 100;
    p.rate = 100;
    p.slices = { makeSlice (0.0f, 0.1f), makeSlice (0.1f, 0.2f), makeSlice (0.2f, 0.3f) };
    p.selected = 2;
    p.scene = 7;
    p.grid.bits[7][2] = 0x0004;     // scene 8, row (slice) 3, step 3

    const auto data = dataLines (write (p));
    EXPECT (std::find (data.begin(), data.end(), "selected = 3") != data.end());
    EXPECT (std::find (data.begin(), data.end(), "scene = 8") != data.end());
    EXPECT (std::find (data.begin(), data.end(), "pattern = 8 3 ..X.............") != data.end());
}

KTEST (project_file, numbers_are_written_the_short_way)
{
    Project p;
    p.frames = 1;
    p.rate = 1;
    p.tempo = 133.5f;
    p.swing = 0.6f;

    Slice s = makeSlice (0.0f, 0.1f);
    s.pan = -0.3f;
    s.pitch = (float) std::pow (2.0, 1.0 / 12.0);
    s.fadeInMs = 12.0f;
    p.slices = { s };

    auto text = write (p);
    EXPECT (text.find ("tempo = 133.5\n") != std::string::npos);
    EXPECT (text.find ("swing = 0.6\n") != std::string::npos);
    EXPECT (text.find ("start=0 end=0.1 ") != std::string::npos);
    EXPECT (text.find ("pan=-0.3 ") != std::string::npos);
    EXPECT (text.find ("fade_in=12 ") != std::string::npos);
    EXPECT (text.find ("-0") == std::string::npos || text.find ("pan=-0.3") != std::string::npos);

    // Negative zero is zero.
    p.slices[0].start = -0.0f;
    p.slices[0].pan = -0.0f;
    text = write (p);
    EXPECT (text.find ("start=0 ") != std::string::npos);
    EXPECT (text.find ("pan=0 ") != std::string::npos);
    EXPECT (text.find ("start=-0") == std::string::npos);
    EXPECT (text.find ("pan=-0") == std::string::npos);

    // Whatever the number, it reads back as the same float.
    const float awkward[] = { 1.0f / 3.0f, 0.1f, 359.99997f, 1.0e-7f, 123456.78f, 0.99999994f, 2.0f / 3.0f, 16777215.0f };

    for (const float v : awkward)
    {
        p.slices[0].start = v;
        p.slices[0].end = v;
        const auto parsed = parseProject (write (p));
        EXPECT (parsed.status == ProjectParse::Status::ok);
        EXPECT (sameBits (parsed.project.slices[0].start, v));
        EXPECT (sameBits (parsed.project.slices[0].end, v));
    }
}

KTEST (project_file, floats_are_written_with_just_the_digits_they_need)
{
    struct Case { float value; const char* text; };

    const Case cases[] = {
        { 0.0f, "0" }, { 1.0f, "1" }, { 0.5f, "0.5" }, { 0.1f, "0.1" }, { -0.3f, "-0.3" }, { 133.5f, "133.5" },
        { 1.0f / 3.0f, "0.33333334" },          // eight decimals
        { 2.0f / 3.0f, "0.6666667" },
        { 0.99999994f, "0.99999994" },          // the float just below 1
        { 359.99997f, "359.99997" },
        { 123456.78f, "123456.78" },
        { 16777215.0f, "16777215" },            // the last whole number a float counts one by one
        { 1.0e-7f, "0.0000001" },
        { 1.0e-16f, "0.0000000000000001" },
        { 1.0e-17f, "0.00000000000000001" },    // seventeen decimals: the most that is tried
        { 1.0e-18f, "1.00000005e-18" }          // beyond that: nine significant digits, which always read back
    };

    Project p;
    p.frames = 1;
    p.rate = 1;
    p.slices = { makeSlice (0.0f, 0.1f) };

    for (const auto& c : cases)
    {
        p.slices[0].start = c.value;
        p.tempo = c.value;
        const auto text = write (p);
        EXPECT_TEXT (dataLines (text)[4], std::string ("tempo = ") + c.text);
        EXPECT (text.find (std::string ("start=") + c.text + " end=0.1 ") != std::string::npos);

        const auto parsed = parseProject (text);
        EXPECT (parsed.status == ProjectParse::Status::ok);
        EXPECT (sameBits (parsed.project.slices[0].start, c.value));
        EXPECT (sameBits (parsed.project.tempo, c.value));
    }
}

KTEST (project_file, every_float_reads_back_exactly_even_the_extremes)
{
    const float extremes[] = {
        1.2345678e-20f, -7.6543217e-25f,            // tiny, and not "nice" numbers: they need all nine digits
        1.17549435e-38f,                            // the smallest normal float
        1.0e-40f, 1.4e-45f,                         // denormals, down to the smallest
        3.4028235e38f, -3.4028235e38f,              // the largest: thirty-nine digits, forty with the minus sign
        16777216.0f, 1.0e10f, 0.007812501f
    };

    Project p;
    p.frames = 1;
    p.rate = 1;
    p.slices = { makeSlice (0.0f, 0.1f) };

    for (const float v : extremes)
    {
        p.tempo = v;
        p.swing = v;
        p.slices[0] = makeSlice (v, v);
        p.slices[0].pitch = v;
        p.slices[0].volume = v;
        p.slices[0].pan = v;
        p.slices[0].fadeInMs = v;
        p.slices[0].fadeOutMs = v;

        const auto parsed = parseProject (write (p));
        EXPECT (parsed.status == ProjectParse::Status::ok);
        EXPECT (identical (parsed.project, p));
    }

    // Written, a float is never longer than the reader accepts (the largest, with its sign, is exactly at the limit).
    p.tempo = -3.4028235e38f;
    EXPECT_EQ (dataLines (write (p))[4].size(), std::string ("tempo = ").size() + 40);
}

KTEST (project_file, a_name_with_a_new_line_stays_on_one_line)
{
    Project p;
    p.frames = 1;
    p.rate = 1;
    p.audio = "two\nlines\r.wav";

    const auto text = write (p);
    const auto parsed = parseProject (text);
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT (parsed.project.audio == "two?lines?.wav");

    // Spaces and accents are kept.
    p.audio = "Mi\xC3\xA9rcoles beat 2.wav";
    EXPECT (parseProject (write (p)).project.audio == p.audio);

    // Every control character goes, from the first to the last (and delete), and nothing else does.
    std::string controls;
    std::string expected;

    for (int c = 1; c < 0x20; ++c)
    {
        controls += (char) c;
        expected += '?';
    }

    p.audio = "a" + controls + "\x7f z~ \x80\xC3\xA9";
    EXPECT_TEXT (parseProject (write (p)).project.audio, "a" + expected + "? z~ \x80\xC3\xA9");

    p.audio = std::string ("nul\0inside", 10);
    EXPECT_TEXT (parseProject (write (p)).project.audio, "nul?inside");
}

KTEST (project_file, a_very_long_name_is_cut)
{
    Project p;
    p.frames = 1;
    p.rate = 1;

    // Written: 1024 bytes are kept, and no more.
    p.audio = std::string (1024, 'n');
    EXPECT_EQ (parseProject (write (p)).project.audio.size(), (std::size_t) 1024);
    EXPECT (parseProject (write (p)).project.audio == p.audio);

    p.audio = std::string (1025, 'n');
    EXPECT_EQ (parseProject (write (p)).project.audio.size(), (std::size_t) 1024);
    p.audio = std::string (100000, 'n');
    EXPECT_EQ (parseProject (write (p)).project.audio.size(), (std::size_t) 1024);

    // The reader would cut a longer name anyway, so look at the text itself: the line is cut when it is written.
    const auto audioLine = [] (const Project& q) -> std::string
    {
        for (const auto& l : dataLines (write (q)))
            if (startsWith (l, "audio"))
                return l;

        return {};
    };

    const std::size_t label = std::string ("audio = ").size();

    for (const std::size_t length : { (std::size_t) 1023, (std::size_t) 1024 })
    {
        p.audio = std::string (length, 'n');
        EXPECT_EQ (audioLine (p).size(), label + length);
    }

    for (const std::size_t length : { (std::size_t) 1025, (std::size_t) 100000 })
    {
        p.audio = std::string (length, 'n');
        EXPECT_EQ (audioLine (p).size(), label + 1024);
    }

    // Read: the same, whatever the file says.
    EXPECT_EQ (parseProject (replacing (smallFile(), "audio", "audio = " + std::string (1024, 'm'))).project.audio.size(), (std::size_t) 1024);
    EXPECT_EQ (parseProject (replacing (smallFile(), "audio", "audio = " + std::string (1025, 'm'))).project.audio.size(), (std::size_t) 1024);
    EXPECT_EQ (parseProject (replacing (smallFile(), "audio", "audio = " + std::string (50000, 'm'))).project.audio.size(), (std::size_t) 1024);
}

KTEST (project_file, the_reader_is_forgiving_about_layout)
{
    const Project expected = parseProject (smallFile()).project;
    EXPECT (parseProject (smallFile()).status == ProjectParse::Status::ok);

    // Windows line ends, a byte order mark, blank lines and comments anywhere, spaces and tabs around the '='.
    std::string crlf;
    for (auto& l : lines (smallFile()))
        crlf += l + "\r\n";

    auto parsed = parseProject (crlf);
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT (identical (parsed.project, expected));

    parsed = parseProject (std::string ("\xEF\xBB\xBF") + smallFile());
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT (identical (parsed.project, expected));

    parsed = parseProject ("# a comment first\n\n   \n" + replacing (smallFile(), "tempo", "  tempo\t=\t120   \n# in the middle\n\n"));
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT (identical (parsed.project, expected));

    parsed = parseProject (replacing (smallFile(), "rate", "rate=44100"));
    EXPECT (parsed.status == ProjectParse::Status::ok);

    // The fields of a slice in any order, separated by any blanks; the last line without a new line.
    parsed = parseProject (replacing (smallFile(), "slice",
                                      "slice =  enabled=1\tfade_out=50 fade_in=1   mode=shot pan=0 volume=1 pitch=1 end=0.5 start=0.25"));
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT (identical (parsed.project, expected));

    std::string noFinalNewline = smallFile();
    noFinalNewline.pop_back();
    EXPECT (parseProject (noFinalNewline).status == ProjectParse::Status::ok);
}

KTEST (project_file, a_project_without_audio_name_is_fine)
{
    const auto parsed = parseProject (without (smallFile(), "audio"));
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT (parsed.project.audio.empty());
}

KTEST (project_file, damaged_files_are_refused)
{
    const std::string good = smallFile();
    EXPECT (! isDamaged (good));

    // Nothing, or not a project at all.
    EXPECT (isDamaged (""));
    EXPECT (isDamaged ("\n\n# only a comment\n"));
    EXPECT (isDamaged ("hello\n"));
    EXPECT (isDamaged ("tempo = 120\n" + good));                         // the version has to come first
    EXPECT (isDamaged ("kerf_project\n"));                              // no '='
    EXPECT (isDamaged (replacing (good, "kerf_project", "kerf_project = one")));
    EXPECT (isDamaged (replacing (good, "kerf_project", "kerf_project = 0")));
    EXPECT (isDamaged (replacing (good, "kerf_project", "kerf_project = -1")));
    EXPECT (isDamaged (replacing (good, "kerf_project", "kerf_project = 1.0")));
    EXPECT (isDamaged (replacing (good, "kerf_project", "kerf_project = ")));

    // Something that is not a line of the format.
    EXPECT (isDamaged (replacing (good, "audio", "audio")));              // a name with no '=' and no value
    EXPECT (isDamaged (replacing (good, "audio", "an odd line")));
    EXPECT (isDamaged (replacing (good, "audio", "colour = red")));
    EXPECT (isDamaged (replacing (good, "audio", "Tempo = 120")));       // names are lower case
    EXPECT (isDamaged (good + "audio = b.wav\n"));                       // anything after the end line

    // The parts of the file that have to be there (all but the name of the audio) ...
    for (const char* name : { "frames", "rate", "tempo", "swing", "length", "scene", "selected", "end" })
        EXPECT (isDamaged (without (good, name)));

    // ... and that may be there only once.
    for (const char* name : { "audio", "frames", "rate", "tempo", "swing", "length", "scene", "selected", "end", "pattern" })
        EXPECT (isDamaged (doubling (good, name)));
}

KTEST (project_file, numbers_that_are_not_numbers_are_refused)
{
    const std::string good = smallFile();

    for (const char* line : { "frames = x", "frames = ", "frames = -1", "frames = -", "frames = 1.5", "frames = 99999999999999999999",
                               "frames = 1000000000000000000",          // nineteen digits: more than a whole number of this size is read from
                               "rate = -1", "rate = -", "rate = -44100", "rate = 44100hz", "rate = 4 4", "tempo = fast", "tempo = nan", "tempo = inf", "tempo = -inf", "tempo = 0x10",
                               "tempo = 1,5", "tempo = 1e999", "tempo = ", "tempo = -", "tempo = +", "tempo = e5", "tempo = 1e+", "swing = .",
                               "swing = --1", "swing = 1e", "length = 1.5", "length = x", "length = -", "length = - 3", "length = +3", "length = -999999999999999999",
                               "scene = a", "scene = 1.0", "scene = -", "selected = ?", "selected = ", "selected = -" })
    {
        const std::string name = std::string (line).substr (0, std::string (line).find (' '));
        EXPECT (isDamaged (replacing (good, name, line)));
    }

    // The ones that are numbers, whatever their size, are read (and brought into range later).
    for (const char* line : { "tempo = 1e2", "tempo = 1E2", "tempo = 1E+2", "tempo = 1.5E-1", "tempo = 1e-2", "tempo = +120", "tempo = 120.",
                               "tempo = .5", "tempo = -5", "tempo = 99999", "length = 0", "length = -3", "length = 999999999999",
                               "length = -99999999999999999", "frames = 999999999999999999", "scene = 0", "scene = 99", "selected = 0",
                               "selected = 12345" })
    {
        const std::string name = std::string (line).substr (0, std::string (line).find (' '));
        EXPECT (! isDamaged (replacing (good, name, line)));
    }

    // A number is read up to forty characters; anything longer is not a number worth trusting.
    EXPECT (! isDamaged (replacing (good, "tempo", "tempo = 0." + std::string (38, '0'))));
    EXPECT (isDamaged (replacing (good, "tempo", "tempo = 0." + std::string (39, '0'))));
    EXPECT (! isDamaged (replacing (good, "tempo", "tempo = -0." + std::string (37, '0'))));
    EXPECT (isDamaged (replacing (good, "tempo", "tempo = -0." + std::string (38, '0'))));
}

KTEST (project_file, whole_numbers_beyond_what_an_int_holds_are_held_back)
{
    const std::string good = smallFile();

    // The number is read whole (as far as eighteen digits go) and then held to a million either way,
    // so that nothing wraps around to a small number: 4294967297 would be 1 as a 32-bit number.
    for (const char* big : { "999999999999", "4294967297", "99999999999999999" })
    {
        EXPECT_EQ (parseProject (replacing (good, "length", std::string ("length = ") + big)).project.length, 1000000);
        EXPECT_EQ (parseProject (replacing (good, "length", std::string ("length = -") + big)).project.length, -1000000);
        EXPECT_EQ (parseProject (replacing (good, "scene", std::string ("scene = ") + big)).project.scene, 999999);
        EXPECT_EQ (parseProject (replacing (good, "selected", std::string ("selected = ") + big)).project.selected, 999999);
    }

    // Eighteen digits are the most a number may have (seventeen with a minus sign).
    EXPECT_EQ (parseProject (replacing (good, "length", "length = 999999999999999999")).project.length, 1000000);
    EXPECT_EQ (parseProject (replacing (good, "scene", "scene = 999999999999999999")).project.scene, 999999);

    // The numbers just inside that are read as they are.
    EXPECT_EQ (parseProject (replacing (good, "length", "length = 1000000")).project.length, 1000000);
    EXPECT_EQ (parseProject (replacing (good, "length", "length = 999999")).project.length, 999999);
    EXPECT_EQ (parseProject (replacing (good, "length", "length = -1000000")).project.length, -1000000);
    EXPECT_EQ (parseProject (replacing (good, "length", "length = -999999")).project.length, -999999);
    EXPECT_EQ (parseProject (replacing (good, "scene", "scene = 1000000")).project.scene, 999999);
    EXPECT_EQ (parseProject (replacing (good, "scene", "scene = 999999")).project.scene, 999998);
    EXPECT_EQ (parseProject (replacing (good, "selected", "selected = 1000000")).project.selected, 999999);
    EXPECT_EQ (parseProject (replacing (good, "selected", "selected = 999999")).project.selected, 999998);

    // Counting from one: 'scene = 0' and 'selected = 0' and anything below are the first, and none.
    EXPECT_EQ (parseProject (replacing (good, "scene", "scene = 0")).project.scene, 0);
    EXPECT_EQ (parseProject (replacing (good, "scene", "scene = -7")).project.scene, 0);
    EXPECT_EQ (parseProject (replacing (good, "scene", "scene = 1")).project.scene, 0);
    EXPECT_EQ (parseProject (replacing (good, "scene", "scene = 2")).project.scene, 1);
    EXPECT_EQ (parseProject (replacing (good, "selected", "selected = 0")).project.selected, 0);
    EXPECT_EQ (parseProject (replacing (good, "selected", "selected = -7")).project.selected, 0);
    EXPECT_EQ (parseProject (replacing (good, "selected", "selected = 1")).project.selected, 0);
    EXPECT_EQ (parseProject (replacing (good, "selected", "selected = 2")).project.selected, 1);

    // The same goes for the version: a number from the future is a future, whatever its size.
    const auto future = parseProject ("kerf_project = 99999999999\n");
    EXPECT (future.status == ProjectParse::Status::newer);
    EXPECT_EQ (future.version, 1000000);
    EXPECT_EQ (parseProject ("kerf_project = 2\n").version, 2);
}

KTEST (project_file, a_slice_field_needs_its_equals_sign)
{
    // (A field without one could never have a value of its own, but the file is then plainly not what
    // Kerf writes, and the error says so.)
    for (const char* line : { "slice = start end=0.5 pitch=1 volume=1 pan=0 mode=shot fade_in=1 fade_out=50 enabled=1",
                              "slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode fade_in=1 fade_out=50 enabled=1",
                              "slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode=shot fade_in=1 fade_out=50 enabled" })
    {
        const auto parsed = parseProject (replacing (smallFile(), "slice", line));
        EXPECT (parsed.status == ProjectParse::Status::damaged);
        EXPECT (parsed.error.find ("name=value") != std::string::npos);
    }
}

KTEST (project_file, damaged_slices_are_refused)
{
    const std::string good = smallFile();
    const std::vector<std::string> fields = { "start=0.25", "end=0.5", "pitch=1", "volume=1", "pan=0", "mode=shot", "fade_in=1", "fade_out=50", "enabled=1" };

    // Each field is needed, and may be given once.
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        std::string missing = "slice =", twice = "slice =";

        for (std::size_t k = 0; k < fields.size(); ++k)
        {
            if (k != i)
                missing += " " + fields[k];

            twice += " " + fields[k];
        }

        twice += " " + fields[i];

        EXPECT (isDamaged (replacing (good, "slice", missing)));
        EXPECT (isDamaged (replacing (good, "slice", twice)));
    }

    EXPECT (isDamaged (replacing (good, "slice", "slice =")));
    EXPECT (isDamaged (replacing (good, "slice", kSliceLine + " colour=red")));
    EXPECT (isDamaged (replacing (good, "slice", kSliceLine + " 0.5")));
    EXPECT (isDamaged (replacing (good, "slice", "slice = start 0.25 end 0.5")));
    EXPECT (isDamaged (replacing (good, "slice", "slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode=shot fade_in=1 fade_out=50 enabled=2")));
    EXPECT (isDamaged (replacing (good, "slice", "slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode=shot fade_in=1 fade_out=50 enabled=yes")));
    EXPECT (isDamaged (replacing (good, "slice", "slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode=Shot fade_in=1 fade_out=50 enabled=1")));
    EXPECT (isDamaged (replacing (good, "slice", "slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode=1 fade_in=1 fade_out=50 enabled=1")));
    EXPECT (! isDamaged (replacing (good, "slice", "slice = start=0.25 end=0.5 pitch=1 volume=1 pan=0 mode=loop fade_in=1 fade_out=50 enabled=0")));

    // A number is a number in every field.
    for (const char* name : { "start", "end", "pitch", "volume", "pan", "fade_in", "fade_out" })
    {
        for (const char* bad : { "nan", "inf", "x", "", "1..2", "0x1p3", "1,5" })
        {
            std::string line = kSliceLine;
            const std::string key = std::string (name) + "=";
            const auto at = line.find (" " + key);
            const auto from = at + 1 + key.size();
            const auto to = line.find (' ', from);
            line.replace (from, (to == std::string::npos ? line.size() : to) - from, bad);
            EXPECT (isDamaged (replacing (good, "slice", line)));
        }
    }
}

KTEST (project_file, damaged_patterns_are_refused)
{
    const std::string good = smallFile();

    for (const char* line : { "pattern = 1 1", "pattern = 1 1 X...X...X...X... X", "pattern = 1 X...X...X...X...", "pattern = x 1 X...X...X...X...",
                               "pattern = 1 y X...X...X...X...", "pattern = 0 1 X...X...X...X...", "pattern = 9 1 X...X...X...X...",
                               "pattern = 1 0 X...X...X...X...", "pattern = 1 129 X...X...X...X...", "pattern = 1 1 X...X...X...X..",
                               "pattern = 1 1 X...X...X...X....", "pattern = 1 1 x...x...x...x...", "pattern = 1 1 1...1...1...1...",
                               "pattern = 1 1 X...X...X...X..-", "pattern = -1 1 X...X...X...X...", "pattern =", "pattern = 1 2 X...X...X...X..." })
        EXPECT (isDamaged (replacing (good, "pattern", line)));

    // The same row of the same scene twice; a different scene is fine.
    EXPECT (isDamaged (replacing (good, "end", "pattern = 1 1 ................\nend = 1 2")));
    EXPECT (! isDamaged (replacing (good, "end", "pattern = 2 1 ................\nend = 1 2")));

    // A row with no step lit is allowed, and counts as a line.
    EXPECT (! isDamaged (replacing (good, "pattern", "pattern = 1 1 ................")));
    EXPECT (isDamaged (replacing (good, "end", "pattern = 3 1 X...............\nend = 1 1")));

    // The grid has eight scenes of 128 rows, and no more, even when the file has slices to spare.
    std::string many = "kerf_project = 1\nframes = 100\nrate = 100\ntempo = 120\nswing = 0.5\nlength = 16\nscene = 1\nselected = 1\n";

    for (int i = 0; i < 130; ++i)
        many += kSliceLine + "\n";

    const std::string steps = "pattern = ";
    EXPECT (! isDamaged (many + steps + "1 128 X...X...X...X...\nend = 130 1\n"));
    EXPECT (! isDamaged (many + steps + "8 1 X...X...X...X...\nend = 130 1\n"));
    EXPECT (isDamaged (many + steps + "1 129 X...X...X...X...\nend = 130 1\n"));
    EXPECT (isDamaged (many + steps + "1 130 X...X...X...X...\nend = 130 1\n"));
    EXPECT (isDamaged (many + steps + "9 1 X...X...X...X...\nend = 130 1\n"));
    EXPECT (isDamaged (many + steps + "0 1 X...X...X...X...\nend = 130 1\n"));
    EXPECT (isDamaged (many + steps + "1 0 X...X...X...X...\nend = 130 1\n"));

    // Refused at once for what is wrong with it (so that nothing is stored outside the grid on the way): the message says
    // which number, scene or row, is out.
    for (const char* line : { "0 1 X...X...X...X...", "9 1 X...X...X...X...", "-3 1 X...X...X...X...", "1000000 1 X...X...X...X..." })
    {
        const auto refused = parseProject (many + steps + line + "\nend = 130 1\n");
        EXPECT (refused.status == ProjectParse::Status::damaged);
        EXPECT (refused.error.find ("scene from 1 to 8") != std::string::npos);
    }

    for (const char* line : { "1 0 X...X...X...X...", "1 129 X...X...X...X...", "1 -4 X...X...X...X...", "1 1000000 X...X...X...X..." })
    {
        const auto refused = parseProject (many + steps + line + "\nend = 130 1\n");
        EXPECT (refused.status == ProjectParse::Status::damaged);
        EXPECT (refused.error.find ("row from 1 to 128") != std::string::npos);
    }

    const auto last = parseProject (many + steps + "8 128 X...............\nend = 130 1\n");
    EXPECT (last.status == ProjectParse::Status::ok);
    EXPECT_EQ ((int) last.project.grid.bits[7][127], 1);
    EXPECT_EQ ((int) last.project.grid.bits[0][0], 0);
}

KTEST (project_file, the_end_line_has_to_match)
{
    const std::string good = smallFile();

    for (const char* line : { "end = 0 1", "end = 2 1", "end = 1 0", "end = 1 2", "end = 1", "end = 1 1 1", "end = a b", "end = -1 1", "end = 1 -1", "end =" })
        EXPECT (isDamaged (replacing (good, "end", line)));

    EXPECT (! isDamaged (replacing (good, "end", "end   =   1    1")));
}

KTEST (project_file, a_file_that_stops_anywhere_is_never_taken_for_complete)
{
    // Every way a copy can be cut short, from nothing to everything but the last character.
    for (const std::string& full : { write (makeProject()), smallFile() })
        for (std::size_t cut = 0; cut + 1 < full.size(); ++cut)
            EXPECT (parseProject (full.substr (0, cut)).status != ProjectParse::Status::ok);

    // Cut in the middle of the last line, at a number that is still a number: the counts disagree.
    Project p = makeProject();
    const auto text = write (p);
    EXPECT (text.find ("end = 3 4\n") != std::string::npos);
    EXPECT (isDamaged (replacing (text, "end", "end = 3 ")));
}

KTEST (project_file, a_file_damaged_in_any_place_is_met_without_harm)
{
    // What comes back from a card can be in any state. For every place in two typical files: the byte there changed
    // into each of a few awkward ones, taken out, or an awkward one put in front of it; and every line gone, said twice,
    // blanked or swapped with the next. Reading has to end, and be one of two things: a project - which can be written
    // and read again as the same work, and which comes into range - or a damaged file, with the reason.
    const char awkward[] = { '\0', '\n', '\r', ' ', '=', '#', '-', '+', '.', 'e', '0', '9', 'X', (char) 0xFF };
    std::size_t read = 0, refused = 0, variants = 0;
    std::string firstProblem;

    const auto meet = [&] (const std::string& bytes)
    {
        ++variants;
        const auto problem = [&] (const std::string& what) { if (firstProblem.empty()) firstProblem = what + " in: " + bytes.substr (0, 400); };
        const ProjectParse r = parseProject (bytes);

        if (r.status != ProjectParse::Status::ok)
        {
            ++refused;

            if (r.error.empty())
                problem ("no reason given");

            return;
        }

        ++read;

        // It can be written and read again, as the same work.
        std::string again;

        if (! serializeProject (r.project, again))
            return problem ("cannot be written again");

        const ProjectParse back = parseProject (again);

        if (back.status != ProjectParse::Status::ok || ! sameWork (r.project, back.project) || r.project.frames != back.project.frames
            || r.project.rate != back.project.rate || ! sameBits (r.project.tempo, back.project.tempo) || ! sameBits (r.project.swing, back.project.swing))
            return problem ("does not read back as the same");

        // And whatever it said, the numbers come into the range the controls allow.
        Project q = r.project;
        q.sanitize (7.5);

        for (const auto& sl : q.slices)
            if (! (sl.start >= 0.0f && sl.start <= sl.end && sl.end <= 7.5f && sl.pitch >= 0.25f && sl.pitch <= 4.0f && sl.volume >= 0.0f && sl.volume <= 1.0f
                   && sl.pan >= -1.0f && sl.pan <= 1.0f && sl.fadeInMs >= 0.0f && sl.fadeInMs <= 100.0f && sl.fadeOutMs >= 0.0f && sl.fadeOutMs <= 200.0f))
                return problem ("a slice is out of range after sanitize");

        if (! (q.tempo >= 40.0f && q.tempo <= 300.0f && q.swing >= 0.5f && q.swing <= 0.75f && q.length >= 1 && q.length <= 16 && q.scene >= 0 && q.scene <= 7
               && q.selected >= 0 && (q.slices.empty() ? q.selected == 0 : q.selected < (int) q.slices.size())))
            return problem ("the settings are out of range after sanitize");
    };

    for (const std::string& full : { write (makeProject()), smallFile() })
    {
        for (std::size_t i = 0; i < full.size(); ++i)
        {
            for (const char c : awkward)
            {
                std::string t = full;
                t[i] = c;
                meet (t);
            }

            std::string gone = full;
            gone.erase (i, 1);
            meet (gone);

            for (const char c : { '\n', ' ', '=', '9', (char) 0xFF })
            {
                std::string t = full;
                t.insert (i, 1, c);
                meet (t);
            }
        }

        const auto all = lines (full);

        for (std::size_t l = 0; l < all.size(); ++l)
        {
            auto v = all;
            v.erase (v.begin() + (std::ptrdiff_t) l);
            meet (join (v));

            v = all;
            v.insert (v.begin() + (std::ptrdiff_t) l, all[l]);
            meet (join (v));

            v = all;
            v[l].clear();
            meet (join (v));

            if (l + 1 < all.size())
            {
                v = all;
                std::swap (v[l], v[l + 1]);
                meet (join (v));
            }
        }
    }

    EXPECT_TEXT (firstProblem, std::string());
    EXPECT (variants > 15000);
    EXPECT (read > 200);              // (some of the damage is harmless: a changed name, a digit in a number)
    EXPECT (refused > 5000);
}

KTEST (project_file, a_pattern_needs_a_slice)
{
    const std::string good = smallFile();

    // Row 2 with one slice.
    EXPECT (isDamaged (replacing (good, "end", "pattern = 1 2 X...............\nend = 1 2")));

    // Patterns written before the slices they are for are fine.
    const std::string patternFirst = "kerf_project = 1\nframes = 1\nrate = 1\ntempo = 120\nswing = 0.5\nlength = 16\nscene = 1\nselected = 1\n"
                                     "pattern = 1 1 X...............\n" + kSliceLine + "\nend = 1 1\n";
    EXPECT (! isDamaged (patternFirst));
}

KTEST (project_file, a_newer_version_is_recognised_and_not_read)
{
    const std::string future = "kerf_project = 2\nsomething = the next version writes differently\n[not even lines]\n";
    const auto parsed = parseProject (future);

    EXPECT (parsed.status == ProjectParse::Status::newer);
    EXPECT_EQ (parsed.version, 2);
    EXPECT (parsed.error.find ("newer") != std::string::npos);
    EXPECT (parsed.project.slices.empty());

    EXPECT (parseProject ("# comment\n\nkerf_project = 99\n").status == ProjectParse::Status::newer);
    EXPECT (parseProject ("kerf_project = 1000000\n").status == ProjectParse::Status::newer);
    EXPECT (parseProject ("kerf_project = 2").status == ProjectParse::Status::newer);
    EXPECT (parseProject (replacing (smallFile(), "kerf_project", "kerf_project = 2")).status == ProjectParse::Status::newer);

    // A version that is not a number is damage, not the future.
    EXPECT (isDamaged ("kerf_project = 2x\n"));
    EXPECT (isDamaged ("kerf_project = 99999999999999999999999\n"));
}

KTEST (project_file, errors_say_where_the_trouble_is)
{
    auto parsed = parseProject (replacing (smallFile(), "tempo", "tempo = fast"));
    EXPECT (parsed.status == ProjectParse::Status::damaged);
    EXPECT (parsed.error.find ("line 5") != std::string::npos);      // the tempo line
    EXPECT (parsed.error.find ("tempo") != std::string::npos);

    parsed = parseProject (replacing (smallFile(), "slice", "slice = start=0.25"));
    EXPECT (parsed.error.find ("line 10") != std::string::npos);

    parsed = parseProject (without (smallFile(), "end"));
    EXPECT (parsed.error.find ("cut short") != std::string::npos);

    parsed = parseProject ("");
    EXPECT (parsed.error.find ("empty") != std::string::npos);
    parsed = parseProject ("# nothing but a comment\n\n   \n");
    EXPECT (parsed.error.find ("empty") != std::string::npos);

    // Negative counts in the end line are not what the file holds.
    for (const char* line : { "end = -1 1", "end = 1 -1", "end = -1 -1", "end = 0 -1" })
        EXPECT (isDamaged (replacing (smallFile(), "end", line)));

    parsed = parseProject (without (smallFile(), "frames"));
    EXPECT (parsed.error.find ("frames") != std::string::npos);

    // A damaged result carries nothing of the file.
    parsed = parseProject (replacing (smallFile(), "end", "end = 5 5"));
    EXPECT (parsed.status == ProjectParse::Status::damaged);
    EXPECT (parsed.project.slices.empty());
    EXPECT_EQ (parsed.project.patternCount(), 0);
}

KTEST (project_file, size_limits)
{
    // A file too big to be a project is not even read; one of exactly the biggest size is looked at
    // (and is damaged for another reason).
    EXPECT (isDamaged (std::string (Project::kMaxFileBytes + 1, '#')));
    EXPECT (parseProject (std::string (Project::kMaxFileBytes + 1, '#')).error.find ("too big") != std::string::npos);
    EXPECT (isDamaged (std::string (Project::kMaxFileBytes, '#')));
    EXPECT (parseProject (std::string (Project::kMaxFileBytes, '#')).error.find ("too big") == std::string::npos);

    // The slices a project may hold: the most it can, and one more.
    Project p;
    p.frames = 100;
    p.rate = 100;
    p.slices.assign ((std::size_t) Project::kMaxSlices, makeSlice (0.0f, 0.5f));

    std::string text;
    EXPECT (serializeProject (p, text));
    EXPECT (text.size() < Project::kMaxFileBytes);

    const auto parsed = parseProject (text);
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT_EQ (parsed.project.slices.size(), (std::size_t) Project::kMaxSlices);

    p.slices.push_back (makeSlice (0.0f, 0.5f));
    EXPECT (! serializeProject (p, text));
    EXPECT (text.empty());

    // ... and a file that holds one too many.
    std::string manual = "kerf_project = 1\nframes = 100\nrate = 100\ntempo = 120\nswing = 0.5\nlength = 16\nscene = 1\nselected = 1\n";

    for (int i = 0; i < Project::kMaxSlices + 1; ++i)
        manual += kSliceLine + "\n";

    manual += "end = " + std::to_string (Project::kMaxSlices + 1) + " 0\n";
    const auto tooMany = parseProject (manual);
    EXPECT (tooMany.status == ProjectParse::Status::damaged);
    EXPECT (tooMany.error.find ("too many") != std::string::npos);
}

KTEST (project_file, a_project_that_cannot_be_read_back_is_not_written)
{
    Project p = makeProject();
    std::string text = "stale";

    p.tempo = std::nanf ("");
    EXPECT (! serializeProject (p, text));
    EXPECT (text.empty());

    p = makeProject();
    p.swing = INFINITY;
    EXPECT (! serializeProject (p, text));

    for (int field = 0; field < 7; ++field)
    {
        p = makeProject();
        float* targets[] = { &p.slices[1].start, &p.slices[1].end, &p.slices[1].pitch, &p.slices[1].volume, &p.slices[1].pan,
                             &p.slices[1].fadeInMs, &p.slices[1].fadeOutMs };
        *targets[field] = std::nanf ("");
        EXPECT (! serializeProject (p, text));
        *targets[field] = -INFINITY;
        EXPECT (! serializeProject (p, text));
    }
}

KTEST (project_file, with_no_slices_nothing_is_selected)
{
    Project p;
    p.frames = 1;
    p.rate = 1;
    p.selected = 3;                                  // (whatever the cursor says, there is nothing to select)

    EXPECT (write (p).find ("selected = 0\n") != std::string::npos);

    p.slices = { makeSlice (0.0f, 0.1f), makeSlice (0.1f, 0.2f), makeSlice (0.2f, 0.3f), makeSlice (0.3f, 0.4f) };
    EXPECT (write (p).find ("selected = 4\n") != std::string::npos);
}

KTEST (project_file, only_rows_with_a_slice_are_written)
{
    Project p = makeProject();
    p.grid.bits[0][50] = 0xFFFF;       // a pattern for a slice that does not exist
    const auto text = write (p);
    EXPECT (text.find ("pattern = 1 51") == std::string::npos);

    const auto parsed = parseProject (text);
    EXPECT (parsed.status == ProjectParse::Status::ok);
    EXPECT_EQ (parsed.project.grid.bits[0][50], 0);
    EXPECT_EQ (parsed.project.patternCount(), 4);
}

KTEST (project_file, patterns_are_written_in_scene_then_row_order)
{
    Project p;
    p.frames = 1;
    p.rate = 1;
    p.slices.assign (4, makeSlice (0.0f, 0.1f));
    p.grid.bits[3][0] = 1;
    p.grid.bits[0][3] = 2;
    p.grid.bits[0][1] = 4;
    p.grid.bits[3][2] = 8;

    std::vector<std::string> patterns;

    for (auto& l : dataLines (write (p)))
        if (startsWith (l, "pattern"))
            patterns.push_back (l.substr (10, 3));

    const std::vector<std::string> expected = { "1 2", "1 4", "4 1", "4 3" };
    EXPECT (patterns == expected);
}

// ============================================================================== belonging

KTEST (project_belongs, same_length_and_rate_only)
{
    Project p;
    p.frames = 1000;
    p.rate = 44100;

    Sample s;
    s.data.resize (2, 1000);
    s.sampleRate = 44100.0;
    EXPECT (p.belongsTo (s));

    s.name = "another name";
    EXPECT (p.belongsTo (s));                       // the name is not part of it

    Sample longer;
    longer.data.resize (2, 1001);
    longer.sampleRate = 44100.0;
    EXPECT (! p.belongsTo (longer));

    Sample shorter;
    shorter.data.resize (2, 999);
    shorter.sampleRate = 44100.0;
    EXPECT (! p.belongsTo (shorter));

    Sample otherRate;
    otherRate.data.resize (2, 1000);
    otherRate.sampleRate = 48000.0;
    EXPECT (! p.belongsTo (otherRate));

    Sample mono;
    mono.data.resize (1, 1000);
    mono.sampleRate = 44100.0;
    EXPECT (p.belongsTo (mono));                    // mono or stereo does not matter

    Sample rounded;
    rounded.data.resize (2, 1000);
    rounded.sampleRate = 44100.0000001;
    EXPECT (p.belongsTo (rounded));

    // A rate is a whole number of hertz; one that is not quite is rounded to the nearest, not cut.
    rounded.sampleRate = 44099.6;
    EXPECT (p.belongsTo (rounded));
    rounded.sampleRate = 44100.4;
    EXPECT (p.belongsTo (rounded));
    rounded.sampleRate = 44100.6;
    EXPECT (! p.belongsTo (rounded));
    rounded.sampleRate = 44099.4;
    EXPECT (! p.belongsTo (rounded));
}

KTEST (project_pattern_count, counts_the_lit_rows_that_have_a_slice)
{
    Project p;
    EXPECT_EQ (p.patternCount(), 0);

    p.slices = { makeSlice (0.0f, 0.1f), makeSlice (0.1f, 0.2f) };
    p.grid.bits[0][0] = 1;
    p.grid.bits[0][1] = 0x8000;
    p.grid.bits[7][1] = 0xFFFF;
    EXPECT_EQ (p.patternCount(), 3);

    // A row without a slice does not count, whatever it holds.
    p.grid.bits[0][2] = 1;
    p.grid.bits[3][127] = 1;
    EXPECT_EQ (p.patternCount(), 3);

    // With more slices than rows, the rows are all there are (and nothing is read beyond the grid).
    Project many;
    many.slices.assign (130, makeSlice (0.0f, 0.1f));
    many.grid.bits[1][0] = 1;
    EXPECT_EQ (many.patternCount(), 1);
    many.grid.bits[7][127] = 5;
    EXPECT_EQ (many.patternCount(), 2);
}

// ================================================================================ sanitize

KTEST (project_sanitize, slices_are_pulled_into_the_sample)
{
    Project p;
    p.slices = { makeSlice (-1.0f, 0.5f), makeSlice (0.5f, 99.0f), makeSlice (3.0f, 2.0f), makeSlice (7.0f, 8.0f), makeSlice (0.25f, 0.75f) };
    p.sanitize (4.0);

    EXPECT_EQ (p.slices[0].start, 0.0);
    EXPECT_EQ (p.slices[0].end, 0.5);
    EXPECT_EQ (p.slices[1].start, 0.5);
    EXPECT_EQ (p.slices[1].end, 4.0);
    EXPECT_EQ (p.slices[2].start, 3.0);
    EXPECT_EQ (p.slices[2].end, 3.0);           // it runs forwards: an end before the start is the start
    EXPECT_EQ (p.slices[3].start, 4.0);         // wholly after the end of the sample
    EXPECT_EQ (p.slices[3].end, 4.0);
    EXPECT_EQ (p.slices[4].start, 0.25);        // inside: untouched
    EXPECT_EQ (p.slices[4].end, 0.75);
}

KTEST (project_sanitize, parameters_are_brought_into_the_range_of_the_controls)
{
    Project p;
    Slice s = makeSlice (0.0f, 1.0f);
    s.pitch = 100.0f;
    s.volume = 5.0f;
    s.pan = -3.0f;
    s.fadeInMs = 5000.0f;
    s.fadeOutMs = 5000.0f;
    Slice t = makeSlice (0.0f, 1.0f);
    t.pitch = 0.0f;
    t.volume = -1.0f;
    t.pan = 3.0f;
    t.fadeInMs = -5.0f;
    t.fadeOutMs = -5.0f;
    p.slices = { s, t };
    p.sanitize (4.0);

    EXPECT_EQ (p.slices[0].pitch, 4.0);
    EXPECT_EQ (p.slices[0].volume, 1.0);
    EXPECT_EQ (p.slices[0].pan, -1.0);
    EXPECT_EQ (p.slices[0].fadeInMs, 100.0);
    EXPECT_EQ (p.slices[0].fadeOutMs, 200.0);
    EXPECT_EQ (p.slices[1].pitch, 0.25);
    EXPECT_EQ (p.slices[1].volume, 0.0);
    EXPECT_EQ (p.slices[1].pan, 1.0);
    EXPECT_EQ (p.slices[1].fadeInMs, 0.0);
    EXPECT_EQ (p.slices[1].fadeOutMs, 0.0);
}

KTEST (project_sanitize, numbers_that_are_not_numbers_become_the_defaults)
{
    Project p;
    Slice s;
    s.start = std::nanf ("");
    s.end = INFINITY;
    s.pitch = std::nanf ("");
    s.volume = INFINITY;
    s.pan = -INFINITY;
    s.fadeInMs = std::nanf ("");
    s.fadeOutMs = std::nanf ("");
    p.slices = { s };
    p.tempo = std::nanf ("");
    p.swing = INFINITY;
    p.sanitize (2.0);

    EXPECT_EQ (p.slices[0].start, 0.0);
    EXPECT_EQ (p.slices[0].end, 2.0);
    EXPECT_EQ (p.slices[0].pitch, 1.0);
    EXPECT_EQ (p.slices[0].volume, 1.0);
    EXPECT_EQ (p.slices[0].pan, 0.0);
    EXPECT_EQ (p.slices[0].fadeInMs, 1.0);
    EXPECT_EQ (p.slices[0].fadeOutMs, 50.0);
    EXPECT_EQ (p.tempo, 120.0);
    EXPECT_EQ (p.swing, 0.5);

    Project q;
    q.slices = { makeSlice (0.5f, 1.0f) };
    q.sanitize (std::nan (""));                  // a sample whose length is unknown has no room for anything
    EXPECT_EQ (q.slices[0].end, 0.0);
    q.sanitize (-5.0);
    EXPECT_EQ (q.slices[0].start, 0.0);
}

KTEST (project_sanitize, the_sequencer_settings_and_the_cursor_are_clamped)
{
    Project p;
    p.slices.assign (3, makeSlice (0.0f, 1.0f));
    p.tempo = 5000.0f;
    p.swing = 0.99f;
    p.length = 99;
    p.scene = 99;
    p.selected = 99;
    p.sanitize (4.0);

    EXPECT_EQ (p.tempo, 300.0);
    EXPECT_NEAR (p.swing, 0.75, 1.0e-6);
    EXPECT_EQ (p.length, 16);
    EXPECT_EQ (p.scene, 7);
    EXPECT_EQ (p.selected, 2);

    p.tempo = 1.0f;
    p.swing = 0.1f;
    p.length = -4;
    p.scene = -4;
    p.selected = -4;
    p.sanitize (4.0);

    EXPECT_EQ (p.tempo, 40.0);
    EXPECT_EQ (p.swing, 0.5);
    EXPECT_EQ (p.length, 1);
    EXPECT_EQ (p.scene, 0);
    EXPECT_EQ (p.selected, 0);

    Project none;
    none.selected = 5;
    none.sanitize (4.0);
    EXPECT_EQ (none.selected, 0);
}

KTEST (project_sanitize, a_pattern_exists_only_where_a_slice_does)
{
    Project p;
    p.slices.assign (3, makeSlice (0.0f, 1.0f));

    for (int s = 0; s < StepSequencer::kScenes; ++s)
        for (int r = 0; r < StepSequencer::kRows; ++r)
            p.grid.bits[s][r] = 0xFFFF;

    p.sanitize (4.0);

    for (int s = 0; s < StepSequencer::kScenes; ++s)
        for (int r = 0; r < StepSequencer::kRows; ++r)
            EXPECT_EQ (p.grid.bits[s][r], r < 3 ? 0xFFFF : 0);

    EXPECT_EQ (p.patternCount(), 3 * StepSequencer::kScenes);

    // With 200 slices, all 128 rows are there.
    Project many;
    many.slices.assign (200, makeSlice (0.0f, 1.0f));

    for (int r = 0; r < StepSequencer::kRows; ++r)
        many.grid.bits[7][r] = 1;

    many.sanitize (4.0);
    EXPECT_EQ (many.patternCount(), StepSequencer::kRows);
    EXPECT_EQ (many.grid.bits[7][127], 1);
}

KTEST (project_sanitize, values_the_controls_make_are_left_alone)
{
    Project p;
    p.tempo = 123.0f;
    p.swing = 0.667f;

    // Every pitch the controls allow, every volume and pan step, every fade.
    for (int n = -24; n <= 24; ++n)
    {
        Slice s = makeSlice (0.1f * (float) (n + 24), 0.1f * (float) (n + 24) + 0.05f);
        s.pitch = (float) std::pow (2.0, n / 12.0);
        s.volume = std::round ((float) (n + 24) / 48.0f * 20.0f) / 20.0f;
        s.pan = std::round ((float) n / 24.0f * 10.0f) / 10.0f;
        s.fadeInMs = (float) (n + 24) * 100.0f / 48.0f;
        s.fadeOutMs = (float) (n + 24) * 200.0f / 48.0f;
        p.slices.push_back (s);
    }

    const Project before = p;
    p.sanitize (6.0);
    EXPECT (identical (p, before));

    // ... and doing it again changes nothing.
    Project wild = makeProject();
    wild.slices[0].start = -4.0f;
    wild.slices[1].end = 1.0e6f;
    wild.slices[2].pitch = 77.0f;
    wild.sanitize (5.0);
    const Project once = wild;
    wild.sanitize (5.0);
    EXPECT (identical (wild, once));
}

KTEST (project_sanitize, slices_keep_their_order)
{
    Project p;
    p.slices = { makeSlice (3.0f, 3.5f), makeSlice (1.0f, 2.0f), makeSlice (2.0f, 2.5f) };
    p.sanitize (10.0);

    EXPECT_EQ (p.slices[0].start, 3.0);
    EXPECT_EQ (p.slices[1].start, 1.0);
    EXPECT_EQ (p.slices[2].start, 2.0);
}

// ================================================================================ same work

KTEST (project_same_work, equal_projects_are_the_same)
{
    const Project a = makeProject();
    Project b = a;
    EXPECT (sameWork (a, b));
    EXPECT (sameWork (a, a));
    EXPECT (sameWork (Project(), Project()));
}

KTEST (project_same_work, every_change_of_the_work_is_seen)
{
    const Project base = makeProject();

    // Every field of every slice.
    for (std::size_t i = 0; i < base.slices.size(); ++i)
    {
        const auto differs = [&] (const std::function<void (Slice&)>& change)
        {
            Project q = base;
            change (q.slices[i]);
            return ! sameWork (base, q) && ! sameWork (q, base);
        };

        EXPECT (differs ([] (Slice& s) { s.start += 0.001f; }));
        EXPECT (differs ([] (Slice& s) { s.end += 0.001f; }));
        EXPECT (differs ([] (Slice& s) { s.pitch *= 1.5f; }));
        EXPECT (differs ([] (Slice& s) { s.volume = s.volume > 0.5f ? 0.25f : 0.75f; }));
        EXPECT (differs ([] (Slice& s) { s.pan = s.pan > 0.0f ? -0.5f : 0.5f; }));
        EXPECT (differs ([] (Slice& s) { s.mode = s.mode == Slice::Mode::shot ? Slice::Mode::loop : Slice::Mode::shot; }));
        EXPECT (differs ([] (Slice& s) { s.fadeInMs += 1.0f; }));
        EXPECT (differs ([] (Slice& s) { s.fadeOutMs += 1.0f; }));
        EXPECT (differs ([] (Slice& s) { s.enabled = ! s.enabled; }));
    }

    // Slices added or taken away.
    Project q = base;
    q.slices.push_back (makeSlice (0.0f, 1.0f));
    EXPECT (! sameWork (base, q));
    q = base;
    q.slices.pop_back();
    EXPECT (! sameWork (base, q));

    // The sequencer.
    q = base; q.tempo += 1.0f;   EXPECT (! sameWork (base, q));
    q = base; q.swing += 0.01f;  EXPECT (! sameWork (base, q));
    q = base; q.length -= 1;     EXPECT (! sameWork (base, q));

    // One step, anywhere in the grid.
    for (int scene : { 0, 3, 7 })
        for (int row : { 0, 1, 2, 77, 127 })
            for (int step : { 0, 7, 15 })
            {
                q = base;
                q.grid.bits[scene][row] = (std::uint16_t) (q.grid.bits[scene][row] ^ (1u << step));
                EXPECT (! sameWork (base, q));
            }
}

KTEST (project_same_work, moving_around_is_not_a_change)
{
    const Project base = makeProject();
    Project q = base;

    q.selected = 0;
    q.scene = 5;
    q.audio = "another.wav";
    q.frames = 5;
    q.rate = 8000;
    EXPECT (sameWork (base, q));
}

// ================================================================================== engine

namespace
{
    SamplePtr makeSample (int frames, double rate)
    {
        auto s = std::make_shared<Sample>();
        s->data.resize (2, frames);
        s->sampleRate = rate;
        s->name = "loop.wav";
        return s;
    }

    struct EngineRig
    {
        Engine engine;

        explicit EngineRig (int frames = 4 * 44100, double rate = 44100.0)
        {
            engine.prepare (rate);
            engine.loadSample (makeSample (frames, rate));
        }
    };
}

KTEST (project_engine, capture_reads_the_state_of_the_engine)
{
    EngineRig rig;
    auto& e = rig.engine;

    e.slices().replaceAll ({ makeSlice (0.5f, 1.0f), makeSlice (1.0f, 2.0f), makeSlice (2.0f, 3.0f) });
    e.setSelectedSlice (2);
    e.sequencer().setTempo (150.0f);
    e.sequencer().setSwing (0.6f);
    e.sequencer().setLength (10);
    e.sequencer().setScene (4);
    e.sequencer().setCell (0, 1, 3, true);
    e.sequencer().setCell (6, 2, 15, true);

    const Project p = captureProject (e);
    EXPECT (p.audio == "loop.wav");
    EXPECT_EQ (p.frames, 4 * 44100);
    EXPECT_EQ (p.rate, 44100);
    EXPECT_EQ (p.slices.size(), 3u);
    EXPECT_EQ (p.slices[1].start, 1.0);
    EXPECT_EQ (p.selected, 2);
    EXPECT_EQ (p.tempo, 150.0);
    EXPECT_NEAR (p.swing, 0.6, 1.0e-6);
    EXPECT_EQ (p.length, 10);
    EXPECT_EQ (p.scene, 4);
    EXPECT_EQ (p.grid.bits[0][1], 8);
    EXPECT_EQ (p.grid.bits[6][2], 0x8000);
    EXPECT_EQ (p.patternCount(), 2);
}

KTEST (project_engine, capture_without_a_sample_is_empty)
{
    Engine e;
    const Project p = captureProject (e);
    EXPECT_EQ (p.frames, 0);
    EXPECT_EQ (p.rate, 0);
    EXPECT (p.slices.empty());
}

KTEST (project_engine, capture_leaves_out_patterns_of_rows_without_a_slice)
{
    EngineRig rig;
    auto& e = rig.engine;
    e.slices().replaceAll ({ makeSlice (0.0f, 1.0f), makeSlice (1.0f, 2.0f) });
    e.sequencer().setCell (0, 1, 0, true);
    e.sequencer().setCell (0, 5, 0, true);        // no sixth slice

    const Project p = captureProject (e);
    EXPECT_EQ (p.grid.bits[0][1], 1);
    EXPECT_EQ (p.grid.bits[0][5], 0);
    EXPECT_EQ (p.patternCount(), 1);
}

KTEST (project_engine, capture_is_in_range_even_when_the_engine_is_not)
{
    EngineRig rig;
    auto& e = rig.engine;
    Slice s = makeSlice (-3.0f, 99.0f);
    s.pitch = 9.0f;
    e.slices().replaceAll ({ s });

    const Project p = captureProject (e);
    EXPECT_EQ (p.slices[0].start, 0.0);
    EXPECT_EQ (p.slices[0].end, 4.0);
    EXPECT_EQ (p.slices[0].pitch, 4.0);
}

KTEST (project_engine, apply_puts_everything_into_the_engine)
{
    EngineRig rig;
    auto& e = rig.engine;

    Project p = makeProject();
    p.frames = 4 * 44100;
    p.slices = { makeSlice (0.5f, 1.0f), makeSlice (1.0f, 2.0f), makeSlice (2.0f, 3.0f) };
    p.slices[1].pitch = 2.0f;
    p.slices[1].mode = Slice::Mode::loop;
    p.selected = 2;
    p.scene = 6;
    p.tempo = 90.5f;
    p.swing = 0.7f;
    p.length = 9;
    p.grid = StepSequencer::Grid();
    p.grid.bits[2][1] = 0x0F0F;
    p.grid.bits[7][2] = 0x0001;

    EXPECT (applyProject (e, p));

    const auto slices = e.slices().getCurrent();
    EXPECT_EQ (slices->size(), 3u);
    EXPECT_EQ ((*slices)[1].pitch, 2.0);
    EXPECT ((*slices)[1].mode == Slice::Mode::loop);
    EXPECT_EQ (e.selectedSlice(), 2);
    EXPECT_EQ (e.sequencer().scene(), 6);
    EXPECT_EQ (e.sequencer().tempo(), 90.5);
    EXPECT_NEAR (e.sequencer().swing(), 0.7, 1.0e-6);
    EXPECT_EQ (e.sequencer().length(), 9);
    EXPECT_EQ (e.sequencer().rowBits (2, 1), 0x0F0F);
    EXPECT_EQ (e.sequencer().rowBits (7, 2), 1);
    EXPECT_EQ (e.sequencer().cellCount (0), 0);
    EXPECT (! e.sequencer().isPlaying());
}

KTEST (project_engine, apply_replaces_what_was_there)
{
    EngineRig rig;
    auto& e = rig.engine;
    e.slices().replaceAll ({ makeSlice (0.0f, 1.0f), makeSlice (1.0f, 2.0f), makeSlice (2.0f, 3.0f), makeSlice (3.0f, 4.0f) });
    e.sequencer().setCell (0, 3, 0, true);
    e.sequencer().setCell (4, 0, 9, true);

    Project p;
    p.frames = 4 * 44100;
    p.rate = 44100;
    p.slices = { makeSlice (0.5f, 0.75f) };
    p.grid.bits[1][0] = 0x0010;

    EXPECT (applyProject (e, p));
    EXPECT_EQ (e.slices().size(), 1);
    EXPECT_EQ (e.sequencer().cellCount (0), 0);
    EXPECT_EQ (e.sequencer().cellCount (4), 0);
    EXPECT_EQ (e.sequencer().rowBits (1, 0), 0x0010);
}

KTEST (project_engine, apply_refuses_another_sample_and_changes_nothing)
{
    Project p = makeProject();
    p.frames = 4 * 44100;
    p.rate = 44100;

    const auto untouched = [] (Engine& e)
    {
        return e.slices().size() == 1 && e.slices().getCurrent()->front().start == 0.25f && e.sequencer().tempo() == 111.0f
               && e.sequencer().cellCount (0) == 1 && e.selectedSlice() == 0;
    };

    const auto prepare = [] (Engine& e)
    {
        e.slices().replaceAll ({ makeSlice (0.25f, 0.5f) });
        e.sequencer().setTempo (111.0f);
        e.sequencer().setCell (0, 0, 0, true);
    };

    {
        EngineRig other (4 * 44100 + 1, 44100.0);        // one frame longer
        prepare (other.engine);
        EXPECT (! applyProject (other.engine, p));
        EXPECT (untouched (other.engine));
    }
    {
        EngineRig other (4 * 44100, 48000.0);            // another rate
        prepare (other.engine);
        EXPECT (! applyProject (other.engine, p));
        EXPECT (untouched (other.engine));
    }
    {
        Engine none;                                     // no sample at all
        EXPECT (! applyProject (none, p));
        EXPECT_EQ (none.slices().size(), 0);
    }
    {
        EngineRig same;
        prepare (same.engine);
        EXPECT (applyProject (same.engine, p));
        EXPECT (! untouched (same.engine));
    }
}

KTEST (project_engine, apply_keeps_the_engine_in_range)
{
    EngineRig rig;
    auto& e = rig.engine;

    Project p;
    p.frames = 4 * 44100;
    p.rate = 44100;
    p.slices = { makeSlice (-1.0f, 100.0f), makeSlice (9.0f, 3.0f) };
    p.slices[0].pitch = 40.0f;
    p.tempo = 9999.0f;
    p.length = 0;
    p.scene = 50;
    p.selected = 50;
    p.grid.bits[0][9] = 0xFFFF;         // a row with no slice

    EXPECT (applyProject (e, p));
    const auto slices = e.slices().getCurrent();
    EXPECT_EQ ((*slices)[0].start, 0.0);
    EXPECT_EQ ((*slices)[0].end, 4.0);
    EXPECT_EQ ((*slices)[0].pitch, 4.0);
    EXPECT_EQ ((*slices)[1].start, 4.0);
    EXPECT_EQ ((*slices)[1].end, 4.0);
    EXPECT_EQ (e.sequencer().tempo(), 300.0);
    EXPECT_EQ (e.sequencer().length(), 1);
    EXPECT_EQ (e.sequencer().scene(), 7);
    EXPECT_EQ (e.selectedSlice(), 1);
    EXPECT_EQ (e.sequencer().rowBits (0, 9), 0);
}

KTEST (project_engine, what_is_captured_can_be_applied_again_through_the_text)
{
    EngineRig first;
    auto& a = first.engine;
    a.slices().replaceAll ({ makeSlice (0.1f, 0.7f), makeSlice (0.7f, 1.9f), makeSlice (1.9f, 3.99f) });
    a.setSelectedSlice (1);
    a.sequencer().setTempo (97.3f);
    a.sequencer().setSwing (0.58f);
    a.sequencer().setLength (14);
    a.sequencer().setScene (3);
    a.sequencer().setCell (0, 0, 0, true);
    a.sequencer().setCell (3, 2, 15, true);
    a.sequencer().setCell (7, 1, 8, true);
    a.slices().updateAt (1, [] (Slice& s) { s.pitch = 1.5f; s.pan = 0.4f; s.mode = Slice::Mode::loop; s.enabled = false; });

    std::string text;
    EXPECT (serializeProject (captureProject (a), text));
    const auto parsed = parseProject (text);
    EXPECT (parsed.status == ProjectParse::Status::ok);

    EngineRig second;
    EXPECT (applyProject (second.engine, parsed.project));
    EXPECT (identical (captureProject (second.engine), captureProject (a)));
}

KTEST (project_engine, a_new_sample_empties_the_engine_for_a_project_to_come_in)
{
    // What the application relies on when it brings a project back right after loading a sample.
    EngineRig rig;
    auto& e = rig.engine;
    e.slices().replaceAll ({ makeSlice (0.0f, 1.0f) });
    e.sequencer().setCell (0, 0, 0, true);
    e.sequencer().setTempo (77.0f);
    e.sequencer().setScene (3);
    e.setSelectedSlice(0);

    EXPECT (e.loadSample (makeSample (2 * 44100, 44100.0)));
    EXPECT_EQ (e.slices().size(), 0);
    EXPECT_EQ (e.sequencer().cellCount (0), 0);
    EXPECT_EQ (e.sequencer().scene(), 0);
    EXPECT_EQ (e.sequencer().tempo(), 77.0);          // tempo is the person's, not the sample's
    EXPECT (! e.sequencer().isPlaying());
}

KTEST (project_engine, moving_around_does_not_change_the_work)
{
    EngineRig rig;
    auto& e = rig.engine;
    e.slices().replaceAll ({ makeSlice (0.0f, 1.0f), makeSlice (1.0f, 2.0f) });
    e.sequencer().setCell (0, 0, 0, true);

    const Project base = captureProject (e);

    e.setSelectedSlice (1);
    e.sequencer().setScene (5);
    EXPECT (sameWork (base, captureProject (e)));
    EXPECT (captureProject (e).selected == 1 && captureProject (e).scene == 5);

    e.sequencer().setCell (0, 1, 4, true);
    EXPECT (! sameWork (base, captureProject (e)));
}
