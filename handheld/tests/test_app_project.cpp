// Tests for projects as the application runs them: a sample's slices and patterns come back when it
// is loaded again, the work is saved a moment after it stops changing, the header says how things
// stand, and the Settings switch, the library's marker and the toasts all agree.
//
// Like the other application tests this one has no window: buttons go in, frames come out into a
// Canvas, and the folder on disk is looked at afterwards. The clock is the application's own, 60
// frames to the second.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "app.h"
#include "audio_input.h"
#include "canvas.h"
#include "config.h"
#include "engine.h"
#include "fake_input.h"
#include "minitest.h"
#include "path_util.h"
#include "project.h"
#include "project_store.h"
#include "sample.h"
#include "temp_folder.h"
#include "theme.h"
#include "utf8.h"

using namespace kerf;
using namespace kerf::ui;
using namespace kerf::test;
namespace fs = std::filesystem;
namespace store = kerf::ui::ProjectStore;

namespace
{
    // Two seconds of quiet with a click every half second: something for the detector to find.
    void writeClicks (const fs::path& wav)
    {
        AudioBuffer clicks (1, 2 * 44100);

        for (int click = 0; click < 4; ++click)
            for (int i = 0; i < 3000; ++i)
                clicks.set (0, 4410 + click * 22050 + i, 0.9f * std::sin (0.3f * (float) i) * std::exp (-(float) i / 500.0f));

        const std::string bytes = encodeWav16 (clicks, 44100.0);
        std::ofstream out (wav, std::ios::binary);
        out.write (bytes.data(), (std::streamsize) bytes.size());
    }

    FakeDevice microphone()
    {
        FakeDevice d;
        d.id = "fake:mic";
        d.name = "Pretend USB microphone";
        d.channels = 2;
        d.rate = 48000;
        d.direct = true;
        d.toneHz = 220.0;
        return d;
    }

    long countPixels (const Canvas& c, int x, int y, int w, int h, uint32_t colour)
    {
        long n = 0;

        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx)
                if (c.pixelAt (xx, yy) == colour)
                    ++n;

        return n;
    }

    // What the header says above the output meter: the letters on the screen are compared, pixel by pixel, with the
    // same word drawn afresh in the colour it should have ("?" when they are none of the words).
    std::string shownTag (const Canvas& c)
    {
        struct Kind { const char* name; uint32_t colour; };
        const Kind kinds[] = { { "SAVED", theme::ok }, { "EDITED", rgb (234, 179, 8) }, { "SAVE OFF", theme::textDim },
                               { "NO SAVE", theme::danger }, { "OUT", theme::textFaint } };
        std::string found;

        for (const auto& k : kinds)
        {
            Canvas expected (kScreenW, kScreenH);
            expected.clear (0);
            expected.drawText (556, 5, k.name, k.colour, 1);

            bool same = true;
            int lit = 0;

            for (int y = 3; y < 13; ++y)
                for (int x = 554; x < 630; ++x)
                {
                    const bool shown = c.pixelAt (x, y) == k.colour, wanted = expected.pixelAt (x, y) == k.colour;
                    same = same && shown == wanted;
                    lit += wanted ? 1 : 0;
                }

            if (same && lit > 8)
                found += (found.empty() ? "" : "+") + std::string (k.name);
        }

        return found.empty() ? "?" : found;
    }

    // Is this word on the screen, in this colour and size, somewhere in the box? It is drawn afresh on a blank canvas and
    // looked for pixel by pixel, with a clear margin round it, so what is found is these very letters and no others.
    bool showsWord (const Canvas& c, const std::string& word, uint32_t colour, int scale, int x0, int y0, int x1, int y1)
    {
        Canvas sample (kScreenW, kScreenH);
        sample.clear (0);
        sample.drawText (20, 20, word, colour, scale);

        int left = kScreenW, right = -1, top = kScreenH, bottom = -1;

        for (int y = 0; y < kScreenH; ++y)
            for (int x = 0; x < kScreenW; ++x)
                if (sample.pixelAt (x, y) == colour)
                {
                    left = std::min (left, x);
                    right = std::max (right, x);
                    top = std::min (top, y);
                    bottom = std::max (bottom, y);
                }

        if (right < 0)
            return false;

        const int margin = 2;

        for (int oy = y0; oy + (bottom - top) < y1; ++oy)
            for (int ox = x0; ox + (right - left) < x1; ++ox)
            {
                bool same = true;

                for (int dy = -margin; same && dy <= bottom - top + margin; ++dy)
                    for (int dx = -margin; same && dx <= right - left + margin; ++dx)
                        same = (c.pixelAt (ox + dx, oy + dy) == colour) == (sample.pixelAt (left + dx, top + dy) == colour);

                if (same)
                    return true;
            }

        return false;
    }

    // What the "Projects" row of the settings says on the right (the row is the selected one, so its value is in the brand's colour).
    bool settingShows (const Canvas& c, const std::string& word)
    {
        return showsWord (c, word, theme::brandHi, 2, 300, 20, 540, 460);
    }

    // Does the library's row show this name, cut to `chars` glyphs the way the library cuts it, and no more of it? The name is
    // drawn afresh where the row has it and compared pixel by pixel, in the colour the row gives it.
    bool rowShowsName (const Canvas& c, int row, const std::string& name, int chars, uint32_t colour)
    {
        Canvas expected (kScreenW, kScreenH);
        expected.clear (0);
        const int y = 52 + row * 30;
        expected.drawText (54, y + 6, elide (name, chars), colour, 2);

        bool same = true;
        int lit = 0;

        for (int yy = y + 2; yy < y + 26; ++yy)
            for (int xx = 46; xx < kScreenW - 10; ++xx)
            {
                const bool shown = c.pixelAt (xx, yy) == colour, wanted = expected.pixelAt (xx, yy) == colour;
                same = same && shown == wanted;
                lit += wanted ? 1 : 0;
            }

        return same && lit > 100;
    }

    // The application on a folder, with a pad that taps buttons. Make the folder first: it has to
    // outlive the session (the application writes its last project as it goes).
    struct Session
    {
        const TempFolder& dir;
        Engine engine;
        Config cfg;
        InputHub hub;                                // before the app, which closes its input when it goes
        FakeInput* fake = nullptr;
        std::unique_ptr<App> app;
        Canvas canvas { kScreenW, kScreenH };
        std::vector<float> block;
        std::vector<std::string> log;

        explicit Session (const TempFolder& folder, bool projects = true, bool withInput = false) : dir (folder)
        {
            engine.prepare (44100.0);
            cfg.projects = projects;

            if (withInput)
            {
                auto input = std::make_unique<FakeInput> (std::vector<FakeDevice> { microphone() });
                fake = input.get();
                hub.add (std::move (input));
            }

            app = std::make_unique<App> (engine, cfg, fs::path(), dir.path);
            app->setLogger ([this] (const std::string& line) { log.push_back (line); });

            if (withInput)
                app->setInputHub (&hub);
        }

        ~Session() { app.reset(); }

        void frames (int n, const Pad& pad, bool drawAll = true)
        {
            for (int i = 0; i < n; ++i)
            {
                app->update (pad, 1.0f / 60.0f);
                block.assign (2 * 735, 0.0f);
                engine.render (block.data(), 735);

                if (drawAll || i == n - 1)
                    app->draw (canvas);
            }
        }

        void frames (int n)
        {
            Pad quiet;
            quiet.usingKeyboard = false;
            frames (n, quiet);
        }

        // Time goes by with the pad quiet; only the last frame is drawn (drawing is most of the work).
        void seconds (double s)
        {
            Pad quiet;
            quiet.usingKeyboard = false;
            frames ((int) std::lround (s * 60.0), quiet, false);
        }

        void tap (Btn b, int settle = 2)
        {
            Pad pad;
            pad.pressed[(int) b] = pad.repeated[(int) b] = pad.down[(int) b] = true;
            pad.anyInput = true;
            frames (1, pad);
            frames (settle);
        }

        // Select, as a tap: down for a frame, up on the next (Kerf acts when it is let go).
        void select()
        {
            Pad down;
            down.pressed[(int) Btn::Select] = down.repeated[(int) Btn::Select] = down.down[(int) Btn::Select] = true;
            down.anyInput = true;
            down.usingKeyboard = false;
            frames (1, down);

            Pad up;
            up.released[(int) Btn::Select] = true;
            up.anyInput = true;
            up.usingKeyboard = false;
            frames (1, up);
            frames (2);
        }

        void tapTimes (Btn b, int times)
        {
            for (int i = 0; i < times; ++i)
                tap (b, 1);
        }

        // Loads a sample of the folder, the way the program does, and waits for it.
        void load (const std::string& name)
        {
            app->loadFile (dir / name);
            app->waitForLoad();
            frames (2);
        }

        bool toastHas (const std::string& part) const { return app->toast().find (part) != std::string::npos; }

        int logged (const std::string& part) const
        {
            int n = 0;

            for (const auto& line : log)
                if (line.find (part) != std::string::npos)
                    ++n;

            return n;
        }

        ProjectKeeper::Tag tag() const { return app->project().tag(); }

        // Start, down to "Settings...", A, down to "Projects" (the menus are the same in the tests as on the device).
        void openSettingsAtProjects()
        {
            tap (Btn::Start);
            tapTimes (Btn::Down, 9);                  // the slicer's menu: Settings... is the tenth
            tap (Btn::A);
            tapTimes (Btn::Down, 8);                  // Projects is the ninth of the settings
            EXPECT (app->screen() == Screen::settings);
        }
    };

    std::string projectText (std::int64_t frames, std::int64_t rate)
    {
        Project p;
        p.audio = "clicks.wav";
        p.frames = frames;
        p.rate = rate;
        Slice a, b;
        a.start = 0.1f; a.end = 0.5f;
        b.start = 0.6f; b.end = 1.0f;
        p.slices = { a, b };
        p.grid.bits[0][1] = 0x0F0F;

        std::string text;
        EXPECT (serializeProject (p, text));
        return text;
    }
}

// ------------------------------------------------------------------------------------- loading

KTEST (AppProject, a_sample_with_no_project_loads_as_usual_and_leaves_no_file)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir);
    s.load ("clicks.wav");

    EXPECT (s.app->screen() == Screen::editor);
    EXPECT (s.toastHas ("Loaded. Press"));
    EXPECT (s.tag() == ProjectKeeper::Tag::none);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("OUT"));
    EXPECT (! s.app->canUndo());

    s.seconds (20.0);                                   // looking is not editing
    EXPECT_EQ (dir.names().size(), (std::size_t) 1);
    EXPECT_EQ (s.log.size(), (std::size_t) 0);
}

KTEST (AppProject, the_work_comes_back_when_the_sample_is_loaded_in_another_run)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    int found = 0;

    {
        Session s (dir);
        s.load ("clicks.wav");
        found = s.app->detectSlices (0.5f);
        EXPECT (found >= 3);
        s.tap (Btn::R1);                                // the cursor goes to the second slice
        s.engine.sequencer().setCell (0, 1, 4, true);
        s.engine.sequencer().setCell (3, 2, 9, true);
        s.engine.sequencer().setTempo (97.5f);
        s.engine.sequencer().setSwing (0.6f);
        s.engine.sequencer().setLength (12);
        s.engine.sequencer().setScene (3);
        s.seconds (2.0);
        EXPECT (s.tag() == ProjectKeeper::Tag::saved);
    }                                                   // the program ends

    Session again (dir);
    again.load ("clicks.wav");

    EXPECT (again.toastHas ("Project restored: " + std::to_string (found) + " slices"));
    EXPECT_EQ (again.engine.slices().size(), found);
    EXPECT_EQ (again.app->selectedSlice(), 1);
    EXPECT_EQ (again.engine.selectedSlice(), 1);
    EXPECT (again.engine.sequencer().cell (0, 1, 4));
    EXPECT (again.engine.sequencer().cell (3, 2, 9));
    EXPECT (! again.engine.sequencer().cell (0, 1, 5));
    EXPECT_NEAR (again.engine.sequencer().tempo(), 97.5, 0.001);
    EXPECT_NEAR (again.engine.sequencer().swing(), 0.6, 0.001);
    EXPECT_EQ (again.engine.sequencer().length(), 12);
    EXPECT_EQ (again.engine.sequencer().scene(), 3);
    EXPECT (again.tag() == ProjectKeeper::Tag::saved);
    EXPECT_TEXT (shownTag (again.canvas), std::string ("SAVED"));
    EXPECT (again.app->screen() == Screen::editor);
    EXPECT_EQ (again.logged ("project: restored clicks.wav.kerf"), 1);

    // It is back as work that was done before: nothing to undo, nothing to save, no backup made.
    EXPECT (! again.app->canUndo());
    again.seconds (10.0);
    EXPECT_EQ (dir.names().size(), (std::size_t) 2);
    EXPECT_EQ (again.logged ("project: saved"), 0);
}

KTEST (AppProject, a_project_with_no_slices_is_restored_with_the_other_words)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    {
        Session s (dir);
        s.load ("clicks.wav");
        s.engine.sequencer().setTempo (150.0f);         // only a tempo
        s.seconds (2.0);
    }

    Session again (dir);
    again.load ("clicks.wav");
    EXPECT (again.toastHas ("Project restored"));
    EXPECT (! again.toastHas ("slice"));
    EXPECT_NEAR (again.engine.sequencer().tempo(), 150.0, 0.001);

    // One slice is "1 slice".
    {
        Session s (dir);
        s.load ("clicks.wav");
        s.tap (Btn::X);
        s.seconds (2.0);
    }

    Session third (dir);
    third.load ("clicks.wav");
    EXPECT (third.toastHas ("Project restored: 1 slice"));
    EXPECT (! third.toastHas ("slices"));
}

KTEST (AppProject, a_sample_loaded_while_projects_are_off_is_not_looked_at)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    {
        Session s (dir);
        s.load ("clicks.wav");
        s.tap (Btn::X);
        s.seconds (2.0);
    }

    Session off (dir, false);
    off.load ("clicks.wav");
    EXPECT (off.toastHas ("Loaded. Press"));
    EXPECT_EQ (off.engine.slices().size(), 0);
    EXPECT (off.tag() == ProjectKeeper::Tag::off);
    EXPECT_TEXT (shownTag (off.canvas), std::string ("SAVE OFF"));

    off.tap (Btn::X);
    off.seconds (30.0);
    EXPECT_EQ (dir.names().size(), (std::size_t) 2);                 // the project of the earlier run, as it was
    EXPECT_EQ (store::read (dir / "clicks.wav.kerf").project.slices.size(), (std::size_t) 1);
    EXPECT_EQ (off.logged ("project:"), 0);
}

// ------------------------------------------------------------------------------------- saving

KTEST (AppProject, an_edit_is_saved_a_moment_later_and_the_header_follows)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir);
    s.load ("clicks.wav");

    s.tap (Btn::X);                                     // a slice at the centre of the view
    EXPECT_EQ (s.engine.slices().size(), 1);
    EXPECT (s.tag() == ProjectKeeper::Tag::edited);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("EDITED"));
    EXPECT (! dir.has ("clicks.wav.kerf"));

    s.seconds (1.0);                                    // a second after: still waiting
    EXPECT_TEXT (shownTag (s.canvas), std::string ("EDITED"));
    EXPECT (! dir.has ("clicks.wav.kerf"));

    s.seconds (0.6);                                    // a second and a half
    EXPECT (s.tag() == ProjectKeeper::Tag::saved);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("SAVED"));
    EXPECT (dir.has ("clicks.wav.kerf"));
    EXPECT_EQ (s.logged ("project: saved clicks.wav.kerf (1 slices"), 1);
    EXPECT_EQ (dir.names().size(), (std::size_t) 2);

    // Another edit, another save; the project on disk is always the work as it is.
    s.tap (Btn::Y);                                     // opens the detect dialog ... then A runs it
    s.tap (Btn::A);
    s.seconds (2.0);
    EXPECT (s.tag() == ProjectKeeper::Tag::saved);
    EXPECT (sameWork (store::read (dir / "clicks.wav.kerf").project, captureProject (s.engine)));
}

KTEST (AppProject, the_sequencer_page_work_is_saved_too)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir);
    s.load ("clicks.wav");
    s.app->detectSlices (0.5f);
    s.seconds (2.0);
    EXPECT (s.tag() == ProjectKeeper::Tag::saved);

    s.select();                                         // SLICER -> SEQ
    EXPECT (s.app->screen() == Screen::sequencer);
    s.tap (Btn::A);                                     // a step at the cursor
    EXPECT (s.engine.sequencer().cell (0, 0, 0));
    EXPECT_TEXT (shownTag (s.canvas), std::string ("EDITED"));         // the header is the same on this page

    s.seconds (2.0);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("SAVED"));
    EXPECT (store::read (dir / "clicks.wav.kerf").project.grid.bits[0][0] == 1);
}

KTEST (AppProject, the_last_moments_are_saved_when_the_program_ends)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");

    {
        Session s (dir);
        s.load ("clicks.wav");
        s.tap (Btn::X);
        EXPECT (s.tag() == ProjectKeeper::Tag::edited);
        EXPECT (! dir.has ("clicks.wav.kerf"));
    }                                                   // no waiting for the moment: the program ends

    EXPECT (dir.has ("clicks.wav.kerf"));
    EXPECT_EQ (store::read (dir / "clicks.wav.kerf").project.slices.size(), (std::size_t) 1);
}

KTEST (AppProject, the_sample_that_is_left_is_saved_before_the_next_comes_in)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    writeClicks (dir / "other.wav");
    Session s (dir);

    s.load ("clicks.wav");
    s.tap (Btn::X);
    EXPECT (! dir.has ("clicks.wav.kerf"));

    s.load ("other.wav");                               // at once, before the moment has come
    EXPECT (dir.has ("clicks.wav.kerf"));
    EXPECT (! dir.has ("other.wav.kerf"));
    EXPECT_EQ (s.engine.slices().size(), 0);            // the new sample starts with none of the old one's slices
    EXPECT (s.tag() == ProjectKeeper::Tag::none);
    EXPECT (s.toastHas ("Loaded. Press"));

    s.tap (Btn::X);
    s.seconds (2.0);
    EXPECT (dir.has ("other.wav.kerf"));

    // Back to the first: its slice is there.
    s.load ("clicks.wav");
    EXPECT (s.toastHas ("Project restored: 1 slice"));
    EXPECT_EQ (s.engine.slices().size(), 1);
}

KTEST (AppProject, loading_the_same_sample_again_brings_back_even_the_last_unsaved_edit)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir);

    s.load ("clicks.wav");
    s.tap (Btn::X);
    s.load ("clicks.wav");                              // the edit is written first, then read back

    EXPECT (s.toastHas ("Project restored: 1 slice"));
    EXPECT_EQ (s.engine.slices().size(), 1);
}

KTEST (AppProject, a_project_is_made_by_the_first_change_and_never_by_just_walking_about)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir);
    s.load ("clicks.wav");

    s.select();                                         // the sequencer page
    s.tap (Btn::Down);                                  // the cursor moves
    s.tap (Btn::Right);
    s.select();                                         // and back
    s.tap (Btn::Start);                                 // the menu
    s.tap (Btn::B);
    s.seconds (20.0);

    EXPECT (s.tag() == ProjectKeeper::Tag::none);
    EXPECT_EQ (dir.names().size(), (std::size_t) 1);
}

// ------------------------------------------------------------------------------ the settings

KTEST (AppProject, the_settings_switch_stops_the_saving_and_starts_it_again)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir);
    s.load ("clicks.wav");
    s.tap (Btn::X);                                     // an edit that is waiting
    EXPECT (s.tag() == ProjectKeeper::Tag::edited);

    s.openSettingsAtProjects();
    EXPECT (settingShows (s.canvas, "on"));             // the row says how things stand ...
    EXPECT (! settingShows (s.canvas, "off"));

    s.tap (Btn::Right);                                 // off
    EXPECT (! s.cfg.projects);
    EXPECT (s.toastHas ("Projects off"));
    EXPECT (s.tag() == ProjectKeeper::Tag::off);
    EXPECT (settingShows (s.canvas, "off"));            // ... and after the switch
    EXPECT (! settingShows (s.canvas, "on"));

    s.tap (Btn::B);                                     // back to the menu
    s.tap (Btn::B);                                     // and to the slicer
    EXPECT (s.app->screen() == Screen::editor);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("SAVE OFF"));

    s.seconds (30.0);
    EXPECT (! dir.has ("clicks.wav.kerf"));             // nothing is saved while it is off

    s.openSettingsAtProjects();
    EXPECT (settingShows (s.canvas, "off"));
    s.tap (Btn::Left);                                  // on again (the switch flips either way)
    EXPECT (s.cfg.projects);
    EXPECT (settingShows (s.canvas, "on"));
    EXPECT (s.toastHas ("Projects on"));
    EXPECT (! s.toastHas ("next sample"));              // this sample was opened with it on, so it takes hold at once
    s.tap (Btn::B);
    s.tap (Btn::B);

    s.seconds (2.0);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("SAVED"));
    EXPECT (dir.has ("clicks.wav.kerf"));
}

KTEST (AppProject, switching_on_does_not_reach_a_sample_opened_without_it)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir, false);
    s.load ("clicks.wav");
    s.tap (Btn::X);
    EXPECT (s.tag() == ProjectKeeper::Tag::off);

    s.openSettingsAtProjects();
    EXPECT (settingShows (s.canvas, "off"));
    s.tap (Btn::Right);
    EXPECT (s.cfg.projects);
    EXPECT (settingShows (s.canvas, "on"));
    EXPECT (s.toastHas ("from the next sample"));
    s.tap (Btn::B);
    s.tap (Btn::B);

    s.seconds (30.0);
    EXPECT (! dir.has ("clicks.wav.kerf"));
    EXPECT_TEXT (shownTag (s.canvas), std::string ("SAVE OFF"));

    s.load ("clicks.wav");                              // the next time it is opened
    s.tap (Btn::X);
    s.seconds (2.0);
    EXPECT (dir.has ("clicks.wav.kerf"));
}

KTEST (AppProject, the_setting_is_a_key_of_the_configuration_file)
{
    TempFolder dir;
    Config c;
    EXPECT (c.projects);                                // on, unless the person says otherwise

    c.projects = false;
    const fs::path file = dir / "kerf.cfg";
    EXPECT (c.save (file));
    EXPECT (dir.read ("kerf.cfg").find ("projects = 0") != std::string::npos);

    const Config loaded = Config::load (file);
    EXPECT (! loaded.projects);

    c.projects = true;
    EXPECT (c.save (file));
    EXPECT (Config::load (file).projects);
}

// ------------------------------------------------------------------ projects that cannot be used

KTEST (AppProject, a_project_for_another_length_of_audio_is_said_so_and_set_aside_at_the_first_save)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    const std::string foreign = projectText (3 * 44100, 44100);
    dir.write ("clicks.wav.kerf", foreign);

    Session s (dir);
    s.load ("clicks.wav");
    EXPECT (s.toastHas ("Old project does not fit this sample"));
    EXPECT_EQ (s.engine.slices().size(), 0);
    EXPECT (s.tag() == ProjectKeeper::Tag::none);
    s.seconds (10.0);
    EXPECT_TEXT (dir.read ("clicks.wav.kerf"), foreign);

    s.tap (Btn::X);
    s.seconds (2.0);
    EXPECT (s.tag() == ProjectKeeper::Tag::saved);
    EXPECT_TEXT (dir.read ("clicks.wav.kerf.old"), foreign);
    EXPECT_EQ (store::read (dir / "clicks.wav.kerf").project.slices.size(), (std::size_t) 1);
}

KTEST (AppProject, a_damaged_project_is_said_so_and_set_aside_at_the_first_save)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    dir.write ("clicks.wav.kerf", "kerf_project = 1\nslice = start=not a number\n");

    Session s (dir);
    s.load ("clicks.wav");
    EXPECT (s.toastHas ("Project file is damaged - not used"));
    EXPECT (s.tag() == ProjectKeeper::Tag::none);

    s.tap (Btn::X);
    s.seconds (2.0);
    EXPECT_TEXT (dir.read ("clicks.wav.kerf.old"), std::string ("kerf_project = 1\nslice = start=not a number\n"));
    EXPECT (store::read (dir / "clicks.wav.kerf").kind == store::Read::Kind::ok);
}

KTEST (AppProject, a_project_from_a_newer_kerf_is_said_so_and_never_touched)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    const std::string newer = "kerf_project = 7\nsomething = new\n";
    dir.write ("clicks.wav.kerf", newer);

    Session s (dir);
    s.load ("clicks.wav");
    EXPECT (s.toastHas ("Project from a newer Kerf - not used"));
    EXPECT (s.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("NO SAVE"));

    s.tap (Btn::X);
    s.seconds (30.0);
    EXPECT_TEXT (dir.read ("clicks.wav.kerf"), newer);
    EXPECT_EQ (dir.names().size(), (std::size_t) 2);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("NO SAVE"));
}

KTEST (AppProject, when_the_disk_will_not_take_it_the_person_is_told_and_the_save_is_retried)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir);
    s.load ("clicks.wav");

    dir.makeFolder ("clicks.wav.kerf.tmp");             // what the save needs to write is in the way
    s.tap (Btn::X);
    s.seconds (2.0);

    EXPECT (s.toastHas ("Cannot save the project"));
    EXPECT (s.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("NO SAVE"));
    EXPECT (! dir.has ("clicks.wav.kerf"));

    std::error_code ec;
    fs::remove (dir / "clicks.wav.kerf.tmp", ec);
    s.seconds (6.0);                                    // five seconds later it tries again

    EXPECT (s.tag() == ProjectKeeper::Tag::saved);
    EXPECT_TEXT (shownTag (s.canvas), std::string ("SAVED"));
    EXPECT (dir.has ("clicks.wav.kerf"));
}

// ----------------------------------------------------------------------------------- the library

KTEST (AppProject, the_library_marks_the_samples_that_have_a_project)
{
    TempFolder dir;
    writeClicks (dir / "a-clicks.wav");
    writeClicks (dir / "b-clicks.wav");
    writeClicks (dir / "c-clicks.wav");
    dir.write ("a-clicks.wav.kerf", projectText (2 * 44100, 44100));
    dir.write ("c-clicks.wav.kerf.bak", "an old backup does not count");
    dir.write ("b-clicks.kerf", "a project of some other name does not count");

    Session s (dir);
    s.select();                                         // REC -> LOAD
    EXPECT (s.app->screen() == Screen::browser);
    s.frames (2);

    const auto& entries = s.app->browser().entries();
    int marked = 0;

    for (const auto& e : entries)
    {
        if (e.hasProject)
        {
            ++marked;
            EXPECT_TEXT (e.name, std::string ("a-clicks.wav"));
        }
    }

    EXPECT_EQ (marked, 1);
    EXPECT_EQ (entries.size(), (std::size_t) 4);        // "..", and the three samples (nothing else is listed)

    // The mark is on the screen, on the row of that sample and on no other.
    const auto rowOf = [&] (const std::string& name) { for (size_t i = 0; i < entries.size(); ++i) if (entries[i].name == name) return (int) i; return -1; };
    const auto markPixels = [&] (const std::string& name)
    {
        const int y = 52 + rowOf (name) * 30;
        return countPixels (s.canvas, 380, y + 6, 220, 18, theme::brandHi);
    };

    EXPECT (markPixels ("a-clicks.wav") > 40);
    EXPECT_EQ (markPixels ("b-clicks.wav"), 0L);
    EXPECT_EQ (markPixels ("c-clicks.wav"), 0L);

    // The mark is a word in a frame, both in the brand's colour: letters inside the frame, not only the frame.
    {
        const int y = 52 + rowOf ("a-clicks.wav") * 30;
        int left = 10000, right = -1, top = 10000, bottom = -1;

        for (int yy = y + 6; yy < y + 24; ++yy)
            for (int xx = 300; xx < kScreenW; ++xx)
                if (s.canvas.pixelAt (xx, yy) == theme::brandHi)
                {
                    left = std::min (left, xx);
                    right = std::max (right, xx);
                    top = std::min (top, yy);
                    bottom = std::max (bottom, yy);
                }

        EXPECT (right - left >= 28 && bottom - top >= 8);
        EXPECT (countPixels (s.canvas, left + 2, top + 2, right - left - 3, bottom - top - 3, theme::brandHi) > 30);
    }
}

KTEST (AppProject, a_project_that_is_made_shows_in_the_library_without_looking_the_folder_up_again)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    writeClicks (dir / "other.wav");
    Session s (dir);

    s.select();                                         // LOAD: the list is read now, when there is no project yet
    EXPECT (s.app->screen() == Screen::browser);

    const auto markedNames = [&]
    {
        std::string names;

        for (const auto& e : s.app->browser().entries())
            if (e.hasProject)
                names += e.name + ";";

        return names;
    };

    EXPECT_TEXT (markedNames(), std::string());

    s.tap (Btn::A);                                     // the list starts on clicks.wav, the first after "..": load it
    s.app->waitForLoad();
    s.frames (2);
    EXPECT (s.app->screen() == Screen::editor);

    s.tap (Btn::X);
    s.seconds (2.0);
    EXPECT (dir.has ("clicks.wav.kerf"));

    s.tap (Btn::Start);
    s.tap (Btn::Down);                                  // Record sample...
    s.tap (Btn::A);
    EXPECT (s.app->screen() == Screen::record);
    s.select();                                         // and the library again: the page, not a new look at the folder
    EXPECT (s.app->screen() == Screen::browser);

    EXPECT_TEXT (markedNames(), std::string ("clicks.wav;"));
    EXPECT (s.app->browser().entries()[(size_t) s.app->browser().indexOfName ("other.wav")].hasProject == false);
}

KTEST (AppProject, the_library_keeps_its_place_when_a_project_is_made_while_it_is_open)
{
    TempFolder dir;
    writeClicks (dir / "b-second.wav");
    writeClicks (dir / "c-third.wav");
    Session s (dir);
    s.load ("b-second.wav");
    s.tap (Btn::X);                                     // a slice: its project is made 1.5 s from now

    s.tap (Btn::Start);                                 // Load sample...: the library opens on the first sample
    s.tap (Btn::A);
    EXPECT (s.app->screen() == Screen::browser);
    s.tap (Btn::Down);
    EXPECT_TEXT (s.app->browser().entries()[(size_t) s.app->browserSelection()].name, std::string ("c-third.wav"));

    writeClicks (dir / "a-first.wav");                  // the folder changes while the person looks: a sample sorts before the others
    s.seconds (2.0);                                    // the project of b-second.wav is made, and the list is read again
    EXPECT (dir.has ("b-second.wav.kerf"));

    const auto& entries = s.app->browser().entries();
    EXPECT_EQ (entries.size(), (std::size_t) 4);        // "..", a-first, b-second, c-third
    EXPECT_TEXT (entries[(size_t) s.app->browserSelection()].name, std::string ("c-third.wav"));     // the same sample is selected
    EXPECT (entries[(size_t) s.app->browser().indexOfName ("b-second.wav")].hasProject);
    EXPECT (! entries[(size_t) s.app->browser().indexOfName ("c-third.wav")].hasProject);
    EXPECT (s.app->screen() == Screen::browser);
}

KTEST (AppProject, the_library_stays_on_its_row_when_the_sample_it_was_on_leaves_the_folder)
{
    TempFolder dir;
    writeClicks (dir / "a-first.wav");
    writeClicks (dir / "b-second.wav");
    writeClicks (dir / "c-third.wav");
    writeClicks (dir / "d-fourth.wav");
    Session s (dir);
    s.load ("a-first.wav");
    s.tap (Btn::X);                                     // a slice: its project is made 1.5 s from now

    s.tap (Btn::Start);                                 // Load sample...: the library opens on the first sample
    s.tap (Btn::A);
    EXPECT (s.app->screen() == Screen::browser);
    s.tapTimes (Btn::Down, 2);
    EXPECT_EQ (s.app->browserSelection(), 3);
    EXPECT_TEXT (s.app->browser().entries()[3].name, std::string ("c-third.wav"));

    dir.remove ("c-third.wav");                         // the sample the person is on is taken away while they look
    s.seconds (2.0);                                    // the project is made, and the list is read again
    EXPECT (dir.has ("a-first.wav.kerf"));

    // The sample is gone, the row is not: the selection stays where it was, now on the next sample.
    const auto& entries = s.app->browser().entries();
    EXPECT_EQ (entries.size(), (std::size_t) 4);        // "..", a-first, b-second, d-fourth
    EXPECT_EQ (s.app->browserSelection(), 3);
    EXPECT_TEXT (entries[3].name, std::string ("d-fourth.wav"));
    EXPECT (s.app->screen() == Screen::browser);
}

KTEST (AppProject, the_library_is_not_left_past_its_end_when_the_last_sample_leaves_the_folder)
{
    TempFolder dir;
    writeClicks (dir / "a-first.wav");
    writeClicks (dir / "b-second.wav");
    writeClicks (dir / "c-third.wav");
    Session s (dir);
    s.load ("a-first.wav");
    s.tap (Btn::X);

    s.tap (Btn::Start);
    s.tap (Btn::A);
    EXPECT (s.app->screen() == Screen::browser);
    s.tapTimes (Btn::Down, 2);                          // c-third.wav, the last row
    EXPECT_EQ (s.app->browserSelection(), 3);

    s.tap (Btn::Start);                                 // back to the slicer: the library is not on the screen any more
    EXPECT (s.app->screen() == Screen::editor);

    dir.remove ("c-third.wav");
    s.seconds (2.0);                                    // the project is made, and the library's list is read again
    EXPECT (dir.has ("a-first.wav.kerf"));

    // There is one row less: the selection is on the new last row, not one past the end (nothing on the library's own page
    // is there to put it right: it is not up).
    const auto& entries = s.app->browser().entries();
    EXPECT_EQ (entries.size(), (std::size_t) 3);        // "..", a-first, b-second
    EXPECT_EQ (s.app->browserSelection(), 2);
    EXPECT_TEXT (entries[2].name, std::string ("b-second.wav"));
    EXPECT (entries[1].hasProject);

    // The library, by the other way in (the page behind REC, which does not read the folder again): A on the row it is on loads that sample.
    s.tap (Btn::Start);
    s.tap (Btn::Down);                                  // Record sample...
    s.tap (Btn::A);
    EXPECT (s.app->screen() == Screen::record);
    s.select();
    EXPECT (s.app->screen() == Screen::browser);
    EXPECT_EQ (s.app->browserSelection(), 2);

    s.tap (Btn::A, 1);
    s.app->waitForLoad();
    EXPECT_TEXT (pathToUtf8 (s.app->samplePath().filename()), std::string ("b-second.wav"));
}

KTEST (AppProject, a_long_name_gives_way_to_the_mark_and_does_not_run_into_it)
{
    TempFolder dir;
    const std::string plain = "a very long sample name recorded in the studio, take four.wav";
    const std::string marked = "a very long sample name recorded in the studio, take three.wav";
    writeClicks (dir / plain);
    writeClicks (dir / marked);
    dir.write (marked + ".kerf", projectText (2 * 44100, 44100));
    Session s (dir);
    s.select();                                         // LOAD
    s.frames (2);
    EXPECT (s.app->screen() == Screen::browser);

    // "four" sorts before "three": the plain name is the first row (the selected one), the other the second.
    const int plainRow = s.app->browser().indexOfName (plain);
    const int markedRow = s.app->browser().indexOfName (marked);
    EXPECT_EQ (plainRow, 1);
    EXPECT_EQ (markedRow, 2);
    EXPECT_EQ (s.app->browserSelection(), 1);
    EXPECT (! s.app->browser().entries()[1].hasProject);
    EXPECT (s.app->browser().entries()[2].hasProject);

    // A name with no project in the way has the room for 28 glyphs: 25 letters and "...".
    const uint32_t white = rgb (255, 255, 255);
    EXPECT (rowShowsName (s.canvas, plainRow, plain, 28, white));
    EXPECT (! rowShowsName (s.canvas, plainRow, plain, 20, white));          // (and the check does tell one cut from another)

    // The name that has the mark gives way to it: the mark starts at 472 and the cell of a 25th glyph ends at 454, 18 px short of it
    // (a 26th would end at 470, with nothing between the name and the frame).
    EXPECT (rowShowsName (s.canvas, markedRow, marked, 25, theme::text));
    EXPECT (! rowShowsName (s.canvas, markedRow, marked, 24, theme::text));
    EXPECT (! rowShowsName (s.canvas, markedRow, marked, 26, theme::text));
    EXPECT (! rowShowsName (s.canvas, markedRow, marked, 28, theme::text));

    // And the mark itself: right of the name, in the colour of the brand, with room to spare.
    const int y = 52 + markedRow * 30;
    int markLeft = 10000, nameRight = -1;

    for (int yy = y + 6; yy < y + 24; ++yy)
        for (int xx = 0; xx < kScreenW; ++xx)
        {
            const uint32_t px = s.canvas.pixelAt (xx, yy);

            if (px == theme::brandHi && xx >= 300)
                markLeft = std::min (markLeft, xx);

            if (px == theme::text)
                nameRight = std::max (nameRight, xx);
        }

    EXPECT (markLeft < 600);
    EXPECT (nameRight > 200);
    EXPECT (nameRight < markLeft - 16);
}

KTEST (AppProject, a_project_made_by_the_save_before_the_next_sample_shows_in_the_library_too)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    writeClicks (dir / "other.wav");
    Session s (dir);

    s.select();                                         // LOAD: the list is read now, when there is no project yet
    s.tap (Btn::A);                                     // clicks.wav
    s.app->waitForLoad();
    s.frames (2);
    s.tap (Btn::X);
    EXPECT (! dir.has ("clicks.wav.kerf"));             // the moment to save has not come

    s.load ("other.wav");                               // the next sample is asked for at once: the first is saved on its way out
    EXPECT (dir.has ("clicks.wav.kerf"));
    EXPECT (! dir.has ("other.wav.kerf"));

    s.tap (Btn::Start);
    s.tap (Btn::Down);                                  // Record sample...
    s.tap (Btn::A);
    EXPECT (s.app->screen() == Screen::record);
    s.select();                                         // the library again: the page, not a new look at the folder
    EXPECT (s.app->screen() == Screen::browser);

    const auto& entries = s.app->browser().entries();
    EXPECT (entries[(size_t) s.app->browser().indexOfName ("clicks.wav")].hasProject);
    EXPECT (! entries[(size_t) s.app->browser().indexOfName ("other.wav")].hasProject);
}

KTEST (AppProject, a_save_that_fails_on_the_way_out_is_still_told_when_the_next_sample_is_in)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    writeClicks (dir / "other.wav");
    Session s (dir);

    s.load ("clicks.wav");
    s.tap (Btn::X);
    dir.makeFolder ("clicks.wav.kerf.tmp");             // the file cannot be made

    s.load ("other.wav");                               // the flush on the way out fails
    EXPECT (! dir.has ("clicks.wav.kerf"));
    EXPECT (s.tag() == ProjectKeeper::Tag::none);       // the new sample has no trouble of its own
    EXPECT (s.toastHas ("Cannot save the project"));    // but the person is told about the one that was left
    EXPECT_EQ (s.logged ("cannot save clicks.wav.kerf"), 1);
}

// -------------------------------------------------------------------------- recording a sample

KTEST (AppProject, a_take_becomes_a_sample_whose_slices_are_kept_under_its_own_name)
{
    TempFolder dir;

    {
        Session s (dir, true, true);
        s.frames (2);                                   // Kerf starts on REC and opens the input

        s.tap (Btn::A);                                 // record
        s.seconds (1.5);
        s.tap (Btn::A);                                 // stop
        s.tap (Btn::A);                                 // save and load
        s.app->waitForLoad();
        s.frames (2);

        EXPECT (s.app->screen() == Screen::editor);
        EXPECT (dir.has ("rec-001.wav"));
        EXPECT (s.tag() == ProjectKeeper::Tag::none);

        s.tap (Btn::X);
        s.seconds (2.0);
        EXPECT (s.tag() == ProjectKeeper::Tag::saved);
        EXPECT (dir.has ("rec-001.wav.kerf"));
    }

    Session again (dir, true, true);
    again.load ("rec-001.wav");
    EXPECT (again.toastHas ("Project restored: 1 slice"));
}

KTEST (AppProject, nothing_is_written_while_a_take_is_in_hand)
{
    TempFolder dir;
    writeClicks (dir / "clicks.wav");
    Session s (dir, true, true);
    s.frames (2);
    s.load ("clicks.wav");

    s.tap (Btn::X);                                     // an edit, waiting for its moment
    EXPECT (s.tag() == ProjectKeeper::Tag::edited);

    s.tap (Btn::Start);
    s.tap (Btn::Down);                                  // Record sample...
    s.tap (Btn::A);
    EXPECT (s.app->screen() == Screen::record);
    s.frames (2);

    s.tap (Btn::A);                                     // a take starts: the disk and the time are its own
    EXPECT (s.app->recording().phase() == RecordSession::Phase::recording);
    s.seconds (15.0);
    EXPECT (! dir.has ("clicks.wav.kerf"));

    s.tap (Btn::A);                                     // stopped, but still in hand: it waits to be saved or thrown away
    EXPECT (s.app->recording().phase() == RecordSession::Phase::stopped);
    s.seconds (15.0);
    EXPECT (! dir.has ("clicks.wav.kerf"));
    EXPECT (s.tag() == ProjectKeeper::Tag::edited);

    s.tap (Btn::B);                                     // thrown away: the work is saved
    EXPECT (s.app->recording().phase() == RecordSession::Phase::idle);
    s.frames (2);
    EXPECT (dir.has ("clicks.wav.kerf"));
    EXPECT (s.tag() == ProjectKeeper::Tag::saved);
}
