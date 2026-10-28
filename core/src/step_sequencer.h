#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace kerf
{
    // A 16-step pattern sequencer for the slices: one row per slice (row N plays slice N), up to
    // eight scenes (patterns), a tempo, a swing amount and a pattern length. It only decides WHEN
    // a step happens; the Engine plays the slices.
    //
    // Threading contract (the same as Engine's):
    //  - the pattern and transport calls below ("UI side") are lock-free atomics and may be made
    //    from the UI thread at any time, also while audio is running;
    //  - process() is the only audio-thread call.
    //
    // Timing: a step is a 16th note. Every step has an exact position on one absolute clock (in
    // frames, kept as a double), and process() hands out the steps that fall inside each block with
    // the frame they start on. Nothing depends on how the audio device chops the stream into
    // blocks, so the tempo never drifts and the same song renders to the same samples whatever the
    // buffer size is.
    class StepSequencer
    {
    public:
        static constexpr int kSteps = 16;
        static constexpr int kScenes = 8;
        static constexpr int kRows = 128;     // slices that can be sequenced (the first 128)
        static constexpr int kMaxEvents = 8;  // steps one block can hold (a block is at most 1024 frames)

        static constexpr float kMinTempo = 40.0f, kMaxTempo = 300.0f, kDefaultTempo = 120.0f;
        static constexpr float kMinSwing = 0.5f, kMaxSwing = 0.75f;   // 0.5 = straight, 0.667 = triplet feel

        // One step that starts inside the block handed to process().
        struct Event
        {
            int frame = 0;   // position inside the block, 0 .. numFrames-1
            int step = 0;    // 0 .. length-1
            int scene = 0;   // the scene that step belongs to (a scene change waits for the pattern to wrap)
        };

        // Every pattern at once: for undo, and for keeping the rows with their slices.
        struct Grid
        {
            uint16_t bits[kScenes][kRows] = {};
        };

        StepSequencer() { clearAll(); }

        StepSequencer (const StepSequencer&) = delete;
        StepSequencer& operator= (const StepSequencer&) = delete;

        // ------------------------------------------------------------ pattern (UI side)

        bool cell (int scene, int row, int step) const noexcept
        {
            if (! inRange (scene, row, step))
                return false;

            return (bits_[scene][row].load (std::memory_order_relaxed) >> step) & 1u;
        }

        void setCell (int scene, int row, int step, bool on) noexcept
        {
            if (! inRange (scene, row, step))
                return;

            const auto mask = (uint16_t) (1u << step);

            if (on) bits_[scene][row].fetch_or (mask, std::memory_order_relaxed);
            else    bits_[scene][row].fetch_and ((uint16_t) ~mask, std::memory_order_relaxed);
        }

        uint16_t rowBits (int scene, int row) const noexcept
        {
            return inRange (scene, row, 0) ? bits_[scene][row].load (std::memory_order_relaxed) : (uint16_t) 0;
        }

        void setRowBits (int scene, int row, uint16_t b) noexcept
        {
            if (inRange (scene, row, 0))
                bits_[scene][row].store (b, std::memory_order_relaxed);
        }

        void clearScene (int scene) noexcept
        {
            if (scene < 0 || scene >= kScenes)
                return;

            for (auto& r : bits_[scene])
                r.store (0, std::memory_order_relaxed);
        }

        void clearAll() noexcept
        {
            for (int s = 0; s < kScenes; ++s)
                clearScene (s);
        }

        void copyScene (int from, int to) noexcept
        {
            if (from < 0 || from >= kScenes || to < 0 || to >= kScenes || from == to)
                return;

            for (int r = 0; r < kRows; ++r)
                bits_[to][r].store (bits_[from][r].load (std::memory_order_relaxed), std::memory_order_relaxed);
        }

        // Steps switched on in a scene (rows beyond `rows` are not counted).
        int cellCount (int scene, int rows = kRows) const noexcept
        {
            int n = 0;
            const int limit = std::min (rows, kRows);

            for (int r = 0; r < limit; ++r)
            {
                auto b = rowBits (scene, r);

                for (; b != 0; b = (uint16_t) (b & (b - 1u)))
                    ++n;
            }

            return n;
        }

        // A slice was inserted at `row`: the rows from there on move down one, so every pattern
        // stays with its slice, and the new slice starts silent. (All scenes.)
        void insertRow (int row) noexcept
        {
            if (row < 0 || row >= kRows)
                return;

            for (int s = 0; s < kScenes; ++s)
            {
                for (int r = kRows - 1; r > row; --r)
                    bits_[s][r].store (bits_[s][r - 1].load (std::memory_order_relaxed), std::memory_order_relaxed);

                bits_[s][row].store (0, std::memory_order_relaxed);
            }
        }

        // The slice at `row` was removed: its pattern goes and the rows below move up one.
        void removeRow (int row) noexcept
        {
            if (row < 0 || row >= kRows)
                return;

            for (int s = 0; s < kScenes; ++s)
            {
                for (int r = row; r < kRows - 1; ++r)
                    bits_[s][r].store (bits_[s][r + 1].load (std::memory_order_relaxed), std::memory_order_relaxed);

                bits_[s][kRows - 1].store (0, std::memory_order_relaxed);
            }
        }

        Grid snapshot() const noexcept
        {
            Grid g;

            for (int s = 0; s < kScenes; ++s)
                for (int r = 0; r < kRows; ++r)
                    g.bits[s][r] = bits_[s][r].load (std::memory_order_relaxed);

            return g;
        }

        void restore (const Grid& g) noexcept
        {
            for (int s = 0; s < kScenes; ++s)
                for (int r = 0; r < kRows; ++r)
                    bits_[s][r].store (g.bits[s][r], std::memory_order_relaxed);
        }

        // ------------------------------------------------------------ transport (UI side)

        void setTempo (float bpm) noexcept { tempo_.store (std::clamp (bpm, kMinTempo, kMaxTempo), std::memory_order_relaxed); }
        float tempo() const noexcept { return tempo_.load (std::memory_order_relaxed); }

        void setSwing (float ratio) noexcept { swing_.store (std::clamp (ratio, kMinSwing, kMaxSwing), std::memory_order_relaxed); }
        float swing() const noexcept { return swing_.load (std::memory_order_relaxed); }

        void setLength (int steps) noexcept { length_.store (std::clamp (steps, 1, kSteps), std::memory_order_relaxed); }
        int length() const noexcept { return length_.load (std::memory_order_relaxed); }

        // The scene being edited. While the sequencer runs, the one that sounds only changes when
        // the pattern wraps round (so a change always lands on the beat).
        void setScene (int scene) noexcept { scene_.store (std::clamp (scene, 0, kScenes - 1), std::memory_order_relaxed); }
        int scene() const noexcept { return scene_.load (std::memory_order_relaxed); }

        // The scene that sounds right now (differs from scene() only until the next wrap).
        int activeScene() const noexcept { return activeScene_.load (std::memory_order_relaxed); }

        void play() noexcept { playing_.store (true, std::memory_order_release); }
        void stop() noexcept { playing_.store (false, std::memory_order_release); }
        bool isPlaying() const noexcept { return playing_.load (std::memory_order_acquire); }

        // The step that started last, or -1 when stopped (the audio thread's progress, for the display).
        int currentStep() const noexcept { return currentStep_.load (std::memory_order_relaxed); }

        // A new sample means new slices: stop and forget every pattern. Tempo, swing and length
        // are settings of the person, not of the sample, and stay.
        void reset() noexcept
        {
            stop();
            clearAll();
            setScene (0);
        }

        // ------------------------------------------------------------ audio thread

        // Moves the clock on by numFrames and writes the steps that start inside the block to
        // `out` (earliest first; at most maxOut). Returns how many. `justStopped` is true on the
        // one block where a stop was noticed, so the caller can release what the sequencer started.
        int process (int numFrames, double sampleRate, Event* out, int maxOut, bool& justStopped) noexcept
        {
            justStopped = false;
            const bool wantPlay = playing_.load (std::memory_order_acquire);
            int count = 0;

            if (wantPlay && ! running_)
            {
                running_ = true;
                nextStep_ = 0;
                nextStepAt_ = blockStart_;     // step 1 starts on the very first frame
                active_ = scene_.load (std::memory_order_relaxed);
                activeScene_.store (active_, std::memory_order_relaxed);
            }
            else if (! wantPlay && running_)
            {
                running_ = false;
                currentStep_.store (-1, std::memory_order_relaxed);
                justStopped = true;
            }

            if (running_)
            {
                const int len = length_.load (std::memory_order_relaxed);
                const double blockEnd = blockStart_ + (double) numFrames;

                while (count < maxOut && nextStepAt_ < blockEnd)
                {
                    if (nextStep_ >= len)     // wrapped (or the pattern was made shorter)
                        nextStep_ = 0;

                    if (nextStep_ == 0)       // the top of the pattern: a requested scene change lands here
                    {
                        active_ = scene_.load (std::memory_order_relaxed);
                        activeScene_.store (active_, std::memory_order_relaxed);
                    }

                    Event e;
                    e.frame = nextStepAt_ <= blockStart_ ? 0 : std::min (numFrames - 1, (int) (nextStepAt_ - blockStart_));
                    e.step = nextStep_;
                    e.scene = active_;
                    out[count++] = e;

                    nextStepAt_ += framesUntilFollowingStep (nextStep_, sampleRate);
                    ++nextStep_;
                }

                if (count > 0)
                    currentStep_.store (out[count - 1].step, std::memory_order_relaxed);
            }

            blockStart_ += (double) numFrames;
            return count;
        }

        // Frames from the start of one step to the start of the next. With swing the steps come in
        // pairs: the first of a pair lasts `swing` of the pair, the second the rest.
        double framesUntilFollowingStep (int step, double sampleRate) const noexcept
        {
            const double sixteenth = sampleRate * 15.0 / (double) tempo();
            const double ratio = (double) swing();
            return (step & 1) == 0 ? 2.0 * sixteenth * ratio : 2.0 * sixteenth * (1.0 - ratio);
        }

    private:
        static constexpr bool inRange (int scene, int row, int step) noexcept
        {
            return scene >= 0 && scene < kScenes && row >= 0 && row < kRows && step >= 0 && step < kSteps;
        }

        // shared
        std::atomic<uint16_t> bits_[kScenes][kRows];
        std::atomic<float> tempo_ { kDefaultTempo };
        std::atomic<float> swing_ { 0.5f };
        std::atomic<int> length_ { kSteps };
        std::atomic<int> scene_ { 0 };
        std::atomic<int> activeScene_ { 0 };
        std::atomic<bool> playing_ { false };
        std::atomic<int> currentStep_ { -1 };

        // audio thread only
        bool running_ = false;
        int nextStep_ = 0;
        int active_ = 0;
        double blockStart_ = 0.0;   // absolute frame number of the first frame of the next block
        double nextStepAt_ = 0.0;   // absolute frame number at which the next step starts
    };
}
