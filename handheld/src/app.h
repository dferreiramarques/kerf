#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

#include "audio_input.h"
#include "canvas.h"
#include "config.h"
#include "engine.h"
#include "file_browser.h"
#include "input.h"
#include "project_keeper.h"
#include "record_session.h"
#include "waveform_cache.h"

namespace kerf::ui
{
    constexpr int kScreenW = 640;
    constexpr int kScreenH = 480;
    constexpr int kMenuRows = 10; // rows a menu shows before it starts to scroll

    // Kerf has two sets of pages and Select goes round the pages of the set you are in:
    //  - `record`, `browser` and `projects` (REC, LOAD and PROJ) get a sample or a project: Kerf
    //    starts on `record`; `browser` (the library of samples) and `projects` (the library of
    //    projects) are the pages behind it, one tap of Select apart: REC > LOAD > PROJ > REC;
    //  - `editor` (the slicer) and `sequencer` (SLICER and SEQ) work on a sample once there is one.
    // Everything else is a dialog or a utility screen that returns to the page it came from.
    enum class Screen { editor, browser, detect, menu, settings, help, inputTest, sequencer, record, projects };

    // What the SDL layer knows about the attached controller (shown on the "Input test" screen).
    struct InputDebug
    {
        std::string deviceName;            // empty = no gamepad attached
        bool isController = false;         // true: SDL has a button mapping; false: raw joystick fallback
        int axes = 0, buttons = 0, hats = 0;
        float axis[8] = {};
        unsigned long long buttonMask = 0; // raw joystick buttons currently down
        int lastButton = -1;               // last raw button number pressed
        std::string lastControllerButton;  // SDL's name for it, e.g. "a" or "dpup"
    };

    // What the host (window, audio, MIDI) tells the app about itself.
    struct HostStatus
    {
        std::string audioInfo;             // e.g. "44100 Hz  512"
        bool audioOk = true;
        int midiDevices = 0;                 // every MIDI input being listened to
        int midiUsbDirect = 0;               // of which Kerf reads itself because the system has no driver for them
        unsigned long long midiMessages = 0; // running counter: the MIDI light flashes when it changes
        uint32_t midiLast = 0;               // the latest message: status << 16 | data1 << 8 | data2, 0 = none yet
    };

    struct MenuItem
    {
        std::string label;
        std::function<std::string()> value;   // shown on the right, optional
        std::function<void (int)> adjust;     // Left/Right, optional
        std::function<void()> activate;       // A, optional
    };

    struct ListMenu
    {
        std::string title;
        std::vector<MenuItem> items;
        int sel = 0;
        int scroll = 0;
        std::string footer;
    };

    // The whole handheld application: screens, editing logic and drawing. It talks to the audio
    // engine only through kerf::Engine's thread-safe API and renders into a Canvas, so it runs
    // identically in a window, on the device and headless in tests.
    class App
    {
    public:
        App (kerf::Engine& engine, Config& config, std::filesystem::path configFile, std::filesystem::path startDir);
        ~App();

        void update (const Pad& pad, float dt);
        void draw (Canvas& canvas);

        bool wantsQuit() const noexcept { return quit_; }

        // Loads a sample in the background and shows a "Loading" box until it is ready. `doneNote`
        // is what the toast says when it has loaded (the default asks to auto-detect).
        void loadFile (const std::filesystem::path& file, std::string doneNote = {});
        bool isLoading() const;
        void waitForLoad(); // for scripts and tests: blocks until the load finished and was applied

        // Same as the dialog's "detect" button. Returns the number of slices found.
        int detectSlices (float sensitivity);

        void setHostStatus (const HostStatus& s) { host_ = s; }

        // The inputs the Record screen can listen to (the host owns the hub and keeps it alive for as
        // long as the app exists). Without one, the screen says that there is no input. Kerf starts on
        // the Record page, which opens its input as soon as there is a hub to open it on.
        void setInputHub (InputHub* hub);
        void setInputDebug (const InputDebug& d) { debug_ = d; }
        void saveSettings();

        // Where the application tells what it does with projects (the host's log); optional.
        void setLogger (std::function<void (const std::string&)> logger) { log_ = logger; project_.setLogger (std::move (logger)); }

        // ---- inspection (host and tests) ----
        Screen screen() const noexcept { return screen_; }
        Screen mainScreen() const noexcept { return mainScreen_; } // the last of SLICER / SEQ: where the work goes on
        Screen page() const noexcept { return page_; }             // the last of REC / LOAD / SLICER / SEQ: where menus and help lead back to
        int seqRow() const noexcept { return seqRow_; }            // sequencer cursor: slice index ...
        int seqCol() const noexcept { return seqCol_; }            // ... and step (0-based)
        bool seqCanUndo() const noexcept { return ! seqUndo_.empty(); }
        int selectedSlice() const noexcept { return selected_; }
        double viewStart() const noexcept { return viewStart_; }
        double viewEnd() const noexcept { return viewEnd_; }
        int paramRow() const noexcept { return paramRow_; }
        int helpPageIndex() const noexcept { return helpPage_; }   // which of the pages of the controls help is (or was last) up
        const std::string& toast() const noexcept { return toast_; }
        bool canUndo() const noexcept { return ! undo_.empty(); }
        const WaveformCache& waveform() const noexcept { return wave_; }
        const RecordSession& recording() const noexcept { return session_; }
        const FileBrowser& browser() const noexcept { return browser_; }
        int browserSelection() const noexcept { return browserSel_; }                 // the row the library is on
        const std::filesystem::path& lastSaved() const noexcept { return lastSaved_; }   // the latest take written to disk
        const std::filesystem::path& saveDirectory() const noexcept { return startDir_; }
        const ProjectKeeper& project() const noexcept { return project_; }           // the autosave of the sample being worked on
        const FileBrowser& projectLibrary() const noexcept { return projects_; }
        int projectLibrarySelection() const noexcept { return projectsSel_; }
        const std::filesystem::path& projectFile() const noexcept { return projectFile_; }   // the project file of the work in hand ("" = none yet)

        // How the work in hand stands against its project file (the header says it; the Save menu entry too).
        enum class WorkTag { none, saved, edited, unsaved, problem };
        WorkTag workTag() const;

        // Same as the Start menu's "Save project" / "Save as new project" and the project library's "open".
        void saveProject (bool asNew);
        void openProjectFile (const std::filesystem::path& file);
        const std::filesystem::path& samplePath() const noexcept { return samplePath_; }   // the file it was loaded from

        static constexpr int paramRows = 9;
        enum Param { pStart, pEnd, pPitch, pVolume, pPan, pMode, pFadeIn, pFadeOut, pEnabled };

    private:
        // The kinds of slice edit that renumber the slices (and so move the patterns). The other
        // kinds (handle moves, parameters) are small numbers and merge while they follow each other.
        enum UndoKind { kUndoDetect = 103, kUndoAdd = 104, kUndoDelete = 105, kUndoClear = 106 };

        struct Undo
        {
            std::vector<kerf::Slice> slices;
            int selected = 0;

            // A pattern row belongs to its slice, so undoing an edit that renumbers the slices has
            // to move the patterns back too:
            //  - detect and clear-all replace every slice: all the patterns come back (`grid`);
            //  - add: the new slice's row goes again (`addedRow`);
            //  - delete: the slice's row comes back as it was (`removedRow`, `removedBits`).
            // Whatever was programmed on the other rows in the meantime is left alone.
            std::shared_ptr<const kerf::StepSequencer::Grid> grid;
            int addedRow = -1;
            int removedRow = -1;
            uint16_t removedBits[kerf::StepSequencer::kScenes] = {};
        };

        // One undo step on the sequencer page: the rows of one scene as they were.
        struct SeqUndo
        {
            int scene = 0;
            std::vector<uint16_t> rows;
        };

        // screens (app.cpp)
        void setScreen (Screen s);
        void updateBrowser (const Pad& pad);
        void updateDetect (const Pad& pad);
        void updateMenu (ListMenu& menu, const Pad& pad, Screen backTo);
        void updateHelp (const Pad& pad);
        void updateInputTest (const Pad& pad);
        void openMenu();
        void openSettings();
        void openBrowser();          // the library, at the folder it was last in
        void prepareBrowser();       // ... without showing it
        void refreshBrowserMarks();  // reads the folder the library shows again (a project was made), keeping the selection
        void updateProjects (const Pad& pad);
        void openProjects();         // the library of projects
        void prepareProjects();      // ... set to the folder of the samples, without showing it
        void refreshProjects();      // reads the folder it shows again (a project was saved), keeping the selection
        std::filesystem::path freeProjectFile() const;   // the next project name nobody uses, next to the sample
        bool confirmDiscard();       // unsaved work is about to be lost: the first time it says so, the second time (soon after) it lets go
        void checkEdited();          // does the work differ from what was saved?
        std::string projectMenuValue() const;
        void openRecord();
        void buildMenu();
        void buildSliceMenu();
        void buildSequencerMenu();
        void buildHomeMenu();
        void buildSettings();
        void finishLoad();
        void showToast (std::string text, float seconds = 2.5f);

        // pages (app.cpp)
        void cyclePage (int direction);
        bool selectTap (const Pad& pad);                      // REC / LOAD: a tap of Select, seen when the button is let go
        Screen workOrLibrary() const { return wave_.valid() ? mainScreen_ : Screen::browser; }   // where leaving REC leads
        static bool isHomePage (Screen s) noexcept { return s == Screen::record || s == Screen::browser || s == Screen::projects; }
        void openHelp();

        // editing (app_editor.cpp)
        void updateEditor (const Pad& pad, float dt);
        void handleSticks (const Pad& pad, float dt);
        void handleDpad (const Pad& pad);
        void adjustParam (int row, int dir, int steps, const Pad& pad);
        void selectSlice (int index);
        void addSlice();
        void deleteSelected();
        void clearSlices();
        void undo();
        void pushUndo (int kind);
        void mutateSelected (const std::function<void (kerf::Slice&)>& fn);
        void moveHandle (bool start, double delta);
        void zoomAbout (double anchor, double factor);
        void fitAll();
        void fitSelected();
        void toggleView();
        bool viewShowsAll() const;   // the whole sample is in view (as opposed to a close-up of the slice)
        void ensureVisible (double t);
        void clampView();
        void syncHandles();
        void startPlay();
        void stopPlay();
        int pianoNote (int slice) const;

        // record screen (app_record.cpp)
        void enterRecord();
        void leaveRecord();
        bool takeInHand() const;      // a take is being recorded, or waits to be saved or thrown away
        void updateRecord (const Pad& pad, float dt);
        void saveTake (bool thenLoad);
        void drawRecord (Canvas& c);
        void drawRecordRows (Canvas& c);
        void drawRecordMeters (Canvas& c);
        void drawRecordTake (Canvas& c);
        void drawRecordHints (Canvas& c);

        // sequencer page (app_sequencer.cpp)
        void updateSequencer (const Pad& pad, float dt);
        void enterSequencer();
        void leaveSequencer();
        void toggleTransport();
        void seqMoveCursor (int dRow, int dCol, bool paint);
        void seqSetCell (int row, int col, bool on);
        void seqChangeScene (int direction);
        void seqChangeLength (int direction);
        void seqChangeTempo (int direction);
        void seqChangeSwing (int direction);
        void seqCopySceneToNext();
        void seqClearRow();
        void seqClearScene();
        void seqPushUndo (int scene);
        void seqUndo();
        void seqJog (const Pad& pad, float dt);
        void seqAudition (int row, bool start);
        void clampSeqCursor();

        // drawing (app_draw.cpp, sequencer page in app_sequencer.cpp)
        void drawEditor (Canvas& c);
        void drawSequencer (Canvas& c);
        void drawSeqStrip (Canvas& c);
        void drawSeqGrid (Canvas& c);
        void drawSeqHints (Canvas& c);
        void drawPage (Canvas& c, Screen page);
        void drawHeader (Canvas& c);
        void drawHomeHeader (Canvas& c, Screen page);
        void drawWaveform (Canvas& c);
        void drawSliceList (Canvas& c);
        void drawParams (Canvas& c);
        void drawHints (Canvas& c);
        void drawToast (Canvas& c);
        void drawBrowser (Canvas& c);
        void drawLibrary (Canvas& c, Screen page);   // LOAD (samples) and PROJ (projects)
        void drawDetect (Canvas& c);
        void drawMenu (Canvas& c, const ListMenu& menu);
        void drawHelp (Canvas& c);
        void drawInputTest (Canvas& c);
        void drawLoading (Canvas& c);
        void drawDialog (Canvas& c, int x, int y, int w, int h, const std::string& title);
        std::string paramLabel (int row) const;
        std::string paramValue (int row, const kerf::Slice& s) const;
        double timeToX (double t, int x0, int width) const;

        kerf::Engine& engine_;
        Config& cfg_;
        std::filesystem::path configFile_;
        std::filesystem::path startDir_;

        Screen screen_ = Screen::editor;
        Screen mainScreen_ = Screen::editor; // the work page last shown (editor or sequencer)
        Screen page_ = Screen::editor;       // the page last shown (record, browser, editor or sequencer): where menus lead back to
        bool quit_ = false;
        double time_ = 0.0;
        bool usingKeyboard_ = true;

        // sample + view
        WaveformCache wave_;
        double viewStart_ = 0.0, viewEnd_ = 1.0;

        // editor
        int selected_ = 0;
        int paramRow_ = 0;
        double handleStart_ = 0.0, handleEnd_ = 0.0;   // double-precision shadows of the selected slice's handles
        int handleSlice_ = -1;
        bool playing_ = false;
        int playingSlice_ = -1;
        bool selectChord_ = false;
        bool selectArmed_ = false;                     // Select went down on this page (REC / LOAD): a tap flips to the other one
        float retriggerTimer_ = 0.0f;
        bool needRetrigger_ = false;
        std::vector<Undo> undo_;
        double lastEditTime_ = -100.0;
        int lastEditKind_ = -1;
        int pianoNotes_[16] = {};                      // note currently sounding per piano key (-1 = none)

        // sequencer page
        int seqRow_ = 0, seqCol_ = 0;                  // cursor: slice index, step
        int seqScroll_ = 0;                            // first visible row
        bool seqPainting_ = false;                     // A or X is held: moving the cursor sets cells to seqPaintValue_
        bool seqPaintValue_ = false;
        bool seqHearing_ = false;                      // Y is held: the row's slice is being auditioned
        int auditionRow_ = -1;
        float auditionTimer_ = 0.0f;                   // a short audition (a step switched on) stops by itself
        float tempoJog_ = 120.0f, swingJog_ = 0.5f;    // un-rounded values while a stick moves them
        std::vector<SeqUndo> seqUndo_;

        // browser
        FileBrowser browser_;
        int browserSel_ = 0, browserScroll_ = 0;
        std::string browserError_;

        // the library of projects (PROJ)
        FileBrowser projects_ { FileBrowser::Kind::projects };
        int projectsSel_ = 0, projectsScroll_ = 0;
        bool projectsMoved_ = false;                   // the person has moved around in it: it stays where it is

        // detect dialog
        float detectSlider_ = 0.5f;

        // record screen
        InputHub* inputs_ = nullptr;
        RecordSession session_;
        int recordRow_ = 0;                            // 0 input, 1 channels, 2 gain
        int recordSaving_ = 0;                         // 1 = save and load, 2 = save: the box is drawn first, the file written next frame
        std::filesystem::path lastSaved_;

        // menus
        ListMenu menu_, settings_;
        int helpPage_ = 0;

        // loading
        std::future<kerf::LoadResult> loading_;
        std::string loadingName_;
        std::filesystem::path loadingPath_;            // the file being loaded ...
        std::filesystem::path samplePath_;             // ... and the one that was loaded last
        std::string loadingNote_;                      // what the toast says when this load is done ("" = the usual words)

        // the autosave of the sample being worked on (project_keeper.h; off unless the setting asks for it)
        ProjectKeeper project_;

        std::function<void (const std::string&)> log_;

        // the project file of the work in hand: made by "Save project", or opened from the library of projects
        std::filesystem::path projectFile_;
        kerf::Project projectBase_;                    // the work as it was last saved (or opened, or as the sample came in)
        bool projectEdited_ = false;                   // the work differs from projectBase_
        bool projectSaveFailed_ = false;               // the last "Save project" did not work
        double editCheckAt_ = 0.0;
        double discardUntil_ = 0.0;                    // confirmDiscard() has warned: a second try before this time goes through

        // a project being opened: its sample is loading
        kerf::Project pendingProject_;
        std::filesystem::path pendingProjectFile_;
        bool pendingProjectSet_ = false;

        // feedback
        std::string toast_;
        float toastTime_ = 0.0f;
        HostStatus host_;
        unsigned long long lastMidiCount_ = 0;
        float midiFlash_ = 0.0f;
        InputDebug debug_;
        Pad lastPad_;
        float peakShown_ = 0.0f;
        std::vector<WaveformCache::Column> cols_; // scratch for drawing the waveform
    };
}
