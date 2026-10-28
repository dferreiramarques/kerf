#include "midi_input.h"

#include <chrono>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "usb_midi.h"

#if defined(__linux__)
 #include <algorithm>
 #include <cerrno>
 #include <cstdio>
 #include <cstdlib>
 #include <cstring>
 #include <dirent.h>
 #include <fcntl.h>
 #include <poll.h>
 #include <sys/types.h>
 #include <unistd.h>

 #include "usb_midi_io.h"
#elif defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <mmsystem.h>
#endif

namespace kerf::ui
{
    void MidiInput::deliver (const kerf::MidiMessage& m)
    {
        inbox_.push (m);
        last_.store ((uint32_t) m.status << 16 | (uint32_t) m.data1 << 8 | (uint32_t) m.data2);
        messages_.fetch_add (1);
    }

    MidiInput::MidiInput (kerf::MidiInbox& inbox) : inbox_ (inbox) {}
    MidiInput::~MidiInput() { stop(); }

    struct MidiInput::Impl
    {
        std::atomic<bool> running { false };
        std::thread thread;
    };

    namespace
    {
        using namespace std::chrono_literals;

#if defined(__linux__)

        // ------------------------------------------------------------------ Linux: raw MIDI nodes

        struct Port
        {
            int fd = -1;
            std::string path;
            kerf::MidiParser parser;
        };

        std::vector<std::string> findNodes()
        {
            std::vector<std::string> out;

            if (DIR* dir = opendir ("/dev/snd"))
            {
                while (const dirent* e = readdir (dir))
                {
                    int card = 0, device = 0;
                    char tail = 0;

                    // "midiC1D0" and nothing after it
                    if (std::sscanf (e->d_name, "midiC%dD%d%c", &card, &device, &tail) == 2)
                        out.push_back (std::string ("/dev/snd/") + e->d_name);
                }

                closedir (dir);
            }

            // Extra raw-MIDI sources (a FIFO, a pty from a virtual MIDI cable, ...), separated by ':'.
            if (const char* extra = std::getenv ("KERF_MIDI_DEVICE"))
            {
                std::string all = extra;
                size_t start = 0;

                while (start <= all.size())
                {
                    const auto stop = all.find (':', start);
                    const auto item = all.substr (start, stop == std::string::npos ? std::string::npos : stop - start);

                    if (! item.empty())
                        out.push_back (item);

                    if (stop == std::string::npos)
                        break;

                    start = stop + 1;
                }
            }

            return out;
        }

        // -------------------------------------------------- Linux: USB MIDI the kernel has no driver for
        //
        // Claiming the interface and queueing the transfers is in usb_midi_io.h; this part decides
        // which devices to open, and when to give up on one.

        constexpr int kUsbTries = 4;            // a device is opened at most 4 times (the first time and 3 more after it stopped)

        using UsbPorts = std::vector<std::unique_ptr<usbmidi::UsbPort>>;

        void scanUsb (MidiInput& self, usbos::UsbOs& os, UsbPorts& usb, std::map<std::string, int>& attempts, std::set<std::string>& described)
        {
            const usbos::Logger log = [&self] (const std::string& line) { self.log (line); };

            const auto looked = usbmidi::look (usbsys::devicesDir());
            std::set<std::string> present, beingRead;

            for (const auto& p : usb)
                beingRead.insert (p->cand.key());

            // One line about every USB device, so that the log tells why a keyboard is, or is not, picked up.
            for (const auto& d : looked.devices)
            {
                if (described.insert (d.key()).second)
                    log ("midi: usb " + d.name + " (" + usbos::hex ((unsigned) d.vendor, 4) + ":" + usbos::hex ((unsigned) d.product, 4) + " " + d.title
                         + "): interfaces (class.subclass) " + d.interfaces + ((d.midi || ! d.readable) ? "" : ": no MIDI input"));
            }

            for (const auto& c : looked.candidates)
            {
                const auto key = c.key();
                present.insert (key);
                const auto entry = attempts.try_emplace (key, 0);
                int& tries = entry.first->second;

                if (beingRead.count (key) != 0 || tries >= kUsbTries)
                    continue; // already open, or given up on

                if (! c.driver.empty())
                {
                    log ("midi: " + usbmidi::describe (c) + ": the kernel driver '" + c.driver + "' has the MIDI interface"
                         + (c.driver == "usbfs" ? " (another program is reading it)" : ", Kerf uses its raw MIDI node"));
                    tries = kUsbTries;
                    continue;
                }

                // Seen for the first time: wait for the next look. A kernel that has a driver binds
                // it a moment after the device appears, and then Kerf should leave the device to it.
                if (entry.second)
                    continue;

                if (++tries == 1)
                {
                    std::string inputs;

                    for (const auto& e : c.stream.inputs)
                        inputs += (inputs.empty() ? "" : ", ") + std::string ("0x") + usbos::hex (e.address, 2) + " (" + std::to_string (e.maxPacket & 0x7FF) + " bytes)";

                    log ("midi: " + usbmidi::describe (c) + ": MIDI interface " + std::to_string (c.stream.interfaceNumber)
                         + ", input endpoint " + inputs + ", no kernel driver: reading it directly");
                }
                else
                {
                    log ("midi: " + usbmidi::describe (c) + ": trying again (" + std::to_string (tries) + " of " + std::to_string (kUsbTries) + ")");
                }

                if (auto port = usbmidi::openUsbPort (c, os, log))
                {
                    log ("midi: " + usbmidi::describe (c) + ": ready");
                    usb.push_back (std::move (port));
                }
            }

            // Forget what is gone, so that a device plugged in again is looked at again.
            for (auto it = attempts.begin(); it != attempts.end();)
                it = present.count (it->first) != 0 ? std::next (it) : attempts.erase (it);
        }

        void runLoop (MidiInput& self, const std::atomic<bool>& running)
        {
            usbos::SystemUsbOs systemOs;
            usbos::UsbOs& os = self.usbOs() != nullptr ? *self.usbOs() : systemOs;
            const usbmidi::Deliver deliver = [&self] (const kerf::MidiMessage& m) { self.deliver (m); };
            const usbos::Logger log = [&self] (const std::string& line) { self.log (line); };

            std::vector<Port> ports;
            UsbPorts usb;
            std::map<std::string, int> attempts;
            std::set<std::string> described;
            auto lastScan = std::chrono::steady_clock::now();
            bool scanNow = true;

            self.log (std::string ("midi: listening to the raw MIDI devices; reading USB MIDI devices that have no kernel driver is ")
                      + (self.usbDirectEnabled() ? "on" : "off (usb_midi = 0)"));

            auto updateCounts = [&]
            {
                self.setDeviceCount ((int) (ports.size() + usb.size()));
                self.setUsbDirectCount ((int) usb.size());
            };

            while (running.load())
            {
                const auto now = std::chrono::steady_clock::now();

                if (scanNow || now - lastScan >= self.scanInterval())
                {
                    scanNow = false;
                    lastScan = now;

                    for (const auto& path : findNodes())
                    {
                        bool known = false;

                        for (const auto& p : ports)
                            known = known || p.path == path;

                        if (known)
                            continue;

                        const int fd = ::open (path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);

                        if (fd >= 0)
                        {
                            Port p;
                            p.fd = fd;
                            p.path = path;
                            ports.push_back (std::move (p));
                        }
                    }

                    if (self.usbDirectEnabled())
                        scanUsb (self, os, usb, attempts, described);

                    updateCounts();
                }

                const size_t portCount = ports.size();
                const size_t usbCount = usb.size();
                std::vector<pollfd> fds;
                fds.reserve (portCount + usbCount);

                for (const auto& p : ports)
                    fds.push_back ({ p.fd, POLLIN, 0 });

                for (const auto& u : usb)
                    fds.push_back ({ u->fd, POLLOUT, 0 }); // usbfs reports a finished transfer as "writable"

                // Wake up for the next look at the devices at the latest. With nothing open this is a short sleep.
                const auto untilScan = std::chrono::duration_cast<std::chrono::milliseconds> (lastScan + self.scanInterval() - std::chrono::steady_clock::now());
                const int timeout = (int) std::clamp<long long> (untilScan.count(), 1, 100);
                const int ready = ::poll (fds.empty() ? nullptr : fds.data(), (nfds_t) fds.size(), timeout);

                if (ready <= 0)
                    continue;

                bool anyDead = false;
                std::vector<bool> dead (portCount, false);

                for (size_t i = 0; i < portCount; ++i)
                {
                    auto& p = ports[i];
                    const short ev = fds[i].revents;

                    if (ev & POLLIN)
                    {
                        uint8_t buf[256];

                        for (;;)
                        {
                            const ssize_t got = ::read (p.fd, buf, sizeof buf);

                            if (got > 0)
                            {
                                kerf::MidiMessage m;

                                for (ssize_t k = 0; k < got; ++k)
                                    if (p.parser.feed (buf[k], m))
                                        self.deliver (m);

                                if (got < (ssize_t) sizeof buf)
                                    break;
                            }
                            else
                            {
                                if (got == 0 || (errno != EAGAIN && errno != EINTR))
                                    dead[i] = true;

                                break;
                            }
                        }
                    }

                    if ((ev & (POLLERR | POLLNVAL)) || ((ev & POLLHUP) && ! (ev & POLLIN)))
                        dead[i] = true;

                    anyDead = anyDead || dead[i];
                }

                std::vector<bool> usbGone (usbCount, false);

                for (size_t j = 0; j < usbCount; ++j)
                {
                    const short ev = fds[portCount + j].revents;

                    if (ev != 0 && ! usbmidi::serviceUsb (*usb[j], deliver, log, ev))
                    {
                        usbGone[j] = true;
                        anyDead = true;
                    }
                }

                if (anyDead)
                {
                    for (size_t i = ports.size(); i-- > 0;)
                    {
                        if (i < portCount && dead[i])
                        {
                            ::close (ports[i].fd);
                            ports.erase (ports.begin() + (std::ptrdiff_t) i);
                        }
                    }

                    for (size_t j = usb.size(); j-- > 0;)
                    {
                        if (j < usbCount && usbGone[j])
                        {
                            self.log ("midi: " + usbmidi::describe (usb[j]->cand) + ": " + (usb[j]->why.empty() ? std::string ("stopped") : usb[j]->why));
                            usb.erase (usb.begin() + (std::ptrdiff_t) j);
                        }
                    }

                    updateCounts();
                }
            }

            for (auto& p : ports)
                ::close (p.fd);

            usb.clear();
            self.setDeviceCount (0);
            self.setUsbDirectCount (0);
        }

#elif defined(_WIN32)

        // ------------------------------------------------------------------------ Windows: WinMM

        void CALLBACK midiProc (HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR p1, DWORD_PTR)
        {
            if (msg != MIM_DATA)
                return;

            const auto packed = (uint32_t) p1;
            const auto status = (uint8_t) (packed & 0xFF);

            if (status < 0x80 || status >= 0xF0)
                return; // only channel messages matter

            kerf::MidiMessage m;
            m.status = status;
            m.data1 = (uint8_t) ((packed >> 8) & 0x7F);
            m.data2 = (uint8_t) ((packed >> 16) & 0x7F);
            reinterpret_cast<MidiInput*> (instance)->deliver (m);
        }

        void runLoop (MidiInput& self, const std::atomic<bool>& running)
        {
            std::vector<HMIDIIN> open;
            std::vector<std::string> lastNames;
            bool first = true;
            int retryTick = 0;

            auto closeAll = [&]
            {
                for (auto h : open)
                {
                    midiInStop (h);
                    midiInReset (h);
                    midiInClose (h);
                }

                open.clear();
            };

            while (running.load())
            {
                const UINT count = midiInGetNumDevs();
                std::vector<std::string> names;

                for (UINT i = 0; i < count; ++i)
                {
                    MIDIINCAPSA caps {};
                    names.push_back (midiInGetDevCapsA (i, &caps, sizeof caps) == MMSYSERR_NOERROR ? std::string (caps.szPname) : std::string ("?"));
                }

                // Reopen everything when the device list changed, and retry now and then when a
                // device was busy (another program had it open).
                const bool changed = first || names != lastNames;
                const bool retry = ! changed && open.size() < count && (++retryTick % 3) == 0;

                if (changed || retry)
                {
                    first = false;
                    lastNames = names;
                    closeAll();

                    for (UINT i = 0; i < count; ++i)
                    {
                        HMIDIIN h = nullptr;

                        if (midiInOpen (&h, i, (DWORD_PTR) &midiProc, (DWORD_PTR) &self, CALLBACK_FUNCTION) == MMSYSERR_NOERROR)
                        {
                            midiInStart (h);
                            open.push_back (h);
                        }
                    }

                    self.setDeviceCount ((int) open.size());
                }

                for (int i = 0; i < 10 && running.load(); ++i)
                    std::this_thread::sleep_for (100ms);
            }

            closeAll();
            self.setDeviceCount (0);
        }

#else

        // ------------------------------------------------------------------------------- others

        void runLoop (MidiInput&, const std::atomic<bool>& running)
        {
            while (running.load())
                std::this_thread::sleep_for (200ms);
        }

#endif
    }

    void MidiInput::start()
    {
        if (impl_)
            return;

        impl_ = std::make_unique<Impl>();
        impl_->running = true;
        impl_->thread = std::thread ([this] { runLoop (*this, impl_->running); });
    }

    void MidiInput::stop()
    {
        if (! impl_)
            return;

        impl_->running = false;

        if (impl_->thread.joinable())
            impl_->thread.join();

        impl_.reset();
    }
}
