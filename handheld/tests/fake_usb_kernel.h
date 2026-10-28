#pragma once

// A pretend Linux kernel for the tests of the direct USB readers (MIDI and audio).
//
// It stands in for usbfs: it keeps the transfers that the program queues, completes them when the
// test says so, makes the file "writable" the way usbfs does, and can fail, stall or unplug. What
// the real kernel does is written down in usb_midi_io.h and usb_audio_io.h; the pretend one follows
// that.

#if defined(__linux__)

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <atomic>
#include <fcntl.h>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include "usb_os.h"

namespace kerf::ui::usbfake
{
    using Bytes = std::vector<uint8_t>;

    // ------------------------------------------------------------------------------ a thread-safe log

    struct Lines
    {
        std::mutex mutex;
        std::vector<std::string> lines;

        void add (const std::string& line)
        {
            std::lock_guard<std::mutex> lock (mutex);
            lines.push_back (line);
        }

        usbos::Logger logger() { return [this] (const std::string& line) { add (line); }; }

        int count (const std::string& part)
        {
            std::lock_guard<std::mutex> lock (mutex);
            return (int) std::count_if (lines.begin(), lines.end(), [&] (const std::string& l) { return l.find (part) != std::string::npos; });
        }

        bool has (const std::string& part) { return count (part) > 0; }
    };

    template <typename Predicate>
    bool waitFor (Predicate done, int timeoutMs = 5000)
    {
        const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds (timeoutMs);

        while (std::chrono::steady_clock::now() < end)
        {
            if (done())
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }

        return done();
    }

    // ------------------------------------------------------------------------------- the pretend kernel

    // What a control transfer looked like, for the tests to look at.
    struct ControlCall
    {
        int type = 0;
        int request = 0;
        int value = 0;
        int index = 0;
        int length = 0;
        Bytes sent;                             // the data of a transfer to the device
    };

    // One pretend USB device, as usbfs shows it to a program that has opened it.
    struct FakeDevice
    {
        std::string path;                       // "/dev/bus/usb/004/002"
        std::map<int, int> endpoints;           // address -> USBDEVFS_URB_TYPE_*
        std::set<unsigned> interfaces { 1 };    // the interfaces the device has
        std::map<int, unsigned> endpointInterface;  // address -> its interface (the lowest one when not listed)
        std::map<int, std::set<unsigned>> endpointAlt;  // address -> the alternate settings it exists in (every one when not listed)

        bool nodeExists = true;                 // false: /dev has no entry for the device yet
        bool connected = true;
        int openError = 0;                      // errno that open() fails with, 0 = works
        int claimError = 0;                     // errno of every claim ...
        std::map<unsigned, int> claimErrors;    // ... or of the claim of one interface
        int setInterfaceError = 0;              // errno of every change of setting ...
        std::map<std::pair<unsigned, unsigned>, int> altErrors;     // ... or of one (interface, setting)
        int submitError = 0;                    // errno of the submits that fail ...
        int submitsAllowed = -1;                // ... once this many have worked; -1 = all work
        int submitsFailing = -1;                // ... and this many fail before the kernel takes transfers again; -1 = all the rest
        bool makeNodeFails = false;
        bool completeAtOnce = false;            // every transfer finishes the moment it is queued (an endless stream)
        int isoMaxPacket = 0;                   // isochronous packets longer than this are refused (EMSGSIZE); 0 = no limit

        // Answers control transfers: the number of bytes moved, or -errno. Without it the device stalls.
        std::function<int (usbdevfs_ctrltransfer&)> onControl;

        int fd = -1;                            // the file the program polls while it has the device open (a pipe's write end)
        int readEnd = -1;                       // the other end of that pipe; the pretend kernel empties and fills it
        std::set<unsigned> claimedInterfaces;
        std::map<unsigned, unsigned> alt;       // interface -> the alternate setting it is in (0 at first)
        int autoClaims = 0;                     // interfaces that the kernel had to claim for the program, which had not
        int opens = 0;
        long nextFrame = 0;
        int lastFlags = 0;                      // flags of the last isochronous transfer queued
        std::deque<usbdevfs_urb*> pending;      // queued, not finished
        std::deque<usbdevfs_urb*> completed;    // finished, not yet fetched
        std::vector<std::string> calls;         // what the program did, in order
        std::vector<ControlCall> controls;
    };

    class FakeKernel : public usbos::UsbOs
    {
    public:
        std::mutex mutex;
        std::map<std::string, std::unique_ptr<FakeDevice>> devices;
        std::vector<std::string> awake;
        std::vector<std::string> madeNodes;

        FakeDevice& add (int bus, int number, std::map<int, int> endpoints = { { 0x81, USBDEVFS_URB_TYPE_BULK } })
        {
            auto device = std::make_unique<FakeDevice>();
            char path[96];
            std::snprintf (path, sizeof path, "/dev/bus/usb/%03d/%03d", bus, number);
            device->path = path;
            device->endpoints = std::move (endpoints);
            auto& ref = *device;
            std::lock_guard<std::mutex> lock (mutex);
            devices[path] = std::move (device);
            return ref;
        }

        // ---- UsbOs

        int open (const char* path, int flags) override
        {
            std::lock_guard<std::mutex> lock (mutex);
            const auto it = devices.find (path);

            if (it == devices.end() || ! it->second->nodeExists)
            {
                errno = ENOENT;
                return -1;
            }

            FakeDevice& d = *it->second;
            ++d.opens;
            d.calls.push_back ("open");

            if (d.openError != 0) { errno = d.openError; return -1; }
            if (! d.connected) { errno = ENODEV; return -1; }
            if ((flags & O_ACCMODE) != O_RDWR) { errno = EINVAL; return -1; } // usbfs only reports finished transfers to a file opened for writing

            int ends[2];

            if (::pipe2 (ends, O_NONBLOCK | O_CLOEXEC) != 0)
                return -1;

            d.readEnd = ends[0];
            d.fd = ends[1];
            signal (d, false);
            return d.fd;
        }

        int close (int fd) override
        {
            std::lock_guard<std::mutex> lock (mutex);
            FakeDevice* d = byFd (fd);

            if (d == nullptr) { errno = EBADF; return -1; }

            d->calls.push_back ("close");
            ::close (d->fd);

            if (d->readEnd >= 0)
                ::close (d->readEnd);

            d->fd = -1;
            d->readEnd = -1;
            d->claimedInterfaces.clear();   // closing the file releases whatever it claimed
            d->alt.clear();
            d->pending.clear();
            d->completed.clear();
            return 0;
        }

        int ioctl (int fd, unsigned long request, void* argument) override
        {
            std::lock_guard<std::mutex> lock (mutex);
            FakeDevice* d = byFd (fd);

            if (d == nullptr) { errno = EBADF; return -1; }

            // Fetching finished transfers is allowed after the device has gone; nothing else is.
            if (request == USBDEVFS_REAPURBNDELAY)
            {
                if (! d->completed.empty())
                {
                    *static_cast<usbdevfs_urb**> (argument) = d->completed.front();
                    d->completed.pop_front();
                    d->calls.push_back ("reap");

                    if (d->completed.empty() && d->connected)
                        signal (*d, false);

                    return 0;
                }

                errno = d->connected ? EAGAIN : ENODEV;
                return -1;
            }

            if (! d->connected) { errno = ENODEV; return -1; }

            switch (request)
            {
                case USBDEVFS_CLAIMINTERFACE:
                {
                    const unsigned number = *static_cast<unsigned*> (argument);
                    d->calls.push_back ("claim " + std::to_string (number));

                    if (d->claimError != 0) { errno = d->claimError; return -1; }

                    const auto failing = d->claimErrors.find (number);

                    if (failing != d->claimErrors.end()) { errno = failing->second; return -1; }
                    if (d->claimedInterfaces.count (number) != 0) return 0;        // already the program's own
                    if (d->interfaces.count (number) == 0) { errno = ENOENT; return -1; }

                    d->claimedInterfaces.insert (number);
                    return 0;
                }

                case USBDEVFS_RELEASEINTERFACE:
                {
                    const unsigned number = *static_cast<unsigned*> (argument);
                    d->calls.push_back ("release " + std::to_string (number));

                    if (d->claimedInterfaces.erase (number) == 0) { errno = EINVAL; return -1; }

                    d->alt[number] = 0;                 // the kernel puts the interface back to setting 0
                    forget (*d, number);
                    return 0;
                }

                case USBDEVFS_SETINTERFACE:
                {
                    const auto* setting = static_cast<usbdevfs_setinterface*> (argument);
                    d->calls.push_back ("alt " + std::to_string (setting->interface) + " " + std::to_string (setting->altsetting));

                    if (d->claimedInterfaces.count (setting->interface) == 0) { errno = EINVAL; return -1; }
                    if (d->setInterfaceError != 0) { errno = d->setInterfaceError; return -1; }

                    const auto failing = d->altErrors.find ({ setting->interface, setting->altsetting });

                    if (failing != d->altErrors.end()) { errno = failing->second; return -1; }

                    d->alt[setting->interface] = setting->altsetting;
                    cancel (*d, setting->interface, -ESHUTDOWN);       // the endpoints of the old setting are disabled
                    return 0;
                }

                case USBDEVFS_SUBMITURB:
                {
                    auto* urb = static_cast<usbdevfs_urb*> (argument);
                    const char* kind = urb->type == USBDEVFS_URB_TYPE_ISO ? "iso " : (urb->type == USBDEVFS_URB_TYPE_BULK ? "bulk " : "interrupt ");
                    d->calls.push_back ("submit " + usbos::hex (urb->endpoint, 2) + " " + kind + std::to_string (urb->buffer_length));

                    const unsigned owner = interfaceOf (*d, urb->endpoint);

                    if (d->claimedInterfaces.count (owner) == 0) { errno = EHOSTUNREACH; return -1; }
                    if (d->submitsAllowed == 0)
                    {
                        if (d->submitsFailing > 0 && --d->submitsFailing == 0)
                            d->submitsAllowed = -1;             // that was the last one that fails: the kernel takes transfers again

                        errno = d->submitError != 0 ? d->submitError : EIO;
                        return -1;
                    }

                    if (d->submitsAllowed > 0) --d->submitsAllowed;

                    const auto endpoint = d->endpoints.find (urb->endpoint);

                    if (endpoint == d->endpoints.end()) { errno = ENOENT; return -1; }
                    if (! inCurrentSetting (*d, urb->endpoint)) { errno = ENOENT; return -1; }
                    if (endpoint->second != urb->type || urb->buffer == nullptr) { errno = EINVAL; return -1; }

                    if (urb->type == USBDEVFS_URB_TYPE_ISO)
                    {
                        // The kernel's own limits (devio.c): 1 to 128 packets, each at most 49152 bytes, and it takes the
                        // length of the buffer from their sum, so the program's buffer has to be at least that long.
                        if (urb->number_of_packets < 1 || urb->number_of_packets > 128) { errno = EINVAL; return -1; }

                        long total = 0;

                        for (int i = 0; i < urb->number_of_packets; ++i)
                        {
                            const long length = (long) urb->iso_frame_desc[i].length;

                            if (length > 49152) { errno = EINVAL; return -1; }
                            if (d->isoMaxPacket > 0 && length > d->isoMaxPacket) { errno = EMSGSIZE; return -1; }

                            total += length;
                        }

                        if (total > (long) urb->buffer_length) { errno = EINVAL; return -1; }
                        if ((urb->flags & USBDEVFS_URB_ISO_ASAP) == 0) { errno = EINVAL; return -1; }     // starting at a frame number is not supported here

                        d->lastFlags = (int) urb->flags;
                        urb->buffer_length = (int) total;
                    }
                    else if (urb->buffer_length <= 0)
                    {
                        errno = EINVAL;
                        return -1;
                    }

                    if (d->completeAtOnce)
                    {
                        urb->status = 0;
                        urb->actual_length = 0;
                        d->completed.push_back (urb);
                        signal (*d, true);
                    }
                    else
                    {
                        d->pending.push_back (urb);
                    }

                    return 0;
                }

                case USBDEVFS_CLEAR_HALT:
                    d->calls.push_back ("halt " + usbos::hex (*static_cast<unsigned*> (argument), 2));
                    return 0;

                case USBDEVFS_CONTROL:
                    return control (*d, *static_cast<usbdevfs_ctrltransfer*> (argument));

                default:
                    errno = ENOTTY;
                    return -1;
            }
        }

        int makeNode (int bus, int device, int major, int minor) override
        {
            std::lock_guard<std::mutex> lock (mutex);
            char path[96];
            std::snprintf (path, sizeof path, "/dev/bus/usb/%03d/%03d", bus, device);
            madeNodes.push_back (std::string (path) + " " + std::to_string (major) + ":" + std::to_string (minor));
            const auto it = devices.find (path);

            if (it == devices.end() || it->second->makeNodeFails) { errno = EPERM; return -1; }

            it->second->nodeExists = true;
            return 0;
        }

        void keepAwake (const std::string& deviceName) override
        {
            std::lock_guard<std::mutex> lock (mutex);
            awake.push_back (deviceName);
        }

        // ---- what a test does to the device

        // The device sends `bytes`: the oldest transfer waiting on the endpoint finishes with them.
        bool deliver (FakeDevice& d, const Bytes& bytes, int endpoint = 0x81)
        {
            std::lock_guard<std::mutex> lock (mutex);
            return finish (d, endpoint, 0, &bytes);
        }

        // The oldest transfer waiting on the endpoint fails with -errnoValue.
        bool fail (FakeDevice& d, int errnoValue, int endpoint = 0x81)
        {
            std::lock_guard<std::mutex> lock (mutex);
            return finish (d, endpoint, -errnoValue, nullptr);
        }

        // The device sends one isochronous transfer's worth: packets[i] is what packet i carries
        // (a packet that is missing carries nothing) and statuses[i] its result (missing = fine).
        // The oldest transfer waiting on the endpoint finishes with that.
        bool deliverIso (FakeDevice& d, const std::vector<Bytes>& packets, int endpoint = 0x82, const std::vector<int>& statuses = {})
        {
            std::lock_guard<std::mutex> lock (mutex);
            return finishIso (d, endpoint, packets, statuses);
        }

        // The same, with the packets made by make (i), as many as the transfer has.
        bool deliverIsoWith (FakeDevice& d, const std::function<Bytes (int)>& make, int endpoint = 0x82)
        {
            std::lock_guard<std::mutex> lock (mutex);

            for (const usbdevfs_urb* urb : d.pending)
            {
                if (urb->endpoint != endpoint || urb->type != USBDEVFS_URB_TYPE_ISO)
                    continue;

                std::vector<Bytes> packets;

                for (int i = 0; i < urb->number_of_packets; ++i)
                    packets.push_back (make (i));

                return finishIso (d, endpoint, packets, {});
            }

            return false;
        }

        void unplug (FakeDevice& d)
        {
            std::lock_guard<std::mutex> lock (mutex);
            d.connected = false;
            d.pending.clear();
            d.completed.clear();

            if (d.fd >= 0)
            {
                signal (d, true);

                // usbfs reports a device that has gone as an error on the file; a pipe whose
                // other end is closed does the same.
                ::close (d.readEnd);
                d.readEnd = -1;
            }
        }

        // The file reports an error, as usbfs's does for a device that has gone, while the device itself
        // goes on answering what is asked of it: a program must not wait for the answers to find out.
        void hangUp (FakeDevice& d)
        {
            std::lock_guard<std::mutex> lock (mutex);

            if (d.readEnd >= 0)
            {
                ::close (d.readEnd);
                d.readEnd = -1;
            }
        }

        int pendingCount (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return (int) d.pending.size(); }
        int completedCount (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return (int) d.completed.size(); }       // finished, not yet fetched

        // Changes the transfer that finished last, before the program fetches it (to make the kernel
        // report something odd). False when none is waiting to be fetched.
        bool editLastFinished (FakeDevice& d, const std::function<void (usbdevfs_urb&)>& change)
        {
            std::lock_guard<std::mutex> lock (mutex);

            if (d.completed.empty())
                return false;

            change (*d.completed.back());
            return true;
        }

        bool isClaimed (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return ! d.claimedInterfaces.empty(); }
        bool isClaimed (FakeDevice& d, unsigned number) { std::lock_guard<std::mutex> lock (mutex); return d.claimedInterfaces.count (number) != 0; }
        unsigned altOf (FakeDevice& d, unsigned number) { std::lock_guard<std::mutex> lock (mutex); return d.alt.count (number) != 0 ? d.alt[number] : 0u; }
        int autoClaimCount (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return d.autoClaims; }
        int openCount (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return d.opens; }
        bool isOpen (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return d.fd >= 0; }
        int lastFlags (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return d.lastFlags; }
        std::vector<std::string> callsOf (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return d.calls; }
        std::vector<ControlCall> controlsOf (FakeDevice& d) { std::lock_guard<std::mutex> lock (mutex); return d.controls; }

        // The packets (number and length of each) of the oldest transfer waiting on the endpoint; 0 if none.
        int waitingPackets (FakeDevice& d, int endpoint, int* packetLength = nullptr)
        {
            std::lock_guard<std::mutex> lock (mutex);

            for (const usbdevfs_urb* urb : d.pending)
                if (urb->endpoint == endpoint && urb->type == USBDEVFS_URB_TYPE_ISO)
                {
                    if (packetLength != nullptr)
                        *packetLength = (int) urb->iso_frame_desc[0].length;

                    return urb->number_of_packets;
                }

            return 0;
        }

        int callCount (FakeDevice& d, const std::string& prefix)
        {
            std::lock_guard<std::mutex> lock (mutex);
            return (int) std::count_if (d.calls.begin(), d.calls.end(), [&] (const std::string& c) { return c.rfind (prefix, 0) == 0; });
        }

    private:
        FakeDevice* byFd (int fd)
        {
            for (auto& entry : devices)
                if (entry.second->fd == fd && fd >= 0)
                    return entry.second.get();

            return nullptr;
        }

        static unsigned interfaceOf (const FakeDevice& d, int endpoint)
        {
            const auto it = d.endpointInterface.find (endpoint);

            if (it != d.endpointInterface.end())
                return it->second;

            return d.interfaces.empty() ? 0u : *d.interfaces.begin();
        }

        // Is the endpoint part of the setting its interface is in?
        static bool inCurrentSetting (const FakeDevice& d, int endpoint)
        {
            const auto needed = d.endpointAlt.find (endpoint);

            if (needed == d.endpointAlt.end())
                return true;

            const auto now = d.alt.find (interfaceOf (d, endpoint));
            return needed->second.count (now == d.alt.end() ? 0u : now->second) != 0;
        }

        // The transfers queued on the endpoints of an interface end with `status`.
        static void cancel (FakeDevice& d, unsigned number, int status)
        {
            bool any = false;

            for (auto it = d.pending.begin(); it != d.pending.end();)
            {
                usbdevfs_urb* urb = *it;

                if (interfaceOf (d, urb->endpoint) != number)
                {
                    ++it;
                    continue;
                }

                urb->status = status;
                urb->actual_length = 0;
                d.completed.push_back (urb);
                it = d.pending.erase (it);
                any = true;
            }

            if (any)
                signal (d, true);
        }

        // Releasing an interface throws away its transfers, finished or not.
        static void forget (FakeDevice& d, unsigned number)
        {
            const auto gone = [&] (const usbdevfs_urb* urb) { return interfaceOf (d, urb->endpoint) == number; };
            std::erase_if (d.pending, gone);
            std::erase_if (d.completed, gone);

            if (d.completed.empty() && d.connected)
                signal (d, false);
        }

        // A control transfer. The kernel wants the interface that a class or standard request is
        // aimed at (or the interface of the endpoint it is aimed at) to be claimed, and claims it
        // itself, with a warning, when the program has not.
        int control (FakeDevice& d, usbdevfs_ctrltransfer& t)
        {
            ControlCall call;
            call.type = t.bRequestType;
            call.request = t.bRequest;
            call.value = t.wValue;
            call.index = t.wIndex;
            call.length = t.wLength;

            if ((t.bRequestType & 0x80) == 0 && t.data != nullptr)
                call.sent.assign (static_cast<const uint8_t*> (t.data), static_cast<const uint8_t*> (t.data) + t.wLength);

            d.controls.push_back (call);
            d.calls.push_back ("control " + usbos::hex (call.type, 2) + " " + usbos::hex (call.request, 2) + " " + usbos::hex (call.value, 4)
                               + " " + usbos::hex (call.index, 4) + " " + std::to_string (call.length));

            if (t.wLength > 4096) { errno = EINVAL; return -1; }

            const int kind = (t.bRequestType >> 5) & 3;
            const int recipient = t.bRequestType & 0x1F;

            if (kind != 2 && (recipient == 1 || recipient == 2))
            {
                long owner = -1;

                if (recipient == 1)
                {
                    owner = t.wIndex & 0xFF;
                }
                else if ((t.wIndex & 0x7F) != 0)
                {
                    int endpoint = t.wIndex & 0xFF;

                    if (d.endpoints.count (endpoint) == 0 || ! inCurrentSetting (d, endpoint))
                        endpoint ^= 0x80;       // some programs forget the direction bit, and the kernel lets it through

                    if (d.endpoints.count (endpoint) == 0 || ! inCurrentSetting (d, endpoint)) { errno = ENOENT; return -1; }

                    owner = interfaceOf (d, endpoint);
                }

                if (owner >= 0)
                {
                    if (d.interfaces.count ((unsigned) owner) == 0) { errno = ENOENT; return -1; }

                    if (d.claimedInterfaces.count ((unsigned) owner) == 0)
                    {
                        ++d.autoClaims;
                        d.claimedInterfaces.insert ((unsigned) owner);
                        d.calls.push_back ("autoclaim " + std::to_string (owner));
                    }
                }
            }

            if (! d.onControl) { errno = EPIPE; return -1; }

            const int result = d.onControl (t);

            if (result < 0) { errno = -result; return -1; }

            return result;
        }

        // usbfs says "a transfer has finished" by making the file writable (POLLOUT) and never says
        // anything else about it (never POLLIN). The write end of a pipe does the same: writable
        // while the pipe has room, and reading it is never reported. So "nothing has finished" is a
        // full pipe, and "something has" an empty one.
        static void signal (FakeDevice& d, bool finished)
        {
            if (d.readEnd < 0)
                return;

            if (finished)
            {
                char sink[4096];

                while (::read (d.readEnd, sink, sizeof sink) > 0)
                {
                }
            }
            else
            {
                static const char block[4096] = {};

                while (::write (d.fd, block, sizeof block) > 0)
                {
                }
            }
        }

        bool finish (FakeDevice& d, int endpoint, int status, const Bytes* bytes)
        {
            if (! d.connected || d.fd < 0)
                return false;

            for (auto it = d.pending.begin(); it != d.pending.end(); ++it)
            {
                usbdevfs_urb* urb = *it;

                if (urb->endpoint != endpoint)
                    continue;

                urb->status = status;
                urb->actual_length = 0;

                if (bytes != nullptr)
                {
                    const size_t n = std::min<size_t> (bytes->size(), (size_t) urb->buffer_length);
                    std::copy (bytes->begin(), bytes->begin() + (std::ptrdiff_t) n, static_cast<uint8_t*> (urb->buffer));
                    urb->actual_length = (int) n;
                }

                d.pending.erase (it);
                d.completed.push_back (urb);
                signal (d, true);
                return true;
            }

            return false;
        }

        bool finishIso (FakeDevice& d, int endpoint, const std::vector<Bytes>& packets, const std::vector<int>& statuses)
        {
            if (! d.connected || d.fd < 0)
                return false;

            for (auto it = d.pending.begin(); it != d.pending.end(); ++it)
            {
                usbdevfs_urb* urb = *it;

                if (urb->endpoint != endpoint || urb->type != USBDEVFS_URB_TYPE_ISO)
                    continue;

                // Packet i sits at the sum of the lengths of the packets before it, whatever it carried.
                size_t offset = 0;
                int total = 0, errors = 0;

                for (int i = 0; i < urb->number_of_packets; ++i)
                {
                    auto& packet = urb->iso_frame_desc[i];
                    const int status = (size_t) i < statuses.size() ? statuses[(size_t) i] : 0;
                    packet.actual_length = 0;
                    packet.status = (unsigned) status;

                    if (status != 0)
                    {
                        ++errors;
                    }
                    else if ((size_t) i < packets.size())
                    {
                        const size_t n = std::min<size_t> (packets[(size_t) i].size(), packet.length);
                        std::copy (packets[(size_t) i].begin(), packets[(size_t) i].begin() + (std::ptrdiff_t) n, static_cast<uint8_t*> (urb->buffer) + offset);
                        packet.actual_length = (unsigned) n;
                        total += (int) n;
                    }

                    offset += packet.length;
                }

                urb->status = 0;
                urb->actual_length = total;
                urb->error_count = errors;
                urb->start_frame = (int) d.nextFrame;
                d.nextFrame += urb->number_of_packets;

                d.pending.erase (it);
                d.completed.push_back (urb);
                signal (d, true);
                return true;
            }

            return false;
        }
    };

    // A pretend isochronous device that keeps sending: a thread that, every `everyMs`, finishes the
    // oldest waiting transfer of the endpoint with the packets that make (n) gives, n counting the
    // packets from 0 without end. It stops when it is destroyed.
    class IsoPump
    {
    public:
        IsoPump (FakeKernel& kernel, FakeDevice& device, int endpoint, std::function<Bytes (long)> make, int everyMs = 8)
            : kernel_ (kernel), device_ (device), endpoint_ (endpoint), make_ (std::move (make)), every_ (everyMs)
        {
            thread_ = std::thread ([this] { run(); });
        }

        ~IsoPump() { stop(); }

        void stop()
        {
            running_ = false;

            if (thread_.joinable())
                thread_.join();
        }

        long transfers() const { return transfers_.load(); }       // how many it has finished
        long idle() const { return idle_.load(); }                 // how often it found none waiting

    private:
        void run()
        {
            while (running_)
            {
                if (kernel_.deliverIsoWith (device_, [this] (int) { return make_ (counter_++); }, endpoint_))
                    ++transfers_;
                else
                    ++idle_;

                std::this_thread::sleep_for (std::chrono::milliseconds (every_));
            }
        }

        FakeKernel& kernel_;
        FakeDevice& device_;
        int endpoint_;
        std::function<Bytes (long)> make_;
        int every_;
        std::atomic<bool> running_ { true };
        std::atomic<long> transfers_ { 0 };
        std::atomic<long> idle_ { 0 };
        long counter_ = 0;
        std::thread thread_;
    };
}

#endif // __linux__
