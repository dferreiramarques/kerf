#pragma once

// USB MIDI without a kernel driver, the part that talks to the kernel (Linux only).
//
// A device is read through usbfs (/dev/bus/usb/<bus>/<device>), the way libusb does it: claim the
// MIDI Streaming interface, keep a couple of transfers ("URBs") queued on its input endpoint, and
// when one has finished decode the packets in it and queue it again. usbfs says that a transfer
// has finished by making the file "writable" (POLLOUT) and the finished transfer is then fetched
// with REAPURBNDELAY.
//
// Everything that is a system call goes through UsbOs (usb_os.h), so that the tests can put a
// pretend kernel in its place (tests/test_usb_midi_io.cpp). The descriptor parsing, the packet
// decoding and the search for devices are in usb_midi.h.

#if defined(__linux__)

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <linux/usbdevice_fs.h>
#include <memory>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "midi.h"
#include "usb_midi.h"
#include "usb_os.h"

namespace kerf::ui::usbmidi
{
    using usbos::Logger;
    using usbos::SystemUsbOs;
    using usbos::UsbOs;
    using usbos::hex;

    using Deliver = std::function<void (const kerf::MidiMessage&)>;

    // ------------------------------------------------------------------------------- one device

    constexpr int kTransfersPerEndpoint = 2;   // one is read while the other is being decoded
    constexpr int kMaxErrorsInARow = 25;
    constexpr int kTransfersToLog = 6;         // the bytes of the first few are written to the log

    struct UsbRequest
    {
        std::vector<uint8_t> buffer;
        uint8_t endpoint = 0;
        uint8_t type = USBDEVFS_URB_TYPE_BULK;
        usbdevfs_urb urb;                      // last: it ends in a zero-length array (isochronous packets, not used here)
    };

    struct UsbPort
    {
        UsbOs* os = nullptr;
        Candidate cand;
        int fd = -1;
        bool claimed = false;
        bool dead = false;
        std::string why;                       // what ended it, for the log
        int errors = 0;                        // transfers that failed in a row
        int logged = 0;                        // transfers whose bytes were written to the log
        PacketDecoder decoder;
        std::vector<std::unique_ptr<UsbRequest>> requests;

        UsbPort() = default;
        UsbPort (const UsbPort&) = delete;
        UsbPort& operator= (const UsbPort&) = delete;

        ~UsbPort()
        {
            if (fd < 0 || os == nullptr)
                return;

            // Releasing the interface and closing the file cancel every transfer still queued.
            if (claimed)
            {
                unsigned int number = (unsigned int) cand.stream.interfaceNumber;
                os->ioctl (fd, USBDEVFS_RELEASEINTERFACE, &number);
            }

            os->close (fd);
        }
    };

    inline std::string describe (const Candidate& c)
    {
        return "usb " + c.name + " (" + hex ((unsigned) c.vendor, 4) + ":" + hex ((unsigned) c.product, 4) + " " + c.title + ")";
    }

    inline bool submitRequest (UsbPort& port, UsbRequest& request)
    {
        std::memset (&request.urb, 0, sizeof request.urb);
        request.urb.type = request.type;
        request.urb.endpoint = request.endpoint;
        request.urb.buffer = request.buffer.data();
        request.urb.buffer_length = (int) request.buffer.size();
        request.urb.usercontext = &request;
        return port.os->ioctl (port.fd, USBDEVFS_SUBMITURB, &request.urb) == 0;
    }

    // Opens the device, claims the MIDI Streaming interface and queues the first reads. Returns
    // nothing, after writing the reason to the log, when any of that fails.
    inline std::unique_ptr<UsbPort> openUsbPort (const Candidate& c, UsbOs& os, const Logger& log)
    {
        const std::string who = "midi: " + describe (c) + ": ";
        const int fd = usbos::openDeviceNode (os, c.bus, c.device, c.major, c.minor, who, log);

        if (fd < 0)
            return nullptr;

        int err = 0;

        auto port = std::make_unique<UsbPort>();
        port->os = &os;
        port->cand = c;
        port->fd = fd; // from here on the destructor closes it

        unsigned int number = (unsigned int) c.stream.interfaceNumber;

        if (os.ioctl (fd, USBDEVFS_CLAIMINTERFACE, &number) < 0)
        {
            err = errno;
            log (who + "cannot claim interface " + std::to_string (number) + ": " + std::strerror (err)
                 + (err == EBUSY ? " (another program or driver is using it)" : ""));
            return nullptr;
        }

        port->claimed = true;

        if (c.stream.alt != 0)
        {
            usbdevfs_setinterface setting;
            std::memset (&setting, 0, sizeof setting);
            setting.interface = number;
            setting.altsetting = (unsigned int) c.stream.alt;

            if (os.ioctl (fd, USBDEVFS_SETINTERFACE, &setting) < 0)
            {
                err = errno;
                log (who + "cannot select setting " + std::to_string (c.stream.alt) + ": " + std::strerror (err));
                return nullptr;
            }
        }

        os.keepAwake (c.name);

        for (const auto& endpoint : c.stream.inputs)
        {
            for (int copy = 0; copy < kTransfersPerEndpoint; ++copy)
            {
                auto request = std::make_unique<UsbRequest>();
                request->endpoint = endpoint.address;
                request->type = endpoint.transferType() == 3 ? USBDEVFS_URB_TYPE_INTERRUPT : USBDEVFS_URB_TYPE_BULK;
                request->buffer.assign (std::max<size_t> ((size_t) (endpoint.maxPacket & 0x7FF), 64), 0);

                if (! submitRequest (*port, *request))
                {
                    err = errno;
                    log (who + "cannot queue a read on endpoint 0x" + hex (endpoint.address, 2) + ": " + std::strerror (err));
                    return nullptr;
                }

                port->requests.push_back (std::move (request));
            }
        }

        return port;
    }

    // Handles the transfers that have finished: decodes them and queues them again. `revents` is
    // what poll() said. Returns false when the device is gone or has failed too often; port.why
    // then says which.
    inline bool serviceUsb (UsbPort& port, const Deliver& deliver, const Logger& log, short revents)
    {
        if (revents & (POLLHUP | POLLERR | POLLNVAL))
        {
            port.dead = true;
            port.why = "unplugged";
        }

        // A cap, so that a device which answers without end cannot keep the thread here; poll()
        // reports whatever is left straight away.
        for (int handled = 0; handled < 64; ++handled)
        {
            usbdevfs_urb* done = nullptr;

            if (port.os->ioctl (port.fd, USBDEVFS_REAPURBNDELAY, &done) < 0)
            {
                if (errno != EAGAIN)
                {
                    port.dead = true;
                    port.why = "unplugged"; // ENODEV
                }

                break;
            }

            if (done == nullptr)
                break;

            auto* request = static_cast<UsbRequest*> (done->usercontext);
            const int status = done->status;

            if (status == 0)
            {
                port.errors = 0;
                const int got = done->actual_length;

                if (got > 0 && got <= (int) request->buffer.size())
                {
                    if (port.logged < kTransfersToLog)
                    {
                        ++port.logged;
                        std::string bytes;

                        for (int i = 0; i < got && i < 16; ++i)
                            bytes += " " + hex (request->buffer[(size_t) i], 2);

                        log ("midi: " + describe (port.cand) + ": data" + bytes);
                    }

                    port.decoder.feed (request->buffer.data(), (size_t) got, deliver);
                }
            }
            else if (status == -ENODEV || status == -ESHUTDOWN)
            {
                port.dead = true;
                port.why = "unplugged";
            }
            else if (status == -ENOENT || status == -ECONNRESET)
            {
                // cancelled: nothing to do but queue it again
            }
            else
            {
                if (++port.errors > kMaxErrorsInARow)
                {
                    port.dead = true;
                    port.why = "stopped: the transfers kept failing (" + std::string (std::strerror (-status)) + ")";
                }

                if (status == -EPIPE)
                {
                    unsigned int endpoint = request->endpoint;
                    port.os->ioctl (port.fd, USBDEVFS_CLEAR_HALT, &endpoint);
                }
            }

            if (! port.dead && ! submitRequest (port, *request))
            {
                const int err = errno;

                if (err == ENODEV)
                {
                    port.dead = true;
                    port.why = "unplugged";
                }
                else if (++port.errors > kMaxErrorsInARow)
                {
                    port.dead = true;
                    port.why = "stopped: a read could not be queued again (" + std::string (std::strerror (err)) + ")";
                }
            }
        }

        return ! port.dead;
    }
}

#endif // __linux__
