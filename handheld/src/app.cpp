#include "app.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include "format.h"
#include "help_text.h"
#include "path_util.h"
#include "project_store.h"
#include "utf8.h"

namespace fs = std::filesystem;

namespace kerf::ui
{
    namespace
    {
        constexpr int kBrowserRows = 12;
    }

    App::App (kerf::Engine& engine, Config& config, fs::path configFile, fs::path startDir)
        : engine_ (engine), cfg_ (config), configFile_ (std::move (configFile)), startDir_ (std::move (startDir)), project_ (engine)
    {
        std::fill (std::begin (pianoNotes_), std::end (pianoNotes_), -1);

        project_.setEnabled (cfg_.projects);

        engine_.setMasterVolume (cfg_.masterVolume);
        engine_.setNoteOffset (cfg_.noteOffset);
        engine_.setMidiMode (cfg_.midiMode == 1 ? kerf::VoiceEngine::MidiMode::sampler : kerf::VoiceEngine::MidiMode::trigger);

        buildMenu();
        buildSettings();

        // Kerf starts on the Record page: that is how most samples come in. The library (LOAD) is the
        // page behind it, one tap of Select away, and is ready for that.
        prepareBrowser();
        prepareProjects();
        setScreen (Screen::record);
    }

    App::~App()
    {
        // Never leave a background decode running into freed memory.
        if (loading_.valid())
            loading_.wait();

        project_.flush();    // the work of the last moments is written before the program ends
        leaveRecord();       // closes the input: the host's hub may go away right after the app
        stopPlay();
        saveSettings();
    }

    void App::saveSettings()
    {
        if (! configFile_.empty())
            cfg_.save (configFile_);
    }

    void App::showToast (std::string text, float seconds)
    {
        toast_ = std::move (text);
        toastTime_ = seconds;
    }

    void App::setScreen (Screen s)
    {
        if (screen_ == Screen::editor && s != Screen::editor)
        {
            stopPlay();

            for (auto& n : pianoNotes_)
            {
                if (n >= 0)
                {
                    engine_.noteOff (n);
                    n = -1;
                }
            }
        }

        // Only what the hands were doing stops when the page is left: the sequencer itself keeps
        // running (so a slice can be tuned on the other page while the pattern plays).
        if (screen_ == Screen::sequencer && s != Screen::sequencer)
            leaveSequencer();

        if (screen_ == Screen::record && s != Screen::record)
            leaveRecord();

        const bool enteringSequencer = s == Screen::sequencer && screen_ != Screen::sequencer;
        const bool enteringRecord = s == Screen::record && screen_ != Screen::record;

        if (s == Screen::editor || s == Screen::sequencer)
            mainScreen_ = s;

        if (s == Screen::editor || s == Screen::sequencer || isHomePage (s))
            page_ = s;

        if (s != screen_)
            selectArmed_ = false;   // a Select that went down on the page before is not a tap on this one

        if (s == Screen::projects && screen_ != Screen::projects)
            refreshProjects();      // a project may have been saved since it was last up

        screen_ = s;

        if (enteringSequencer)
            enterSequencer();

        if (enteringRecord)
            enterRecord();
    }

    // Select: the next page of Kerf (slicer, then sequencer, then the slicer again).
    void App::cyclePage (int direction)
    {
        static const Screen pages[] = { Screen::editor, Screen::sequencer };
        constexpr int count = (int) (sizeof pages / sizeof pages[0]);

        int at = 0;

        for (int i = 0; i < count; ++i)
            if (pages[i] == mainScreen_)
                at = i;

        setScreen (pages[((at + direction) % count + count) % count]);
    }

    // Select on REC and LOAD: a tap flips to the other one. Like on the work pages the page changes when
    // the button is let go, and only if nothing else was pressed with it, so that Select + Start (the
    // way out of the program) can never flip a page on the way.
    bool App::selectTap (const Pad& pad)
    {
        if (pad.pressed[(int) Btn::Select])
        {
            selectArmed_ = true;
            selectChord_ = false;
        }

        if (pad.down[(int) Btn::Select] || pad.released[(int) Btn::Select])
        {
            for (int b = 0; b < kBtnCount; ++b)
                if (b != (int) Btn::Select && pad.pressed[b])
                    selectChord_ = true;
        }

        const bool tapped = pad.released[(int) Btn::Select] && selectArmed_ && ! selectChord_;

        if (pad.released[(int) Btn::Select])
            selectArmed_ = false;

        return tapped;
    }

    void App::openHelp()
    {
        // The page that matches the device in your hands and the page of Kerf you are on.
        if (isHomePage (page_))
            helpPage_ = usingKeyboard_ ? kHelpKeyboardHome : kHelpPadHome;
        else
            helpPage_ = (usingKeyboard_ ? 2 : 0) + (mainScreen_ == Screen::sequencer ? 1 : 0);

        setScreen (Screen::help);
    }

    // ------------------------------------------------------------------------------ loading

    bool App::isLoading() const { return loading_.valid(); }

    void App::loadFile (const fs::path& file, std::string doneNote)
    {
        if (isLoading())
            return;

        loadingName_ = pathToUtf8 (file.filename());
        loadingPath_ = file;
        loadingNote_ = std::move (doneNote);
        loading_ = std::async (std::launch::async, [file] { return kerf::decodeAudioFile (file); });
    }

    void App::waitForLoad()
    {
        if (loading_.valid())
        {
            loading_.wait();
            finishLoad();
        }
    }

    void App::finishLoad()
    {
        auto result = loading_.get();
        const std::string note = std::move (loadingNote_);
        loadingNote_.clear();

        // A project being opened comes with the sample it was made for: if the sample does not load, neither does the project.
        const bool opening = std::exchange (pendingProjectSet_, false);
        const kerf::Project wanted = opening ? std::move (pendingProject_) : kerf::Project();
        const fs::path wantedFile = opening ? pendingProjectFile_ : fs::path();

        if (result.sample == nullptr)
        {
            showToast ("Could not load " + elide (loadingName_, 22) + ": " + result.error, 4.0f);

            if (log_)
                log_ ("load failed: " + pathToUtf8 (loadingPath_) + ": " + result.error);

            return;
        }

        stopPlay();

        // The work on the sample that is being left is written before the new one takes its place.
        project_.flush();

        if (! engine_.loadSample (result.sample))
        {
            showToast ("Audio engine busy - try again", 3.0f);
            return;
        }

        wave_.build (result.sample);
        selected_ = 0;
        handleSlice_ = -1;
        paramRow_ = 0;
        undo_.clear();
        seqUndo_.clear();
        seqRow_ = seqCol_ = seqScroll_ = 0;
        lastEditKind_ = -1;
        engine_.setSelectedSlice (0);

        // The sample comes in with no slices and no patterns; the autosave (if it is on) and the project
        // the person opened (if one was) put them back, the project last.
        samplePath_ = loadingPath_;
        const auto opened = project_.open (samplePath_);
        const bool projectApplied = opening && kerf::applyProject (engine_, wanted);

        projectFile_ = projectApplied ? wantedFile : fs::path();
        projectBase_ = kerf::captureProject (engine_);
        projectEdited_ = false;
        projectSaveFailed_ = false;
        discardUntil_ = 0.0;
        selected_ = engine_.selectedSlice();

        if (opening && log_)
            log_ (std::string (projectApplied ? "project opened: " : "project does not fit its sample: ") + pathToUtf8 (wantedFile));

        fitAll();
        setScreen (Screen::editor);

        std::string message = note.empty() ? std::string ("Loaded. Press ") + (usingKeyboard_ ? "2" : "Y") + " to auto-detect" : note;

        switch (opened.kind)
        {
            case ProjectKeeper::Opened::Kind::restored:
                message = opened.slices == 0 ? std::string ("Project restored")
                                             : "Project restored: " + integer (opened.slices) + (opened.slices == 1 ? " slice" : " slices");
                break;

            case ProjectKeeper::Opened::Kind::mismatch: message = "Old autosave does not fit this sample"; break;
            case ProjectKeeper::Opened::Kind::damaged:  message = "Autosave file is damaged - not used"; break;
            case ProjectKeeper::Opened::Kind::newer:    message = "Autosave from a newer Kerf - not used"; break;
            case ProjectKeeper::Opened::Kind::off:
            case ProjectKeeper::Opened::Kind::fresh:    break;
        }

        if (opening)
        {
            const int count = engine_.slices().size();

            message = projectApplied ? "Opened " + elide (pathToUtf8 (wantedFile.filename()), 20) + ": " + integer (count) + (count == 1 ? " slice" : " slices")
                                     : "Project does not fit " + elide (loadingName_, 16);
        }

        showToast (message, 4.0f);
    }

    int App::detectSlices (float sensitivity)
    {
        if (! wave_.valid())
            return 0;

        pushUndo (kUndoDetect);
        stopPlay();
        const int found = engine_.detectTransients(std::clamp (sensitivity, 0.1f, 1.0f));
        engine_.sequencer().clearAll();   // new slices: the old patterns belonged to the old ones
        seqUndo_.clear();
        selected_ = 0;
        handleSlice_ = -1;
        engine_.setSelectedSlice (0);
        return found;
    }

    // -------------------------------------------------------------------------------- update

    void App::update (const Pad& pad, float dt)
    {
        dt = std::clamp (dt, 0.0f, 0.1f);
        time_ += dt;
        usingKeyboard_ = pad.usingKeyboard;
        lastPad_ = pad;

        toastTime_ = std::max (0.0f, toastTime_ - dt);

        if (host_.midiMessages != lastMidiCount_)
        {
            lastMidiCount_ = host_.midiMessages;
            midiFlash_ = 0.15f;
        }
        else
        {
            midiFlash_ = std::max (0.0f, midiFlash_ - dt);
        }

        engine_.collectGarbage();
        peakShown_ = std::max (engine_.peakLevel(), peakShown_ - dt * 1.2f);

        if (pad.quit)
            quit_ = true;

        // The way out on a handheld, which has no window to close: hold Select and Start together.
        if (pad.held[(int) Btn::Select] >= 1.0f && pad.held[(int) Btn::Start] >= 1.0f)
            quit_ = true;

        // Files dropped on the window (desktop builds): the last one wins. Not with a take in hand:
        // loading leaves the Record page, and the take would be thrown away by accident.
        if (! pad.droppedFiles.empty() && ! isLoading())
        {
            if (takeInHand())
                showToast (session_.phase() == RecordSession::Phase::recording ? "Stop the take first" : "Save or discard the take first", 2.5f);
            else if (confirmDiscard())
                loadFile (pathFromUtf8 (pad.droppedFiles.back()));
        }

        if (loading_.valid() && loading_.wait_for (std::chrono::seconds (0)) == std::future_status::ready)
            finishLoad();

        if (isLoading())
            return; // modal: the loading box swallows input

        for (auto c : pad.commands)
            if (c == KeyCommand::help && screen_ != Screen::help && screen_ != Screen::record)
                openHelp();

        switch (screen_)
        {
            case Screen::editor:    updateEditor (pad, dt); break;
            case Screen::sequencer: updateSequencer (pad, dt); break;
            case Screen::browser:   updateBrowser (pad); break;
            case Screen::detect:    updateDetect (pad); break;
            case Screen::menu:      updateMenu (menu_, pad, page_); break;
            case Screen::settings:  updateMenu (settings_, pad, Screen::menu); break;
            case Screen::help:      updateHelp (pad); break;
            case Screen::inputTest: updateInputTest (pad); break;
            case Screen::record:    updateRecord (pad, dt); break;
            case Screen::projects:  updateProjects (pad); break;
        }

        // Does the work still match its project file? (a few times a second is plenty)
        if (wave_.valid() && time_ >= editCheckAt_)
        {
            editCheckAt_ = time_ + 0.3;
            checkEdited();
        }

        // The autosave (when it is on) keeps the work as it changes (project_keeper.h); a take being recorded has the disk and the time to itself.
        project_.update (time_, ! takeInHand());

        const auto notice = project_.takeNotice();

        if (! notice.empty())
            showToast (notice, 3.5f);

        // A project that has just been made is a new mark in the library's list of samples.
        if (project_.takeCreated())
            refreshBrowserMarks();
    }

    // ------------------------------------------------------------------------------- browser

    // The library as it starts: the folder it was last in, or the samples folder.
    void App::prepareBrowser()
    {
        fs::path dir = cfg_.lastDir.empty() ? startDir_ : pathFromUtf8 (cfg_.lastDir);

        if (! browser_.open (dir) && ! browser_.open (startDir_))
            browser_.openRoots();

        browserSel_ = browser_.entries().size() > 1 ? 1 : 0;
        browserScroll_ = 0;
    }

    // The library shows a mark on the samples that have a project. When one is made while the library
    // is not on the screen, its list is out of date: read the folder again, on the same entry.
    void App::refreshBrowserMarks()
    {
        if (browser_.atRootsList() || browser_.directory().empty())
            return;

        const auto& before = browser_.entries();
        const std::string name = browserSel_ >= 0 && browserSel_ < (int) before.size() ? before[(size_t) browserSel_].name : std::string();

        if (! browser_.open (browser_.directory()))
            return;

        const int at = name.empty() ? -1 : browser_.indexOfName (name);
        const int count = (int) browser_.entries().size();

        if (at >= 0)
            browserSel_ = at;

        browserSel_ = std::clamp (browserSel_, 0, std::max (0, count - 1));
        browserScroll_ = std::clamp (browserScroll_, 0, std::max (0, count - kBrowserRows));
    }

    void App::openBrowser()
    {
        prepareBrowser();
        setScreen (Screen::browser);
    }

    void App::updateBrowser (const Pad& pad)
    {
        const int n = (int) browser_.entries().size();
        const bool haveSample = wave_.valid();

        // Select: the next page, PROJ (and from there REC again).
        if (selectTap (pad))
        {
            setScreen (Screen::projects);
            return;
        }

        if (n > 0)
        {
            if (pad.repeated[(int) Btn::Up])   browserSel_ = (browserSel_ + n - 1) % n;
            if (pad.repeated[(int) Btn::Down]) browserSel_ = (browserSel_ + 1) % n;
            if (pad.repeated[(int) Btn::L1])   browserSel_ = std::max (0, browserSel_ - kBrowserRows);
            if (pad.repeated[(int) Btn::R1])   browserSel_ = std::min (n - 1, browserSel_ + kBrowserRows);
        }

        if (pad.pressed[(int) Btn::A] && n > 0)
        {
            const bool wasParent = browser_.entries()[(size_t) browserSel_].isParent;
            const auto cameFrom = pathToUtf8 (browser_.directory().filename());
            fs::path chosen;

            switch (browser_.activate (browserSel_, chosen))
            {
                case FileBrowser::Result::navigated:
                    cfg_.lastDir = browser_.directoryUtf8();
                    browserSel_ = 0;
                    if (wasParent)
                        browserSel_ = std::max (0, browser_.indexOfName (cameFrom));
                    else if (browser_.entries().size() > 1)
                        browserSel_ = 1;
                    browserScroll_ = 0;
                    break;

                case FileBrowser::Result::chosenFile:
                    cfg_.lastDir = browser_.directoryUtf8();

                    if (confirmDiscard())
                        loadFile (chosen);

                    break;

                case FileBrowser::Result::none:
                    break;
            }
        }
        else if (pad.pressed[(int) Btn::B])
        {
            const auto cameFrom = pathToUtf8 (browser_.directory().filename());

            if (browser_.goUp())
            {
                cfg_.lastDir = browser_.directoryUtf8();
                browserSel_ = std::max (0, browser_.indexOfName (cameFrom));
                browserScroll_ = 0;
            }
            else if (haveSample)
            {
                setScreen (mainScreen_);
            }
        }
        else if (pad.pressed[(int) Btn::Start])
        {
            // Back to the sample being worked on; before there is one, the menu has the settings, the help and the way out.
            if (haveSample) setScreen (mainScreen_);
            else            openMenu();

            return;
        }

        const int count = (int) browser_.entries().size();
        browserSel_ = std::clamp (browserSel_, 0, std::max (0, count - 1));

        if (browserSel_ < browserScroll_) browserScroll_ = browserSel_;
        if (browserSel_ >= browserScroll_ + kBrowserRows) browserScroll_ = browserSel_ - kBrowserRows + 1;
        browserScroll_ = std::clamp (browserScroll_, 0, std::max (0, count - kBrowserRows));
    }

    // ------------------------------------------------------------------------------ projects

    // Projects are saved next to their samples, so the library of projects starts where the samples are.
    void App::prepareProjects()
    {
        fs::path dir = ! samplePath_.empty() ? samplePath_.parent_path() : cfg_.lastDir.empty() ? startDir_ : pathFromUtf8 (cfg_.lastDir);

        if (dir.empty() || ! projects_.open (dir))
            if (! projects_.open (startDir_))
                projects_.openRoots();

        projectsSel_ = projects_.entries().size() > 1 ? 1 : 0;
        projectsScroll_ = 0;
        projectsMoved_ = false;
    }

    // When a project has been saved the library's list is out of date: read the folder again, on the same entry
    // (or, if nobody has moved around in it, start again from the folder of the sample and look for the project in hand).
    void App::refreshProjects()
    {
        if (! projectsMoved_)
        {
            prepareProjects();

            if (! projectFile_.empty())
            {
                const int at = projects_.indexOfName (pathToUtf8 (projectFile_.filename()));

                if (at >= 0)
                    projectsSel_ = at;
            }

            projectsScroll_ = std::clamp (projectsSel_ - kBrowserRows + 1, 0, std::max (0, (int) projects_.entries().size() - kBrowserRows));
            return;
        }

        if (projects_.atRootsList() || projects_.directory().empty())
            return;

        const auto& before = projects_.entries();
        const std::string name = projectsSel_ >= 0 && projectsSel_ < (int) before.size() ? before[(size_t) projectsSel_].name : std::string();

        if (! projects_.open (projects_.directory()))
            return;

        const int at = name.empty() ? -1 : projects_.indexOfName (name);
        const int count = (int) projects_.entries().size();

        if (at >= 0)
            projectsSel_ = at;

        projectsSel_ = std::clamp (projectsSel_, 0, std::max (0, count - 1));
        projectsScroll_ = std::clamp (projectsScroll_, 0, std::max (0, count - kBrowserRows));
    }

    void App::openProjects()
    {
        setScreen (Screen::projects);
    }

    void App::updateProjects (const Pad& pad)
    {
        const int n = (int) projects_.entries().size();
        const bool haveSample = wave_.valid();

        // Select: the next page, REC again.
        if (selectTap (pad))
        {
            setScreen (Screen::record);
            return;
        }

        if (n > 0)
        {
            if (pad.repeated[(int) Btn::Up])   { projectsSel_ = (projectsSel_ + n - 1) % n; projectsMoved_ = true; }
            if (pad.repeated[(int) Btn::Down]) { projectsSel_ = (projectsSel_ + 1) % n; projectsMoved_ = true; }
            if (pad.repeated[(int) Btn::L1])   { projectsSel_ = std::max (0, projectsSel_ - kBrowserRows); projectsMoved_ = true; }
            if (pad.repeated[(int) Btn::R1])   { projectsSel_ = std::min (n - 1, projectsSel_ + kBrowserRows); projectsMoved_ = true; }
        }

        if (pad.pressed[(int) Btn::A] && n > 0)
        {
            const bool wasParent = projects_.entries()[(size_t) projectsSel_].isParent;
            const auto cameFrom = pathToUtf8 (projects_.directory().filename());
            fs::path chosen;
            projectsMoved_ = true;

            switch (projects_.activate (projectsSel_, chosen))
            {
                case FileBrowser::Result::navigated:
                    projectsSel_ = 0;
                    if (wasParent)
                        projectsSel_ = std::max (0, projects_.indexOfName (cameFrom));
                    else if (projects_.entries().size() > 1)
                        projectsSel_ = 1;
                    projectsScroll_ = 0;
                    break;

                case FileBrowser::Result::chosenFile:
                    if (confirmDiscard())
                        openProjectFile (chosen);

                    break;

                case FileBrowser::Result::none:
                    break;
            }
        }
        else if (pad.pressed[(int) Btn::B])
        {
            const auto cameFrom = pathToUtf8 (projects_.directory().filename());
            projectsMoved_ = true;

            if (projects_.goUp())
            {
                projectsSel_ = std::max (0, projects_.indexOfName (cameFrom));
                projectsScroll_ = 0;
            }
            else if (haveSample)
            {
                setScreen (mainScreen_);
            }
        }
        else if (pad.pressed[(int) Btn::Start])
        {
            if (haveSample) setScreen (mainScreen_);
            else            openMenu();

            return;
        }

        const int count = (int) projects_.entries().size();
        projectsSel_ = std::clamp (projectsSel_, 0, std::max (0, count - 1));

        if (projectsSel_ < projectsScroll_) projectsScroll_ = projectsSel_;
        if (projectsSel_ >= projectsScroll_ + kBrowserRows) projectsScroll_ = projectsSel_ - kBrowserRows + 1;
        projectsScroll_ = std::clamp (projectsScroll_, 0, std::max (0, count - kBrowserRows));
    }

    // The next name for a project of the sample in hand, in the folder of the sample: "kick.kerf", then
    // "kick 2.kerf" ... A name that is taken is never used: saving a new project never replaces another one.
    fs::path App::freeProjectFile() const
    {
        const fs::path dir = samplePath_.parent_path();
        const std::string stem = pathToUtf8 (samplePath_.stem());
        std::error_code ec;

        for (int n = 1; n < 1000; ++n)
        {
            const std::string name = n == 1 ? stem + ".kerf" : stem + " " + std::to_string (n) + ".kerf";
            const fs::path file = dir / pathFromUtf8 (name);

            if (! fs::exists (file, ec))
                return file;
        }

        return {};
    }

    // "Save project": the work in hand goes to its project file; with none yet (or "as new") to a new one next to the sample.
    void App::saveProject (bool asNew)
    {
        if (! wave_.valid() || samplePath_.empty())
        {
            showToast ("Load a sample first");
            return;
        }

        const fs::path file = (asNew || projectFile_.empty()) ? freeProjectFile() : projectFile_;
        const kerf::Project work = kerf::captureProject (engine_);
        std::string text, error;

        if (file.empty())
        {
            error = "no free file name";
        }
        else if (! kerf::serializeProject (work, text))
        {
            error = "too many slices";
        }
        else
        {
            // What the file held before this save stays in name.kerf.bak.
            std::error_code ec;
            std::string ignored;

            if (fs::exists (file, ec))
                ProjectStore::backUp (file, ignored);

            if (ProjectStore::write (file, text, error))
                error.clear();
            else if (error.empty())
                error = "the file could not be written";
        }

        if (! error.empty())
        {
            projectSaveFailed_ = true;
            showToast ("Could not save: " + error, 5.0f);

            if (log_)
                log_ ("project save failed: " + pathToUtf8 (file) + ": " + error);

            return;
        }

        projectFile_ = file;
        projectBase_ = work;
        projectEdited_ = false;
        projectSaveFailed_ = false;
        discardUntil_ = 0.0;
        refreshProjects();

        const int count = (int) work.slices.size();
        showToast ("Saved " + elide (pathToUtf8 (file.filename()), 20) + " (" + integer (count) + (count == 1 ? " slice)" : " slices)"), 3.0f);

        if (log_)
            log_ ("project saved: " + pathToUtf8 (file));
    }

    // Opens a project file: its sample is found (next to it) and loaded, and the project goes in once it has.
    void App::openProjectFile (const fs::path& file)
    {
        if (isLoading())
            return;

        const auto read = ProjectStore::read (file);
        const std::string name = elide (pathToUtf8 (file.filename()), 22);

        if (read.kind != ProjectStore::Read::Kind::ok)
        {
            const char* why = read.kind == ProjectStore::Read::Kind::newer ? "from a newer Kerf"
                            : read.kind == ProjectStore::Read::Kind::missing ? "not found" : "not a Kerf project";
            showToast (name + ": " + why, 4.0f);

            if (log_)
                log_ ("project not opened: " + pathToUtf8 (file) + ": " + why + (read.error.empty() ? std::string() : " (" + read.error + ")"));

            return;
        }

        // The project names its sample; the sample is looked for beside the project first.
        const fs::path audioName = pathFromUtf8 (read.project.audio).filename();
        fs::path audio;
        std::error_code ec;

        if (! audioName.empty())
        {
            const fs::path folders[] = { file.parent_path(), startDir_, samplePath_.parent_path() };

            for (const auto& dir : folders)
            {
                const fs::path candidate = dir / audioName;

                if (fs::is_regular_file (candidate, ec))
                {
                    audio = candidate;
                    break;
                }
            }
        }

        if (audio.empty())
        {
            showToast ("Sample not found: " + elide (pathToUtf8 (audioName), 22), 4.0f);

            if (log_)
                log_ ("project not opened: " + pathToUtf8 (file) + ": its sample " + pathToUtf8 (audioName) + " is not there");

            return;
        }

        pendingProject_ = read.project;
        pendingProjectFile_ = file;
        pendingProjectSet_ = true;
        loadFile (audio);
    }

    void App::checkEdited()
    {
        projectEdited_ = wave_.valid() && ! kerf::sameWork (kerf::captureProject (engine_), projectBase_);
    }

    // Loading something else (or leaving Kerf) would drop work that is not saved: the first time, Kerf says so
    // and does nothing; the same again a moment later goes through. With the autosave on the work is kept anyway.
    bool App::confirmDiscard()
    {
        if (cfg_.projects || ! wave_.valid())
            return true;

        checkEdited();

        if (! projectEdited_)
            return true;

        if (time_ < discardUntil_)
        {
            discardUntil_ = 0.0;
            return true;
        }

        discardUntil_ = time_ + 8.0;
        showToast ("Unsaved changes - repeat to discard", 4.0f);
        return false;
    }

    App::WorkTag App::workTag() const
    {
        if (! wave_.valid())
            return WorkTag::none;

        if (projectSaveFailed_)
            return WorkTag::problem;

        if (! projectFile_.empty())
            return projectEdited_ ? WorkTag::edited : WorkTag::saved;

        return projectEdited_ ? WorkTag::unsaved : WorkTag::none;
    }

    std::string App::projectMenuValue() const
    {
        switch (workTag())
        {
            case WorkTag::saved:    return "SAVED";
            case WorkTag::edited:   return "EDITED";
            case WorkTag::unsaved:  return "UNSAVED";
            case WorkTag::problem:  return "NO SAVE";
            case WorkTag::none:     break;
        }

        return {};
    }

    // ------------------------------------------------------------------------- detect dialog

    void App::updateDetect (const Pad& pad)
    {
        if (pad.repeated[(int) Btn::Left])  detectSlider_ = std::max (0.1f, std::round ((detectSlider_ - 0.05f) * 20.0f) / 20.0f);
        if (pad.repeated[(int) Btn::Right]) detectSlider_ = std::min (1.0f, std::round ((detectSlider_ + 0.05f) * 20.0f) / 20.0f);

        if (pad.pressed[(int) Btn::A])
        {
            const int found = detectSlices (detectSlider_);
            setScreen (Screen::editor);

            if (found > 0)
            {
                fitAll();
                showToast (integer (found) + " slices found");
            }
            else
            {
                showToast ("No slices found - try a higher sensitivity", 3.5f);
            }
        }
        else if (pad.pressed[(int) Btn::B] || pad.pressed[(int) Btn::Start])
        {
            setScreen (Screen::editor);
        }
    }

    // -------------------------------------------------------------------------------- menus

    void App::openMenu()
    {
        buildMenu();
        menu_.sel = 0;
        menu_.scroll = 0;
        setScreen (Screen::menu);
    }

    void App::openSettings()
    {
        buildSettings();
        settings_.sel = 0;
        settings_.scroll = 0;
        setScreen (Screen::settings);
    }

    void App::buildMenu()
    {
        if (isHomePage (page_))                    buildHomeMenu();
        else if (mainScreen_ == Screen::sequencer) buildSequencerMenu();
        else                                       buildSliceMenu();
    }

    // The menu of REC and LOAD while there is no sample yet (with one, Start leads back to it).
    void App::buildHomeMenu()
    {
        menu_.title = "MENU";
        menu_.footer = "A select    B/Start close";
        menu_.items.clear();

        menu_.items.push_back ({ "Load project...", nullptr, nullptr, [this] { openProjects(); } });
        menu_.items.push_back ({ "Settings...", nullptr, nullptr, [this] { openSettings(); } });
        menu_.items.push_back ({ "Controls / help", nullptr, nullptr, [this] { openHelp(); } });
        menu_.items.push_back ({ "Input test", nullptr, nullptr, [this] { setScreen (Screen::inputTest); } });
        menu_.items.push_back ({ "Quit", nullptr, nullptr, [this] { if (confirmDiscard()) { saveSettings(); quit_ = true; } } });
    }

    void App::buildSliceMenu()
    {
        menu_.title = "MENU";
        menu_.footer = projectFile_.empty() ? std::string ("A select    B/Start close")
                                            : "Project: " + elide (pathToUtf8 (projectFile_.filename()), 30);
        menu_.items.clear();

        auto needSample = [this] (std::function<void()> action)
        {
            return [this, action] {
                if (! wave_.valid()) { showToast ("Load a sample first"); setScreen (Screen::editor); return; }
                action();
            };
        };

        menu_.items.push_back ({ "Save project", [this] { return projectMenuValue(); }, nullptr,
                                 needSample ([this] { saveProject (false); setScreen (Screen::editor); }) });
        menu_.items.push_back ({ "Save as new project", nullptr, nullptr,
                                 needSample ([this] { saveProject (true); setScreen (Screen::editor); }) });
        menu_.items.push_back ({ "Load project...", nullptr, nullptr, [this] { openProjects(); } });
        menu_.items.push_back ({ "Load sample...", nullptr, nullptr, [this] { openBrowser(); } });
        menu_.items.push_back ({ "Record sample...", nullptr, nullptr, [this] { openRecord(); } });
        menu_.items.push_back ({ "Auto-detect slices...", nullptr, nullptr, needSample ([this] { setScreen (Screen::detect); }) });
        menu_.items.push_back ({ "Add slice", nullptr, nullptr, needSample ([this] { setScreen (Screen::editor); addSlice(); }) });
        menu_.items.push_back ({ "Delete slice", nullptr, nullptr, needSample ([this] { setScreen (Screen::editor); deleteSelected(); }) });
        menu_.items.push_back ({ "Clear all slices", nullptr, nullptr, needSample ([this] { setScreen (Screen::editor); clearSlices(); }) });
        menu_.items.push_back ({ "Undo", nullptr, nullptr, [this] { setScreen (Screen::editor); undo(); } });
        menu_.items.push_back ({ "Zoom",
            [this] { return std::string (! wave_.valid() ? "-" : viewShowsAll() ? "ALL" : "SLICE"); },
            [this] (int) { toggleView(); },
            needSample ([this] { setScreen (Screen::editor); toggleView(); }) });
        menu_.items.push_back ({ "Go to sequencer", nullptr, nullptr, needSample ([this] { cyclePage (+1); }) });
        menu_.items.push_back ({ "Settings...", nullptr, nullptr, [this] { openSettings(); } });
        menu_.items.push_back ({ "Controls / help", nullptr, nullptr, [this] { openHelp(); } });
        menu_.items.push_back ({ "Input test", nullptr, nullptr, [this] { setScreen (Screen::inputTest); } });
        menu_.items.push_back ({ "Quit", nullptr, nullptr, [this] { if (confirmDiscard()) { saveSettings(); quit_ = true; } } });
    }

    void App::buildSequencerMenu()
    {
        menu_.title = "SEQUENCER";
        menu_.footer = projectFile_.empty() ? std::string ("A select  Left/Right change  B close")
                                            : "Project: " + elide (pathToUtf8 (projectFile_.filename()), 30);
        menu_.items.clear();

        auto& seq = engine_.sequencer();

        menu_.items.push_back ({ "Play / stop",
            [&seq] { return std::string (seq.isPlaying() ? "PLAYING" : "STOPPED"); }, nullptr,
            [this] { toggleTransport(); setScreen (Screen::sequencer); } });

        menu_.items.push_back ({ "Tempo",
            [&seq] { return fixed ((double) seq.tempo(), 1) + " BPM"; },
            [this] (int d) { seqChangeTempo (d); }, nullptr });

        menu_.items.push_back ({ "Swing",
            [&seq] { return integer (std::lround (seq.swing() * 100.0f)) + "%"; },
            [this] (int d) { seqChangeSwing (d); }, nullptr });

        menu_.items.push_back ({ "Length",
            [&seq] { return integer (seq.length()) + " steps"; },
            [this] (int d) { seqChangeLength (d); }, nullptr });

        menu_.items.push_back ({ "Scene",
            [&seq] { return integer (seq.scene() + 1) + " of " + integer (kerf::StepSequencer::kScenes); },
            [this] (int d) { seqChangeScene (d); }, nullptr });

        menu_.items.push_back ({ "Copy scene to next", nullptr, nullptr, [this] { seqCopySceneToNext(); setScreen (Screen::sequencer); } });
        menu_.items.push_back ({ "Clear row", nullptr, nullptr, [this] { seqClearRow(); setScreen (Screen::sequencer); } });
        menu_.items.push_back ({ "Clear scene", nullptr, nullptr, [this] { seqClearScene(); setScreen (Screen::sequencer); } });
        menu_.items.push_back ({ "Undo", nullptr, nullptr, [this] { seqUndo(); setScreen (Screen::sequencer); } });
        menu_.items.push_back ({ "Save project", [this] { return projectMenuValue(); }, nullptr,
                                 [this] { saveProject (false); setScreen (Screen::sequencer); } });
        menu_.items.push_back ({ "Save as new project", nullptr, nullptr, [this] { saveProject (true); setScreen (Screen::sequencer); } });
        menu_.items.push_back ({ "Load project...", nullptr, nullptr, [this] { openProjects(); } });
        menu_.items.push_back ({ "Go to slicer", nullptr, nullptr, [this] { cyclePage (+1); } });
        menu_.items.push_back ({ "Settings...", nullptr, nullptr, [this] { openSettings(); } });
        menu_.items.push_back ({ "Controls / help", nullptr, nullptr, [this] { openHelp(); } });
        menu_.items.push_back ({ "Quit", nullptr, nullptr, [this] { if (confirmDiscard()) { saveSettings(); quit_ = true; } } });
    }

    void App::buildSettings()
    {
        settings_.title = "SETTINGS";
        settings_.footer = "Left/Right change    B back";
        settings_.items.clear();

        settings_.items.push_back ({ "Master volume",
            [this] { return integer ((long long) std::lround (cfg_.masterVolume * 100.0f)) + "%"; },
            [this] (int d) {
                cfg_.masterVolume = std::clamp (std::round ((cfg_.masterVolume + 0.05f * (float) d) * 20.0f) / 20.0f, 0.0f, 1.0f);
                engine_.setMasterVolume (cfg_.masterVolume);
            }, nullptr });

        settings_.items.push_back ({ "MIDI mode",
            [this] { return std::string (cfg_.midiMode == 1 ? "Sampler" : "Trigger"); },
            [this] (int) {
                cfg_.midiMode = cfg_.midiMode == 1 ? 0 : 1;
                engine_.setMidiMode (cfg_.midiMode == 1 ? kerf::VoiceEngine::MidiMode::sampler : kerf::VoiceEngine::MidiMode::trigger);
            }, nullptr });

        settings_.items.push_back ({ "Slice 1 = note",
            [this] { return noteName (cfg_.noteOffset) + " (" + integer (cfg_.noteOffset) + ")"; },
            [this] (int d) {
                cfg_.noteOffset = std::clamp (cfg_.noteOffset + d, 0, 127);
                engine_.setNoteOffset (cfg_.noteOffset);
            }, nullptr });

        settings_.items.push_back ({ "Stick speed",
            [this] { return integer ((long long) std::lround (cfg_.stickSpeed * 100.0f)) + "%"; },
            [this] (int d) { cfg_.stickSpeed = std::clamp (std::round ((cfg_.stickSpeed + 0.05f * (float) d) * 20.0f) / 20.0f, 0.1f, 2.0f); },
            nullptr });

        settings_.items.push_back ({ "Stick dead zone",
            [this] { return integer ((long long) std::lround (cfg_.deadzone * 100.0f)) + "%"; },
            [this] (int d) { cfg_.deadzone = std::clamp (std::round ((cfg_.deadzone + 0.01f * (float) d) * 100.0f) / 100.0f, 0.05f, 0.6f); },
            nullptr });

        settings_.items.push_back ({ "Swap A / B",
            [this] { return std::string (cfg_.swapAB ? "on" : "off"); },
            [this] (int) { cfg_.swapAB = ! cfg_.swapAB; }, nullptr });

        settings_.items.push_back ({ "Swap X / Y",
            [this] { return std::string (cfg_.swapXY ? "on" : "off"); },
            [this] (int) { cfg_.swapXY = ! cfg_.swapXY; }, nullptr });

        settings_.items.push_back ({ "Record limit",
            [this] { return limitText (cfg_.recordLimit); },
            [this] (int d) {
                static const int limits[] = { 30, 60, 120, 180, 300 };
                int i = 0;
                for (int k = 0; k < 5; ++k) if (limits[k] <= cfg_.recordLimit) i = k;
                cfg_.recordLimit = limits[std::clamp (i + d, 0, 4)];
            }, nullptr });

        settings_.items.push_back ({ "Autosave",
            [this] { return std::string (cfg_.projects ? "on" : "off"); },
            [this] (int) {
                cfg_.projects = ! cfg_.projects;
                project_.setEnabled (cfg_.projects);

                // Turning it off stops the saving at once; turning it on does not reach a sample that was opened without it.
                showToast (! cfg_.projects ? "Autosave off"
                                           : project_.tag() == ProjectKeeper::Tag::off ? "Autosave on from the next sample" : "Autosave on", 3.5f);
            }, nullptr });

        settings_.items.push_back ({ "Buffer (restart)",
            [this] { return integer (cfg_.audioBuffer); },
            [this] (int d) {
                static const int sizes[] = { 128, 256, 512, 1024, 2048 };
                int i = 0;
                for (int k = 0; k < 5; ++k) if (sizes[k] == cfg_.audioBuffer) i = k;
                cfg_.audioBuffer = sizes[std::clamp (i + d, 0, 4)];
            }, nullptr });

        settings_.items.push_back ({ "Back", nullptr, nullptr, [this] { saveSettings(); setScreen (Screen::menu); } });
    }

    void App::updateMenu (ListMenu& m, const Pad& pad, Screen backTo)
    {
        const int n = (int) m.items.size();
        if (n == 0)
            return;

        if (pad.repeated[(int) Btn::Up])   m.sel = (m.sel + n - 1) % n;
        if (pad.repeated[(int) Btn::Down]) m.sel = (m.sel + 1) % n;

        const Screen before = screen_;
        auto& item = m.items[(size_t) m.sel];

        if (item.adjust)
        {
            if (pad.repeated[(int) Btn::Left])  item.adjust (-1);
            if (pad.repeated[(int) Btn::Right]) item.adjust (+1);
        }

        if (pad.pressed[(int) Btn::A] && item.activate)
            item.activate(); // may switch screens or rebuild the menu: do not touch `item` afterwards

        if (screen_ != before)
            return;

        if (pad.pressed[(int) Btn::B] || pad.pressed[(int) Btn::Start])
        {
            if (&m == &settings_)
                saveSettings();

            setScreen (backTo);
            return;
        }

        if (m.sel < m.scroll) m.scroll = m.sel;
        if (m.sel >= m.scroll + kMenuRows) m.scroll = m.sel - kMenuRows + 1;
    }

    void App::updateHelp (const Pad& pad)
    {
        // Six pages: gamepad / keyboard, each for the slicer, the sequencer and REC / LOAD.
        if (pad.pressed[(int) Btn::Left] || pad.pressed[(int) Btn::L1])  helpPage_ = (helpPage_ + kHelpPages - 1) % kHelpPages;
        if (pad.pressed[(int) Btn::Right] || pad.pressed[(int) Btn::R1]) helpPage_ = (helpPage_ + 1) % kHelpPages;

        if (pad.pressed[(int) Btn::B] || pad.pressed[(int) Btn::Start] || pad.pressed[(int) Btn::A])
            setScreen (page_);
    }

    void App::updateInputTest (const Pad& pad)
    {
        // B on the test screen leaves it; everything else is just displayed.
        if (pad.pressed[(int) Btn::B] || pad.held[(int) Btn::Start] > 0.8f)
            setScreen (Screen::menu);
    }
}
