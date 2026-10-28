#pragma once

// Keeps the project of the sample being worked on: brings it back when the sample is loaded, notices
// when the work changes and saves it a moment after the person stops, and tells the screen how
// things stand (SAVED, EDITED, NO SAVE). The files themselves are the ProjectStore's.
//
// The rules, in short:
//  - A project is made the first time the work is changed, never by merely looking at a sample.
//  - Saving waits until nothing has changed for a moment (so a stick that is still moving is not
//    written a hundred times), and for no longer than ten seconds of steady work.
//  - A project that is there but cannot be used (another sample's, damaged, a newer Kerf's) is not
//    touched while the person only looks. A damaged or foreign one is set aside, not overwritten,
//    when the first real save comes; a newer Kerf's is never touched at all.
//  - The first save of a session keeps the project as it was in a .bak file.
//  - A save that fails is tried again every few seconds while there is something to save.
//
// UI thread only (it uses the Engine's UI-side calls).

#include <filesystem>
#include <functional>
#include <string>
#include <utility>

#include "engine.h"
#include "project.h"

namespace kerf::ui
{
    class ProjectKeeper
    {
    public:
        static constexpr double kIdleSeconds = 1.5;           // nothing changed for this long: save
        static constexpr double kLongestWaitSeconds = 10.0;   // ... or the work has been unsaved this long: save
        static constexpr double kRetrySeconds = 5.0;          // after a failed save

        // What the header says.
        enum class Tag
        {
            none,      // nothing to say: no sample, or one that has no project yet and no changes
            saved,     // the project on disk is the work as it is
            edited,    // changed, waiting for the moment to save it
            off,       // projects are switched off (in the settings)
            problem    // cannot be saved: a disk problem, a project from a newer Kerf ...
        };

        // What happened when a sample was opened.
        struct Opened
        {
            enum class Kind
            {
                off,         // projects are switched off: nothing was looked for
                fresh,       // there is no project for this sample yet
                restored,    // there was one, and it is back
                mismatch,    // there is one, but for audio of another length or rate
                damaged,     // there is one, but it cannot be read
                newer        // there is one, from a newer Kerf: left alone, nothing will be saved
            };

            Kind kind = Kind::fresh;
            int slices = 0;       // restored: what came back
            int patterns = 0;
        };

        explicit ProjectKeeper (kerf::Engine& engine) : engine_ (engine) {}

        // Where the keeper says what it does (the host's log).
        void setLogger (std::function<void (const std::string&)> logger) { log_ = std::move (logger); }

        // The "Projects" setting. Off: nothing more is saved, at once. On: takes hold with the next
        // sample that is opened (a sample opened while it was off stays unsaved until then).
        void setEnabled (bool on) noexcept { enabled_ = on; }

        // A sample has just been loaded into the engine from `audioFile`: look for its project and,
        // if there is a usable one, put it into the engine. Whatever the keeper had open is dropped
        // (call flush() before loading if that work should be saved).
        Opened open (const std::filesystem::path& audioFile);

        // Once per frame. `now` is the application's clock in seconds; `mayWrite` is false while
        // something else needs the disk and the time (a take is being recorded).
        void update (double now, bool mayWrite);

        // Saves now, if there is anything to save (the sample is about to be replaced, or the
        // program is ending).
        void flush();

        Tag tag() const noexcept;

        // The project file of the open sample ("" when none is open).
        const std::filesystem::path& file() const noexcept { return file_; }

        // Something the person should be told, once ("" when there is nothing).
        std::string takeNotice();

        // True once after the save that made the project file (the library's list of samples has a
        // mark for those that have one, and is out of date until it is read again).
        bool takeCreated() noexcept { return std::exchange (created_, false); }

    private:
        enum class Mode { closed, off, tracking, blocked };
        enum class OnDisk { nothing, usable, unusable };

        void save (const Project& work, double now);
        void fail (double now, const std::string& why);
        void block (const std::string& why);
        void log (const std::string& line) const;

        kerf::Engine& engine_;
        std::function<void (const std::string&)> log_;
        bool enabled_ = true;

        Mode mode_ = Mode::closed;
        std::filesystem::path file_;

        Project saved_;                 // the work as the file on disk has it (or, before there is one, as it was when the sample came in)
        Project seen_;                  // the work as of the last frame
        bool dirty_ = false;            // the work is not what is saved
        bool onDisk_ = false;           // there is a project file that holds the work as saved_
        double lastChange_ = 0.0;       // when the work last changed (it means something while dirty_ is set)
        double dirtySince_ = 0.0;       // when it first differed from what is saved (the same)
        double lastNow_ = 0.0;          // the clock as of the last update(): flush() has no clock of its own

        OnDisk before_ = OnDisk::nothing;   // what was in the project file when the sample was opened ...
        bool firstSaveDone_ = false;        // ... and whether this session has replaced it yet
        bool failed_ = false;
        bool created_ = false;              // the file was made by a save and nobody has been told yet
        double retryAt_ = 0.0;              // after a failed save: not before this time (failed_ is set)
        std::string lastError_;             // ... and what went wrong, so that the same trouble is logged once
        std::string notice_;
    };
}
