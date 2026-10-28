#include "record_session.h"

#include <algorithm>
#include <system_error>

#include "format.h"

namespace fs = std::filesystem;

namespace kerf::ui
{
    // ------------------------------------------------------------------------------ the screen

    void RecordSession::begin (const std::string& preferredId, int gainDb, double maxSeconds, const std::string& preferredChannels)
    {
        end();
        active_ = true;
        gainDb_ = std::clamp (gainDb, kMinGainDb, kMaxGainDb);
        maxSeconds_ = maxSeconds;
        wantedId_ = preferredId;
        recorder_.reset();
        recorder_.setGainDb (gainDb_);
        status_ = InputStatus {};
        lastState_ = InputState::idle;
        framesSeen_ = 0;
        sinceStart_ = 0.0;
        sinceRefresh_ = 0.0;
        fullNoticed_ = false;
        wentAway_ = false;
        notice_.clear();
        lastChoiceLabel_ = preferredChannels;
        refreshSources (true);
    }

    void RecordSession::end()
    {
        if (hub_ != nullptr && active_)
            hub_->close();

        active_ = false;
        recorder_.reset();
        sources_.clear();
        sourceIndex_ = -1;
        choices_.clear();
        choiceIndex_ = 0;
        wantedId_.clear();
        status_ = InputStatus {};
        lastState_ = InputState::idle;
        framesSeen_ = 0;
        notice_.clear();
        std::vector<float>().swap (buffer_);
    }

    void RecordSession::update (double seconds)
    {
        if (! active_ || hub_ == nullptr)
            return;

        const double dt = std::max (seconds, 0.0);
        hub_->poll (dt);
        recorder_.tick (dt);
        sinceStart_ += dt;

        status_ = hub_->status();
        configureStream();

        // The sound that came before the input ended is part of the take too.
        if (status_.state == InputState::running || status_.state == InputState::ended)
            pull();

        endTakeWithInput();
        lastState_ = status_.state;

        if (recorder_.full() && ! fullNoticed_)
        {
            fullNoticed_ = true;
            setNotice ("The take is full (" + clockTime (recorder_.takeSeconds()) + ")");
        }

        // Plugging a microphone in while the screen is up works: the list is looked at again now and then.
        sinceRefresh_ += dt;

        if (sinceRefresh_ >= kRefreshSeconds)
        {
            sinceRefresh_ = 0.0;

            if (! recorder_.recording())
                refreshSources (true);
        }
    }

    // ------------------------------------------------------------------------------- the inputs

    void RecordSession::refreshSources (bool allowOpen)
    {
        if (hub_ == nullptr)
        {
            sources_.clear();
            sourceIndex_ = -1;
            return;
        }

        sources_ = hub_->sources();

        int wantedAt = -1;

        for (size_t i = 0; i < sources_.size(); ++i)
            if (! wantedId_.empty() && sources_[i].id == wantedId_)
                wantedAt = (int) i;

        if (hub_->isOpen())
        {
            // Keep pointing at the open input while it is listed.
            sourceIndex_ = -1;

            for (size_t i = 0; i < sources_.size(); ++i)
                if (sources_[i].id == hub_->current().id)
                    sourceIndex_ = (int) i;

            // An input that has stopped is opened again when it has been unplugged and is back; one
            // that is still listed is not tried again by itself (it would fail the same way).
            const InputState s = hub_->state();

            if (s == InputState::ended || s == InputState::failed)
            {
                if (wantedAt < 0)
                    wentAway_ = true;
                else if (wentAway_ && allowOpen && ! recorder_.hasTake())
                    openIndex (wantedAt);
            }
        }
        else if (allowOpen && ! sources_.empty())
        {
            openIndex (wantedAt >= 0 ? wantedAt : 0);
        }
        else
        {
            sourceIndex_ = -1;
        }
    }

    void RecordSession::openIndex (int index)
    {
        if (hub_ == nullptr || index < 0 || index >= (int) sources_.size())
            return;

        sourceIndex_ = index;
        wantedId_ = sources_[(size_t) index].id;
        recorder_.reset();
        choices_.clear();
        choiceIndex_ = 0;
        framesSeen_ = 0;
        sinceStart_ = 0.0;
        fullNoticed_ = false;
        wentAway_ = false;
        lastState_ = InputState::idle;
        hub_->open (sources_[(size_t) index]);
        status_ = hub_->status();
        configureStream();
        lastState_ = status_.state;
    }

    // Sets the recorder up when the stream starts to run.
    void RecordSession::configureStream()
    {
        if (status_.state != InputState::running || recorder_.configured() || status_.channels <= 0 || status_.sampleRate <= 0)
            return;

        recorder_.configure (status_.channels, status_.sampleRate, maxSeconds_);
        choices_ = channelChoices (status_.channels);
        choiceIndex_ = 0;

        for (size_t i = 0; i < choices_.size(); ++i)
            if (! lastChoiceLabel_.empty() && choices_[i].label == lastChoiceLabel_)
                choiceIndex_ = (int) i;

        if (! choices_.empty())
            recorder_.setChoice (choices_[(size_t) choiceIndex_]);
    }

    // A take that is under way ends with its input: what was recorded is kept, unless it is nothing.
    void RecordSession::endTakeWithInput()
    {
        const InputState now = status_.state;

        if ((now == InputState::ended || now == InputState::failed) && lastState_ != now && recorder_.recording())
        {
            recorder_.stop();

            if (recorder_.takeSeconds() < kShortestTake)
            {
                recorder_.discard();
                setNotice ("The input stopped - nothing was kept");
            }
            else
            {
                setNotice ("The input stopped - the take so far is kept");
            }
        }
    }

    void RecordSession::pull()
    {
        if (! recorder_.configured())
            return;

        const int channels = recorder_.channels();
        const int capacity = std::max (256, recorder_.sampleRate() / 20);       // 50 ms at a time
        buffer_.resize ((size_t) capacity * (size_t) channels);

        // A cap, so that a ring that was never emptied (the screen was busy) cannot hold the frame up.
        for (int guard = 0; guard < 64; ++guard)
        {
            const int n = hub_->read (buffer_.data(), capacity);

            if (n <= 0)
                break;

            framesSeen_ += (uint64_t) n;
            recorder_.feed (buffer_.data(), n);
        }
    }

    bool RecordSession::canChange() const noexcept
    {
        return phase() == Phase::idle;
    }

    void RecordSession::chooseSource (int delta)
    {
        if (! active_ || ! canChange() || sources_.empty() || delta == 0)
            return;

        const int n = (int) sources_.size();
        const int next = sourceIndex_ < 0 ? (delta > 0 ? 0 : n - 1) : ((sourceIndex_ + delta) % n + n) % n;
        openIndex (next);
    }

    void RecordSession::chooseChannels (int delta)
    {
        if (! active_ || ! canChange() || choices_.empty() || delta == 0)
            return;

        const int n = (int) choices_.size();
        choiceIndex_ = ((choiceIndex_ + delta) % n + n) % n;
        recorder_.setChoice (choices_[(size_t) choiceIndex_]);
        lastChoiceLabel_ = choices_[(size_t) choiceIndex_].label;
    }

    void RecordSession::adjustGain (int steps)
    {
        if (! active_ || ! canChange() || steps == 0)
            return;

        gainDb_ = std::clamp (gainDb_ + steps * kGainStepDb, kMinGainDb, kMaxGainDb);
        recorder_.setGainDb (gainDb_);
    }

    void RecordSession::reopen()
    {
        if (! active_ || ! canChange())
            return;

        refreshSources (false);

        for (size_t i = 0; i < sources_.size(); ++i)
            if (sources_[i].id == wantedId_)
            {
                openIndex ((int) i);
                return;
            }

        if (! sources_.empty())
            openIndex (0);
    }

    std::string RecordSession::sourceId() const
    {
        if (sourceIndex_ >= 0 && sourceIndex_ < (int) sources_.size())
            return sources_[(size_t) sourceIndex_].id;

        return wantedId_;
    }

    std::string RecordSession::sourceName() const
    {
        if (sourceIndex_ >= 0 && sourceIndex_ < (int) sources_.size())
            return sources_[(size_t) sourceIndex_].name;

        if (hub_ != nullptr && hub_->isOpen())
            return hub_->current().name;

        return {};
    }

    // --------------------------------------------------------------------------------- the take

    RecordSession::Phase RecordSession::phase() const noexcept
    {
        if (recorder_.recording())
            return Phase::recording;

        return recorder_.hasTake() ? Phase::stopped : Phase::idle;
    }

    bool RecordSession::toggleRecording()
    {
        if (! active_ || hub_ == nullptr)
            return false;

        switch (phase())
        {
            case Phase::idle:
            {
                if (! ready())
                    return false;

                // Whatever is waiting in the queue is older than the button press.
                const int capacity = std::max (256, recorder_.sampleRate() / 20);
                buffer_.resize ((size_t) capacity * (size_t) recorder_.channels());

                for (int guard = 0; guard < 256 && hub_->read (buffer_.data(), capacity) > 0; ++guard)
                {
                }

                fullNoticed_ = false;
                return recorder_.start();
            }

            case Phase::recording:
                recorder_.stop();

                if (recorder_.takeSeconds() < kShortestTake)
                {
                    recorder_.discard();
                    setNotice ("Too short - nothing was kept");
                }

                return true;

            case Phase::stopped:
                break;
        }

        return false;
    }

    void RecordSession::newTake()
    {
        recorder_.discard();
        fullNoticed_ = false;
    }

    bool RecordSession::save (const fs::path& dir, fs::path& saved, std::string& error)
    {
        if (! recorder_.hasTake())
        {
            error = "nothing has been recorded";
            return false;
        }

        std::error_code ec;
        fs::create_directories (dir, ec);

        const fs::path target = Recorder::nextTakePath (dir);

        if (! recorder_.saveWav16 (target, error))
            return false;

        saved = target;
        return true;
    }

    // ------------------------------------------------------------------------------- the words

    bool RecordSession::quiet() const noexcept
    {
        return ready() && framesSeen_ == 0 && sinceStart_ > kQuietSeconds;
    }

    std::string RecordSession::statusText() const
    {
        if (hub_ == nullptr || (sources_.empty() && ! hub_->isOpen()))
            return "No audio input found";

        switch (status_.state)
        {
            case InputState::idle:
                return "Looking for the input...";

            case InputState::starting:
                return "Opening " + sourceName() + "...";

            case InputState::failed:
                return status_.message.empty() ? std::string ("The input could not be opened") : status_.message;

            case InputState::ended:
                return status_.message.empty() ? std::string ("The input stopped") : "The input stopped: " + status_.message;

            case InputState::running:
                break;
        }

        if (quiet())
            return "Nothing is coming from the input yet";

        std::string text = integer (status_.sampleRate) + " Hz, " + integer (status_.channels) + (status_.channels == 1 ? " channel" : " channels");

        if (status_.dropped > 0)
            text += ", " + integer ((long long) status_.dropped) + " frames lost";

        return text;
    }

    std::string RecordSession::takeNotice()
    {
        std::string out;
        out.swap (notice_);
        return out;
    }
}
