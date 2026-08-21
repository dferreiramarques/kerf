#pragma once

#include <juce_core/juce_core.h>
#include <vector>
#include <memory>
#include <functional>
#include "Slice.h"

// Holds the full slice list. Mutated only from the message thread (WebBridge callbacks, UI),
// read from the audio thread once per block via getCurrent(). No fixed slice-count cap: the
// snapshot is a copy-on-write std::vector published behind a SpinLock, so publishing a new list
// never blocks or allocates on the audio thread - it just takes a very short lock to copy a
// shared_ptr. Auto-detecting transients on a long, dense sample keeps everything it finds.
class SliceBank
{
public:
    using Snapshot = std::shared_ptr<const std::vector<Slice>>;

    SliceBank() : current (std::make_shared<const std::vector<Slice>>()) {}

    // Audio-thread-safe: takes the lock only long enough to copy a shared_ptr.
    Snapshot getCurrent() const noexcept
    {
        const juce::SpinLock::ScopedLockType lock (mutex);
        return current;
    }

    // Message-thread only from here down.
    void replaceAll (std::vector<Slice> newSlices)
    {
        publish (std::move (newSlices));
    }

    int add (Slice slice)
    {
        auto copy = *getCurrent();
        copy.push_back (slice);
        const auto newIndex = (int) copy.size() - 1;
        publish (std::move (copy));
        return newIndex;
    }

    void removeAt (int index)
    {
        auto copy = *getCurrent();
        if (index >= 0 && index < (int) copy.size())
        {
            copy.erase (copy.begin() + index);
            publish (std::move (copy));
        }
    }

    void clear()
    {
        publish ({});
    }

    void updateAt (int index, const std::function<void (Slice&)>& mutator)
    {
        auto copy = *getCurrent();
        if (index >= 0 && index < (int) copy.size())
        {
            mutator (copy[(size_t) index]);
            publish (std::move (copy));
        }
    }

    int size() const noexcept { return (int) getCurrent()->size(); }

private:
    void publish (std::vector<Slice> newSlices)
    {
        auto newShared = std::make_shared<const std::vector<Slice>> (std::move (newSlices));
        const juce::SpinLock::ScopedLockType lock (mutex);
        current = newShared;
    }

    mutable juce::SpinLock mutex;
    Snapshot current;

    JUCE_DECLARE_NON_COPYABLE (SliceBank)
};
