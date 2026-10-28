#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include "midi.h"

namespace kerf::ui
{
    namespace usbos { class UsbOs; }

    // Listens to every USB / class-compliant MIDI input and hands the channel messages to the
    // engine's MidiInbox (which the audio thread drains). Devices can be plugged in and out at
    // any time: a small background thread looks for changes about once a second.
    //
    //   Linux    raw MIDI device files (/dev/snd/midiC*D*), no ALSA library needed; and, for USB
    //            MIDI devices that the kernel has no driver for (muOS on the RG35XX H), Kerf's own
    //            reading through usbfs (see usb_midi.h)
    //   Windows  WinMM (midiIn*)
    //   others   does nothing (deviceCount() stays 0)
    class MidiInput
    {
    public:
        explicit MidiInput (kerf::MidiInbox& inbox);
        ~MidiInput();

        MidiInput (const MidiInput&) = delete;
        MidiInput& operator= (const MidiInput&) = delete;

        // These are set before start().
        void setUsbDirect (bool on) noexcept { usbEnabled_ = on; }
        void setLogger (std::function<void (const std::string&)> fn) { logger_ = std::move (fn); }
        void setUsbOs (usbos::UsbOs* os) noexcept { usbOs_ = os; }                                  // tests: a pretend kernel
        void setScanInterval (std::chrono::milliseconds every) noexcept { scanEvery_ = every; }       // how often the devices are looked for

        void start();
        void stop();

        int deviceCount() const noexcept { return devices_.load(); }              // every input being listened to
        int usbDirectCount() const noexcept { return usbDirect_.load(); }         // of which read by Kerf itself
        unsigned long long messageCount() const noexcept { return messages_.load(); }
        uint32_t lastMessage() const noexcept { return last_.load(); }            // status << 16 | data1 << 8 | data2, 0 = none yet

        // Used by the platform code.
        void deliver (const kerf::MidiMessage& m);
        void setDeviceCount (int n) noexcept { devices_.store (n); }
        void setUsbDirectCount (int n) noexcept { usbDirect_.store (n); }
        bool usbDirectEnabled() const noexcept { return usbEnabled_; }
        usbos::UsbOs* usbOs() const noexcept { return usbOs_; }
        std::chrono::milliseconds scanInterval() const noexcept { return scanEvery_; }
        void log (const std::string& line) const { if (logger_) logger_ (line); }

    private:
        struct Impl;

        kerf::MidiInbox& inbox_;
        std::unique_ptr<Impl> impl_;
        std::function<void (const std::string&)> logger_;
        bool usbEnabled_ = true;
        usbos::UsbOs* usbOs_ = nullptr;
        std::chrono::milliseconds scanEvery_ { 1000 };
        std::atomic<int> devices_ { 0 };
        std::atomic<int> usbDirect_ { 0 };
        std::atomic<unsigned long long> messages_ { 0 };
        std::atomic<uint32_t> last_ { 0 };
    };
}
