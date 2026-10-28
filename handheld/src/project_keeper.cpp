#include "project_keeper.h"

#include <utility>

#include "path_util.h"
#include "project_store.h"

namespace fs = std::filesystem;

namespace kerf::ui
{
    namespace
    {
        std::string nameOf (const fs::path& p)
        {
            return pathToUtf8 (p.filename());
        }
    }

    void ProjectKeeper::log (const std::string& line) const
    {
        if (log_)
            log_ ("project: " + line);
    }

    ProjectKeeper::Opened ProjectKeeper::open (const fs::path& audioFile)
    {
        // Whatever was open is dropped: the caller flushed it first if it mattered. What the keeper has to tell
        // the application (a notice, a new file) is not part of the open sample and stays: the flush that came
        // just before this may have raised it, and the application takes it after.
        mode_ = Mode::closed;
        file_.clear();
        dirty_ = false;
        onDisk_ = false;
        before_ = OnDisk::nothing;
        firstSaveDone_ = false;
        failed_ = false;

        Opened opened;
        const auto sample = engine_.sample();

        if (sample == nullptr)
            return opened;

        file_ = ProjectStore::projectPath (audioFile);

        // The work as the sample comes in is the starting point: nothing is "changed" until it differs.
        saved_ = seen_ = captureProject (engine_);

        if (! enabled_)
        {
            mode_ = Mode::off;
            opened.kind = Opened::Kind::off;
            return opened;
        }

        mode_ = Mode::tracking;

        const std::string name = nameOf (file_);
        const auto found = ProjectStore::read (file_);

        switch (found.kind)
        {
            case ProjectStore::Read::Kind::missing:
                opened.kind = Opened::Kind::fresh;
                break;

            case ProjectStore::Read::Kind::newer:
                mode_ = Mode::blocked;
                opened.kind = Opened::Kind::newer;
                log (name + " was made by a newer Kerf (" + found.error + "); left alone, nothing will be saved for this sample");
                break;

            case ProjectStore::Read::Kind::damaged:
                before_ = OnDisk::unusable;
                opened.kind = Opened::Kind::damaged;
                log (name + " cannot be used (" + found.error + "); it is kept as it is until the work is saved, then set aside");
                break;

            case ProjectStore::Read::Kind::ok:
                if (applyProject (engine_, found.project))        // (it says no to a project made for other audio)
                {
                    saved_ = seen_ = captureProject (engine_);
                    onDisk_ = true;
                    before_ = OnDisk::usable;
                    opened.kind = Opened::Kind::restored;
                    opened.slices = (int) saved_.slices.size();
                    opened.patterns = saved_.patternCount();
                    log ("restored " + name + " (" + std::to_string (opened.slices) + " slices, " + std::to_string (opened.patterns) + " pattern rows)");
                }
                else
                {
                    before_ = OnDisk::unusable;
                    opened.kind = Opened::Kind::mismatch;
                    log (name + " is for audio of another length or rate (" + std::to_string (found.project.frames) + " frames at "
                         + std::to_string (found.project.rate) + " Hz, this one has " + std::to_string (saved_.frames) + " at "
                         + std::to_string (saved_.rate) + "); it is kept as it is until the work is saved, then set aside");
                }

                break;
        }

        return opened;
    }

    void ProjectKeeper::update (double now, bool mayWrite)
    {
        lastNow_ = now;

        if (mode_ != Mode::tracking)
            return;

        const Project work = captureProject (engine_);

        // The project belongs to the sample it was opened for. If the engine has another one, the
        // application loaded a sample without telling the keeper: never write that one's work here.
        if (work.frames != saved_.frames || work.rate != saved_.rate)
        {
            block ("the engine has another sample than the one this project was opened for");
            return;
        }

        if (! sameWork (work, seen_))
        {
            seen_ = work;
            lastChange_ = now;
        }

        // The clock of "waiting to be saved" is held at the present until the work first differs from what is saved.
        if (! dirty_)
            dirtySince_ = now;

        dirty_ = ! sameWork (work, saved_);

        if (! dirty_ || ! enabled_ || ! mayWrite)
            return;

        if (failed_ && now < retryAt_)
            return;

        if (now - lastChange_ >= kIdleSeconds || now - dirtySince_ >= kLongestWaitSeconds)
            save (work, now);
    }

    void ProjectKeeper::flush()
    {
        if (mode_ != Mode::tracking || ! enabled_)
            return;

        const Project work = captureProject (engine_);

        if (work.frames != saved_.frames || work.rate != saved_.rate || sameWork (work, saved_))
            return;

        save (work, lastNow_);
    }

    void ProjectKeeper::save (const Project& work, double now)
    {
        std::string text, error;

        if (! serializeProject (work, text))
        {
            fail (now, "there are too many slices to save");
            return;
        }

        const std::string name = nameOf (file_);

        // The first save of a session meets the project that was there when the sample was opened.
        if (! firstSaveDone_)
        {
            if (before_ == OnDisk::usable)
            {
                if (ProjectStore::backUp (file_, error))
                    log ("kept the earlier " + name + " as " + name + ".bak");
                else
                    log ("no backup of " + name + " (" + error + ")");     // not a reason to lose the save as well
            }
            else if (before_ == OnDisk::unusable)
            {
                const auto moved = ProjectStore::setAside (file_);

                if (moved.empty())
                {
                    fail (now, "cannot set the old " + name + " aside");
                    return;
                }

                before_ = OnDisk::nothing;
                log ("the earlier " + name + " could not be used; set aside as " + nameOf (moved));
            }
        }

        if (! ProjectStore::write (file_, text, error))
        {
            fail (now, error);
            return;
        }

        created_ = created_ || ! onDisk_;
        saved_ = work;
        seen_ = work;
        dirty_ = false;
        onDisk_ = true;
        firstSaveDone_ = true;

        if (failed_)
        {
            failed_ = false;
            log ("saving works again");
        }

        log ("saved " + name + " (" + std::to_string (work.slices.size()) + " slices, " + std::to_string (work.patternCount())
             + " pattern rows, " + std::to_string (text.size()) + " bytes)");
    }

    void ProjectKeeper::fail (double now, const std::string& why)
    {
        if (! failed_ || why != lastError_)
            log ("cannot save " + nameOf (file_) + ": " + why);

        if (! failed_)
            notice_ = "Cannot save the project";

        failed_ = true;
        lastError_ = why;
        retryAt_ = now + kRetrySeconds;
    }

    void ProjectKeeper::block (const std::string& why)
    {
        mode_ = Mode::blocked;
        log ("saving stopped for " + nameOf (file_) + ": " + why);
    }

    ProjectKeeper::Tag ProjectKeeper::tag() const noexcept
    {
        switch (mode_)
        {
            case Mode::closed:   return Tag::none;
            case Mode::off:      return Tag::off;
            case Mode::blocked:  return Tag::problem;
            case Mode::tracking: break;
        }

        if (! enabled_)
            return Tag::off;

        if (failed_)
            return Tag::problem;

        if (dirty_)
            return Tag::edited;

        return onDisk_ ? Tag::saved : Tag::none;
    }

    std::string ProjectKeeper::takeNotice()
    {
        return std::exchange (notice_, std::string());
    }
}
