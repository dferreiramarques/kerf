// Tests for the handheld's UI-independent building blocks: text handling, the software canvas,
// the waveform summary, input tracking, settings and the file browser.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>

#include "canvas.h"
#include "config.h"
#include "file_browser.h"
#include "format.h"
#include "help_text.h"
#include "input.h"
#include "minitest.h"
#include "path_util.h"
#include "png_writer.h"
#include "utf8.h"
#include "waveform_cache.h"

using namespace kerf;
using namespace kerf::ui;
namespace fs = std::filesystem;

namespace
{
    SamplePtr makeSample (int frames, int channels, double rate, std::function<float (int, int)> fn)
    {
        auto s = std::make_shared<Sample>();
        s->data.resize (channels, frames);
        s->sampleRate = rate;
        s->name = "t";

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                s->data.set (ch, i, fn (ch, i));

        return s;
    }
}

// ------------------------------------------------------------------------------------------ utf8

KTEST (Utf8, decodes_ascii_latin1_and_survives_garbage)
{
    const std::string s = "a\xC3\xA7\xC3\xA3o"; // "acao" with c-cedilla and a-tilde
    size_t i = 0;
    EXPECT_EQ ((int) nextCodePoint (s, i), (int) 'a');
    EXPECT_EQ ((int) nextCodePoint (s, i), 0xE7);
    EXPECT_EQ ((int) nextCodePoint (s, i), 0xE3);
    EXPECT_EQ ((int) nextCodePoint (s, i), (int) 'o');
    EXPECT_EQ ((int) i, (int) s.size());

    // A lone continuation byte, a truncated sequence and an overlong tail must all terminate.
    const std::string bad = "\x80\xC3\xE2\x82";
    size_t j = 0;
    int guard = 0;
    while (j < bad.size() && guard++ < 10)
        nextCodePoint (bad, j);
    EXPECT (j == bad.size());
}

KTEST (Utf8, counts_glyphs_and_elides_without_cutting_a_character)
{
    EXPECT_EQ (countGlyphs ("Grava\xC3\xA7\xC3\xA3o"), 8);
    EXPECT (elide ("short", 10) == "short");
    EXPECT (elide ("abcdefghij", 8) == "abcde...");
    EXPECT (elide ("Grava\xC3\xA7\xC3\xA3o final.wav", 10) == "Grava\xC3\xA7\xC3\xA3...");
    EXPECT (elide ("abc", 0).empty());
    EXPECT (elide ("abcdef", 2) == "..");
}

// ---------------------------------------------------------------------------------------- canvas

KTEST (Canvas, fills_clips_and_blends)
{
    Canvas c (20, 10);
    c.clear (rgb (0, 0, 0));
    c.fillRect (2, 2, 4, 3, rgb (255, 0, 0));
    EXPECT_EQ ((int) c.pixelAt (2, 2), (int) rgb (255, 0, 0));
    EXPECT_EQ ((int) c.pixelAt (5, 4), (int) rgb (255, 0, 0));
    EXPECT_EQ ((int) c.pixelAt (6, 4), (int) rgb (0, 0, 0));

    // Out-of-range drawing is ignored rather than crashing.
    c.fillRect (-5, -5, 100, 100, rgb (0, 0, 255));
    EXPECT_EQ ((int) c.pixelAt (0, 0), (int) rgb (0, 0, 255));
    EXPECT_EQ ((int) c.pixelAt (19, 9), (int) rgb (0, 0, 255));
    c.plot (-1, 3, rgb (1, 2, 3));
    c.plot (3, 100, rgb (1, 2, 3));

    c.clear (rgb (0, 0, 0));
    c.setClip (5, 5, 3, 3);
    c.fillRect (0, 0, 20, 10, rgb (0, 255, 0));
    EXPECT_EQ ((int) c.pixelAt (4, 5), (int) rgb (0, 0, 0));
    EXPECT_EQ ((int) c.pixelAt (5, 5), (int) rgb (0, 255, 0));
    EXPECT_EQ ((int) c.pixelAt (7, 7), (int) rgb (0, 255, 0));
    EXPECT_EQ ((int) c.pixelAt (8, 7), (int) rgb (0, 0, 0));
    c.resetClip();

    c.clear (rgb (100, 100, 100));
    c.fillRect (0, 0, 2, 2, argb (128, 200, 200, 200)); // ~50% white over mid-grey
    const auto p = c.pixelAt (0, 0);
    EXPECT_NEAR ((int) ((p >> 16) & 255), 150, 2);
}

KTEST (Canvas, draws_text_with_the_bitmap_font)
{
    Canvas c (64, 16);
    c.clear (rgb (0, 0, 0));
    const int endX = c.drawText (0, 0, "A", rgb (255, 255, 255));
    EXPECT_EQ (endX, 8);

    // 'A' in the 8x8 font: top row is 0x0C -> pixels 2 and 3 lit, pixel 0 dark.
    EXPECT_EQ ((int) c.pixelAt (0, 0), (int) rgb (0, 0, 0));
    EXPECT_EQ ((int) c.pixelAt (2, 0), (int) rgb (255, 255, 255));
    EXPECT_EQ ((int) c.pixelAt (3, 0), (int) rgb (255, 255, 255));
    EXPECT_EQ ((int) c.pixelAt (4, 0), (int) rgb (0, 0, 0));

    // Scale 2 doubles each dot and the advance.
    c.clear (rgb (0, 0, 0));
    EXPECT_EQ (c.drawText (0, 0, "A", rgb (255, 255, 255), 2), 16);
    EXPECT_EQ ((int) c.pixelAt (4, 0), (int) rgb (255, 255, 255));
    EXPECT_EQ ((int) c.pixelAt (5, 1), (int) rgb (255, 255, 255));
    EXPECT_EQ ((int) c.pixelAt (3, 0), (int) rgb (0, 0, 0));

    EXPECT_EQ (Canvas::textWidth ("Grava\xC3\xA7\xC3\xA3o", 2), 8 * 16);

    // Accented letters have glyphs of their own (they must not collapse to '?').
    Canvas a (16, 8), q (16, 8);
    a.clear (rgb (0, 0, 0)); q.clear (rgb (0, 0, 0));
    a.drawText (0, 0, "\xC3\xA7", rgb (255, 255, 255));   // c-cedilla
    q.drawText (0, 0, "?", rgb (255, 255, 255));
    EXPECT (! std::equal (a.pixels(), a.pixels() + 16 * 8, q.pixels()));

    // Characters outside ASCII+Latin-1 fall back to '?', and text crossing the edge is clipped.
    Canvas z (16, 8);
    z.clear (rgb (0, 0, 0));
    z.drawText (12, 0, "\xE2\x82\xAC\xE2\x82\xAC", rgb (255, 255, 255)); // euro signs, partly off-canvas
}

KTEST (Canvas, writes_a_valid_png)
{
    Canvas c (16, 8);
    c.clear (rgb (10, 20, 30));
    c.fillRect (4, 2, 4, 4, rgb (200, 100, 50));

    const auto path = (fs::temp_directory_path() / "kerf-canvas-test.png").string();
    EXPECT (c.savePng (path));

    std::ifstream in (path, std::ios::binary);
    std::string bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    in.close();

    EXPECT (bytes.size() > 100);
    EXPECT (bytes.compare (0, 8, "\x89PNG\r\n\x1a\n", 8) == 0);
    EXPECT (bytes.find ("IHDR") != std::string::npos);
    EXPECT (bytes.find ("IDAT") != std::string::npos);
    EXPECT (bytes.find ("IEND") != std::string::npos);

    // Width and height sit big-endian right after "IHDR".
    const auto h = bytes.find ("IHDR");
    EXPECT_EQ ((int) (unsigned char) bytes[h + 7], 16);
    EXPECT_EQ ((int) (unsigned char) bytes[h + 11], 8);

    std::error_code ec;
    fs::remove (path, ec);
}

// ---------------------------------------------------------------------------- waveform summary

namespace
{
    void bruteForce (const Sample& s, long long a, long long b, float& lo, float& hi)
    {
        lo = 1e9f; hi = -1e9f;

        for (int ch = 0; ch < s.data.numChannels(); ++ch)
            for (long long i = a; i < b; ++i)
            {
                lo = std::min (lo, s.data.get (ch, (int) i));
                hi = std::max (hi, s.data.get (ch, (int) i));
            }
    }
}

KTEST (WaveformCache, matches_a_brute_force_scan_when_zoomed_in)
{
    std::mt19937 rng (7);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    auto s = makeSample (50000, 2, 44100.0, [&] (int, int) { return dist (rng); });

    WaveformCache cache;
    cache.build (s);
    EXPECT (cache.valid());

    // 30 frames per pixel is below the first block size, so the cache reads raw audio: exact.
    const int width = 100;
    std::vector<WaveformCache::Column> cols ((size_t) width);
    const double t0 = 0.1, t1 = t0 + 3000.0 / 44100.0;
    cache.columns (t0, t1, width, cols.data());

    for (int x = 0; x < width; ++x)
    {
        const double fpp = 3000.0 / width;
        const long long a = (long long) std::floor (t0 * 44100.0 + fpp * x);
        const long long b = std::max (a + 1, (long long) std::ceil (t0 * 44100.0 + fpp * (x + 1)));
        float lo, hi;
        bruteForce (*s, a, b, lo, hi);
        EXPECT (cols[(size_t) x].valid);
        EXPECT_EQ (cols[(size_t) x].lo, lo);
        EXPECT_EQ (cols[(size_t) x].hi, hi);
    }
}

KTEST (WaveformCache, never_underestimates_when_zoomed_out_and_stays_close)
{
    std::mt19937 rng (11);
    std::uniform_real_distribution<float> dist (-1.0f, 1.0f);
    const int frames = 441000; // 10 s
    auto s = makeSample (frames, 2, 44100.0, [&] (int, int) { return dist (rng); });

    WaveformCache cache;
    cache.build (s);

    const int width = 600;
    std::vector<WaveformCache::Column> cols ((size_t) width);

    for (double span : { 10.0, 4.0, 1.0, 0.25 })
    {
        const double t0 = 0.0, t1 = span;
        cache.columns (t0, t1, width, cols.data());
        const double fpp = span * 44100.0 / width;

        for (int x = 0; x < width; ++x)
        {
            const long long a = (long long) std::floor (fpp * x);
            const long long b = std::max (a + 1, (long long) std::ceil (fpp * (x + 1)));
            float lo, hi;
            bruteForce (*s, a, std::min<long long> (b, frames), lo, hi);

            EXPECT (cols[(size_t) x].valid);
            // A superset of the exact range ...
            EXPECT (cols[(size_t) x].lo <= lo + 1e-6f);
            EXPECT (cols[(size_t) x].hi >= hi - 1e-6f);
            // ... and for noise (extremes everywhere) never wildly larger than the sample range.
            EXPECT (cols[(size_t) x].lo >= -1.0f && cols[(size_t) x].hi <= 1.0f);
        }
    }
}

KTEST (WaveformCache, finds_a_single_spike_at_any_zoom)
{
    const int frames = 2000000;
    auto s = makeSample (frames, 1, 44100.0, [&] (int, int i) { return i == 1234567 ? 0.9f : 0.0f; });

    WaveformCache cache;
    cache.build (s);

    std::vector<WaveformCache::Column> cols (320);
    cache.columns (0.0, s->durationSeconds(), 320, cols.data());

    int hits = 0;
    for (auto& c : cols)
        if (c.hi > 0.5f) ++hits;

    EXPECT (hits >= 1 && hits <= 2);

    const int expectedColumn = (int) (1234567.0 / frames * 320.0);
    EXPECT (cols[(size_t) expectedColumn].hi > 0.5f || cols[(size_t) std::max (0, expectedColumn - 1)].hi > 0.5f);
}

KTEST (WaveformCache, marks_columns_outside_the_audio_as_invalid)
{
    auto s = makeSample (44100, 1, 44100.0, [&] (int, int) { return 0.5f; }); // 1 s
    WaveformCache cache;
    cache.build (s);

    std::vector<WaveformCache::Column> cols (100);
    cache.columns (-0.5, 1.5, 100, cols.data()); // 2 s window: first quarter before, last quarter after

    EXPECT (! cols[5].valid);
    EXPECT (cols[50].valid);
    EXPECT (! cols[95].valid);
    EXPECT_NEAR (cols[50].hi, 0.5f, 1e-6);

    // Degenerate requests leave all columns invalid instead of crashing.
    cache.columns (1.0, 1.0, 100, cols.data());
    EXPECT (! cols[50].valid);
    WaveformCache empty;
    empty.columns (0.0, 1.0, 100, cols.data());
    EXPECT (! cols[0].valid);
}

// ----------------------------------------------------------------------------------- input

KTEST (PadTracker, reports_edges_and_holds)
{
    PadTracker t;
    t.setButton (Btn::A, true, PadTracker::Source::gamepad);

    auto f1 = t.frame (1.0f / 60);
    EXPECT (f1.pressed[(int) Btn::A] && f1.down[(int) Btn::A] && f1.repeated[(int) Btn::A]);
    EXPECT (! f1.usingKeyboard);

    auto f2 = t.frame (1.0f / 60);
    EXPECT (! f2.pressed[(int) Btn::A] && f2.down[(int) Btn::A] && ! f2.repeated[(int) Btn::A]);
    EXPECT (f2.held[(int) Btn::A] > 0.0f);

    t.setButton (Btn::A, false, PadTracker::Source::gamepad);
    auto f3 = t.frame (1.0f / 60);
    EXPECT (f3.released[(int) Btn::A] && ! f3.down[(int) Btn::A]);
    EXPECT_EQ (f3.held[(int) Btn::A], 0.0f);
}

KTEST (PadTracker, a_tap_shorter_than_a_frame_is_not_lost)
{
    PadTracker t;
    t.setButton (Btn::X, true, PadTracker::Source::keyboard);
    t.setButton (Btn::X, false, PadTracker::Source::keyboard);

    auto f = t.frame (1.0f / 60);
    EXPECT (f.pressed[(int) Btn::X]);
    EXPECT (f.released[(int) Btn::X]);
    EXPECT (! f.down[(int) Btn::X]);
    EXPECT (f.usingKeyboard);
}

KTEST (PadTracker, auto_repeats_after_a_delay_at_a_steady_rate)
{
    PadTracker t;
    t.setButton (Btn::Right, true, PadTracker::Source::keyboard);
    t.frame (0.0f);

    int repeats = 0;
    float time = 0.0f;
    bool early = false;

    for (int i = 0; i < 120; ++i) // 2 seconds at 60 fps
    {
        auto f = t.frame (1.0f / 60);
        time += 1.0f / 60;

        if (f.repeated[(int) Btn::Right])
        {
            ++repeats;
            if (time < PadTracker::repeatDelay - 0.02f) early = true;
        }
    }

    EXPECT (! early);
    // (2.0 - 0.38) / 0.075 = ~21.6 repeats, at 60 fps granularity somewhere around 18-22.
    EXPECT (repeats >= 17 && repeats <= 23);
}

KTEST (PadTracker, two_sources_for_one_button_stay_down_until_both_release)
{
    PadTracker t;
    t.setButton (Btn::B, true, PadTracker::Source::keyboard);
    t.setButton (Btn::B, true, PadTracker::Source::gamepad);
    t.frame (0.016f);
    t.setButton (Btn::B, false, PadTracker::Source::keyboard);
    auto f = t.frame (0.016f);
    EXPECT (f.down[(int) Btn::B]);
    t.setButton (Btn::B, false, PadTracker::Source::gamepad);
    auto g = t.frame (0.016f);
    EXPECT (! g.down[(int) Btn::B] && g.released[(int) Btn::B]);
}

KTEST (PadTracker, keyboard_sticks_ramp_up_and_the_bigger_source_wins)
{
    PadTracker t;
    t.setKeyAxis (0, +1);
    auto f1 = t.frame (1.0f / 60);
    EXPECT (f1.lx > 0.25f && f1.lx < 0.4f); // gentle start for fine control
    float last = f1.lx;

    for (int i = 0; i < 90; ++i)
        last = t.frame (1.0f / 60).lx;

    EXPECT_NEAR (last, 1.0f, 1e-6);

    t.setKeyAxis (0, 0);
    EXPECT_EQ (t.frame (1.0f / 60).lx, 0.0f);

    t.setKeyAxis (2, -1);
    t.setSticks (0, 0, 0.9f, 0, PadTracker::Source::gamepad);
    auto g = t.frame (1.0f / 60);
    EXPECT_NEAR (g.rx, 0.9f, 1e-6); // the pad's 0.9 beats the key's ~0.3 on the same axis
}

KTEST (PadTracker, piano_drop_and_quit_events_are_delivered_once)
{
    PadTracker t;
    t.pianoKey (3, true);
    t.pianoKey (3, false);
    t.dropFile ("/music/a.wav");
    t.requestQuit();

    auto f = t.frame (0.016f);
    EXPECT_EQ ((int) f.piano.size(), 2);
    EXPECT (f.piano[0].down && f.piano[0].slice == 3 && ! f.piano[1].down);
    EXPECT_EQ ((int) f.droppedFiles.size(), 1);
    EXPECT (f.quit);

    auto g = t.frame (0.016f);
    EXPECT (g.piano.empty() && g.droppedFiles.empty());
}

KTEST (PadTracker, releaseAll_lets_go_of_everything)
{
    PadTracker t;
    t.setButton (Btn::A, true, PadTracker::Source::gamepad);
    t.setKeyAxis (0, 1);
    t.frame (0.016f);

    t.releaseAll();
    auto f = t.frame (0.016f);
    EXPECT (! f.down[(int) Btn::A] && f.released[(int) Btn::A]);
    EXPECT_EQ (f.lx, 0.0f);
}

KTEST (Input, button_names_round_trip)
{
    for (int i = 0; i < kBtnCount; ++i)
    {
        Btn b;
        EXPECT (btnFromName (btnName ((Btn) i), b));
        EXPECT_EQ ((int) b, i);
    }

    Btn b;
    EXPECT (btnFromName ("l1", b) && b == Btn::L1);
    EXPECT (! btnFromName ("nope", b));
}

// --------------------------------------------------------------------------------------- format

KTEST (Format, the_time_of_a_take_is_rounded_to_a_tenth_before_it_is_split_into_minutes)
{
    EXPECT (clockTime (0.0) == "0.0s");
    EXPECT (clockTime (-3.0) == "0.0s");
    EXPECT (clockTime (12.34) == "12.3s");
    EXPECT (clockTime (59.94) == "59.9s");
    EXPECT (clockTime (59.96) == "1:00.0");                  // not "60.0s"
    EXPECT (clockTime (62.3) == "1:02.3");
    EXPECT (clockTime (119.97) == "2:00.0");                 // not "1:60.0"
    EXPECT (clockTime (600.0) == "10:00.0");
    EXPECT (clockTime (std::nan ("")) == "0.0s");
    EXPECT (clockTime (std::numeric_limits<double>::infinity()) == clockTime (1.0e9));       // a huge time is still some text
}

KTEST (Format, sizes_and_limits_read_the_way_the_screens_show_them)
{
    EXPECT (sizeText (0) == "0 B");
    EXPECT (sizeText (1023) == "1023 B");
    EXPECT (sizeText (1024) == "1 KB");
    EXPECT (sizeText (300844) == "294 KB");
    EXPECT (sizeText (5u * 1024u * 1024u) == "5.0 MB");
    EXPECT (sizeText (57600044) == "54.9 MB");

    EXPECT (limitText (30) == "30 s");
    EXPECT (limitText (45) == "45 s");
    EXPECT (limitText (60) == "1 min");
    EXPECT (limitText (90) == "90 s");
    EXPECT (limitText (180) == "3 min");
    EXPECT (limitText (300) == "5 min");
    EXPECT (limitText (0) == "0 s");
    EXPECT (limitText (5) == "5 s");
    EXPECT (limitText (59) == "59 s");
    EXPECT (limitText (61) == "61 s");
}

KTEST (Format, a_long_path_keeps_its_end_and_never_cuts_a_letter_in_two)
{
    EXPECT (elideLeftAscii ("abc", 10) == "abc");
    EXPECT (elideLeftAscii ("/mnt/mmc/kerf/samples", 11) == ".../samples");          // the dots count: 11 characters in all
    EXPECT (elideLeftAscii ("/mnt/mmc/kerf/samples", 12) == "...f/samples");
    EXPECT (elideLeftAscii ("/mnt/mmc/M\xC3\xBA" "sica/loops", 14) == "...sica/loops");     // the cut would fall inside the u with an accent
    EXPECT (elideLeftAscii ("/very/long/path", 3) == "/very/long/path");
}

// ---------------------------------------------------------------------------------------- config

KTEST (Config, round_trips_every_setting)
{
    Config c;
    c.masterVolume = 0.35f;
    c.midiMode = 1;
    c.noteOffset = 36;
    c.usbMidi = false;
    c.audioBuffer = 1024;
    c.audioRate = 48000;
    c.swapAB = true;
    c.swapXY = true;
    c.stickSpeed = 0.8f;
    c.deadzone = 0.2f;
    c.fullscreen = true;
    c.windowScale = 3;
    c.joyAxis[2] = 4;
    c.joyButton[(int) Btn::A] = 7;
    c.usbAudio = false;
    c.recordLimit = 120;
    c.recordGainDb = -6;
    c.recordInput = "usb:0644:805f@1-1.2#2";                 // a '#' is part of the name of an input
    c.recordChannels = "3+4  stereo";                        // two spaces, as the Record screen writes it
    c.lastDir = "C:\\Users\\Studio\\M\xC3\xBAsica #1";
    c.projects = false;

    const auto back = Config::parse (c.serialize());
    EXPECT_NEAR (back.masterVolume, 0.35, 1e-6);
    EXPECT_EQ (back.midiMode, 1);
    EXPECT_EQ (back.noteOffset, 36);
    EXPECT (! back.usbMidi);
    EXPECT_EQ (back.audioBuffer, 1024);
    EXPECT_EQ (back.audioRate, 48000);
    EXPECT (back.swapAB && back.swapXY && back.fullscreen);
    EXPECT_NEAR (back.stickSpeed, 0.8, 1e-6);
    EXPECT_NEAR (back.deadzone, 0.2, 1e-6);
    EXPECT_EQ (back.windowScale, 3);
    EXPECT_EQ (back.joyAxis[2], 4);
    EXPECT_EQ (back.joyButton[(int) Btn::A], 7);
    EXPECT (! back.usbAudio);
    EXPECT_EQ (back.recordLimit, 120);
    EXPECT_EQ (back.recordGainDb, -6);
    EXPECT (back.recordInput == c.recordInput);
    EXPECT (back.recordChannels == c.recordChannels);
    EXPECT (back.lastDir == c.lastDir);
    EXPECT (! back.projects);

    // Nothing is written that the defaults would not give back.
    const Config fresh = Config::parse ("");
    EXPECT (fresh.usbAudio && fresh.usbMidi);
    EXPECT_EQ (fresh.recordLimit, 180);
    EXPECT_EQ (fresh.recordGainDb, 0);
    EXPECT (fresh.recordInput.empty() && fresh.recordChannels.empty());
    EXPECT (fresh.projects);
}

KTEST (Config, projects_are_on_unless_the_file_says_otherwise)
{
    EXPECT (Config::parse ("").projects);

    for (const char* off : { "0", "off", "no", "false" })
        EXPECT (! Config::parse (std::string ("projects = ") + off + "\n").projects);

    for (const char* on : { "1", "on", "yes", "true" })
        EXPECT (Config::parse (std::string ("projects = ") + on + "\n").projects);

    // A value that is not a switch is ignored, as with every other setting.
    EXPECT (Config::parse ("projects = maybe\n").projects);
    EXPECT (Config::parse ("projects =\n").projects);
    EXPECT (! Config::parse ("master_volume = 0.5\nprojects = 0\nnote_offset = 40\n").projects);

    // The file says what the switch is for, and what it was set to.
    Config c;
    EXPECT (c.serialize().find ("projects = 1") != std::string::npos);
    EXPECT (c.serialize().find (".kerf") != std::string::npos);
    c.projects = false;
    EXPECT (c.serialize().find ("projects = 0") != std::string::npos);
    EXPECT (c.serialize().find ("projects = 1") == std::string::npos);
}

KTEST (Config, the_recording_settings_are_range_checked_and_the_names_stay_on_one_line)
{
    auto c = Config::parse ("record_limit = 9999\nrecord_gain = -50\n");
    EXPECT_EQ (c.recordLimit, 300);
    EXPECT_EQ (c.recordGainDb, -12);

    c = Config::parse ("record_limit = 1\nrecord_gain = 99\n");
    EXPECT_EQ (c.recordLimit, 5);
    EXPECT_EQ (c.recordGainDb, 30);

    c = Config::parse ("record_limit = a while\nrecord_gain =\nrecord_input =\nrecord_channels =\n");
    EXPECT_EQ (c.recordLimit, 180);                          // unparsable -> default kept
    EXPECT_EQ (c.recordGainDb, 0);
    EXPECT (c.recordInput.empty() && c.recordChannels.empty());

    // After a number a '#' starts a comment; in the name of an input it does not.
    c = Config::parse ("record_input = sdl:Mic #2 (USB)\nrecord_limit = 60   # a minute\nrecord_channels = 1+2  mono mix\n");
    EXPECT (c.recordInput == "sdl:Mic #2 (USB)");
    EXPECT_EQ (c.recordLimit, 60);
    EXPECT (c.recordChannels == "1+2  mono mix");

    // Control characters are dropped and a name is not endless.
    Config d;
    d.recordInput = std::string ("a\tb\x01") + "c" + std::string (300, 'x');
    d.recordChannels = "line\nbreak";
    d.sanitize();
    EXPECT_EQ (d.recordInput.size(), (size_t) 200);
    EXPECT (d.recordInput.rfind ("abcxx", 0) == 0);
    EXPECT (d.recordChannels == "linebreak");

    const auto back = Config::parse (d.serialize());
    EXPECT (back.recordInput == d.recordInput && back.recordChannels == "linebreak");
}

KTEST (Config, clamps_hand_edited_nonsense_and_ignores_garbage_lines)
{
    const auto c = Config::parse ("master_volume = 7\nnote_offset = -50\naudio_buffer = 3\naudio_rate = 12345\n"
                                  "stick_speed = abc\ndeadzone = 0\nwindow_scale = 99\njoy_axis_lx = 500\n"
                                  "this line has no equals sign\n= value without key\n# a comment\nunknown_key = 5\n");
    EXPECT_NEAR (c.masterVolume, 1.0, 1e-6);
    EXPECT_EQ (c.noteOffset, 0);
    EXPECT_EQ (c.audioBuffer, 64);
    EXPECT_EQ (c.audioRate, 44100);
    EXPECT_NEAR (c.stickSpeed, 0.5, 1e-6);   // unparsable -> default kept
    EXPECT_NEAR (c.deadzone, 0.05, 1e-6);
    EXPECT_EQ (c.windowScale, 4);
    EXPECT_EQ (c.joyAxis[0], 31);

    const auto empty = Config::parse ("");
    EXPECT_NEAR (empty.masterVolume, 0.7, 1e-6);
}

KTEST (Config, saves_and_loads_a_file_and_survives_a_missing_one)
{
    const auto file = fs::temp_directory_path() / "kerf-config-test.cfg";
    Config c;
    c.noteOffset = 48;
    EXPECT (c.save (file));
    EXPECT_EQ (Config::load (file).noteOffset, 48);

    std::error_code ec;
    fs::remove (file, ec);
    EXPECT_EQ (Config::load (file).noteOffset, 60);
}

// ------------------------------------------------------------------------------- file browser

namespace
{
    struct TempTree
    {
        fs::path root;

        TempTree()
        {
            std::error_code ec;
            root = fs::temp_directory_path() / pathFromUtf8 ("kerf-browser-test-\xC3\xA1");
            fs::remove_all (root, ec);
            fs::create_directories (root / "Drums", ec);
            fs::create_directories (root / "bass", ec);
            fs::create_directories (root / ".hidden", ec);
            touch (root / "loop.WAV", 10);
            touch (root / "Voz.mp3", 20);
            touch (root / "notes.txt", 5);
            touch (root / ".secret.wav", 5);
            touch (root / pathFromUtf8 ("Gravação.flac"), 30);
            touch (root / "Drums" / "kick.wav", 40);
        }

        ~TempTree()
        {
            std::error_code ec;
            fs::remove_all (root, ec);
        }

        static void touch (const fs::path& p, int bytes)
        {
            std::ofstream out (p, std::ios::binary);
            out << std::string ((size_t) bytes, 'x');
        }
    };
}

KTEST (FileBrowser, lists_folders_first_then_audio_files_and_hides_the_rest)
{
    TempTree tree;
    FileBrowser fb;
    EXPECT (fb.open (tree.root));

    std::vector<std::string> names;
    for (auto& e : fb.entries()) names.push_back (e.name);

    const std::vector<std::string> expected { "..", "bass", "Drums", "Grava\xC3\xA7\xC3\xA3o.flac", "loop.WAV", "Voz.mp3" };
    EXPECT (names == expected);

    if (fb.entries().size() != expected.size())
        return; // already reported above; do not index into a listing that is not what we expect

    EXPECT (fb.entries()[0].isParent && fb.entries()[0].isDir);
    EXPECT (fb.entries()[1].isDir && ! fb.entries()[3].isDir);
    EXPECT_EQ ((int) fb.entries()[3].size, 30);
}

KTEST (FileBrowser, navigates_into_folders_chooses_files_and_goes_back_up)
{
    TempTree tree;
    FileBrowser fb;
    fb.open (tree.root);

    fs::path chosen;
    EXPECT (fb.activate (fb.indexOfName ("Drums"), chosen) == FileBrowser::Result::navigated);
    EXPECT (fb.directory().filename() == "Drums");
    EXPECT_EQ ((int) fb.entries().size(), 2); // ".." and kick.wav

    EXPECT (fb.activate (fb.indexOfName ("kick.wav"), chosen) == FileBrowser::Result::chosenFile);
    EXPECT (chosen.filename() == "kick.wav");
    EXPECT (fs::exists (chosen));

    EXPECT (fb.goUp());
    EXPECT (fb.directory().filename() == pathFromUtf8 ("kerf-browser-test-\xC3\xA1"));
    EXPECT (fb.indexOfName ("Drums") >= 0);

    // ".." entry also goes up.
    fb.activate (fb.indexOfName ("Drums"), chosen);
    EXPECT (fb.activate (0, chosen) == FileBrowser::Result::navigated);
    EXPECT (fb.indexOfName ("Drums") >= 0);

    EXPECT (fb.activate (999, chosen) == FileBrowser::Result::none);
}

KTEST (FileBrowser, a_bad_folder_leaves_the_listing_untouched)
{
    TempTree tree;
    FileBrowser fb;
    fb.open (tree.root);
    const auto before = fb.entries().size();

    EXPECT (! fb.open (tree.root / "does-not-exist"));
    EXPECT (! fb.error().empty());
    EXPECT_EQ ((int) fb.entries().size(), (int) before);
    EXPECT (fb.directory() == fs::weakly_canonical (tree.root));
}

KTEST (FileBrowser, marks_the_audio_files_that_have_a_project_next_to_them)
{
    std::error_code ec;
    const fs::path root = fs::temp_directory_path() / pathFromUtf8 ("kerf-browser-projects-test-\xC3\xA1");
    fs::remove_all (root, ec);
    fs::create_directories (root / "Folder", ec);

    const auto touch = [&] (const std::string& name)
    {
        std::ofstream out (root / pathFromUtf8 (name), std::ios::binary);
        out << "x";
    };

    touch ("a.wav");            touch ("a.wav.kerf");                  // has one
    touch ("b.mp3");            touch ("b.mp3.kerf.bak");              // a backup is not a project
    touch ("c.wav");            touch ("c.kerf");                      // another name: not c.wav's project
    touch ("d.wav");            touch ("d.flac");  touch ("d.wav.kerf");   // belongs to d.wav alone
    touch ("e.wav");                                                   // e.wav.kerf is a folder, not a project
    fs::create_directories (root / "e.wav.kerf", ec);
    touch ("Gravacao \xC3\xA7.wav");  touch ("Gravacao \xC3\xA7.wav.kerf");
    touch ("F.WAV");            touch ("F.WAV.kerf");
    touch ("alone.wav.kerf");                                          // no sample: nothing to mark, nothing listed
    touch ("old.wav");          touch ("old.wav.kerf.old");
    touch ("a");  touch ("ab");  touch ("abc");  touch ("abcd");  touch ("x.y");  touch ("n.kerf");   // names as short as ".kerf" or shorter

    FileBrowser fb;
    EXPECT (fb.open (root));

    std::string marked, unmarked;

    for (const auto& e : fb.entries())
    {
        if (e.isDir)
        {
            EXPECT (! e.hasProject);                                   // folders are never marked
            continue;
        }

        (e.hasProject ? marked : unmarked) += e.name + ";";
    }

    EXPECT (marked == "a.wav;d.wav;F.WAV;Gravacao \xC3\xA7.wav;");        // in the order of the list
    EXPECT (unmarked == "b.mp3;c.wav;d.flac;e.wav;old.wav;");
    EXPECT (fb.indexOfName ("alone.wav.kerf") < 0);                    // the project files themselves are not listed
    EXPECT (fb.indexOfName ("a.wav.kerf") < 0);
    EXPECT (fb.indexOfName ("e.wav.kerf") >= 0 && fb.entries()[(size_t) fb.indexOfName ("e.wav.kerf")].isDir);   // ... a folder is

    // The mark follows the disk: read the folder again after a project appears.
    EXPECT (! fb.entries()[(size_t) fb.indexOfName ("c.wav")].hasProject);
    touch ("c.wav.kerf");
    EXPECT (fb.open (root));
    EXPECT (fb.entries()[(size_t) fb.indexOfName ("c.wav")].hasProject);

    fs::remove_all (root, ec);
}

KTEST (FileBrowser, recognises_audio_extensions_case_insensitively)
{
    EXPECT (FileBrowser::isAudioFile ("a.wav") && FileBrowser::isAudioFile ("A.WAV"));
    EXPECT (FileBrowser::isAudioFile ("b.Mp3") && FileBrowser::isAudioFile ("c.flac"));
    EXPECT (! FileBrowser::isAudioFile ("d.txt") && ! FileBrowser::isAudioFile ("wav") && ! FileBrowser::isAudioFile ("e.wav.bak"));
}

// ------------------------------------------------------------------------------------------ help

// The help is drawn with fixed columns (app_draw.cpp: drawHelp): keys at x = 14, descriptions in
// the 2x font right after the key column, a one-line note in the 1x font under the rows. A line
// that is too long is cut off by the screen edge, which is what the first draft of the sequencer
// help did.
KTEST (HelpText, every_line_fits_the_640_pixel_screen)
{
    const int screenW = 640, margin = 8;
    const int keysX = margin + 6;
    const int whatX = keysX + (kHelpKeyChars + 1) * 16;

    EXPECT_EQ (kHelpPages, 6);

    for (int p = 0; p < kHelpPages; ++p)
    {
        const auto& page = helpPage (p);

        EXPECT (page.title != nullptr && page.note != nullptr);
        EXPECT (margin + 2 + Canvas::textWidth (page.title, 2) <= screenW - margin);
        EXPECT (keysX + Canvas::textWidth (page.note, 1) <= screenW - margin);
        EXPECT_EQ (page.rowCount, 15);   // 15 rows of 24 px fill the screen above the note

        for (int i = 0; i < page.rowCount; ++i)
        {
            const auto& row = page.rows[i];

            EXPECT (row.keys != nullptr && row.what != nullptr);
            EXPECT (Canvas::textWidth (row.keys, 2) <= kHelpKeyChars * 16);
            EXPECT (whatX + Canvas::textWidth (row.what, 2) <= screenW - margin);
            EXPECT (Canvas::textWidth (row.what, 2) <= kHelpWhatChars * 16);
        }
    }

    // Out-of-range pages are clamped rather than read out of bounds.
    EXPECT (std::string (helpPage (-5).title) == helpPage (0).title);
    EXPECT (std::string (helpPage (99).title) == helpPage (kHelpPages - 1).title);
}
