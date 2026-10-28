#pragma once

#include <atomic>
#include <cstddef>
#include <utility>
#include <vector>

namespace kerf
{
    // Wait-free single-producer / single-consumer ring buffer. Exactly one thread may call
    // push() and exactly one (other) thread may call pop(). T must be default-constructible and
    // movable. Capacity is fixed at construction (one slot is kept free to tell full from empty).
    template <typename T>
    class SpscQueue
    {
    public:
        explicit SpscQueue (size_t capacity = 256) : buffer_ (capacity + 1) {}

        bool push (T&& item)
        {
            const auto head = head_.load (std::memory_order_relaxed);
            const auto next = increment (head);

            if (next == tail_.load (std::memory_order_acquire))
                return false; // full

            buffer_[head] = std::move (item);
            head_.store (next, std::memory_order_release);
            return true;
        }

        bool push (const T& item)
        {
            T copy = item;
            return push (std::move (copy));
        }

        bool pop (T& out)
        {
            const auto tail = tail_.load (std::memory_order_relaxed);

            if (tail == head_.load (std::memory_order_acquire))
                return false; // empty

            out = std::move (buffer_[tail]);
            tail_.store (increment (tail), std::memory_order_release);
            return true;
        }

        bool empty() const noexcept
        {
            return tail_.load (std::memory_order_acquire) == head_.load (std::memory_order_acquire);
        }

    private:
        size_t increment (size_t i) const noexcept { return (i + 1) % buffer_.size(); }

        std::vector<T> buffer_;
        std::atomic<size_t> head_ { 0 };
        std::atomic<size_t> tail_ { 0 };
    };
}
