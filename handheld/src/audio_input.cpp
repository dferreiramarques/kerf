#include "audio_input.h"

#include <algorithm>

namespace kerf::ui
{
    void InputHub::setLogger (InputLogger logger)
    {
        logger_ = std::move (logger);

        for (auto& b : backends_)
            b->setLogger (logger_);
    }

    void InputHub::add (std::unique_ptr<InputBackend> backend)
    {
        if (! backend)
            return;

        backend->setLogger (logger_);
        backends_.push_back (std::move (backend));
    }

    std::vector<InputSource> InputHub::sources()
    {
        std::vector<InputSource> all;

        for (size_t i = 0; i < backends_.size(); ++i)
        {
            for (auto& s : backends_[i]->list())
            {
                s.backend = (int) i;
                all.push_back (std::move (s));
            }
        }

        // The inputs that only Kerf can read come first: on a handheld they are what was plugged in.
        std::stable_sort (all.begin(), all.end(), [] (const InputSource& a, const InputSource& b) { return a.direct && ! b.direct; });
        return all;
    }

    bool InputHub::open (const InputSource& source)
    {
        close();

        if (source.backend < 0 || source.backend >= (int) backends_.size())
            return false;

        active_ = backends_[(size_t) source.backend].get();
        current_ = source;
        active_->start (source, ring_);
        return true;
    }

    void InputHub::close()
    {
        if (active_ != nullptr)
        {
            active_->stop();
            active_ = nullptr;
        }

        current_ = InputSource {};
    }

    InputState InputHub::state() const
    {
        return active_ != nullptr ? active_->state() : InputState::idle;
    }

    InputStatus InputHub::status() const
    {
        InputStatus s;

        if (active_ == nullptr)
            return s;

        s = active_->status();

        if (s.state == InputState::running || s.state == InputState::ended)
            s.dropped += ring_.dropped();

        return s;
    }

    int InputHub::read (float* out, int maxFrames)
    {
        if (active_ == nullptr)
            return 0;

        const InputState s = active_->state();
        return (s == InputState::running || s == InputState::ended) ? ring_.read (out, maxFrames) : 0;
    }

    void InputHub::poll (double seconds)
    {
        if (active_ != nullptr)
            active_->poll (seconds);
    }
}
