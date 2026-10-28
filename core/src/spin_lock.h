#pragma once

#include <atomic>

namespace kerf
{
    // A tiny spin lock for guarding very short critical sections (copying a shared_ptr). The
    // audio thread may take it, so nothing inside the lock may allocate, block or call into the OS.
    class SpinLock
    {
    public:
        void lock() noexcept
        {
            while (flag_.test_and_set (std::memory_order_acquire))
            {
                // spin
            }
        }

        void unlock() noexcept { flag_.clear (std::memory_order_release); }

        class Scoped
        {
        public:
            explicit Scoped (SpinLock& l) noexcept : lock_ (l) { lock_.lock(); }
            ~Scoped() { lock_.unlock(); }
            Scoped (const Scoped&) = delete;
            Scoped& operator= (const Scoped&) = delete;

        private:
            SpinLock& lock_;
        };

    private:
        std::atomic_flag flag_ = ATOMIC_FLAG_INIT;
    };
}
