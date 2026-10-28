// Tests for the project files on disk: where a project lives, how a file is read (and what counts
// as unusable), and that a save never puts a good file at risk.
//
// The tests run as an ordinary program, perhaps as root, so a file that "cannot be written" is made
// by putting a folder where the file should be - which no account is allowed to write through.

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "minitest.h"
#include "path_util.h"
#include "project.h"
#include "project_store.h"
#include "temp_folder.h"

#if defined(KERF_STORE_HOOKS)
 #include "store_hooks.h"
#endif

#if ! defined(_WIN32)
 #include <unistd.h>
#endif

using namespace kerf;
using namespace kerf::test;
namespace fs = std::filesystem;
namespace store = kerf::ui::ProjectStore;

namespace
{
    Project someProject()
    {
        Project p;
        p.audio = "kick.wav";
        p.frames = 44100;
        p.rate = 44100;
        p.tempo = 98.0f;
        p.swing = 0.6f;
        p.length = 12;
        p.scene = 3;
        p.selected = 1;

        Slice a;
        a.start = 0.1f;
        a.end = 0.3f;

        Slice b;
        b.start = 0.4f;
        b.end = 0.9f;
        b.pitch = 2.0f;
        b.mode = Slice::Mode::loop;

        p.slices = { a, b };
        p.grid.bits[0][0] = 0x0101;
        p.grid.bits[1][1] = 0x8000;
        return p;
    }

    std::string textOf (const Project& p)
    {
        std::string text;
        EXPECT (serializeProject (p, text));
        return text;
    }

    std::string nameOf (const fs::path& p)
    {
        return kerf::pathToUtf8 (p.filename());
    }

    // An error text that says what could not be done to which file and, in brackets, why: "cannot replace x.kerf (Is a directory)".
    bool saysWhy (const std::string& error, const std::string& what)
    {
        return error.compare (0, what.size() + 2, what + " (") == 0 && error.size() > what.size() + 3 && error.back() == ')'
               && error.find ("()") == std::string::npos;
    }
}

// ----------------------------------------------------------------------------------------- names

KTEST (ProjectStore, a_project_lives_next_to_its_sample_under_the_whole_file_name)
{
    const fs::path wav = fs::path ("some") / "folder" / "rec-001.wav";
    const fs::path project = store::projectPath (wav);

    EXPECT_TEXT (nameOf (project), std::string ("rec-001.wav.kerf"));
    EXPECT (project.parent_path() == wav.parent_path());

    // The same name with another extension is another sample, so another project.
    EXPECT (store::projectPath ("kick.wav") != store::projectPath ("kick.mp3"));
    EXPECT_TEXT (nameOf (store::projectPath ("kick.mp3")), std::string ("kick.mp3.kerf"));

    EXPECT_TEXT (nameOf (store::projectPath ("no extension")), std::string ("no extension.kerf"));
    EXPECT_TEXT (nameOf (store::projectPath ("two.dots.flac")), std::string ("two.dots.flac.kerf"));

    // Names are UTF-8 text, as everywhere in Kerf.
    const fs::path accented = kerf::pathFromUtf8 ("ra\xC3\xA7\xC3\xA3o.wav");
    EXPECT_TEXT (nameOf (store::projectPath (accented)), std::string ("ra\xC3\xA7\xC3\xA3o.wav.kerf"));
}

KTEST (ProjectStore, the_backup_is_the_project_file_with_bak_added)
{
    const fs::path project = fs::path ("some") / "rec-001.wav.kerf";
    EXPECT_TEXT (nameOf (store::backupPath (project)), std::string ("rec-001.wav.kerf.bak"));
    EXPECT (store::backupPath (project).parent_path() == project.parent_path());
}

// ----------------------------------------------------------------------------------------- read

KTEST (ProjectStore, a_file_that_is_not_there_is_missing_and_that_is_no_error)
{
    TempFolder dir;

    const auto r = store::read (dir / "nothing.wav.kerf");
    EXPECT (r.kind == store::Read::Kind::missing);
    EXPECT (r.error.empty());

    const auto deeper = store::read (dir / "no" / "such" / "folder" / "x.wav.kerf");
    EXPECT (deeper.kind == store::Read::Kind::missing);
}

KTEST (ProjectStore, what_was_written_is_read_back)
{
    TempFolder dir;
    const Project p = someProject();
    std::string error;

    EXPECT (store::write (dir / "kick.wav.kerf", textOf (p), error));
    EXPECT (error.empty());

    const auto r = store::read (dir / "kick.wav.kerf");
    EXPECT (r.kind == store::Read::Kind::ok);
    EXPECT (r.error.empty());
    EXPECT (sameWork (r.project, p));
    EXPECT_EQ (r.project.selected, 1);
    EXPECT_EQ (r.project.scene, 3);
    EXPECT_TEXT (r.project.audio, std::string ("kick.wav"));
    EXPECT_EQ (r.project.frames, (std::int64_t) 44100);
    EXPECT_EQ (r.project.rate, (std::int64_t) 44100);
}

KTEST (ProjectStore, a_file_of_a_newer_kerf_is_reported_as_newer_and_is_not_read)
{
    TempFolder dir;
    const std::string text = "kerf_project = 2\nsomething = that\nis not this Kerf's business\n";
    dir.write ("kick.wav.kerf", text);

    const auto r = store::read (dir / "kick.wav.kerf");
    EXPECT (r.kind == store::Read::Kind::newer);
    EXPECT (! r.error.empty());
    EXPECT (r.project.slices.empty());
    EXPECT_TEXT (dir.read ("kick.wav.kerf"), text);       // reading does not change anything
}

KTEST (ProjectStore, files_that_are_not_projects_are_damaged_with_a_reason)
{
    TempFolder dir;

    dir.write ("a.kerf", "");
    dir.write ("b.kerf", "this is not a project\n");
    dir.write ("c.kerf", std::string ("\x00\x01\x02\xFF\xFE binary", 12));
    dir.write ("d.kerf", textOf (someProject()).substr (0, 120));      // cut short, as a power cut would

    for (const char* name : { "a.kerf", "b.kerf", "c.kerf", "d.kerf" })
    {
        const auto r = store::read (dir / name);
        EXPECT (r.kind == store::Read::Kind::damaged);
        EXPECT (! r.error.empty());
        EXPECT (r.project.slices.empty());
    }
}

KTEST (ProjectStore, a_folder_where_the_file_should_be_is_damaged_not_a_crash)
{
    TempFolder dir;
    dir.makeFolder ("kick.wav.kerf");

    const auto r = store::read (dir / "kick.wav.kerf");
    EXPECT (r.kind == store::Read::Kind::damaged);
    EXPECT_TEXT (r.error, std::string ("kick.wav.kerf is not a file"));
    EXPECT (dir.isFolder ("kick.wav.kerf"));
}

#if ! defined(_WIN32)
namespace
{
    // Root may read and write anything, so the checks that need a file nobody may touch only mean something for an ordinary user.
    bool ordinaryUser()
    {
        if (::geteuid() != 0)
            return true;

        std::fputs ("    (not checked: running as root; run this program as an ordinary user too)\n", stderr);
        return false;
    }
}

KTEST (ProjectStore, a_name_that_cannot_even_be_looked_at_is_damaged_with_a_reason)
{
    TempFolder dir;
    std::error_code ec;
    fs::create_symlink ("loop.kerf", dir / "loop.kerf", ec);           // a link to itself
    EXPECT (! ec);

    const auto r = store::read (dir / "loop.kerf");
    EXPECT (r.kind == store::Read::Kind::damaged);
    EXPECT (saysWhy (r.error, "cannot look at loop.kerf"));
    EXPECT (r.project.slices.empty());
}

KTEST (ProjectStore, a_file_that_may_not_be_read_is_damaged_with_a_reason)
{
    if (! ordinaryUser())
        return;

    TempFolder dir;
    dir.write ("x.kerf", textOf (someProject()));
    std::error_code ec;
    fs::permissions (dir / "x.kerf", fs::perms::none, ec);

    const auto r = store::read (dir / "x.kerf");
    std::string error;
    EXPECT (! store::backUp (dir / "x.kerf", error));                   // and it cannot be backed up either

    fs::permissions (dir / "x.kerf", fs::perms::owner_read | fs::perms::owner_write, ec);
    EXPECT (r.kind == store::Read::Kind::damaged);
    EXPECT_TEXT (r.error, std::string ("cannot open x.kerf"));
    EXPECT_TEXT (error, std::string ("cannot open x.kerf"));
    EXPECT (! dir.has ("x.kerf.bak"));
}

KTEST (ProjectStore, a_folder_that_may_not_be_searched_makes_the_file_damaged_with_a_reason)
{
    if (! ordinaryUser())
        return;

    TempFolder dir;
    dir.makeFolder ("locked");
    dir.write ("locked/x.kerf", textOf (someProject()));
    std::error_code ec;
    fs::permissions (dir / "locked", fs::perms::none, ec);

    const auto r = store::read (dir / "locked" / "x.kerf");
    fs::permissions (dir / "locked", fs::perms::owner_all, ec);

    EXPECT (r.kind == store::Read::Kind::damaged);
    EXPECT (saysWhy (r.error, "cannot look at x.kerf"));
}

KTEST (ProjectStore, a_folder_that_may_not_be_written_says_so_and_changes_nothing)
{
    if (! ordinaryUser())
        return;

    TempFolder dir;
    std::string error;
    EXPECT (store::write (dir / "x.kerf", "the good one", error));

    std::error_code ec;
    fs::permissions (dir.path, fs::perms::owner_read | fs::perms::owner_exec, ec);
    const bool saved = store::write (dir / "x.kerf", "the new one", error);
    fs::permissions (dir.path, fs::perms::owner_all, ec);

    EXPECT (! saved);
    EXPECT (saysWhy (error, "cannot create x.kerf.tmp"));
    EXPECT_TEXT (dir.read ("x.kerf"), std::string ("the good one"));
    EXPECT (! dir.has ("x.kerf.tmp"));
}
#endif

KTEST (ProjectStore, a_file_beyond_the_size_limit_is_damaged_without_being_read)
{
    TempFolder dir;
    const fs::path big = dir / "big.wav.kerf";

    {
        dir.write ("big.wav.kerf", textOf (someProject()));
        std::error_code ec;
        fs::resize_file (big, Project::kMaxFileBytes + 1, ec);      // a sparse file: costs nothing
        EXPECT (! ec);
    }

    const auto r = store::read (big);
    EXPECT (r.kind == store::Read::Kind::damaged);
    EXPECT_TEXT (r.error, std::string ("big.wav.kerf is too big"));          // the store's own words: the file was not read at all

    // Exactly at the limit is the parser's business, not the store's: it is read (and found wanting).
    std::error_code ec;
    fs::resize_file (big, Project::kMaxFileBytes, ec);
    const auto exactly = store::read (big);
    EXPECT (exactly.kind == store::Read::Kind::damaged);
    EXPECT (exactly.error.find ("too big") == std::string::npos);
}

// ---------------------------------------------------------------------------------------- write

KTEST (ProjectStore, a_write_makes_the_file_with_exactly_the_text_and_leaves_nothing_else)
{
    TempFolder dir;
    std::string error;

    const std::string text = textOf (someProject());
    EXPECT (store::write (dir / "kick.wav.kerf", text, error));
    EXPECT_TEXT (dir.read ("kick.wav.kerf"), text);
    EXPECT_EQ (dir.names().size(), (std::size_t) 1);        // no .tmp left behind

    // Bytes, not text: nothing is translated on any system.
    const std::string awkward = std::string ("a\r\nb\n\x00" "c\x1A\xFF", 9);
    EXPECT (store::write (dir / "odd.kerf", awkward, error));
    EXPECT_TEXT (dir.read ("odd.kerf"), awkward);

    // An empty text is a file of no bytes.
    EXPECT (store::write (dir / "empty.kerf", "", error));
    EXPECT (dir.has ("empty.kerf"));
    EXPECT_EQ (dir.read ("empty.kerf").size(), (std::size_t) 0);

    // A big one is written whole.
    const std::string big (3 * 1024 * 1024 + 17, 'k');
    EXPECT (store::write (dir / "big.kerf", big, error));
    EXPECT (dir.read ("big.kerf") == big);
    EXPECT (! dir.has ("big.kerf.tmp"));
}

#if defined(KERF_STORE_HOOKS)

// The reason for writing to a part file at all: whatever happens, the file is either the old one or the
// whole new one. For that the bytes have to be on the disk (not only in the system's cache) before the
// new file takes the old one's place, and these tests watch the two calls that do it.

KTEST (ProjectStore, a_save_is_on_the_disk_before_it_takes_the_old_files_place)
{
    TempFolder dir;
    std::string error;
    const std::string text = textOf (someProject());

    kerf::test::syscalls::start();
    EXPECT (store::write (dir / "kick.wav.kerf", text, error));
    const auto events = kerf::test::syscalls::stop();

    // One flush, of the part file with the whole text in it, and then the rename - in that order.
    EXPECT_EQ (events.size(), (std::size_t) 2);

    if (events.size() == 2)
    {
        EXPECT_TEXT (events[0], "fsync kick.wav.kerf.tmp " + std::to_string (text.size()));
        EXPECT_TEXT (events[1], std::string ("rename kick.wav.kerf.tmp kick.wav.kerf"));
    }
}

KTEST (ProjectStore, a_backup_is_flushed_before_it_takes_its_place_too)
{
    TempFolder dir;
    std::string error;
    const std::string text = textOf (someProject());
    EXPECT (store::write (dir / "kick.wav.kerf", text, error));

    kerf::test::syscalls::start();
    EXPECT (store::backUp (dir / "kick.wav.kerf", error));
    const auto events = kerf::test::syscalls::stop();

    EXPECT_EQ (events.size(), (std::size_t) 2);

    if (events.size() == 2)
    {
        EXPECT_TEXT (events[0], "fsync kick.wav.kerf.bak.tmp " + std::to_string (text.size()));
        EXPECT_TEXT (events[1], std::string ("rename kick.wav.kerf.bak.tmp kick.wav.kerf.bak"));
    }
}

KTEST (ProjectStore, a_call_that_fails_stops_the_save_and_leaves_the_old_file_alone)
{
    using kerf::test::syscalls::Call;

    TempFolder dir;
    std::string error;
    const std::string before = textOf (someProject());
    EXPECT (store::write (dir / "kick.wav.kerf", before, error));

    Project other = someProject();
    other.tempo = 77.0f;
    const std::string after = textOf (other);

    // Whichever step of the writing goes wrong - the bytes cannot be written, cannot be flushed,
    // cannot be put on the disk, or the file cannot be closed - and whatever the disk says about it
    // (full, broken, a card that went away), the old file stays and nothing is left behind.
    struct Failure { Call call; int why; };
    const Failure failures[] = { { Call::fwrite, ENOSPC }, { Call::fflush, ENOSPC }, { Call::fsync, EIO }, { Call::fsync, ENOSPC },
                                 { Call::fsync, EBADF }, { Call::fsync, EROFS }, { Call::fclose, EIO } };

    for (const auto& f : failures)
    {
        kerf::test::syscalls::start();
        kerf::test::syscalls::failNext (f.call, f.why);
        error.clear();

        EXPECT (! store::write (dir / "kick.wav.kerf", after, error));
        const auto events = kerf::test::syscalls::stop();

        EXPECT (error.find ("cannot write kick.wav.kerf") != std::string::npos);
        EXPECT (error.find ("full") != std::string::npos);
        EXPECT_EQ (events.size(), (std::size_t) 1);                                    // the flush happened; the rename never did
        EXPECT_TEXT (dir.read ("kick.wav.kerf"), before);
        EXPECT (! dir.has ("kick.wav.kerf.tmp"));
        EXPECT_EQ (dir.names().size(), (std::size_t) 1);
    }

    // And it works again as soon as the disk does.
    EXPECT (store::write (dir / "kick.wav.kerf", after, error));
    EXPECT_TEXT (dir.read ("kick.wav.kerf"), after);
}

KTEST (ProjectStore, a_file_system_that_cannot_be_told_to_flush_is_not_a_failed_save)
{
    TempFolder dir;
    std::string error;
    const std::string text = textOf (someProject());

    // Some file systems (and some cards' drivers) answer "I do not do that": the write is still good.
    for (const int why : { EINVAL, ENOSYS, ENOTSUP })
    {
        kerf::test::syscalls::start();
        kerf::test::syscalls::failNext (kerf::test::syscalls::Call::fsync, why);

        EXPECT (store::write (dir / "kick.wav.kerf", text, error));
        const auto events = kerf::test::syscalls::stop();

        EXPECT_EQ (events.size(), (std::size_t) 2);
        EXPECT_TEXT (dir.read ("kick.wav.kerf"), text);
        EXPECT_EQ (dir.names().size(), (std::size_t) 1);

        std::error_code ec;
        fs::remove (dir / "kick.wav.kerf", ec);
    }
}

#endif

#if defined(__linux__)

// The size a file system reports is not always what it gives: the files of /sys say 4096 and give a
// few bytes. A file that gives less than its size said is not read as if it were whole.
KTEST (ProjectStore, a_file_that_gives_less_than_its_size_said_is_damaged_not_half_read)
{
    std::error_code ignored;

    if (! fs::exists ("/sys/devices/system/cpu", ignored))
        return;                                                  // no sysfs here: nothing to try

    int tried = 0;

    for (const char* candidate : { "/sys/kernel/uevent_seqnum", "/sys/devices/system/cpu/online", "/sys/devices/system/cpu/possible",
                                    "/sys/devices/system/cpu/present" })
    {
        std::error_code ec;
        const auto said = fs::file_size (candidate, ec);

        if (ec || said == 0)
            continue;

        std::ifstream in (candidate, std::ios::binary);
        const std::string given ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());

        if (given.size() >= said)
            continue;

        const auto r = store::read (candidate);
        EXPECT (r.kind == store::Read::Kind::damaged);
        EXPECT (r.error.find ("cannot read all of") != std::string::npos);

        std::string error;
        EXPECT (! store::backUp (candidate, error));
        EXPECT (error.find ("cannot read all of") != std::string::npos);
        ++tried;
    }

    EXPECT (tried > 0);
}

#endif

KTEST (ProjectStore, a_write_replaces_the_file_that_was_there)
{
    TempFolder dir;
    std::string error;

    EXPECT (store::write (dir / "x.kerf", "first version, which is rather longer than the second", error));
    EXPECT (store::write (dir / "x.kerf", "second", error));
    EXPECT_TEXT (dir.read ("x.kerf"), std::string ("second"));     // not "second" + the rest of the first
    EXPECT_EQ (dir.names().size(), (std::size_t) 1);

    EXPECT (store::write (dir / "x.kerf", "third, longer than the one before it", error));
    EXPECT_TEXT (dir.read ("x.kerf"), std::string ("third, longer than the one before it"));
}

KTEST (ProjectStore, a_write_into_a_folder_that_is_not_there_says_so)
{
    TempFolder dir;
    std::string error;

    EXPECT (! store::write (dir / "gone" / "x.kerf", "text", error));
    EXPECT (saysWhy (error, "cannot create x.kerf.tmp"));
    EXPECT (dir.names().empty());
}

KTEST (ProjectStore, when_the_part_file_cannot_be_made_the_old_file_stays_as_it_was)
{
    TempFolder dir;
    std::string error;

    EXPECT (store::write (dir / "x.kerf", "the good one", error));
    dir.makeFolder ("x.kerf.tmp");                          // the file the write needs is taken by a folder

    error.clear();
    EXPECT (! store::write (dir / "x.kerf", "the new one", error));
    EXPECT (saysWhy (error, "cannot create x.kerf.tmp"));
    EXPECT_TEXT (dir.read ("x.kerf"), std::string ("the good one"));
    EXPECT (dir.isFolder ("x.kerf.tmp"));                   // what was not ours is not touched
}

KTEST (ProjectStore, when_the_file_cannot_be_replaced_the_part_file_is_removed)
{
    TempFolder dir;
    dir.makeFolder ("x.kerf");                              // a folder is where the project should go
    dir.write ("x.kerf/inside.txt", "keep");

    std::string error;
    EXPECT (! store::write (dir / "x.kerf", "text", error));
    EXPECT (saysWhy (error, "cannot replace x.kerf"));
    EXPECT (! dir.has ("x.kerf.tmp"));                      // nothing left behind
    EXPECT (dir.isFolder ("x.kerf"));
    EXPECT_TEXT (dir.read ("x.kerf/inside.txt"), std::string ("keep"));
}

KTEST (ProjectStore, a_name_with_spaces_and_accents_is_written_and_read)
{
    TempFolder dir;
    const std::string name = "ra\xC3\xA7\xC3\xA3o do bombo.wav.kerf";
    const fs::path file = dir.path / kerf::pathFromUtf8 (name);
    std::string error;

    EXPECT (store::write (file, textOf (someProject()), error));
    EXPECT (store::read (file).kind == store::Read::Kind::ok);
    EXPECT_EQ (dir.names().size(), (std::size_t) 1);
    EXPECT_TEXT (kerf::pathToUtf8 (fs::directory_iterator (dir.path)->path().filename()), name);
}

// ---------------------------------------------------------------------------------------- backup

KTEST (ProjectStore, a_backup_is_a_copy_next_to_the_file)
{
    TempFolder dir;
    const std::string text = textOf (someProject());
    dir.write ("kick.wav.kerf", text);

    std::string error;
    EXPECT (store::backUp (dir / "kick.wav.kerf", error));
    EXPECT (error.empty());
    EXPECT_TEXT (dir.read ("kick.wav.kerf.bak"), text);
    EXPECT_TEXT (dir.read ("kick.wav.kerf"), text);           // the original is as it was
    EXPECT (! dir.has ("kick.wav.kerf.bak.tmp"));
}

KTEST (ProjectStore, a_new_backup_replaces_the_old_one)
{
    TempFolder dir;
    dir.write ("x.kerf", "now");
    dir.write ("x.kerf.bak", "an older backup that is longer than the one that replaces it");

    std::string error;
    EXPECT (store::backUp (dir / "x.kerf", error));
    EXPECT_TEXT (dir.read ("x.kerf.bak"), std::string ("now"));
    EXPECT_EQ (dir.names().size(), (std::size_t) 2);
}

KTEST (ProjectStore, a_backup_of_nothing_fails_and_makes_nothing)
{
    TempFolder dir;
    std::string error;

    EXPECT (! store::backUp (dir / "x.kerf", error));
    EXPECT (saysWhy (error, "cannot read x.kerf"));
    EXPECT (dir.names().empty());
}

KTEST (ProjectStore, a_backup_that_cannot_be_made_leaves_the_old_backup_and_the_file)
{
    TempFolder dir;
    dir.write ("x.kerf", "the project");
    dir.makeFolder ("x.kerf.bak.tmp");                      // the part file of the backup is taken

    std::string error;
    EXPECT (! store::backUp (dir / "x.kerf", error));
    EXPECT (! error.empty());
    EXPECT_TEXT (dir.read ("x.kerf"), std::string ("the project"));

    // And a backup that is itself a folder cannot be replaced.
    TempFolder other;
    other.write ("y.kerf", "the project");
    other.makeFolder ("y.kerf.bak");
    EXPECT (! store::backUp (other / "y.kerf", error));
    EXPECT (other.isFolder ("y.kerf.bak"));
    EXPECT_TEXT (other.read ("y.kerf"), std::string ("the project"));
    EXPECT (! other.has ("y.kerf.bak.tmp"));
}

KTEST (ProjectStore, a_folder_is_not_backed_up_as_if_it_were_a_file)
{
    TempFolder dir;
    dir.makeFolder ("x.kerf");

    std::string error;
    EXPECT (! store::backUp (dir / "x.kerf", error));
    EXPECT (saysWhy (error, "cannot read x.kerf"));
    EXPECT (! dir.has ("x.kerf.bak"));
}

// ------------------------------------------------------------------------------------- set aside

KTEST (ProjectStore, a_project_that_cannot_be_used_is_renamed_to_old)
{
    TempFolder dir;
    dir.write ("x.wav.kerf", "something that cannot be used");

    const auto moved = store::setAside (dir / "x.wav.kerf");
    EXPECT_TEXT (nameOf (moved), std::string ("x.wav.kerf.old"));
    EXPECT (! dir.has ("x.wav.kerf"));
    EXPECT_TEXT (dir.read ("x.wav.kerf.old"), std::string ("something that cannot be used"));
}

KTEST (ProjectStore, what_was_set_aside_before_is_not_overwritten)
{
    TempFolder dir;

    dir.write ("x.kerf", "one");
    EXPECT_TEXT (nameOf (store::setAside (dir / "x.kerf")), std::string ("x.kerf.old"));

    dir.write ("x.kerf", "two");
    EXPECT_TEXT (nameOf (store::setAside (dir / "x.kerf")), std::string ("x.kerf.old2"));

    dir.write ("x.kerf", "three");
    EXPECT_TEXT (nameOf (store::setAside (dir / "x.kerf")), std::string ("x.kerf.old3"));

    EXPECT_TEXT (dir.read ("x.kerf.old"), std::string ("one"));
    EXPECT_TEXT (dir.read ("x.kerf.old2"), std::string ("two"));
    EXPECT_TEXT (dir.read ("x.kerf.old3"), std::string ("three"));
    EXPECT (! dir.has ("x.kerf"));
}

KTEST (ProjectStore, setting_aside_gives_up_after_the_ninety_ninth)
{
    TempFolder dir;

    for (int n = 1; n <= 98; ++n)
        dir.write (n == 1 ? "x.kerf.old" : "x.kerf.old" + std::to_string (n), "taken");

    dir.write ("x.kerf", "the file");
    EXPECT_TEXT (nameOf (store::setAside (dir / "x.kerf")), std::string ("x.kerf.old99"));

    dir.write ("x.kerf", "the file again");
    EXPECT (store::setAside (dir / "x.kerf").empty());       // all 99 are taken
    EXPECT_TEXT (dir.read ("x.kerf"), std::string ("the file again"));       // nothing is lost
    EXPECT_TEXT (dir.read ("x.kerf.old99"), std::string ("the file"));
}

KTEST (ProjectStore, setting_aside_a_file_that_is_not_there_gives_nothing)
{
    TempFolder dir;

    EXPECT (store::setAside (dir / "x.kerf").empty());
    EXPECT (dir.names().empty());
}
