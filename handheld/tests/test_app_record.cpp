// Tests for the Record screen as the application runs it: buttons go in, a take is recorded from a
// pretend microphone, a WAV file is written in the folder and the sample is loaded into the slicer.
//
// The pretend inputs (fake_input.h) make their sound as the application's clock runs, 60 frames to
// the second, so a take of two seconds is 120 frames and every run is the same.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
 #include <process.h>
#else
 #include <unistd.h>
#endif

#include "app.h"
#include "audio_input.h"
#include "canvas.h"
#include "config.h"
#include "engine.h"
#include "fake_input.h"
#include "help_text.h"
#include "minitest.h"
#include "path_util.h"
#include "sample.h"
#include "theme.h"

using namespace kerf;
using namespace kerf::ui;
namespace fs = std::filesystem;

namespace
{
    int processId()
    {
#if defined(_WIN32)
        return (int) _getpid();
#else
        return (int) ::getpid();
#endif
    }

    // A folder of its own for the takes, removed when the test is over.
    struct Folder
    {
        fs::path path;

        Folder()
        {
            static int counter = 0;
            path = fs::temp_directory_path() / ("kerf-record-test-" + std::to_string (processId()) + "-" + std::to_string (++counter));
            std::error_code ec;
            fs::remove_all (path, ec);
            fs::create_directories (path, ec);
        }

        ~Folder()
        {
            std::error_code ec;
            fs::remove_all (path, ec);
        }

        bool has (const std::string& name) const { std::error_code ec; return fs::exists (path / name, ec); }

        int countWavs() const
        {
            int n = 0;
            std::error_code ec;

            for (const auto& e : fs::directory_iterator (path, ec))
                if (e.path().extension() == ".wav")
                    ++n;

            return n;
        }
    };

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

    FakeDevice mixer()
    {
        FakeDevice d;
        d.id = "fake:mixer";
        d.name = "Pretend 12-channel mixer";
        d.channels = 12;
        d.rate = 44100;
        d.toneHz = 110.0;
        return d;
    }

    // The application with a pretend sound card behind its Record screen, and a pad that taps buttons.
    struct Rig
    {
        Folder folder;                               // first: removed last
        Engine engine;
        Config cfg;
        InputHub hub;                                // before the app, which closes its input when it goes
        FakeInput* fake = nullptr;
        std::unique_ptr<App> app;
        Canvas canvas { kScreenW, kScreenH };
        std::vector<float> block;
        fs::path configFile;
        bool keyboard = false;                       // the last device touched: a pad, unless a test says otherwise

        explicit Rig (std::vector<FakeDevice> devices = { microphone(), mixer() }, bool withHub = true, bool withConfigFile = false)
        {
            engine.prepare (44100.0);
            cfg.projects = false;                        // these tests are about takes; projects have tests of their own (test_app_project.cpp)

            if (withHub)
            {
                auto input = std::make_unique<FakeInput> (std::move (devices));
                fake = input.get();
                hub.add (std::move (input));
            }

            if (withConfigFile)
                configFile = folder.path / "kerf.cfg";

            app = std::make_unique<App> (engine, cfg, configFile, folder.path);

            if (withHub)
                app->setInputHub (&hub);
        }

        ~Rig() { app.reset(); }

        void frames (int n, const Pad& pad)
        {
            for (int i = 0; i < n; ++i)
            {
                app->update (pad, 1.0f / 60.0f);
                block.assign (2 * 735, 0.0f);
                engine.render (block.data(), 735);
                app->draw (canvas);
            }
        }

        // Quiet frames: the pad says which device was touched last, as the program's pad does.
        void frames (int n)
        {
            Pad quiet;
            quiet.usingKeyboard = keyboard;
            frames (n, quiet);
        }

        // A button goes down for one frame; then `settle` quiet frames. From the keyboard or from a pad.
        void tap (Btn b, int settle = 2, bool fromKeyboard = false)
        {
            keyboard = fromKeyboard;

            Pad pad;
            pad.pressed[(int) b] = pad.repeated[(int) b] = pad.down[(int) b] = true;
            pad.anyInput = true;
            pad.usingKeyboard = keyboard;
            frames (1, pad);
            frames (settle);
        }

        // A button that is held: pressed on the first frame, then repeated by the key repeat of the pad.
        void hold (Btn b, int repeats)
        {
            Pad first;
            first.pressed[(int) b] = first.repeated[(int) b] = first.down[(int) b] = true;
            first.anyInput = true;
            first.usingKeyboard = keyboard;
            frames (1, first);

            Pad again;
            again.repeated[(int) b] = again.down[(int) b] = true;
            again.anyInput = true;
            again.usingKeyboard = keyboard;
            frames (repeats, again);
            frames (2);
        }

        RecordSession::Phase phase() const { return app->recording().phase(); }
        const Recorder& recorder() const { return app->recording().recorder(); }

        // Select, as a tap: down for a frame, up on the next (Kerf acts when it is let go).
        void select (bool fromKeyboard = false)
        {
            keyboard = fromKeyboard;

            Pad down;
            down.pressed[(int) Btn::Select] = down.repeated[(int) Btn::Select] = down.down[(int) Btn::Select] = true;
            down.anyInput = true;
            down.usingKeyboard = keyboard;
            frames (1, down);

            Pad up;
            up.released[(int) Btn::Select] = true;
            up.anyInput = true;
            up.usingKeyboard = keyboard;
            frames (1, up);
            frames (2);
        }

        // Kerf starts on the Record page: this lets the first input start (and comes back from the library with Select).
        void enter()
        {
            if (app->screen() == Screen::browser)
                select();

            frames (2);
        }

        // Leaves the Record page for the library the way a person does: with Select.
        void leave()
        {
            select();
        }

        // Records for `seconds` and stops.
        void takeFor (double seconds)
        {
            tap (Btn::A);
            frames ((int) std::lround (seconds * 60.0));
            tap (Btn::A);
        }

        // Saves and loads, and waits for the loading to be over.
        void saveAndLoad()
        {
            tap (Btn::A);
            app->waitForLoad();
        }

        bool toastHas (const std::string& part) const { return app->toast().find (part) != std::string::npos; }
    };

    // A tenth of a second of a tone, as a WAV file: a sample for the slicer to have.
    void writeTone (const fs::path& wav)
    {
        AudioBuffer tone (1, 4410);

        for (int i = 0; i < 4410; ++i)
            tone.set (0, i, 0.5f * std::sin (0.05f * (float) i));

        const std::string bytes = encodeWav16 (tone, 44100.0);
        std::ofstream out (wav, std::ios::binary);
        out.write (bytes.data(), (std::streamsize) bytes.size());
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

    // The loudest sample of a channel of a sample that was loaded.
    float peakOf (const kerf::Sample& s, int channel)
    {
        float peak = 0.0f;

        for (int i = 0; i < s.data.numFrames(); ++i)
            peak = std::max (peak, std::fabs (s.data.get (channel, i)));

        return peak;
    }
}

// ----------------------------------------------------------------------------------- coming and going

KTEST (RecordScreen, Kerf_starts_on_it_and_Select_flips_it_with_the_library)
{
    Rig r;
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.app->page() == Screen::record);
    r.frames (2);
    EXPECT (r.app->recording().active() && r.hub.isOpen());

    r.select();                                     // the library is the page behind it
    EXPECT (r.app->screen() == Screen::browser);
    EXPECT (r.app->page() == Screen::browser);
    EXPECT (! r.app->recording().active());
    EXPECT (! r.hub.isOpen());                      // the input is closed when the page is left

    r.tap (Btn::Y);                                 // Y does nothing in the library any more
    EXPECT (r.app->screen() == Screen::browser);

    r.select();                                     // and the other way round
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.app->recording().active() && r.hub.isOpen());
    r.select (true);                                // from the keyboard too (the 3 key)
    EXPECT (r.app->screen() == Screen::browser);
}

KTEST (RecordScreen, with_no_sample_yet_B_has_nowhere_to_go_and_Start_opens_the_menu)
{
    Rig r;
    r.frames (2);

    r.tap (Btn::B);                                 // nothing to go back to
    EXPECT (r.app->screen() == Screen::record);

    r.tap (Btn::Start);
    EXPECT (r.app->screen() == Screen::menu);
    EXPECT (r.app->page() == Screen::record);       // the menu belongs to the page it was opened from
    EXPECT (! r.app->recording().active() && ! r.hub.isOpen());

    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.app->recording().active());

    // The library has the same menu, and B leads back to the library.
    r.select();
    r.tap (Btn::Start);
    EXPECT (r.app->screen() == Screen::menu);
    EXPECT (r.app->page() == Screen::browser);
    r.tap (Btn::Start);
    EXPECT (r.app->screen() == Screen::browser);
}

KTEST (RecordScreen, the_menu_before_there_is_a_sample_has_settings_help_the_input_test_and_the_way_out)
{
    Rig r;
    r.frames (2);
    r.tap (Btn::Start);
    EXPECT (r.app->screen() == Screen::menu);

    r.tap (Btn::A);                                 // Settings
    EXPECT (r.app->screen() == Screen::settings);
    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::menu);

    r.tap (Btn::Down);                              // Controls / help: B leads back to the page the menu came from
    r.tap (Btn::A);
    EXPECT (r.app->screen() == Screen::help);
    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.app->recording().active());

    r.tap (Btn::Start);
    r.tap (Btn::Down);
    r.tap (Btn::Down);
    r.tap (Btn::A);                                 // Input test
    EXPECT (r.app->screen() == Screen::inputTest);
    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::menu);
    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::record);

    r.tap (Btn::Start);
    r.tap (Btn::Down);
    r.tap (Btn::Down);
    r.tap (Btn::Down);
    EXPECT (! r.app->wantsQuit());
    r.tap (Btn::A);                                 // Quit
    EXPECT (r.app->wantsQuit());
}

KTEST (RecordScreen, with_a_sample_B_and_Start_lead_back_to_the_page_it_is_worked_on)
{
    Rig r;
    const auto wav = r.folder.path / "tone.wav";
    writeTone (wav);
    r.app->loadFile (wav);
    r.app->waitForLoad();
    EXPECT (r.app->screen() == Screen::editor);

    // From the slicer menu ("Record sample..." comes after "Load sample...").
    r.tap (Btn::Start);
    r.tap (Btn::Down);
    r.tap (Btn::A);
    EXPECT (r.app->screen() == Screen::record);
    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::editor);

    r.tap (Btn::Start);
    r.tap (Btn::Down);
    r.tap (Btn::A);
    EXPECT (r.app->screen() == Screen::record);
    r.tap (Btn::Start);                             // Start leads back too (the menu is for before there is a sample)
    EXPECT (r.app->screen() == Screen::editor);

    // The library leads back the same way, and Select flips between the two pages that get a sample.
    r.tap (Btn::Start);
    r.tap (Btn::A);                                 // "Load sample..."
    EXPECT (r.app->screen() == Screen::browser);
    r.select();
    EXPECT (r.app->screen() == Screen::record);
    r.select();
    EXPECT (r.app->screen() == Screen::browser);
    r.tap (Btn::Start);
    EXPECT (r.app->screen() == Screen::editor);
    EXPECT (r.app->recording().active() == false);
}

KTEST (RecordScreen, B_climbs_the_library_and_at_the_very_top_leads_back_to_the_sample_if_there_is_one)
{
    // No sample: there is nowhere to go back to, so B only climbs and then stays at the top.
    {
        Rig r;
        r.leave();
        EXPECT (r.app->screen() == Screen::browser);

        for (int i = 0; i < 64; ++i)
            r.tap (Btn::B, 1);

        EXPECT (r.app->screen() == Screen::browser);
    }

    // With a sample: the same climb, and then B leads back to the slicer.
    {
        Rig r;
        const auto wav = r.folder.path / "tone.wav";
        writeTone (wav);
        r.app->loadFile (wav);
        r.app->waitForLoad();
        EXPECT (r.app->screen() == Screen::editor);

        r.tap (Btn::Start);
        r.tap (Btn::A);                             // "Load sample..."
        EXPECT (r.app->screen() == Screen::browser);

        for (int i = 0; i < 64 && r.app->screen() == Screen::browser; ++i)
            r.tap (Btn::B, 1);

        EXPECT (r.app->screen() == Screen::editor);
    }
}

KTEST (RecordScreen, Select_leaves_a_take_that_is_in_hand_alone_and_says_why)
{
    Rig r;
    r.frames (2);
    r.tap (Btn::A);
    r.frames (20);
    EXPECT (r.phase() == RecordSession::Phase::recording);

    r.select();
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.phase() == RecordSession::Phase::recording);
    EXPECT (r.toastHas ("Stop the take first"));

    r.tap (Btn::A);
    EXPECT (r.phase() == RecordSession::Phase::stopped);
    r.select();
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.phase() == RecordSession::Phase::stopped && r.recorder().hasTake());
    EXPECT (r.toastHas ("Save or discard the take first"));

    r.tap (Btn::B);                                 // thrown away: now the page can be left
    r.select();
    EXPECT (r.app->screen() == Screen::browser);
}

KTEST (RecordScreen, Select_is_a_tap_only_when_it_goes_down_and_up_on_the_page_and_nothing_else_comes_with_it)
{
    Rig r;
    r.frames (2);

    // A release that was never a press here (Select went down on another screen): nothing happens.
    Pad stray;
    stray.released[(int) Btn::Select] = true;
    r.frames (1, stray);
    EXPECT (r.app->screen() == Screen::record);

    // Select with another button pressed while it is held: a chord, not a tap.
    Pad down;
    down.pressed[(int) Btn::Select] = down.down[(int) Btn::Select] = true;
    r.frames (1, down);

    Pad chord;
    chord.down[(int) Btn::Select] = true;
    chord.pressed[(int) Btn::Down] = chord.repeated[(int) Btn::Down] = chord.down[(int) Btn::Down] = true;
    r.frames (1, chord);

    Pad up;
    up.released[(int) Btn::Select] = true;
    r.frames (1, up);
    r.frames (2);
    EXPECT (r.app->screen() == Screen::record);

    // ... and the next plain tap works as it should, in both directions.
    r.select();
    EXPECT (r.app->screen() == Screen::browser);
    r.frames (1, down);
    r.frames (1, chord);
    r.frames (1, up);
    EXPECT (r.app->screen() == Screen::browser);
    r.select();
    EXPECT (r.app->screen() == Screen::record);

    // The other button may also come down in the very frame in which Select is let go: not a tap either.
    r.frames (1, down);
    Pad both;
    both.released[(int) Btn::Select] = true;
    both.pressed[(int) Btn::Down] = both.repeated[(int) Btn::Down] = both.down[(int) Btn::Down] = true;
    r.frames (1, both);
    r.frames (2);
    EXPECT (r.app->screen() == Screen::record);
}

KTEST (RecordScreen, a_Select_that_went_down_on_another_screen_is_not_a_tap_on_this_one)
{
    Rig r;
    r.leave();
    EXPECT (r.app->screen() == Screen::browser);

    Pad down;
    down.pressed[(int) Btn::Select] = down.down[(int) Btn::Select] = true;
    r.frames (1, down);                             // Select goes down on the library ...

    Pad help;
    help.down[(int) Btn::Select] = true;
    help.commands.push_back (KeyCommand::help);
    r.frames (1, help);                             // ... the help comes up while it is held ...
    EXPECT (r.app->screen() == Screen::help);

    Pad close;
    close.down[(int) Btn::Select] = true;
    close.pressed[(int) Btn::B] = close.down[(int) Btn::B] = true;
    r.frames (1, close);                            // ... and B closes it, with Select still down
    EXPECT (r.app->screen() == Screen::browser);

    Pad up;
    up.released[(int) Btn::Select] = true;
    r.frames (1, up);                               // let go back on the library: no tap, the press was somewhere else
    r.frames (2);
    EXPECT (r.app->screen() == Screen::browser);

    r.select();                                     // a whole tap still works
    EXPECT (r.app->screen() == Screen::record);
}

KTEST (RecordScreen, a_tap_that_changes_nothing_leaves_nothing_armed_for_a_release_later)
{
    Rig r;
    r.frames (2);
    r.tap (Btn::A);                                 // a take is running: Select says so, and stays
    r.frames (20);
    r.select();
    EXPECT (r.toastHas ("Stop the take first"));
    EXPECT (r.app->screen() == Screen::record);

    r.tap (Btn::A);                                 // stopped, thrown away: the page may be left now
    r.tap (Btn::B);
    EXPECT (r.phase() == RecordSession::Phase::idle);

    Pad stray;
    stray.released[(int) Btn::Select] = true;       // a release with no press before it: nothing
    r.frames (1, stray);
    r.frames (2);
    EXPECT (r.app->screen() == Screen::record);
}

KTEST (RecordScreen, the_help_key_in_the_library_leads_back_to_the_library)
{
    Rig r;
    r.leave();
    EXPECT (r.app->screen() == Screen::browser);

    Pad pad;
    pad.usingKeyboard = false;
    pad.commands.push_back (KeyCommand::help);
    r.frames (1, pad);
    EXPECT (r.app->screen() == Screen::help);
    EXPECT_EQ (r.app->helpPageIndex(), kHelpPadHome);        // the page for REC and LOAD, with the buttons of a gamepad
    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::browser);

    Pad keys;
    keys.usingKeyboard = true;
    keys.commands.push_back (KeyCommand::help);
    r.frames (1, keys);
    EXPECT_EQ (r.app->helpPageIndex(), kHelpKeyboardHome);   // ... or the keyboard's
}

KTEST (RecordScreen, the_help_from_the_menu_before_there_is_a_sample_opens_the_page_for_this_pair_of_pages)
{
    Rig r;
    r.frames (2);
    r.tap (Btn::Start);
    r.tap (Btn::Down);
    r.tap (Btn::A);                                 // Controls / help
    EXPECT (r.app->screen() == Screen::help);
    EXPECT_EQ (r.app->helpPageIndex(), kHelpPadHome);

    r.tap (Btn::Right);                             // the pages go round: REC / LOAD, keyboard, then the slicer again
    EXPECT_EQ (r.app->helpPageIndex(), kHelpKeyboardHome);
    r.tap (Btn::Right);
    EXPECT_EQ (r.app->helpPageIndex(), 0);
    r.tap (Btn::Left);
    EXPECT_EQ (r.app->helpPageIndex(), kHelpKeyboardHome);
    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::record);

    // With a sample loaded the help opens on the page of the slicer, as before.
    const auto wav = r.folder.path / "tone.wav";
    writeTone (wav);
    r.app->loadFile (wav);
    r.app->waitForLoad();
    r.tap (Btn::Start);
    for (int i = 0; i < 10; ++i)
        r.tap (Btn::Down, 0);

    r.tap (Btn::A);
    EXPECT (r.app->screen() == Screen::help);
    EXPECT_EQ (r.app->helpPageIndex(), 0);
}

KTEST (RecordScreen, without_any_input_it_says_so_and_cannot_record)
{
    Rig r ({}, false);
    r.enter();
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (! r.app->recording().available());
    EXPECT (r.app->recording().statusText() == "No audio input found");

    r.tap (Btn::A);
    EXPECT (r.phase() == RecordSession::Phase::idle);
    EXPECT (r.toastHas ("No input is ready"));

    r.leave();
    EXPECT (r.app->screen() == Screen::browser);
}

KTEST (RecordScreen, the_input_that_Kerf_reads_itself_comes_first_and_the_one_used_last_is_opened_again)
{
    Rig r ({ mixer(), microphone() });              // listed the other way round
    r.enter();
    EXPECT (r.app->recording().sources().size() == 2);
    EXPECT (r.app->recording().sourceName() == "Pretend USB microphone");
    EXPECT (r.app->recording().ready());

    r.tap (Btn::R1);                                // next input
    EXPECT (r.app->recording().sourceName() == "Pretend 12-channel mixer");
    r.tap (Btn::R1);                                // and round to the first again
    EXPECT (r.app->recording().sourceName() == "Pretend USB microphone");
    r.tap (Btn::L1);
    EXPECT (r.app->recording().sourceName() == "Pretend 12-channel mixer");
    EXPECT (r.fake->starts() == 4);

    r.leave();
    EXPECT (r.cfg.recordInput == "fake:mixer");

    r.enter();
    EXPECT (r.app->recording().sourceName() == "Pretend 12-channel mixer");
    EXPECT (r.fake->starts() == 5);
}

KTEST (RecordScreen, the_rows_choose_the_input_the_channels_and_the_gain_and_are_remembered)
{
    Rig r ({ microphone(), mixer() }, true, true);
    r.enter();
    r.tap (Btn::R1);                                // the mixer
    EXPECT (r.recorder().channels() == 12);
    EXPECT (r.app->recording().choices().size() == 18);          // 6 pairs and 12 channels on their own
    EXPECT (r.app->recording().choices()[(size_t) r.app->recording().choiceIndex()].label == "1+2  stereo");

    r.tap (Btn::Down);                              // the channels row
    r.tap (Btn::Right);
    r.tap (Btn::Right);
    EXPECT (r.app->recording().choices()[(size_t) r.app->recording().choiceIndex()].label == "5+6  stereo");
    r.tap (Btn::Left);
    EXPECT (r.app->recording().choices()[(size_t) r.app->recording().choiceIndex()].label == "3+4  stereo");

    r.tap (Btn::Down);                              // the gain row
    r.tap (Btn::Right);
    r.tap (Btn::Right);
    EXPECT (r.app->recording().gainDb() == 6);
    r.tap (Btn::Left);
    EXPECT (r.app->recording().gainDb() == 3);

    r.tap (Btn::Down);                              // round to the input row again
    r.tap (Btn::Left);                              // the microphone
    EXPECT (r.app->recording().sourceName() == "Pretend USB microphone");
    r.tap (Btn::Right);
    EXPECT (r.app->recording().sourceName() == "Pretend 12-channel mixer");

    r.leave();
    EXPECT (r.cfg.recordInput == "fake:mixer");
    EXPECT (r.cfg.recordGainDb == 3);
    EXPECT (r.cfg.recordChannels == "3+4  stereo");

    // They are in the settings file too.
    const Config saved = Config::load (r.configFile);
    EXPECT (saved.recordInput == "fake:mixer" && saved.recordGainDb == 3 && saved.recordChannels == "3+4  stereo");

    // The next time the screen opens with the same choices.
    r.enter();
    EXPECT (r.app->recording().sourceName() == "Pretend 12-channel mixer");
    EXPECT (r.app->recording().gainDb() == 3);
    EXPECT (r.app->recording().choices()[(size_t) r.app->recording().choiceIndex()].label == "3+4  stereo");
}

KTEST (RecordScreen, the_row_that_is_chosen_wraps_and_a_gain_stays_between_its_limits)
{
    Rig r;
    r.enter();
    r.tap (Btn::Up);                                // wraps up to the last row: gain
    for (int i = 0; i < 20; ++i)
        r.tap (Btn::Right, 0);

    EXPECT (r.app->recording().gainDb() == RecordSession::kMaxGainDb);

    for (int i = 0; i < 30; ++i)
        r.tap (Btn::Left, 0);

    EXPECT (r.app->recording().gainDb() == RecordSession::kMinGainDb);
    r.tap (Btn::Down);                              // wraps down to the input row
    r.tap (Btn::Right);
    EXPECT (r.app->recording().sourceName() == "Pretend 12-channel mixer");
}

KTEST (RecordScreen, the_screen_always_starts_on_the_input_row)
{
    Rig r;
    r.enter();
    r.tap (Btn::Down);
    r.tap (Btn::Down);                              // the gain row
    r.leave();

    r.enter();
    r.tap (Btn::Right);                             // on the input row this is the next input ...
    EXPECT (r.app->recording().sourceName() == "Pretend 12-channel mixer");
    EXPECT (r.app->recording().gainDb() == 0);      // ... and not more gain
}

KTEST (RecordScreen, a_button_that_is_held_runs_through_the_gain_but_not_through_the_inputs)
{
    FakeDevice third = microphone();
    third.id = "fake:third";
    third.name = "A third input";
    third.direct = false;

    Rig r ({ microphone(), mixer(), third });
    r.enter();
    EXPECT (r.app->recording().sources().size() == 3);

    r.hold (Btn::Right, 12);                        // opening an input is not something to repeat
    EXPECT (r.app->recording().sourceIndex() == 1);
    EXPECT (r.fake->starts() == 2);

    r.tap (Btn::Down);
    r.tap (Btn::Down);
    r.hold (Btn::Right, 5);                         // the first frame and five repeats: six steps of 3 dB
    EXPECT (r.app->recording().gainDb() == 18);
}

KTEST (RecordScreen, the_settings_of_the_recording_are_left_alone_when_the_page_is_not_up_at_the_end)
{
    Rig r ({ microphone(), mixer() }, true, true);
    r.leave();                                      // the program ends on the library: the Record page is not up
    r.cfg.recordInput = "fake:mixer";
    r.cfg.recordGainDb = 9;
    r.cfg.recordChannels = "3+4  stereo";

    r.app.reset();
    EXPECT (r.cfg.recordInput == "fake:mixer");
    EXPECT_EQ (r.cfg.recordGainDb, 9);
    EXPECT (r.cfg.recordChannels == "3+4  stereo");
    EXPECT (Config::load (r.configFile).recordGainDb == 9);
}

// ----------------------------------------------------------------------------------------- the take

KTEST (RecordScreen, a_take_is_recorded_saved_in_the_folder_and_loaded_into_the_slicer)
{
    Rig r;
    r.enter();
    EXPECT (r.phase() == RecordSession::Phase::idle);

    r.tap (Btn::A);
    EXPECT (r.phase() == RecordSession::Phase::recording);
    r.frames (120);                                 // two seconds of the microphone
    EXPECT (std::fabs (r.recorder().takeSeconds() - 2.0) < 0.1);

    r.tap (Btn::A);
    EXPECT (r.phase() == RecordSession::Phase::stopped);
    const double length = r.recorder().takeSeconds();
    EXPECT (std::fabs (length - 2.0) < 0.15);
    EXPECT (r.recorder().takeChannels() == 2 && r.recorder().sampleRate() == 48000);
    EXPECT (r.folder.countWavs() == 0);             // nothing is written until it is asked for

    r.saveAndLoad();
    EXPECT (r.app->lastSaved().filename() == "rec-001.wav");
    EXPECT (r.folder.has ("rec-001.wav"));
    EXPECT (r.folder.countWavs() == 1);
    EXPECT (r.app->screen() == Screen::editor);     // the recording is in the slicer
    EXPECT (! r.app->recording().active());
    EXPECT (! r.hub.isOpen());
    EXPECT (r.toastHas ("Saved rec-001.wav"));

    const auto sample = r.engine.sample();
    EXPECT (sample != nullptr);

    if (sample == nullptr)
        return;

    EXPECT (sample->name == "rec-001.wav");
    EXPECT (sample->data.numChannels() == 2);
    EXPECT (sample->sampleRate == 48000.0);
    EXPECT (std::fabs (sample->durationSeconds() - length) < 0.001);

    // It is what the microphone sent: 0.05 on the left, 0.12 on the right (fake_input.h).
    EXPECT (std::fabs (peakOf (*sample, 0) - fakeAmplitude (0)) < 0.002);
    EXPECT (std::fabs (peakOf (*sample, 1) - fakeAmplitude (1)) < 0.002);
}

KTEST (RecordScreen, what_is_saved_is_what_the_channels_and_the_gain_say)
{
    Rig r;
    r.enter();
    r.tap (Btn::R1);                                // the mixer: channel k has a tone of its own and a level of its own
    r.tap (Btn::Down);
    r.tap (Btn::Right);                             // channels 3+4
    r.tap (Btn::Down);
    r.tap (Btn::Right);                             // +3 dB
    r.takeFor (0.5);
    r.saveAndLoad();

    const auto sample = r.engine.sample();
    EXPECT (sample != nullptr);

    if (sample == nullptr)
        return;

    const float gain = std::pow (10.0f, 3.0f / 20.0f);
    EXPECT (sample->data.numChannels() == 2 && sample->sampleRate == 44100.0);
    EXPECT (std::fabs (peakOf (*sample, 0) - fakeAmplitude (2) * gain) < 0.003);
    EXPECT (std::fabs (peakOf (*sample, 1) - fakeAmplitude (3) * gain) < 0.003);
}

KTEST (RecordScreen, a_mono_channel_makes_a_mono_file)
{
    Rig r;
    r.enter();
    r.tap (Btn::R1);                                // the mixer
    r.tap (Btn::Down);

    for (int i = 0; i < 6; ++i)                     // past the six pairs: the first channel on its own
        r.tap (Btn::Right, 0);

    EXPECT (r.app->recording().choices()[(size_t) r.app->recording().choiceIndex()].label == "1  mono");
    r.takeFor (0.5);
    r.saveAndLoad();

    const auto sample = r.engine.sample();
    EXPECT (sample != nullptr && sample->data.numChannels() == 1);
}

KTEST (RecordScreen, X_saves_without_loading_and_the_next_take_gets_the_next_number)
{
    Rig r;
    r.enter();
    r.takeFor (0.5);
    r.tap (Btn::X);
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.phase() == RecordSession::Phase::idle);   // ready for another one
    EXPECT (r.folder.has ("rec-001.wav"));
    EXPECT (r.toastHas ("Saved rec-001.wav"));
    EXPECT (r.engine.sample() == nullptr);              // nothing was loaded

    r.takeFor (0.5);
    r.tap (Btn::X);
    EXPECT (r.folder.has ("rec-002.wav") && r.folder.countWavs() == 2);

    r.takeFor (0.5);
    r.saveAndLoad();
    EXPECT (r.folder.has ("rec-003.wav"));
    EXPECT (r.app->lastSaved().filename() == "rec-003.wav");
    EXPECT (r.engine.sample() != nullptr);
}

KTEST (RecordScreen, the_screen_is_left_at_once_when_a_take_is_saved_and_loaded)
{
    Rig r;
    r.enter();
    r.takeFor (0.5);

    r.tap (Btn::A, 1);                              // the frame that writes the file, and nothing after it
    EXPECT (r.app->isLoading());                    // the sample is on its way in ...
    EXPECT (r.app->screen() == Screen::browser);    // ... and the library is up (there was no sample before), not the Record screen
    EXPECT (! r.app->recording().active() && ! r.hub.isOpen());

    r.app->waitForLoad();
    EXPECT (r.app->screen() == Screen::editor);

    // With a sample already in the slicer, the page that had it is the one under the loading box.
    r.tap (Btn::Start);
    r.tap (Btn::Down);
    r.tap (Btn::A);                                 // "Record sample..."
    EXPECT (r.app->screen() == Screen::record);
    r.takeFor (0.5);
    r.tap (Btn::A, 1);
    EXPECT (r.app->isLoading());
    EXPECT (r.app->screen() == Screen::editor);
    EXPECT (! r.app->recording().active() && ! r.hub.isOpen());
    r.app->waitForLoad();
    EXPECT (r.app->screen() == Screen::editor);
    EXPECT (r.app->lastSaved().filename() == "rec-002.wav");
}

KTEST (RecordScreen, the_toast_after_loading_names_the_key_that_is_in_use)
{
    Rig pad;
    pad.enter();
    pad.takeFor (0.5);
    pad.saveAndLoad();
    EXPECT (pad.toastHas ("Saved rec-001.wav. Press Y to auto-detect"));

    Rig keys;
    keys.enter();
    keys.takeFor (0.5);
    keys.tap (Btn::A, 2, true);                     // Space
    keys.app->waitForLoad();
    EXPECT (keys.toastHas ("Saved rec-001.wav. Press 2 to auto-detect"));
}

KTEST (RecordScreen, a_take_that_cannot_be_saved_says_so_and_is_kept_for_another_try)
{
    Rig r;
    r.enter();
    r.takeFor (0.5);

    // A file stands where the folder should be: nothing can be written there.
    std::error_code ec;
    fs::remove_all (r.folder.path, ec);
    {
        std::ofstream blocker (r.folder.path, std::ios::binary);
        blocker << "in the way";
    }

    r.tap (Btn::A);
    EXPECT (r.toastHas ("Could not save"));
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.phase() == RecordSession::Phase::stopped && r.recorder().hasTake());
    EXPECT (r.app->lastSaved().empty());
    EXPECT (r.engine.sample() == nullptr);

    // The folder is back: the same take is saved.
    fs::remove (r.folder.path, ec);
    fs::create_directories (r.folder.path, ec);
    r.saveAndLoad();
    EXPECT (r.folder.has ("rec-001.wav"));
    EXPECT (r.engine.sample() != nullptr);
}

KTEST (RecordScreen, the_file_browser_lists_a_take_that_was_saved_while_it_was_away)
{
    Rig r;

    auto listed = [&] (const std::string& name)
    {
        for (const auto& e : r.app->browser().entries())
            if (e.name == name)
                return true;

        return false;
    };

    EXPECT (! listed ("rec-001.wav"));
    r.enter();
    r.takeFor (0.5);
    r.tap (Btn::X);
    r.leave();
    EXPECT (r.app->screen() == Screen::browser);
    EXPECT (listed ("rec-001.wav"));
}

#if defined(_WIN32)
// Only Windows has a list of drives to be on.
KTEST (RecordScreen, a_take_saved_with_the_list_of_drives_on_show_leaves_the_list_as_it_is)
{
    Rig r;
    r.leave();                                      // to the library

    for (int i = 0; i < 64 && ! r.app->browser().atRootsList(); ++i)
        r.tap (Btn::B);                             // up and up, to the list of drives

    EXPECT (r.app->browser().atRootsList());
    r.enter();
    r.takeFor (0.5);
    r.tap (Btn::X);
    r.leave();
    EXPECT (r.app->screen() == Screen::browser);
    EXPECT (r.app->browser().atRootsList());
}
#endif

KTEST (RecordScreen, B_steps_back_one_level_at_a_time_and_a_discarded_take_is_not_saved)
{
    Rig r;
    r.enter();
    r.tap (Btn::A);
    r.frames (30);
    EXPECT (r.phase() == RecordSession::Phase::recording);

    r.tap (Btn::B);                                 // recording -> stopped (the take is kept)
    EXPECT (r.phase() == RecordSession::Phase::stopped);
    EXPECT (r.app->screen() == Screen::record);

    r.tap (Btn::Start);                             // Start does not leave with a take in hand
    EXPECT (r.phase() == RecordSession::Phase::stopped && r.app->screen() == Screen::record);

    r.tap (Btn::B);                                 // stopped -> idle (thrown away)
    EXPECT (r.phase() == RecordSession::Phase::idle);
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (! r.recorder().hasTake());

    r.tap (Btn::B);                                 // idle: nothing to go back to before there is a sample
    EXPECT (r.app->screen() == Screen::record);
    r.leave();
    EXPECT (r.app->screen() == Screen::browser);
    EXPECT (r.folder.countWavs() == 0);
}

KTEST (RecordScreen, a_take_that_is_too_short_is_thrown_away_with_a_word_about_it)
{
    Rig r;
    r.enter();
    r.tap (Btn::A, 0);
    r.frames (5);                                   // a twelfth of a second
    r.tap (Btn::A, 2);
    EXPECT (r.phase() == RecordSession::Phase::idle);
    EXPECT (r.toastHas ("Too short"));
}

KTEST (RecordScreen, the_take_stops_by_itself_when_it_is_as_long_as_the_limit_allows)
{
    Rig r;
    r.leave();
    r.cfg.recordLimit = 5;
    r.enter();                                      // the page reads the limit as it opens
    EXPECT (r.recorder().maxSeconds() == 5.0);

    r.tap (Btn::A);
    r.frames (60 * 5 + 20);
    EXPECT (r.phase() == RecordSession::Phase::stopped);
    EXPECT (r.recorder().full());
    EXPECT (std::fabs (r.recorder().takeSeconds() - 5.0) < 0.001);
    EXPECT (r.toastHas ("The take is full"));
}

KTEST (RecordScreen, an_input_that_is_unplugged_during_a_take_keeps_what_was_recorded)
{
    FakeDevice mic = microphone();
    mic.endsAfter = 1.0;                            // one second of sound, then it is "unplugged"
    Rig r ({ mic });
    r.enter();
    r.tap (Btn::A);
    r.frames (90);

    EXPECT (r.phase() == RecordSession::Phase::stopped);
    EXPECT (std::fabs (r.recorder().takeSeconds() - 1.0) < 0.1);
    EXPECT (r.toastHas ("kept"));

    r.saveAndLoad();                                // the take can still be saved
    EXPECT (r.folder.has ("rec-001.wav"));
    EXPECT (r.engine.sample() != nullptr);
}

KTEST (RecordScreen, a_microphone_plugged_in_while_the_screen_is_up_is_found_and_opened)
{
    Rig r (std::vector<FakeDevice> {});
    r.enter();
    EXPECT (r.app->recording().sources().empty());
    EXPECT (r.app->recording().statusText() == "No audio input found");
    EXPECT (! r.app->recording().ready());

    r.fake->add (microphone());
    r.frames (120);                                 // the list is looked at again every second and a half

    EXPECT (r.app->recording().sources().size() == 1);
    EXPECT (r.app->recording().sourceName() == "Pretend USB microphone");
    EXPECT (r.app->recording().ready());

    r.takeFor (0.5);
    EXPECT (r.phase() == RecordSession::Phase::stopped);
}

KTEST (RecordScreen, an_input_that_fails_says_why_and_Y_tries_again)
{
    FakeDevice broken = microphone();
    broken.failWith = "the device refused to start";
    Rig r ({ broken });
    r.enter();

    EXPECT (! r.app->recording().ready());
    EXPECT (r.app->recording().statusText() == "the device refused to start");

    r.tap (Btn::A);
    EXPECT (r.phase() == RecordSession::Phase::idle);
    EXPECT (r.toastHas ("No input is ready"));

    const int starts = r.fake->starts();
    r.fake->edit ("fake:mic", [] (FakeDevice& d) { d.failWith.clear(); });      // the fault is gone
    r.tap (Btn::Y);
    EXPECT (r.fake->starts() == starts + 1);
    EXPECT (r.app->recording().ready());
}

KTEST (RecordScreen, leaving_the_program_during_a_take_closes_the_input)
{
    Rig r;
    r.enter();
    r.tap (Btn::A);
    r.frames (10);
    EXPECT (r.phase() == RecordSession::Phase::recording && r.hub.isOpen());

    r.app.reset();                                  // the program ends
    EXPECT (! r.hub.isOpen());
}

KTEST (RecordScreen, the_help_key_does_not_leave_the_screen_and_a_loading_box_is_modal)
{
    Rig r;
    r.enter();

    Pad pad;
    pad.commands.push_back (KeyCommand::help);
    r.frames (1, pad);
    EXPECT (r.app->screen() == Screen::record);
}

KTEST (RecordScreen, a_file_dropped_on_the_window_leaves_a_take_that_is_in_hand_alone)
{
    Rig r;
    const fs::path wav = r.folder.path / "dropped.wav";
    writeTone (wav);
    r.frames (2);

    r.tap (Btn::A);
    r.frames (20);
    EXPECT (r.phase() == RecordSession::Phase::recording);

    Pad drop;
    drop.droppedFiles.push_back (pathToUtf8 (wav));

    r.frames (1, drop);                              // loading would leave the page, and the take with it
    EXPECT (! r.app->isLoading());
    EXPECT (r.app->screen() == Screen::record && r.phase() == RecordSession::Phase::recording);
    EXPECT (r.toastHas ("Stop the take first"));

    r.tap (Btn::A);
    EXPECT (r.phase() == RecordSession::Phase::stopped);
    r.frames (1, drop);
    EXPECT (! r.app->isLoading());
    EXPECT (r.app->screen() == Screen::record && r.recorder().hasTake());
    EXPECT (r.toastHas ("Save or discard the take first"));

    r.tap (Btn::B);                                  // thrown away: the file loads now, as it always did
    EXPECT (r.phase() == RecordSession::Phase::idle);
    EXPECT (! r.app->waveform().valid());
    r.frames (1, drop);
    r.app->waitForLoad();                            // (a file this small can be in before the frame is over: only the outcome is certain)
    EXPECT (r.app->waveform().valid());
    EXPECT (r.app->screen() == Screen::editor);
}

KTEST (RecordScreen, a_file_dropped_on_the_window_loads_from_any_page_without_a_take)
{
    Rig r;
    const fs::path wav = r.folder.path / "dropped.wav";
    writeTone (wav);
    r.frames (2);

    r.select();                                      // the library
    EXPECT (r.app->screen() == Screen::browser);

    Pad drop;
    drop.droppedFiles.push_back (pathToUtf8 (wav));
    EXPECT (! r.app->waveform().valid());
    r.frames (1, drop);
    r.app->waitForLoad();                            // (see above: the load may be over already)
    EXPECT (r.app->waveform().valid());
    EXPECT (r.app->screen() == Screen::editor);
}

// ------------------------------------------------------------------------------------------ drawing

KTEST (RecordScreen, the_meters_follow_the_sound_and_every_state_can_be_drawn)
{
    Rig r;
    r.enter();
    r.frames (30);

    // The pretend microphone sends 0.05 and 0.12: about -26 and -18 dB, so green and no red anywhere on the meters.
    const long green = countPixels (r.canvas, 8, 146, 624, 164, theme::ok);
    const long red = countPixels (r.canvas, 8, 146, 624, 164, theme::danger);
    EXPECT (green > 2000);
    EXPECT (red == 0);

    r.tap (Btn::A);
    r.frames (30);
    EXPECT (countPixels (r.canvas, 8, 316, 624, 114, theme::danger) > 100);     // the REC mark or its bar

    // Too loud: +30 dB on 0.12 is far beyond full scale.
    r.tap (Btn::A);
    r.tap (Btn::B);                                 // throw the take away
    r.tap (Btn::Down);
    r.tap (Btn::Down);

    for (int i = 0; i < 12; ++i)
        r.tap (Btn::Right, 0);

    r.frames (30);
    EXPECT (countPixels (r.canvas, 8, 146, 624, 164, theme::danger) > 200);     // red on the meters and the clip marks

    r.tap (Btn::A);
    r.frames (30);
    EXPECT (r.recorder().clipped());
    r.tap (Btn::A);
    r.frames (2);
    EXPECT (r.recorder().clipped());

    // Every kind of input can be looked at without trouble: a 12 channel mixer, a mono one, none at all.
    r.tap (Btn::B);                                 // the take that clipped is thrown away
    r.leave();
    r.enter();
    r.tap (Btn::R1);
    r.frames (10);
    EXPECT (r.recorder().channels() == 12);
    r.fake->remove ("fake:mixer");
    r.fake->remove ("fake:mic");
    r.frames (200);
    EXPECT (r.app->recording().sources().empty());
}

KTEST (RecordScreen, the_header_lights_the_page_that_is_up)
{
    Rig r;
    r.frames (3);

    // REC and LOAD are tabs one above the other (the same kind as SLICER and SEQ): the one that is up is
    // filled and has a bright edge, the other has a plain one.
    auto lit = [&] (int y) { return r.canvas.pixelAt (110, y) == theme::brandHi && r.canvas.pixelAt (134, y + 3) == theme::brand; };
    auto dark = [&] (int y) { return r.canvas.pixelAt (110, y) == theme::border && r.canvas.pixelAt (134, y + 3) == theme::bg; };
    const int recY = 6, loadY = 23;

    EXPECT (lit (recY) && dark (loadY));            // REC is up: on the Record page ...
    r.select();
    r.frames (3);
    EXPECT (lit (loadY) && dark (recY));            // ... and LOAD is up in the library
    r.select();
    r.frames (3);
    EXPECT (lit (recY) && dark (loadY));

    // The slicer and the sequencer have their own pair: SLICER above, SEQ below, as before.
    const auto wav = r.folder.path / "tone.wav";
    writeTone (wav);
    r.app->loadFile (wav);
    r.app->waitForLoad();
    r.frames (3);
    EXPECT (r.app->screen() == Screen::editor);
    EXPECT (lit (2) && dark (14));                  // SLICER
    r.select();                                     // a page on: the sequencer
    r.frames (3);
    EXPECT (r.app->screen() == Screen::sequencer);
    EXPECT (dark (2) && lit (14));                  // SEQ
}

KTEST (RecordScreen, the_header_names_the_page_that_is_up)
{
    Rig r;
    r.frames (3);

    // "RECORD SAMPLE" is longer than "LOAD SAMPLE": the part that sticks out is only there on the Record page.
    const int shortW = Canvas::textWidth ("LOAD SAMPLE", 2), longW = Canvas::textWidth ("RECORD SAMPLE", 2);
    const int x0 = 144 + shortW + 2, w = longW - shortW - 2;
    EXPECT (w > 4);
    EXPECT (countPixels (r.canvas, x0, 6, w, 16, theme::brandHi) > 0);
    r.select();
    r.frames (3);
    EXPECT (countPixels (r.canvas, x0, 6, w, 16, theme::brandHi) == 0);
    EXPECT (countPixels (r.canvas, 144, 6, shortW, 16, theme::brandHi) > 0);    // and LOAD SAMPLE is there
}

KTEST (RecordScreen, a_page_under_a_menu_keeps_its_own_header)
{
    Rig r;
    r.frames (3);

    // The menu dims what is behind it, so the tabs are told apart by brightness: the lit one is the
    // brightest, and it sits where the tab of that page sits (REC at the top, LOAD below it; the
    // slicer's tabs are higher up and would light the pixel above REC instead).
    auto glow = [&] (int y)
    {
        const uint32_t p = r.canvas.pixelAt (134, y);
        return (int) ((p >> 16) & 255) + (int) ((p >> 8) & 255) + (int) (p & 255);
    };
    const int above = 3, recY = 9, loadY = 26;

    r.tap (Btn::Start);                             // no sample yet: the menu of the two pages that get one
    r.frames (3);
    EXPECT (r.app->screen() == Screen::menu);
    EXPECT (glow (recY) > glow (loadY) + 60 && glow (recY) > glow (above) + 60);    // REC is still the page behind it

    r.tap (Btn::B);
    r.select();
    r.tap (Btn::Start);
    r.frames (3);
    EXPECT (r.app->screen() == Screen::menu);
    EXPECT (glow (loadY) > glow (recY) + 60 && glow (loadY) > glow (above) + 60);   // and LOAD, when the menu came from the library
}

KTEST (RecordScreen, the_demo_inputs_of_the_headless_runner_are_these_four)
{
    Rig r (demoInputs());
    r.enter();
    const auto& sources = r.app->recording().sources();
    EXPECT (sources.size() == 4);

    if (sources.size() != 4)
        return;

    EXPECT (sources[0].name == "Pretend USB microphone" && sources[0].direct);
    EXPECT (sources[1].name == "Pretend silent receiver" && sources[1].direct);
    EXPECT (sources[2].name == "Pretend 12-channel mixer" && ! sources[2].direct);
    EXPECT (sources[3].name == "Pretend broken input");

    r.tap (Btn::R1);                                // the receiver that sends nothing
    r.frames (120);
    EXPECT (r.app->recording().quiet());
    EXPECT (r.app->recording().statusText() == "Nothing is coming from the input yet");
}

// ----------------------------------------------------------------------------------- the menus

KTEST (RecordScreen, the_settings_menu_changes_the_limit_of_a_take)
{
    Rig r;
    EXPECT (r.cfg.recordLimit == 180);

    // Before there is a sample the menu of the Record page has the settings: "Record limit" is the
    // eighth line. The limit goes through 30 s, 1, 2, 3, 5 minutes.
    r.frames (2);
    r.tap (Btn::Start);
    EXPECT (r.app->screen() == Screen::menu);
    r.tap (Btn::A);
    EXPECT (r.app->screen() == Screen::settings);

    for (int i = 0; i < 7; ++i)                     // to "Record limit"
        r.tap (Btn::Down, 0);

    r.tap (Btn::Right);
    EXPECT (r.cfg.recordLimit == 300);
    r.tap (Btn::Right);
    EXPECT (r.cfg.recordLimit == 300);              // that is the longest
    r.tap (Btn::Left);
    r.tap (Btn::Left);
    EXPECT (r.cfg.recordLimit == 120);
    r.tap (Btn::Left);
    r.tap (Btn::Left);
    r.tap (Btn::Left);
    EXPECT (r.cfg.recordLimit == 30);

    r.tap (Btn::B);                                 // back to the menu
    r.tap (Btn::B);                                 // and to the Record page, which uses it
    EXPECT (r.app->screen() == Screen::record);
    r.frames (2);
    EXPECT (r.recorder().maxSeconds() == 30.0);

    // With a sample loaded, the same settings are in the slicer menu, and so is "Record sample...".
    const auto wav = r.folder.path / "tone.wav";
    writeTone (wav);
    r.app->loadFile (wav);
    r.app->waitForLoad();
    EXPECT (r.app->screen() == Screen::editor);

    r.tap (Btn::Start);
    EXPECT (r.app->screen() == Screen::menu);

    for (int i = 0; i < 9; ++i)
        r.tap (Btn::Down, 0);

    r.tap (Btn::A);
    EXPECT (r.app->screen() == Screen::settings);

    for (int i = 0; i < 7; ++i)
        r.tap (Btn::Down, 0);

    r.tap (Btn::Right);
    r.tap (Btn::Right);
    EXPECT (r.cfg.recordLimit == 120);

    r.tap (Btn::B);                                 // back to the menu
    r.tap (Btn::B);                                 // and to the slicer
    EXPECT (r.app->screen() == Screen::editor);

    // "Record sample..." is the second entry: it opens the page, which uses the new limit, and B returns to the slicer.
    r.tap (Btn::Start);
    r.tap (Btn::Down);
    r.tap (Btn::A);
    r.frames (2);
    EXPECT (r.app->screen() == Screen::record);
    EXPECT (r.recorder().maxSeconds() == 120.0);
    r.tap (Btn::B);
    EXPECT (r.app->screen() == Screen::editor);
}
