#pragma once

// A pretend microphone for the tests and for `kerf_headless --fake-input`: it makes sound as the
// application's clock runs (poll() with the frame time), so a run is the same every time and needs
// no sound card.
//
// Channel k carries a sine of toneHz * (k + 1) Hz at an amplitude of 0.05 + 0.07 * k, so that a
// recording shows which channels were taken (and the first channel is the quietest).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "audio_input.h"

namespace kerf::ui
{
    struct FakeDevice
    {
        std::string id;                 // "fake:..." (made from the name when empty)
        std::string name;
        int channels = 2;
        int rate = 48000;
        bool direct = false;
        double toneHz = 220.0;
        bool silent = false;            // sends only zeros
        bool sendsNothing = false;      // runs, but no sound ever arrives
        std::string failWith {};        // opening it fails with this message
        double endsAfter = 0.0;         // seconds of sound after which it is "unplugged" (0 = never)
    };

    inline float fakeAmplitude (int channel) { return 0.05f + 0.07f * (float) channel; }

    // The pretend inputs of `--fake-input` (the headless runner and the program): a USB microphone and
    // a receiver that Kerf would read itself, a mixer with twelve channels, and one that cannot be
    // opened. The recording screen lists the ones Kerf reads itself first.
    inline std::vector<FakeDevice> demoInputs()
    {
        FakeDevice mic;
        mic.id = "fake:mic";
        mic.name = "Pretend USB microphone";
        mic.channels = 2;
        mic.rate = 48000;
        mic.direct = true;

        FakeDevice quiet;
        quiet.id = "fake:silent";
        quiet.name = "Pretend silent receiver";
        quiet.channels = 1;
        quiet.rate = 48000;
        quiet.direct = true;
        quiet.sendsNothing = true;

        FakeDevice mixer;
        mixer.id = "fake:mixer";
        mixer.name = "Pretend 12-channel mixer";
        mixer.channels = 12;
        mixer.rate = 44100;
        mixer.toneHz = 110.0;

        FakeDevice broken;
        broken.id = "fake:broken";
        broken.name = "Pretend broken input";
        broken.channels = 2;
        broken.failWith = "the device refused to start (pretend)";

        return { mic, quiet, mixer, broken };
    }

    class FakeInput : public InputBackend
    {
    public:
        FakeInput() = default;
        explicit FakeInput (std::vector<FakeDevice> devices) { for (auto& d : devices) add (std::move (d)); }
        ~FakeInput() override { stop(); }

        void add (FakeDevice d)
        {
            if (d.id.empty())
                d.id = "fake:" + d.name;

            std::lock_guard<std::mutex> lock (mutex_);
            devices_.push_back (std::move (d));
        }

        // Plug a device out or in while the program runs.
        void remove (const std::string& id)
        {
            std::lock_guard<std::mutex> lock (mutex_);
            devices_.erase (std::remove_if (devices_.begin(), devices_.end(), [&] (const FakeDevice& d) { return d.id == id; }), devices_.end());
        }

        // Changes a device that is listed (what it does the next time it is opened).
        void edit (const std::string& id, const std::function<void (FakeDevice&)>& change)
        {
            std::lock_guard<std::mutex> lock (mutex_);

            for (auto& d : devices_)
                if (d.id == id)
                    change (d);
        }

        // How many times a stream was started (a failed start counts).
        int starts() const
        {
            std::lock_guard<std::mutex> lock (mutex_);
            return starts_;
        }

        std::vector<InputSource> list() override
        {
            std::lock_guard<std::mutex> lock (mutex_);
            std::vector<InputSource> out;

            for (const auto& d : devices_)
            {
                InputSource s;
                s.id = d.id;
                s.name = d.name;
                s.detail = "pretend input, " + std::to_string (d.channels) + (d.channels == 1 ? " channel" : " channels");
                s.direct = d.direct;
                out.push_back (std::move (s));
            }

            return out;
        }

        void start (const InputSource& source, CaptureRing& ring) override
        {
            stop();
            FakeDevice dev;
            bool found = false;

            {
                std::lock_guard<std::mutex> lock (mutex_);

                for (const auto& d : devices_)
                    if (d.id == source.id) { dev = d; found = true; }
            }

            std::lock_guard<std::mutex> lock (mutex_);
            status_ = InputStatus {};
            ++starts_;

            if (! found)
            {
                status_.message = "the input is gone";
                state_.store (InputState::failed);
                return;
            }

            if (! dev.failWith.empty())
            {
                status_.message = dev.failWith;
                state_.store (InputState::failed);
                return;
            }

            device_ = dev;
            ring_ = &ring;
            ring.configure (dev.channels, std::max (dev.rate * 2, 1));
            phase_.assign ((size_t) dev.channels, 0.0);
            carry_ = 0.0;
            soundSeconds_ = 0.0;
            status_.channels = dev.channels;
            status_.sampleRate = dev.rate;
            status_.measuredRate = dev.rate;
            status_.message = "listening";
            state_.store (InputState::running, std::memory_order_release);
        }

        void stop() override
        {
            std::lock_guard<std::mutex> lock (mutex_);
            ring_ = nullptr;
            state_.store (InputState::idle, std::memory_order_release);
        }

        InputState state() const override { return state_.load (std::memory_order_acquire); }

        InputStatus status() const override
        {
            std::lock_guard<std::mutex> lock (mutex_);
            InputStatus s = status_;
            s.state = state_.load (std::memory_order_acquire);
            return s;
        }

        void poll (double seconds) override
        {
            std::lock_guard<std::mutex> lock (mutex_);

            if (state_.load (std::memory_order_acquire) != InputState::running || ring_ == nullptr || device_.sendsNothing)
                return;

            seconds = std::clamp (seconds, 0.0, 2.0);                   // no more than the ring holds

            if (device_.endsAfter > 0.0 && soundSeconds_ + seconds >= device_.endsAfter)
                seconds = std::max (0.0, device_.endsAfter - soundSeconds_);

            carry_ += seconds * (double) device_.rate;
            const int frames = (int) (carry_ + 1e-6);
            carry_ -= frames;
            soundSeconds_ += seconds;

            const int channels = device_.channels;
            block_.assign ((size_t) frames * (size_t) channels, 0.0f);

            if (! device_.silent)
            {
                const double twoPi = 6.283185307179586;

                for (int ch = 0; ch < channels; ++ch)
                {
                    const double step = twoPi * device_.toneHz * (double) (ch + 1) / (double) device_.rate;
                    double& phase = phase_[(size_t) ch];

                    for (int i = 0; i < frames; ++i)
                    {
                        block_[(size_t) i * (size_t) channels + (size_t) ch] = fakeAmplitude (ch) * (float) std::sin (phase);
                        phase += step;

                        if (phase > twoPi)
                            phase -= twoPi;
                    }
                }
            }

            if (frames > 0)
            {
                ring_->write (block_.data(), frames);
                status_.frames += (uint64_t) frames;
            }

            if (device_.endsAfter > 0.0 && soundSeconds_ >= device_.endsAfter - 1e-9)
            {
                status_.message = "unplugged";
                state_.store (InputState::ended, std::memory_order_release);
            }
        }

    private:
        mutable std::mutex mutex_;
        std::vector<FakeDevice> devices_;
        FakeDevice device_;
        CaptureRing* ring_ = nullptr;
        InputStatus status_;
        std::atomic<InputState> state_ { InputState::idle };
        std::vector<double> phase_;
        std::vector<float> block_;
        double carry_ = 0.0;
        double soundSeconds_ = 0.0;
        int starts_ = 0;
    };
}
