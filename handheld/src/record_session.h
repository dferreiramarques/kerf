#pragma once

// Everything the Record screen does that is not drawing or reading buttons: which input is open,
// which of its channels are recorded, the level meters, the take, and saving it.
//
// The screen (app_record.cpp) asks this class what to show and tells it what the person pressed.
// The sound comes from an InputHub (audio_input.h), which may be the system's microphone, Kerf's
// own USB audio reader or a pretend input in the tests.

#include <filesystem>
#include <string>
#include <vector>

#include "audio_input.h"
#include "recorder.h"

namespace kerf::ui
{
    class RecordSession
    {
    public:
        enum class Phase { idle, recording, stopped };      // stopped: a take exists

        static constexpr int kGainStepDb = 3;
        static constexpr int kMinGainDb = -12;
        static constexpr int kMaxGainDb = 30;
        static constexpr double kRefreshSeconds = 1.5;      // how often the list of inputs is looked at again
        static constexpr double kQuietSeconds = 1.5;        // without any sound from the device for this long, the screen says so
        static constexpr double kShortestTake = 0.25;       // a shorter take is thrown away

        void attach (InputHub* hub) noexcept { hub_ = hub; }
        bool available() const noexcept { return hub_ != nullptr && ! hub_->empty(); }

        // The screen is entered: looks at the inputs and opens the one named by `preferredId` (the
        // one used last time), else the first. `preferredChannels` is the label of the channels taken
        // last time ("3+4  stereo"): used when the input has them, else the first choice.
        void begin (const std::string& preferredId, int gainDb, double maxSeconds, const std::string& preferredChannels = {});

        // The screen is left: the input is closed and the take forgotten.
        void end();

        bool active() const noexcept { return active_; }

        // Once a frame while the screen is up.
        void update (double seconds);

        // ---- what the person can change (not while recording, and not once there is a take)
        bool canChange() const noexcept;
        void chooseSource (int delta);
        void chooseChannels (int delta);
        void adjustGain (int steps);
        void reopen();                                      // try the input again (after it failed)

        const std::vector<InputSource>& sources() const noexcept { return sources_; }
        int sourceIndex() const noexcept { return sourceIndex_; }
        std::string sourceId() const;
        std::string sourceName() const;
        const std::vector<ChannelChoice>& choices() const noexcept { return choices_; }
        int choiceIndex() const noexcept { return choiceIndex_; }

        // The channels the person picked, to be kept for the next time: unchanged by an input that
        // does not have them, empty while nothing has been picked.
        const std::string& chosenChannels() const noexcept { return lastChoiceLabel_; }
        int gainDb() const noexcept { return gainDb_; }

        // ---- the take
        Phase phase() const noexcept;
        bool toggleRecording();                             // idle -> recording -> stopped
        void newTake();                                     // forgets the take: back to idle
        const Recorder& recorder() const noexcept { return recorder_; }

        // Writes the take into `dir` as rec-001.wav, rec-002.wav ... and says where. The take is kept
        // (end() forgets it).
        bool save (const std::filesystem::path& dir, std::filesystem::path& saved, std::string& error);

        // ---- what to show
        bool ready() const noexcept { return recorder_.configured() && status_.state == InputState::running; }
        const InputStatus& input() const noexcept { return status_; }
        std::string statusText() const;                     // a line about the input's state
        bool soundSeen() const noexcept { return framesSeen_ > 0; }
        bool quiet() const noexcept;                        // the stream runs but no sound has come for a while

        // A message to show once ("The take is full"): returns it and forgets it.
        std::string takeNotice();

    private:
        void refreshSources (bool allowOpen);
        void openIndex (int index);
        void configureStream();
        void endTakeWithInput();
        void pull();
        void setNotice (std::string text) { notice_ = std::move (text); }

        InputHub* hub_ = nullptr;
        bool active_ = false;
        std::vector<InputSource> sources_;
        int sourceIndex_ = -1;
        std::string wantedId_;                              // the input meant to be open (kept while it is unplugged)
        std::vector<ChannelChoice> choices_;
        int choiceIndex_ = 0;
        std::string lastChoiceLabel_;                       // the choice made last: wanted again when another input opens
        int gainDb_ = 0;
        double maxSeconds_ = 180.0;
        bool wentAway_ = false;                             // the open input was unplugged (and may come back)

        Recorder recorder_;
        InputStatus status_;
        InputState lastState_ = InputState::idle;
        std::vector<float> buffer_;
        uint64_t framesSeen_ = 0;
        double sinceStart_ = 0.0;
        double sinceRefresh_ = 0.0;
        bool fullNoticed_ = false;
        std::string notice_;
    };
}
