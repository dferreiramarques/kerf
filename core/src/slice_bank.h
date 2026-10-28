#pragma once

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "slice.h"
#include "spin_lock.h"

namespace kerf
{
    // Holds the full slice list. Mutated only from the UI thread, read from the audio thread
    // once per block via getCurrent(). No fixed slice-count cap: the snapshot is a copy-on-write
    // std::vector published behind a SpinLock, so publishing a new list never blocks or
    // allocates on the audio thread - it just takes a very short lock to copy a shared_ptr.
    class SliceBank
    {
    public:
        using Snapshot = std::shared_ptr<const std::vector<Slice>>;

        SliceBank() : current_ (std::make_shared<const std::vector<Slice>>()) {}

        // Audio-thread-safe: takes the lock only long enough to copy a shared_ptr.
        Snapshot getCurrent() const noexcept
        {
            SpinLock::Scoped lock (mutex_);
            return current_;
        }

        // UI thread only from here down.
        void replaceAll (std::vector<Slice> newSlices) { publish (std::move (newSlices)); }

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

        void clear() { publish ({}); }

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

        SliceBank (const SliceBank&) = delete;
        SliceBank& operator= (const SliceBank&) = delete;

    private:
        void publish (std::vector<Slice> newSlices)
        {
            auto newShared = std::make_shared<const std::vector<Slice>> (std::move (newSlices));
            Snapshot old;
            {
                SpinLock::Scoped lock (mutex_);
                old = std::move (current_);
                current_ = std::move (newShared);
            }
            // `old` is released here, outside the lock, so freeing the previous list never
            // happens while the audio thread could be waiting on the spin lock.
        }

        mutable SpinLock mutex_;
        Snapshot current_;
    };
}
