#include "usb_audio_input.h"

#if defined(__linux__)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "usb_audio.h"
#include "usb_audio_io.h"
#include "usb_os.h"

namespace kerf::ui
{
    namespace
    {
        constexpr int kPollMilliseconds = 50;           // the longest the thread waits for a transfer: how fast a request to stop is noticed
        constexpr double kRingSeconds = 2.0;

        // Names the input for as long as it stays in the same port: the same next time.
        std::string idOf (const usbaudio::Candidate& c)
        {
            return "usb:" + usbos::hex ((unsigned) c.vendor, 4) + ":" + usbos::hex ((unsigned) c.product, 4) + "@" + c.name + "#"
                   + std::to_string (c.stream.interfaceNumber);
        }

        class UsbAudioInput : public InputBackend
        {
        public:
            explicit UsbAudioInput (const UsbAudioInputOptions& options) : os_ (options.os), sysfs_ (options.sysfs) {}
            ~UsbAudioInput() override { stop(); }

            std::vector<InputSource> list() override
            {
                const usbaudio::Scan scan = usbaudio::look (sysfsRoot());

                // One line about every USB audio device, with its descriptors: it is what tells why a
                // microphone is, or is not, offered.
                for (const auto& d : scan.devices)
                {
                    if (! described_.insert (d.key()).second)
                        continue;

                    const std::string who = "audio: usb " + d.name + " (" + usbos::hex ((unsigned) d.vendor, 4) + ":" + usbos::hex ((unsigned) d.product, 4) + " " + d.title + ")";
                    log (who + ": " + d.summary + (d.usable ? "" : ": no recording stream that Kerf can use"));
                    log (who + ": descriptors " + d.descriptors);
                }

                std::string active;

                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    active = activeKey_;
                }

                std::vector<const usbaudio::Candidate*> usable;

                for (const auto& c : scan.candidates)
                {
                    // The device being read has the "usbfs" driver now: it is still the one in use.
                    if (! c.driver.empty() && c.key() != active)
                    {
                        if (told_.insert (c.key()).second)
                            log ("audio: " + usbaudio::describe (c) + ": the kernel driver '" + c.driver + "' has the interface"
                                 + (c.driver == "usbfs" ? " (another program is reading it)" : ", Kerf leaves it to the driver"));

                        continue;
                    }

                    usable.push_back (&c);
                }

                std::vector<InputSource> out;

                for (const auto* c : usable)
                {
                    const int sameTitle = (int) std::count_if (usable.begin(), usable.end(), [&] (const usbaudio::Candidate* other) { return other->title == c->title; });
                    const auto order = usbaudio::rankFormats (c->stream);
                    const int channels = order.empty() ? 0 : c->stream.formats[(size_t) order.front()].channels;

                    InputSource s;
                    s.id = idOf (*c);
                    s.name = sameTitle > 1 ? c->title + " (" + c->name + ")" : c->title;
                    s.detail = "USB, " + std::to_string (channels) + (channels == 1 ? " channel" : " channels") + ", read by Kerf";
                    s.direct = true;
                    out.push_back (std::move (s));
                }

                return out;
            }

            void start (const InputSource& source, CaptureRing& ring) override
            {
                stop();

                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    status_ = InputStatus {};
                }

                const usbaudio::Scan scan = usbaudio::look (sysfsRoot());
                const usbaudio::Candidate* found = nullptr;

                for (const auto& c : scan.candidates)
                    if (idOf (c) == source.id)
                        found = &c;

                if (found == nullptr)
                {
                    finish (InputState::failed, "the input is gone");
                    return;
                }

                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    activeKey_ = found->key();
                }

                state_.store (InputState::starting, std::memory_order_release);
                worker_ = std::thread ([this, cand = *found, &ring] { run (cand, ring); });
            }

            void stop() override
            {
                stopRequested_.store (true);

                if (worker_.joinable())
                    worker_.join();

                stopRequested_.store (false);               // for the next start()

                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    activeKey_.clear();
                }

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

        private:
            std::filesystem::path sysfsRoot() const { return sysfs_.empty() ? usbsys::devicesDir() : sysfs_; }

            void finish (InputState state, const std::string& message)
            {
                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    status_.message = message;
                }

                state_.store (state, std::memory_order_release);
            }

            // The thread: opens the device, then keeps handling the transfers that finish until it is
            // asked to stop or the device goes away.
            void run (usbaudio::Candidate cand, CaptureRing& ring)
            {
                usbos::SystemUsbOs systemOs;
                usbos::UsbOs& os = os_ != nullptr ? *os_ : systemOs;
                const usbos::Logger logger = [this] (const std::string& line) { log (line); };

                std::string reason;
                auto port = usbaudio::openAudioPort (cand, os, logger, &reason);

                if (! port)
                {
                    finish (InputState::failed, reason);
                    return;
                }

                // The ring is set up before the state says "running": from then on the screen reads it.
                ring.configure (port->channels(), (int) (port->rate * kRingSeconds));

                {
                    std::lock_guard<std::mutex> lock (mutex_);
                    status_.channels = port->channels();
                    status_.sampleRate = port->rate;
                    status_.message = "listening";
                }

                state_.store (InputState::running, std::memory_order_release);

                const usbaudio::Deliver deliver = [&ring] (const float* sound, int frames) { ring.write (sound, frames); };
                bool alive = true;

                while (alive && ! stopRequested_.load())
                {
                    pollfd waiting { port->fd, POLLOUT, 0 };      // usbfs reports a finished transfer as "writable"
                    const int ready = ::poll (&waiting, 1, kPollMilliseconds);

                    if (ready < 0)
                    {
                        if (errno == EINTR)
                            continue;

                        port->why = std::string ("poll failed: ") + std::strerror (errno);
                        alive = false;
                    }
                    else if (ready > 0)
                    {
                        alive = usbaudio::serviceAudio (*port, deliver, logger, waiting.revents);
                    }

                    std::lock_guard<std::mutex> lock (mutex_);
                    status_.frames = port->frames;
                    status_.measuredRate = port->measuredRate();
                }

                logger ("audio: " + usbaudio::describe (cand) + ": " + (alive ? "closed" : "stopped (" + port->why + ")") + ": " + usbaudio::summary (*port));

                if (! alive)
                    finish (InputState::ended, port->why);      // after the last write to the ring
            }

            usbos::UsbOs* os_ = nullptr;
            std::filesystem::path sysfs_;

            mutable std::mutex mutex_;
            InputStatus status_;                                // guarded by mutex_
            std::string activeKey_;                             // guarded by mutex_: the device being read

            std::atomic<InputState> state_ { InputState::idle };
            std::atomic<bool> stopRequested_ { false };
            std::thread worker_;

            std::set<std::string> described_;                   // UI thread only
            std::set<std::string> told_;
        };
    }

    std::unique_ptr<InputBackend> makeUsbAudioInput (const UsbAudioInputOptions& options)
    {
        return std::make_unique<UsbAudioInput> (options);
    }
}

#else

namespace kerf::ui
{
    std::unique_ptr<InputBackend> makeUsbAudioInput (const UsbAudioInputOptions&)
    {
        return nullptr;
    }
}

#endif
