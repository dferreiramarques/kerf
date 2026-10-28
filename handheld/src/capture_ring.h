#pragma once

// A queue of audio frames from the one thread that produces them (the USB or system capture
// thread) to the one that uses them (the UI thread), without locks.
//
// A frame is one floating point sample for each channel of the stream, interleaved. The two sides
// only ever touch their own counter and read the other's, so no mutex is needed: the writer
// publishes what it has copied with a release store, the reader sees it with an acquire load.
// The counters count frames since the ring was set up and never wrap in practice (64 bits).
//
// When the reader falls behind and the ring is full, what does not fit is dropped (and counted):
// the recording then has a gap, which the screen reports.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace kerf::ui
{
    class CaptureRing
    {
    public:
        // Sets the ring up for a stream. Not thread safe: only while neither side is using it.
        void configure (int channels, int capacityFrames)
        {
            channels_ = std::max (channels, 1);
            capacity_ = std::max (capacityFrames, 1);
            data_.assign ((size_t) capacity_ * (size_t) channels_, 0.0f);
            head_.store (0, std::memory_order_relaxed);
            tail_.store (0, std::memory_order_relaxed);
            dropped_.store (0, std::memory_order_relaxed);
        }

        int channels() const noexcept { return channels_; }
        int capacity() const noexcept { return capacity_; }

        // Writer side. Returns how many of the `frames` were taken; the rest is counted as dropped.
        int write (const float* interleaved, int frames) noexcept
        {
            if (frames <= 0 || capacity_ <= 0)
                return 0;

            const uint64_t head = head_.load (std::memory_order_relaxed);
            const uint64_t tail = tail_.load (std::memory_order_acquire);
            const int room = capacity_ - (int) (head - tail);
            const int n = std::min (frames, std::max (room, 0));

            if (n < frames)
                dropped_.fetch_add ((uint64_t) (frames - n), std::memory_order_relaxed);

            if (n > 0)
            {
                const int at = (int) (head % (uint64_t) capacity_);
                const int first = std::min (n, capacity_ - at);
                const size_t ch = (size_t) channels_;
                std::memcpy (data_.data() + (size_t) at * ch, interleaved, (size_t) first * ch * sizeof (float));

                if (n > first)
                    std::memcpy (data_.data(), interleaved + (size_t) first * ch, (size_t) (n - first) * ch * sizeof (float));

                head_.store (head + (uint64_t) n, std::memory_order_release);
            }

            return n;
        }

        // Reader side. Returns how many frames were copied to `out` (at most `maxFrames`).
        int read (float* out, int maxFrames) noexcept
        {
            if (maxFrames <= 0 || capacity_ <= 0)
                return 0;

            const uint64_t tail = tail_.load (std::memory_order_relaxed);
            const uint64_t head = head_.load (std::memory_order_acquire);
            const int n = std::min (maxFrames, (int) (head - tail));

            if (n <= 0)
                return 0;

            const int at = (int) (tail % (uint64_t) capacity_);
            const int first = std::min (n, capacity_ - at);
            const size_t ch = (size_t) channels_;
            std::memcpy (out, data_.data() + (size_t) at * ch, (size_t) first * ch * sizeof (float));

            if (n > first)
                std::memcpy (out + (size_t) first * ch, data_.data(), (size_t) (n - first) * ch * sizeof (float));

            tail_.store (tail + (uint64_t) n, std::memory_order_release);
            return n;
        }

        // Frames waiting to be read.
        int available() const noexcept
        {
            return (int) (head_.load (std::memory_order_acquire) - tail_.load (std::memory_order_acquire));
        }

        // Frames that did not fit since the ring was set up.
        uint64_t dropped() const noexcept { return dropped_.load (std::memory_order_relaxed); }

        // Frames written since the ring was set up.
        uint64_t written() const noexcept { return head_.load (std::memory_order_acquire); }

    private:
        std::vector<float> data_;
        int channels_ = 1;
        int capacity_ = 0;
        alignas (64) std::atomic<uint64_t> head_ { 0 };     // frames ever written: moved by the writer
        alignas (64) std::atomic<uint64_t> tail_ { 0 };     // frames ever read: moved by the reader
        alignas (64) std::atomic<uint64_t> dropped_ { 0 };
    };
}
