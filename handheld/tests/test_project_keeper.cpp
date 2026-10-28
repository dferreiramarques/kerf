// Tests for the ProjectKeeper: when a project is brought back, when the work is saved (a moment
// after it stops changing, never in the middle of a stick movement, never for nothing), what is
// done with a project that cannot be used, and what happens when a save fails.
//
// The keeper is given the time, so these tests run on a clock of their own: steps of 1/64 s, which
// add up exactly, so "1.5 seconds" is exactly 96 steps.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "engine.h"
#include "minitest.h"
#include "path_util.h"
#include "project.h"
#include "project_keeper.h"
#include "project_store.h"
#include "sample.h"
#include "temp_folder.h"

using namespace kerf;
using namespace kerf::test;
using kerf::ui::ProjectKeeper;
namespace fs = std::filesystem;
namespace store = kerf::ui::ProjectStore;

namespace
{
    constexpr double kStep = 1.0 / 64.0;

    SamplePtr makeSample (int frames, double rate, const std::string& name = "loop.wav")
    {
        auto s = std::make_shared<Sample>();
        s->data.resize (2, frames);
        s->sampleRate = rate;
        s->name = name;
        return s;
    }

    Slice slice (float start, float end)
    {
        Slice s;
        s.start = start;
        s.end = end;
        return s;
    }

    bool contains (const std::string& text, const std::string& part)
    {
        return text.find (part) != std::string::npos;
    }

    // A keeper with an engine that has a sample of four seconds, in a folder of its own, and a clock.
    struct Rig
    {
        TempFolder dir;
        Engine engine;
        ProjectKeeper keeper { engine };
        std::vector<std::string> log;
        double now = 0.0;
        fs::path audio;

        explicit Rig (int frames = 4 * 44100, double rate = 44100.0, bool open = true, bool enabled = true)
        {
            engine.prepare (rate);
            audio = dir / "rec.wav";
            keeper.setEnabled (enabled);
            keeper.setLogger ([this] (const std::string& line) { log.push_back (line); });

            if (open)
                load (frames, rate);
        }

        // A sample comes into the engine and the keeper is told, as the application does it.
        ProjectKeeper::Opened load (int frames = 4 * 44100, double rate = 44100.0, const std::string& name = "rec.wav")
        {
            audio = dir / name;
            engine.loadSample (makeSample (frames, rate, name));
            return keeper.open (audio);
        }

        // Time passes with a frame every 1/64 s.
        void run (double seconds, bool mayWrite = true)
        {
            const int steps = (int) (seconds / kStep + 0.5);

            for (int i = 0; i < steps; ++i)
            {
                now += kStep;
                keeper.update (now, mayWrite);
            }
        }

        // One frame, at a given time (for the edges of the rules).
        void at (double t, bool mayWrite = true)
        {
            now = t;
            keeper.update (now, mayWrite);
        }

        std::string file() const { return dir.read ("rec.wav.kerf"); }

        int logged (const std::string& part) const
        {
            int n = 0;

            for (const auto& l : log)
                if (contains (l, part))
                    ++n;

            return n;
        }

        // The project on disk as a project.
        store::Read onDisk() const { return store::read (keeper.file()); }

        // Some work: a slice, and a step on it.
        void addSlice (float start = 0.5f, float end = 1.0f)
        {
            engine.slices().add (slice (start, end));
        }
    };

    // The text of a project made for a sample of `frames` frames at `rate`.
    std::string projectText (std::int64_t frames, std::int64_t rate, int sliceCount = 2, float tempo = 100.0f)
    {
        Project p;
        p.audio = "rec.wav";
        p.frames = frames;
        p.rate = rate;
        p.tempo = tempo;

        for (int i = 0; i < sliceCount; ++i)
            p.slices.push_back (slice (0.25f * (float) i, 0.25f * (float) i + 0.2f));

        if (sliceCount > 0)
        {
            p.grid.bits[0][0] = 0x0011;
            p.grid.bits[2][sliceCount - 1] = 0x8000;
        }

        std::string text;
        EXPECT (serializeProject (p, text));
        return text;
    }
}

// ----------------------------------------------------------------------------------- opening

KTEST (ProjectKeeper, looking_at_a_sample_with_no_project_writes_nothing_ever)
{
    Rig r (4 * 44100, 44100.0, false);

    const auto opened = r.load();
    EXPECT (opened.kind == ProjectKeeper::Opened::Kind::fresh);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
    EXPECT_TEXT (kerf::pathToUtf8 (r.keeper.file().filename()), std::string ("rec.wav.kerf"));

    r.run (60.0);                                       // a minute of looking
    r.keeper.flush();

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
    EXPECT (r.dir.names().empty());
    EXPECT (r.log.empty());
    EXPECT_TEXT (r.keeper.takeNotice(), std::string());
}

KTEST (ProjectKeeper, nothing_happens_before_a_sample_is_opened)
{
    Rig r (4 * 44100, 44100.0, false);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
    EXPECT (r.keeper.file().empty());

    r.run (5.0);
    r.keeper.flush();
    EXPECT (r.dir.names().empty());

    // An engine with no sample: opening finds nothing to keep a project for.
    const auto opened = r.keeper.open (r.dir / "none.wav");
    EXPECT (opened.kind == ProjectKeeper::Opened::Kind::fresh);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);

    r.addSlice();
    r.run (20.0);
    r.keeper.flush();
    EXPECT (r.dir.names().empty());
}

KTEST (ProjectKeeper, opening_with_no_sample_in_the_engine_drops_what_was_open)
{
    Rig r;
    r.addSlice();
    r.at (1.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
    EXPECT_TEXT (kerf::pathToUtf8 (r.keeper.file().filename()), std::string ("rec.wav.kerf"));

    // The application never does this, but the engine allows it: the sample goes and none comes in.
    r.engine.loadSample (nullptr);
    const auto opened = r.keeper.open (r.dir / "gone.wav");

    EXPECT (opened.kind == ProjectKeeper::Opened::Kind::fresh);
    EXPECT_EQ (opened.slices, 0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
    EXPECT (r.keeper.file().empty());

    r.run (30.0);
    r.keeper.flush();
    EXPECT (r.dir.names().empty());
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);        // nothing is being kept, and nothing is wrong either
    EXPECT_EQ (r.logged ("saving stopped"), 0);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string());
    EXPECT (! r.keeper.takeCreated());
}

KTEST (ProjectKeeper, what_was_wrong_with_one_samples_project_does_not_follow_to_the_next_sample)
{
    Rig r (4 * 44100, 44100.0, false);

    // A sample whose project is damaged is looked at (and left as it is) ...
    r.dir.write ("bad.wav.kerf", "this is not a project\n");
    EXPECT (r.load (4 * 44100, 44100.0, "bad.wav").kind == ProjectKeeper::Opened::Kind::damaged);

    // ... and the next one, with no project at all, is saved like any other: nothing is set aside, nothing is lost.
    EXPECT (r.load (4 * 44100, 44100.0, "fine.wav").kind == ProjectKeeper::Opened::Kind::fresh);
    r.addSlice();
    r.run (3.0);

    EXPECT (r.dir.has ("fine.wav.kerf"));
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT_EQ (r.logged ("cannot save"), 0);
    EXPECT_EQ (r.logged ("set aside as"), 0);
    EXPECT_TEXT (r.dir.read ("bad.wav.kerf"), std::string ("this is not a project\n"));
    EXPECT (! r.dir.has ("bad.wav.kerf.old"));

    // The same after a project that was brought back: the next sample's first project has no backup to make.
    r.dir.write ("good.wav.kerf", projectText (4 * 44100, 44100, 2));
    EXPECT (r.load (4 * 44100, 44100.0, "good.wav").kind == ProjectKeeper::Opened::Kind::restored);
    EXPECT (r.load (4 * 44100, 44100.0, "new.wav").kind == ProjectKeeper::Opened::Kind::fresh);
    r.addSlice();
    r.run (3.0);

    EXPECT (r.dir.has ("new.wav.kerf"));
    EXPECT (! r.dir.has ("new.wav.kerf.bak"));
    EXPECT (! r.dir.has ("good.wav.kerf.bak"));
    EXPECT_EQ (r.logged ("no backup"), 0);
    EXPECT_EQ (r.logged ("cannot save"), 0);
}

KTEST (ProjectKeeper, a_keeper_that_nobody_switched_is_on)
{
    TempFolder dir;
    Engine engine;
    engine.prepare (44100.0);
    ProjectKeeper keeper (engine);                       // setEnabled() is never called

    dir.write ("rec.wav.kerf", projectText (4 * 44100, 44100, 2));
    engine.loadSample (makeSample (4 * 44100, 44100.0));
    EXPECT (keeper.open (dir / "rec.wav").kind == ProjectKeeper::Opened::Kind::restored);
    EXPECT (keeper.tag() == ProjectKeeper::Tag::saved);

    engine.slices().add (slice (0.5f, 1.0f));
    keeper.update (1.0, true);
    keeper.update (2.5, true);
    EXPECT (keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT_EQ (store::read (dir / "rec.wav.kerf").project.slices.size(), (std::size_t) 3);
}

KTEST (ProjectKeeper, trouble_with_saving_one_sample_does_not_follow_to_the_next)
{
    Rig r;
    r.dir.makeFolder ("rec.wav.kerf.tmp");              // the part file cannot be made
    r.addSlice();
    r.run (3.0);                                        // the save fails
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string ("Cannot save the project"));

    // The next sample starts clean: no trouble on its header, and its first save is not held back.
    r.load (4 * 44100, 44100.0, "next.wav");
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
    r.addSlice();
    r.run (1.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
    r.run (1.0);                                        // 1.5 seconds have passed since the change
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (r.dir.has ("next.wav.kerf"));
    EXPECT_EQ (r.logged ("saving works again"), 0);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string());
}

KTEST (ProjectKeeper, what_the_flush_before_opening_has_to_tell_is_still_there_afterwards)
{
    // A project that the flush made, just before the next sample came in: the list of samples still needs its mark.
    {
        Rig r;
        r.addSlice();
        r.at (1.0);
        r.keeper.flush();
        r.load (4 * 44100, 44100.0, "next.wav");

        EXPECT (r.dir.has ("rec.wav.kerf"));
        EXPECT (r.keeper.takeCreated());
        EXPECT (! r.keeper.takeCreated());
    }

    // A flush that fails: the person is still told, and the next sample is clean.
    {
        Rig r;
        r.dir.makeFolder ("rec.wav.kerf.tmp");
        r.addSlice();
        r.at (1.0);
        r.keeper.flush();
        EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);

        r.load (4 * 44100, 44100.0, "next.wav");
        EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
        EXPECT_TEXT (r.keeper.takeNotice(), std::string ("Cannot save the project"));
        EXPECT_TEXT (r.keeper.takeNotice(), std::string());
        EXPECT (! r.keeper.takeCreated());
    }
}

KTEST (ProjectKeeper, a_project_that_is_there_comes_back_and_that_is_not_a_change)
{
    Rig r (4 * 44100, 44100.0, false);
    const std::string text = projectText (4 * 44100, 44100, 3, 133.0f);
    r.dir.write ("rec.wav.kerf", text);

    const auto opened = r.load();
    EXPECT (opened.kind == ProjectKeeper::Opened::Kind::restored);
    EXPECT_EQ (opened.slices, 3);
    EXPECT_EQ (opened.patterns, 2);
    EXPECT_EQ (r.engine.slices().size(), 3);
    EXPECT_NEAR (r.engine.sequencer().tempo(), 133.0, 0.001);
    EXPECT (r.engine.sequencer().cell (0, 0, 0));
    EXPECT (r.engine.sequencer().cell (0, 0, 4));
    EXPECT (r.engine.sequencer().cell (2, 2, 15));
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);

    // Having it back writes nothing: not at once, not later, and no backup.
    r.run (30.0);
    r.keeper.flush();
    EXPECT_TEXT (r.file(), text);
    EXPECT (r.dir.count ("rec.wav.kerf.") == 0);
    EXPECT_EQ (r.logged ("saved"), 0);
    EXPECT_EQ (r.logged ("restored"), 1);
}

KTEST (ProjectKeeper, a_project_with_numbers_outside_the_controls_is_brought_in_range_and_not_rewritten)
{
    Rig r (4 * 44100, 44100.0, false);

    // Made by hand, with a slice that runs past the end of the sample.
    std::string text = projectText (4 * 44100, 44100, 1);
    const auto at = text.find ("end=");
    EXPECT (at != std::string::npos);
    text.replace (at, text.find (' ', at) - at, "end=99");
    r.dir.write ("rec.wav.kerf", text);

    const auto opened = r.load();
    EXPECT (opened.kind == ProjectKeeper::Opened::Kind::restored);
    const auto slices = *r.engine.slices().getCurrent();
    EXPECT_EQ (slices.size(), (std::size_t) 1);
    EXPECT (slices[0].end <= 4.0f);                     // inside the sample

    r.run (30.0);
    EXPECT_TEXT (r.file(), text);                       // the file said 99, and still does: it was not "changed"
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
}

KTEST (ProjectKeeper, a_project_made_for_other_audio_is_not_used)
{
    for (int variant = 0; variant < 3; ++variant)
    {
        Rig r (4 * 44100, 44100.0, false);
        const std::string text = variant == 0 ? projectText (4 * 44100 + 1, 44100, 2)      // one frame longer
                               : variant == 1 ? projectText (4 * 44100, 48000, 2)          // another rate
                                              : projectText (1000, 8000, 2);
        r.dir.write ("rec.wav.kerf", text);

        const auto opened = r.load();
        EXPECT (opened.kind == ProjectKeeper::Opened::Kind::mismatch);
        EXPECT_EQ (r.engine.slices().size(), 0);
        EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);

        r.run (30.0);
        r.keeper.flush();
        EXPECT_TEXT (r.file(), text);                   // looking at it does not touch it
        EXPECT (r.dir.names().size() == 1);
        EXPECT_EQ (r.logged ("another length or rate"), 1);
    }
}

KTEST (ProjectKeeper, a_damaged_project_is_not_used_and_not_touched_while_only_looking)
{
    Rig r (4 * 44100, 44100.0, false);
    r.dir.write ("rec.wav.kerf", "kerf_project = 1\nthis is cut sh");

    const auto opened = r.load();
    EXPECT (opened.kind == ProjectKeeper::Opened::Kind::damaged);
    EXPECT_EQ (r.engine.slices().size(), 0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);

    r.run (30.0);
    r.keeper.flush();
    EXPECT_TEXT (r.file(), std::string ("kerf_project = 1\nthis is cut sh"));
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);
}

KTEST (ProjectKeeper, a_project_of_a_newer_kerf_is_left_alone_for_good)
{
    Rig r (4 * 44100, 44100.0, false);
    const std::string text = "kerf_project = 2\nfuture = things\n";
    r.dir.write ("rec.wav.kerf", text);

    const auto opened = r.load();
    EXPECT (opened.kind == ProjectKeeper::Opened::Kind::newer);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);

    // The person works on regardless; none of it is written, however long it goes on.
    r.addSlice();
    r.engine.sequencer().setTempo (150.0f);
    r.run (60.0);
    r.keeper.flush();

    EXPECT_TEXT (r.file(), text);
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_EQ (r.logged ("newer Kerf"), 1);
}

KTEST (ProjectKeeper, with_projects_off_nothing_is_read_and_nothing_is_written)
{
    Rig r (4 * 44100, 44100.0, false, false);
    const std::string text = projectText (4 * 44100, 44100, 3);
    r.dir.write ("rec.wav.kerf", text);

    const auto opened = r.load();
    EXPECT (opened.kind == ProjectKeeper::Opened::Kind::off);
    EXPECT_EQ (r.engine.slices().size(), 0);             // the project was not even looked at
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::off);

    r.addSlice();
    r.run (60.0);
    r.keeper.flush();

    EXPECT_TEXT (r.file(), text);
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::off);
    EXPECT (r.log.empty());

    // Turning it on does not reach back into a sample that was opened while it was off.
    r.keeper.setEnabled (true);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::off);
    r.run (60.0);
    r.keeper.flush();
    EXPECT_TEXT (r.file(), text);

    // The next sample opened takes it up.
    const auto next = r.load (4 * 44100, 44100.0, "other.wav");
    EXPECT (next.kind == ProjectKeeper::Opened::Kind::fresh);
    r.addSlice();
    r.run (3.0);
    EXPECT (r.dir.has ("other.wav.kerf"));
}

// ----------------------------------------------------------------------------------- saving

KTEST (ProjectKeeper, the_work_is_saved_a_moment_after_it_stops_changing)
{
    Rig r;

    r.addSlice();
    r.at (1.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);

    r.at (2.4375);                                       // 1.4375 s after the change: not yet
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
    EXPECT (! r.dir.has ("rec.wav.kerf"));

    r.at (2.5);                                          // exactly 1.5 s: now
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (r.dir.has ("rec.wav.kerf"));

    const auto saved = r.onDisk();
    EXPECT (saved.kind == store::Read::Kind::ok);
    EXPECT_EQ (saved.project.slices.size(), (std::size_t) 1);
    EXPECT (sameWork (saved.project, captureProject (r.engine)));
    EXPECT_EQ (r.logged ("saved rec.wav.kerf"), 1);
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);   // no backup (there was no earlier project), no part file
}

KTEST (ProjectKeeper, every_kind_of_change_is_saved)
{
    struct Change
    {
        const char* what;
        std::function<void (Engine&)> make;
    };

    const std::vector<Change> changes = {
        { "add a slice",        [] (Engine& e) { e.slices().add (slice (1.0f, 2.0f)); } },
        { "move a handle",      [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.end = 1.5f; }); } },
        { "start",              [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.start = 0.25f; }); } },
        { "pitch",              [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.pitch = 1.5f; }); } },
        { "volume",             [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.volume = 0.5f; }); } },
        { "pan",                [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.pan = -0.5f; }); } },
        { "mode",               [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.mode = Slice::Mode::loop; }); } },
        { "fade in",            [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.fadeInMs = 20.0f; }); } },
        { "fade out",           [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.fadeOutMs = 100.0f; }); } },
        { "switch off",         [] (Engine& e) { e.slices().updateAt (0, [] (Slice& s) { s.enabled = false; }); } },
        { "delete a slice",     [] (Engine& e) { e.slices().removeAt (0); } },
        { "clear the slices",   [] (Engine& e) { e.slices().clear(); } },
        { "a step on",          [] (Engine& e) { e.sequencer().setCell (0, 0, 3, true); } },
        { "a step in a later scene", [] (Engine& e) { e.sequencer().setCell (5, 0, 9, true); } },
        { "tempo",              [] (Engine& e) { e.sequencer().setTempo (141.0f); } },
        { "swing",              [] (Engine& e) { e.sequencer().setSwing (0.62f); } },
        { "length",             [] (Engine& e) { e.sequencer().setLength (9); } },
    };

    for (const auto& change : changes)
    {
        Rig r;
        r.engine.slices().replaceAll ({ slice (0.5f, 1.0f), slice (2.0f, 3.0f) });
        r.engine.sequencer().setCell (0, 0, 0, true);
        r.run (1.0);
        r.keeper.flush();                                 // the work as a starting point, on disk
        const std::string before = r.file();
        EXPECT (! before.empty());

        change.make (r.engine);
        r.run (3.0);

        const bool differs = before != r.file();
        if (! differs)
            std::printf ("    the change \"%s\" was not saved\n", change.what);

        EXPECT (differs);
        EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
        EXPECT (sameWork (r.onDisk().project, captureProject (r.engine)));
    }
}

KTEST (ProjectKeeper, moving_about_is_not_editing)
{
    Rig r;
    r.engine.slices().replaceAll ({ slice (0.5f, 1.0f), slice (2.0f, 3.0f) });
    r.run (3.0);
    r.keeper.flush();
    const std::string before = r.file();
    const auto logged = r.log.size();

    r.engine.setSelectedSlice (1);                      // the cursor on the slices
    r.engine.sequencer().setScene (3);                  // the scene being looked at
    r.run (30.0);
    r.keeper.flush();

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT_TEXT (r.file(), before);
    EXPECT_EQ (r.log.size(), logged);
}

KTEST (ProjectKeeper, the_cursor_and_the_scene_are_kept_with_the_next_save_and_come_back)
{
    Rig r;
    r.engine.slices().replaceAll ({ slice (0.5f, 1.0f), slice (2.0f, 3.0f), slice (3.0f, 3.5f) });
    r.engine.setSelectedSlice (2);
    r.engine.sequencer().setScene (4);
    r.run (3.0);                                         // the changes are saved with the cursor and scene as they are

    Rig again (4 * 44100, 44100.0, false);
    again.dir.write ("rec.wav.kerf", r.file());
    EXPECT (again.load().kind == ProjectKeeper::Opened::Kind::restored);
    EXPECT_EQ (again.engine.selectedSlice(), 2);
    EXPECT_EQ (again.engine.sequencer().scene(), 4);
}

KTEST (ProjectKeeper, a_change_that_is_undone_before_the_moment_comes_writes_nothing)
{
    Rig r;

    r.engine.sequencer().setTempo (140.0f);
    r.at (1.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);

    r.engine.sequencer().setTempo (120.0f);              // back to what it was when the sample came in
    r.run (30.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
    EXPECT (r.dir.names().empty());
}

KTEST (ProjectKeeper, undoing_to_what_is_on_disk_is_not_saved_again)
{
    Rig r;
    r.addSlice();
    r.run (3.0);                                         // saved with one slice
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    const std::string saved = r.file();
    EXPECT_EQ (r.logged ("saved rec.wav.kerf"), 1);

    r.addSlice (2.0f, 2.5f);
    r.at (r.now + 0.5);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
    r.engine.slices().removeAt (1);                      // back to the one slice
    r.run (30.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT_TEXT (r.file(), saved);
    EXPECT_EQ (r.logged ("saved rec.wav.kerf"), 1);
}

KTEST (ProjectKeeper, work_that_never_stops_is_saved_every_ten_seconds)
{
    Rig r;

    // A stick that is never let go: something changes every half second.
    int saves = 0;
    double firstSave = -1.0, secondSave = -1.0;
    float tempo = 120.0f;

    for (int i = 1; i <= 50; ++i)                        // 25 seconds
    {
        tempo += 1.0f;
        r.engine.sequencer().setTempo (tempo);
        const int before = r.logged ("saved rec.wav.kerf");
        r.at (0.5 * (double) i);

        if (r.logged ("saved rec.wav.kerf") > before)
        {
            ++saves;
            (saves == 1 ? firstSave : secondSave) = r.now;
        }
    }

    EXPECT_EQ (saves, 2);
    EXPECT_NEAR (firstSave, 10.5, 1e-9);                 // the first change was seen at 0.5 s; ten seconds later
    EXPECT_NEAR (secondSave, 21.0, 1e-9);                // the next change (at 11 s) started the next ten
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
}

KTEST (ProjectKeeper, a_save_waits_while_a_take_is_in_hand_and_comes_when_it_is_over)
{
    Rig r;
    r.addSlice();
    r.run (30.0, false);                                 // mayWrite = false the whole time

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
    EXPECT (r.dir.names().empty());

    r.run (kStep, true);                                 // the first frame after: it has waited long enough
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (r.dir.has ("rec.wav.kerf"));
}

KTEST (ProjectKeeper, flush_saves_what_is_waiting_and_only_that)
{
    Rig r;

    r.keeper.flush();                                    // nothing changed: nothing written
    EXPECT (r.dir.names().empty());

    r.addSlice();
    r.at (1.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
    r.keeper.flush();                                    // does not wait for the moment
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (r.dir.has ("rec.wav.kerf"));
    EXPECT_EQ (r.logged ("saved rec.wav.kerf"), 1);

    r.keeper.flush();                                    // nothing more to save
    r.keeper.flush();
    EXPECT_EQ (r.logged ("saved rec.wav.kerf"), 1);

    // A change that the keeper has not even seen yet (no frame has gone by) is saved too.
    r.engine.sequencer().setTempo (150.0f);
    r.keeper.flush();
    EXPECT_NEAR (r.onDisk().project.tempo, 150.0, 0.001);
}

KTEST (ProjectKeeper, flush_does_nothing_when_saving_is_off_or_blocked)
{
    Rig off (4 * 44100, 44100.0, true, false);
    off.addSlice();
    off.keeper.flush();
    EXPECT (off.dir.names().empty());

    Rig newer (4 * 44100, 44100.0, false);
    newer.dir.write ("rec.wav.kerf", "kerf_project = 9\n");
    newer.load();
    newer.addSlice();
    newer.keeper.flush();
    EXPECT_TEXT (newer.file(), std::string ("kerf_project = 9\n"));
    EXPECT_EQ (newer.dir.names().size(), (std::size_t) 1);

    Rig closed (4 * 44100, 44100.0, false);
    closed.engine.loadSample (makeSample (1000, 44100.0));
    closed.keeper.flush();                               // nothing was opened
    EXPECT (closed.dir.names().empty());
}

KTEST (ProjectKeeper, turning_projects_off_stops_the_saving_at_once)
{
    Rig r;
    r.addSlice();
    r.at (1.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);

    r.keeper.setEnabled (false);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::off);
    r.run (60.0);
    r.keeper.flush();
    EXPECT (r.dir.names().empty());

    // And on again, while the same sample is open: the work that waited is saved.
    r.keeper.setEnabled (true);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
    r.run (2.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (r.dir.has ("rec.wav.kerf"));
}

// ------------------------------------------------------------------------------- the earlier project

KTEST (ProjectKeeper, the_first_save_of_a_session_keeps_the_earlier_project_as_a_backup)
{
    Rig r (4 * 44100, 44100.0, false);
    const std::string earlier = projectText (4 * 44100, 44100, 2, 90.0f);
    r.dir.write ("rec.wav.kerf", earlier);
    EXPECT (r.load().kind == ProjectKeeper::Opened::Kind::restored);

    r.engine.sequencer().setTempo (111.0f);
    r.run (3.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT_TEXT (r.dir.read ("rec.wav.kerf.bak"), earlier);
    EXPECT (r.file() != earlier);
    EXPECT_NEAR (r.onDisk().project.tempo, 111.0, 0.001);
    EXPECT_EQ (r.logged ("kept the earlier"), 1);

    // The later saves of the session do not replace the backup: it is how the project was when the session began.
    r.engine.sequencer().setTempo (122.0f);
    r.run (3.0);
    r.engine.sequencer().setTempo (133.0f);
    r.run (3.0);
    EXPECT_NEAR (r.onDisk().project.tempo, 133.0, 0.001);
    EXPECT_TEXT (r.dir.read ("rec.wav.kerf.bak"), earlier);
    EXPECT_EQ (r.logged ("kept the earlier"), 1);
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 2);
}

KTEST (ProjectKeeper, each_session_has_its_own_backup)
{
    Rig r (4 * 44100, 44100.0, false);
    r.dir.write ("rec.wav.kerf", projectText (4 * 44100, 44100, 2, 90.0f));
    r.load();
    r.engine.sequencer().setTempo (111.0f);
    r.run (3.0);
    const std::string secondSession = r.file();

    // The sample is opened again: the project as saved is what the backup of this session will hold.
    EXPECT (r.load().kind == ProjectKeeper::Opened::Kind::restored);
    r.engine.sequencer().setTempo (150.0f);
    r.run (3.0);

    EXPECT_TEXT (r.dir.read ("rec.wav.kerf.bak"), secondSession);
    EXPECT_NEAR (r.onDisk().project.tempo, 150.0, 0.001);
}

KTEST (ProjectKeeper, a_first_project_has_no_backup)
{
    Rig r;
    r.addSlice();
    r.run (3.0);
    r.addSlice (2.0f, 2.5f);
    r.run (3.0);

    EXPECT (r.dir.has ("rec.wav.kerf"));
    EXPECT (! r.dir.has ("rec.wav.kerf.bak"));
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);
    EXPECT_EQ (r.logged ("kept the earlier"), 0);
}

KTEST (ProjectKeeper, a_project_for_other_audio_is_set_aside_when_the_work_is_first_saved)
{
    Rig r (4 * 44100, 44100.0, false);
    const std::string foreign = projectText (5 * 44100, 44100, 4);
    r.dir.write ("rec.wav.kerf", foreign);
    EXPECT (r.load().kind == ProjectKeeper::Opened::Kind::mismatch);

    r.run (20.0);
    EXPECT_TEXT (r.file(), foreign);                     // untouched while there is nothing to save

    r.addSlice();
    r.run (3.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT_TEXT (r.dir.read ("rec.wav.kerf.old"), foreign);
    EXPECT (! r.dir.has ("rec.wav.kerf.bak"));
    const auto now = r.onDisk();
    EXPECT (now.kind == store::Read::Kind::ok);
    EXPECT_EQ (now.project.slices.size(), (std::size_t) 1);
    EXPECT_EQ (now.project.frames, (std::int64_t) (4 * 44100));
    EXPECT_EQ (r.logged ("set aside as rec.wav.kerf.old"), 1);
}

KTEST (ProjectKeeper, a_damaged_project_is_set_aside_when_the_work_is_first_saved)
{
    Rig r (4 * 44100, 44100.0, false);
    r.dir.write ("rec.wav.kerf", "garbage");
    EXPECT (r.load().kind == ProjectKeeper::Opened::Kind::damaged);

    r.run (10.0);
    EXPECT_TEXT (r.file(), std::string ("garbage"));

    r.addSlice();
    r.run (3.0);

    EXPECT_TEXT (r.dir.read ("rec.wav.kerf.old"), std::string ("garbage"));
    EXPECT (r.onDisk().kind == store::Read::Kind::ok);

    // The next one that cannot be used does not overwrite the first one set aside.
    r.dir.write ("rec.wav.kerf", "more garbage");
    EXPECT (r.load().kind == ProjectKeeper::Opened::Kind::damaged);
    r.addSlice();
    r.run (3.0);

    EXPECT_TEXT (r.dir.read ("rec.wav.kerf.old"), std::string ("garbage"));
    EXPECT_TEXT (r.dir.read ("rec.wav.kerf.old2"), std::string ("more garbage"));
    EXPECT (r.onDisk().kind == store::Read::Kind::ok);
}

KTEST (ProjectKeeper, a_save_that_fails_after_the_old_project_was_set_aside_does_not_set_it_aside_again)
{
    Rig r (4 * 44100, 44100.0, false);
    r.dir.write ("rec.wav.kerf", "this is not a project\n");
    EXPECT (r.load().kind == ProjectKeeper::Opened::Kind::damaged);

    r.dir.makeFolder ("rec.wav.kerf.tmp");               // the old file can be moved, the new one cannot be made
    r.engine.sequencer().setTempo (131.0f);
    r.run (3.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT (r.dir.has ("rec.wav.kerf.old"));             // already out of the way
    EXPECT (! r.dir.has ("rec.wav.kerf"));
    EXPECT_EQ (r.logged ("set aside as rec.wav.kerf.old"), 1);

    std::error_code ec;
    fs::remove (r.dir / "rec.wav.kerf.tmp", ec);
    r.run (6.0);                                         // the retry: nothing is left to set aside

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (r.dir.has ("rec.wav.kerf"));
    EXPECT_EQ (r.onDisk().project.tempo, 131.0f);
    EXPECT_TEXT (r.dir.read ("rec.wav.kerf.old"), std::string ("this is not a project\n"));
    EXPECT (! r.dir.has ("rec.wav.kerf.old2"));
    EXPECT_EQ (r.logged ("set aside as"), 1);
    EXPECT_EQ (r.logged ("cannot set"), 0);
}

KTEST (ProjectKeeper, when_the_old_project_cannot_be_set_aside_it_is_not_overwritten)
{
    Rig r (4 * 44100, 44100.0, false);
    r.dir.write ("rec.wav.kerf", "garbage");

    for (int n = 1; n <= 99; ++n)                        // every name it could be set aside under is taken
        r.dir.write (n == 1 ? "rec.wav.kerf.old" : "rec.wav.kerf.old" + std::to_string (n), "taken");

    EXPECT (r.load().kind == ProjectKeeper::Opened::Kind::damaged);
    r.addSlice();
    r.run (3.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (r.file(), std::string ("garbage"));     // nothing was lost
    EXPECT_TEXT (r.keeper.takeNotice(), std::string ("Cannot save the project"));
    EXPECT_EQ (r.logged ("cannot set the old rec.wav.kerf aside"), 1);
}

// --------------------------------------------------------------------------------- when saving fails

KTEST (ProjectKeeper, a_save_that_fails_is_told_once_and_tried_again_every_five_seconds)
{
    Rig r;
    r.dir.makeFolder ("rec.wav.kerf.tmp");              // the part file cannot be made
    r.addSlice();

    r.at (1.0);
    r.at (2.5);                                          // the moment comes: the save fails
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string ("Cannot save the project"));
    EXPECT_TEXT (r.keeper.takeNotice(), std::string());  // once
    EXPECT_EQ (r.logged ("cannot save rec.wav.kerf"), 1);
    EXPECT (! r.dir.has ("rec.wav.kerf"));

    // The disk is fine again; the keeper does not know, and tries again when its five seconds are up.
    std::error_code ec;
    fs::remove (r.dir / "rec.wav.kerf.tmp", ec);

    r.at (7.484375);                                     // 4.984375 s after the failure
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT (! r.dir.has ("rec.wav.kerf"));

    r.at (7.5);                                          // five seconds
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (r.dir.has ("rec.wav.kerf"));
    EXPECT_EQ (r.logged ("saving works again"), 1);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string());
}

KTEST (ProjectKeeper, a_save_that_keeps_failing_is_logged_once_and_not_told_again)
{
    Rig r;
    r.dir.makeFolder ("rec.wav.kerf.tmp");
    r.addSlice();

    r.run (3.0);                                         // first failure
    EXPECT_TEXT (r.keeper.takeNotice(), std::string ("Cannot save the project"));
    r.run (60.0);                                        // a dozen more tries

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string());
    EXPECT_EQ (r.logged ("cannot save rec.wav.kerf"), 1);
    EXPECT (! r.dir.has ("rec.wav.kerf"));
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);   // only the folder that is in the way

    // A new trouble after recovery is told again.
    std::error_code ec;
    fs::remove (r.dir / "rec.wav.kerf.tmp", ec);
    r.run (6.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);

    r.dir.makeFolder ("rec.wav.kerf.tmp");
    r.engine.sequencer().setTempo (151.0f);
    r.run (3.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string ("Cannot save the project"));
}

KTEST (ProjectKeeper, a_different_trouble_while_failing_is_logged_too_but_not_told_again)
{
    Rig r;
    r.dir.makeFolder ("rec.wav.kerf.tmp");                // the part file cannot be made
    r.addSlice();
    r.run (3.0);
    EXPECT_EQ (r.logged ("cannot save rec.wav.kerf"), 1);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string ("Cannot save the project"));

    // Another reason now: the part file can be made, but there is a folder where the project goes.
    std::error_code ec;
    fs::remove (r.dir / "rec.wav.kerf.tmp", ec);
    r.dir.makeFolder ("rec.wav.kerf");
    r.run (6.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_EQ (r.logged ("cannot save rec.wav.kerf"), 2);
    EXPECT_EQ (r.logged ("cannot replace rec.wav.kerf"), 1);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string());   // the person has been told once
    EXPECT (! r.dir.has ("rec.wav.kerf.tmp"));            // and no part file is left behind

    // And the same reason again and again is still logged once.
    r.run (30.0);
    EXPECT_EQ (r.logged ("cannot save rec.wav.kerf"), 2);

    // The way is clear: it is saved, and said so once.
    fs::remove (r.dir / "rec.wav.kerf", ec);
    r.run (6.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT_EQ (r.logged ("saving works again"), 1);
}

KTEST (ProjectKeeper, a_failing_save_does_not_lose_the_earlier_project)
{
    Rig r (4 * 44100, 44100.0, false);
    const std::string earlier = projectText (4 * 44100, 44100, 2, 90.0f);
    r.dir.write ("rec.wav.kerf", earlier);
    r.load();

    r.dir.makeFolder ("rec.wav.kerf.tmp");
    r.engine.sequencer().setTempo (111.0f);
    r.run (30.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (r.file(), earlier);
}

KTEST (ProjectKeeper, a_failing_backup_does_not_stop_the_save)
{
    Rig r (4 * 44100, 44100.0, false);
    const std::string earlier = projectText (4 * 44100, 44100, 2, 90.0f);
    r.dir.write ("rec.wav.kerf", earlier);
    r.dir.makeFolder ("rec.wav.kerf.bak");              // a folder where the backup should go
    r.load();

    r.engine.sequencer().setTempo (111.0f);
    r.run (3.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT_NEAR (r.onDisk().project.tempo, 111.0, 0.001);
    EXPECT (r.dir.isFolder ("rec.wav.kerf.bak"));
    EXPECT_EQ (r.logged ("no backup of rec.wav.kerf"), 1);
}

KTEST (ProjectKeeper, too_many_slices_cannot_be_saved_and_the_person_is_told)
{
    Rig r (60 * 8000, 8000.0);
    std::vector<Slice> many;

    for (int i = 0; i < Project::kMaxSlices + 1; ++i)
        many.push_back (slice (0.001f * (float) i, 0.001f * (float) i + 0.0005f));

    r.engine.slices().replaceAll (many);
    r.run (3.0);

    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_TEXT (r.keeper.takeNotice(), std::string ("Cannot save the project"));
    EXPECT_EQ (r.logged ("too many slices"), 1);
    EXPECT (r.dir.names().empty());

    r.engine.slices().replaceAll ({ slice (1.0f, 2.0f) });
    r.run (6.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (r.dir.has ("rec.wav.kerf"));
}

// ------------------------------------------------------------------------------ the sample changes

KTEST (ProjectKeeper, a_sample_the_keeper_was_not_told_about_is_never_saved_into_the_old_project)
{
    Rig r;
    r.addSlice();
    r.run (3.0);
    const std::string saved = r.file();

    // The application loads another sample and forgets to say so.
    r.engine.loadSample (makeSample (5 * 44100, 44100.0, "other.wav"));
    r.addSlice (1.0f, 1.5f);
    r.run (30.0);
    r.keeper.flush();

    EXPECT_TEXT (r.file(), saved);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_EQ (r.logged ("another sample"), 1);
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);
}

KTEST (ProjectKeeper, a_sample_of_the_same_length_and_another_rate_is_another_sample)
{
    Rig r;
    r.addSlice();
    r.run (3.0);
    const std::string saved = r.file();

    // The same number of frames, but 48000 Hz: not the sample this project was opened for.
    r.engine.loadSample (makeSample (4 * 44100, 48000.0, "other.wav"));
    r.addSlice (1.0f, 1.5f);
    r.run (30.0);
    r.keeper.flush();

    EXPECT_TEXT (r.file(), saved);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_EQ (r.logged ("another sample"), 1);
    EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);
}

KTEST (ProjectKeeper, a_flush_never_writes_the_work_of_a_sample_the_keeper_was_not_told_about)
{
    // Another length, and another rate with the same length; flush() is the first to look, no frame has noticed.
    for (const auto& other : { std::make_pair (5 * 44100, 44100.0), std::make_pair (4 * 44100, 48000.0) })
    {
        Rig r;
        r.addSlice();
        r.run (3.0);
        const std::string saved = r.file();

        r.engine.loadSample (makeSample (other.first, other.second, "other.wav"));
        r.addSlice (1.0f, 1.5f);
        r.addSlice (2.0f, 2.5f);
        r.keeper.flush();

        EXPECT_TEXT (r.file(), saved);
        EXPECT_EQ (r.dir.names().size(), (std::size_t) 1);
        EXPECT_EQ (r.logged ("cannot"), 0);
    }
}

KTEST (ProjectKeeper, an_unannounced_sample_is_not_saved_even_when_a_save_was_overdue)
{
    Rig r;
    r.addSlice();
    r.run (12.0, false);                                  // the work has been waiting for twelve seconds: a take is in hand
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);
    EXPECT (r.dir.names().empty());

    // The application loads another sample without telling the keeper; the very next frame is free to write.
    r.engine.loadSample (makeSample (5 * 44100, 44100.0, "other.wav"));
    r.addSlice (1.0f, 1.5f);
    r.run (kStep, true);

    EXPECT (r.dir.names().empty());
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    r.run (30.0);
    r.keeper.flush();
    EXPECT (r.dir.names().empty());
}

KTEST (ProjectKeeper, a_flush_that_fails_is_tried_again_five_seconds_later_not_at_once)
{
    Rig r;
    r.dir.makeFolder ("rec.wav.kerf.tmp");                // the part file cannot be made
    r.at (100.0);
    r.addSlice();
    r.at (101.0);                                         // the work is seen; the moment has not come
    r.keeper.flush();                                     // (the sample is about to be replaced) fails
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::problem);
    EXPECT_EQ (r.logged ("cannot save rec.wav.kerf"), 1);

    std::error_code ec;
    fs::remove (r.dir / "rec.wav.kerf.tmp", ec);

    r.at (103.0);                                         // the moment has come, but the five seconds have not
    EXPECT (! r.dir.has ("rec.wav.kerf"));
    r.at (105.984375);
    EXPECT (! r.dir.has ("rec.wav.kerf"));
    r.at (106.0);                                         // five seconds after the flush at 101
    EXPECT (r.dir.has ("rec.wav.kerf"));
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
}

KTEST (ProjectKeeper, opening_another_sample_drops_the_work_that_was_not_flushed)
{
    Rig r;
    r.addSlice();
    r.at (1.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);

    r.load (4 * 44100, 44100.0, "next.wav");             // no flush() first: the contract says the caller does that
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
    r.run (10.0);
    r.keeper.flush();

    EXPECT (r.dir.names().empty());
}

KTEST (ProjectKeeper, flushing_before_opening_another_sample_saves_the_first_one)
{
    Rig r;
    r.addSlice();
    r.at (1.0);
    r.keeper.flush();
    r.load (4 * 44100, 44100.0, "next.wav");

    EXPECT (r.dir.has ("rec.wav.kerf"));
    EXPECT (! r.dir.has ("next.wav.kerf"));
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);
    EXPECT_TEXT (kerf::pathToUtf8 (r.keeper.file().filename()), std::string ("next.wav.kerf"));

    // The new sample starts clean: it has none of the first one's slices and its own project.
    EXPECT_EQ (r.engine.slices().size(), 0);
    r.addSlice (1.0f, 1.5f);
    r.run (3.0);
    EXPECT (r.dir.has ("next.wav.kerf"));
    EXPECT_EQ (r.onDisk().project.slices.size(), (std::size_t) 1);
    EXPECT_EQ (store::read (r.dir / "rec.wav.kerf").project.slices.size(), (std::size_t) 1);
}

KTEST (ProjectKeeper, two_samples_with_the_same_name_and_other_extensions_keep_two_projects)
{
    Rig r (4 * 44100, 44100.0, false);

    r.load (4 * 44100, 44100.0, "kick.wav");
    r.addSlice (0.5f, 1.0f);
    r.run (3.0);

    r.load (4 * 44100, 44100.0, "kick.mp3");
    r.addSlice (1.0f, 2.0f);
    r.addSlice (2.0f, 3.0f);
    r.run (3.0);

    EXPECT_EQ (store::read (r.dir / "kick.wav.kerf").project.slices.size(), (std::size_t) 1);
    EXPECT_EQ (store::read (r.dir / "kick.mp3.kerf").project.slices.size(), (std::size_t) 2);

    // And each comes back with its own.
    EXPECT_EQ (r.load (4 * 44100, 44100.0, "kick.wav").slices, 1);
    EXPECT_EQ (r.load (4 * 44100, 44100.0, "kick.mp3").slices, 2);
}

// ------------------------------------------------------------------------------------- the tag

KTEST (ProjectKeeper, the_making_of_the_file_is_told_once_and_only_for_the_save_that_made_it)
{
    Rig r;
    EXPECT (! r.keeper.takeCreated());                   // looking at a sample makes nothing
    r.addSlice();
    r.run (3.0);                                         // the project is made
    EXPECT (r.dir.has ("rec.wav.kerf"));
    EXPECT (r.keeper.takeCreated());
    EXPECT (! r.keeper.takeCreated());

    // Later saves rewrite the file: nothing new for the list of samples.
    r.engine.sequencer().setTempo (150.0f);
    r.run (3.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (! r.keeper.takeCreated());

    // Nobody took the news, and a second save came: the news is still there.
    Rig q;
    q.addSlice();
    q.run (3.0);
    q.engine.sequencer().setTempo (150.0f);
    q.run (3.0);
    EXPECT (q.keeper.takeCreated());
    EXPECT (! q.keeper.takeCreated());

    // A project that was there already is not made again by saving it.
    Rig p (4 * 44100, 44100.0, false);
    p.dir.write ("rec.wav.kerf", projectText (4 * 44100, 44100, 2));
    p.load();
    p.engine.sequencer().setTempo (150.0f);
    p.run (3.0);
    EXPECT (p.keeper.tag() == ProjectKeeper::Tag::saved);
    EXPECT (! p.keeper.takeCreated());
}

KTEST (ProjectKeeper, the_tag_tells_how_things_stand)
{
    Rig r;
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::none);           // a sample nobody has changed

    r.addSlice();
    r.at (1.0);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);

    r.at (2.5);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);

    r.engine.sequencer().setTempo (141.0f);
    r.at (2.5 + kStep);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::edited);

    r.keeper.setEnabled (false);
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::off);
}

KTEST (ProjectKeeper, a_project_that_was_there_shows_as_saved_from_the_start)
{
    Rig r (4 * 44100, 44100.0, false);
    r.dir.write ("rec.wav.kerf", projectText (4 * 44100, 44100, 1));
    r.load();
    EXPECT (r.keeper.tag() == ProjectKeeper::Tag::saved);
}
