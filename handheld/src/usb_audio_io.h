#pragma once

// USB audio input without a kernel driver, the part that talks to the kernel (Linux only).
//
// A device is used through usbfs (/dev/bus/usb/<bus>/<device>), the way libusb does it:
//
//   1. open the device, claim its AudioControl interface (when it has one) and its AudioStreaming
//      interface for recording;
//   2. select the alternate setting that carries the format to use (setting 0 has no endpoint, the
//      others do, and selecting one makes the device start sending);
//   3. set the sample rate with the class request of the audio class: on the endpoint in version 1,
//      on the clock source in version 2, and read it back;
//   4. keep a few isochronous transfers ("URBs") queued on the endpoint. Each holds some milliseconds
//      of sound as a row of packets, one for each frame (full speed: 1 ms) or microframe (high speed:
//      125 us) of the bus. When one has finished, the sound in its packets is decoded and delivered,
//      and the transfer is queued again.
//
// usbfs says that a transfer has finished by making the file "writable" (POLLOUT); the finished
// transfer is then fetched with REAPURBNDELAY. Everything that is a system call goes through
// UsbOs (usb_os.h), so that the tests can put a pretend kernel in its place
// (tests/test_usb_audio_io.cpp). The descriptor parsing, the choice of format and the decoding are
// in usb_audio.h.
//
// What the kernel does with an isochronous transfer (devio.c, which the pretend kernel follows):
//   - 1 to 128 packets, each at most 49152 bytes; the kernel takes the length of the buffer from
//     the sum of the packet lengths, so the buffer must be at least that long;
//   - a packet cannot be longer than the endpoint's maximum (EMSGSIZE);
//   - with USBDEVFS_URB_ISO_ASAP the transfer starts right after the one queued before it, so the
//     stream is continuous as long as a new transfer is queued before the old ones run out;
//   - when it has finished, the buffer holds packet i at the sum of the lengths of the packets
//     before it, and iso_frame_desc[i].actual_length / .status say what arrived;
//   - changing the setting of the interface cancels the transfers on it, and releasing the
//     interface puts it back to setting 0.

#if defined(__linux__)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <poll.h>
#include <string>
#include <utility>
#include <vector>

#include "usb_audio.h"
#include "usb_os.h"

namespace kerf::ui::usbaudio
{
    using usbos::Logger;
    using usbos::SystemUsbOs;
    using usbos::UsbOs;
    using usbos::hex;

    // Decoded sound: `frames` frames of interleaved floating point samples, one per channel.
    using Deliver = std::function<void (const float* interleaved, int frames)>;

    constexpr int kRequestsInFlight = 8;        // transfers queued at a time
    constexpr double kSecondsPerRequest = 0.008;
    constexpr int kMaxErrorsInARow = 25;
    constexpr int kRequestsToLog = 4;           // the first few transfers get a line in the log
    constexpr int kFormatsToTry = 3;
    constexpr int kRequestsBeforeSilenceNotice = 64;

    // ------------------------------------------------------------------------------- one device

    struct FreeDeleter { void operator() (void* p) const noexcept { std::free (p); } };

    // An isochronous transfer: the usbdevfs_urb is followed by one descriptor for each packet, so it
    // is allocated by hand.
    struct IsoRequest
    {
        std::unique_ptr<usbdevfs_urb, FreeDeleter> urb;
        std::vector<uint8_t> buffer;
        int packets = 0;
        int packetBytes = 0;

        IsoRequest (int packetCount, int bytesPerPacket)
            : buffer ((size_t) packetCount * (size_t) bytesPerPacket, 0), packets (packetCount), packetBytes (bytesPerPacket)
        {
            urb.reset (static_cast<usbdevfs_urb*> (std::calloc (1, sizeBytes())));
        }

        size_t sizeBytes() const noexcept { return sizeof (usbdevfs_urb) + (size_t) packets * sizeof (usbdevfs_iso_packet_desc); }
        bool ok() const noexcept { return urb != nullptr; }
    };

    inline double steadySeconds()
    {
        return std::chrono::duration<double> (std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    struct AudioPort
    {
        UsbOs* os = nullptr;
        Candidate cand;
        int fd = -1;
        std::vector<unsigned> claimed;          // interfaces, in the order they were claimed
        unsigned alt = 0;                       // the setting of the streaming interface now
        bool dead = false;
        std::string why;                        // what ended it, for the log

        Format format;                          // the format in use
        int rate = 0;                           // the sample rate in use
        PcmDecoder decoder;
        std::vector<std::unique_ptr<IsoRequest>> requests;
        std::vector<float> block;               // the sound of the transfer being handled
        std::function<double()> clock = steadySeconds;

        // What happened, for the log and for the screen.
        int errors = 0;                         // transfers in a row in which every packet failed
        int inFlight = 0;
        uint64_t transfers = 0;
        uint64_t frames = 0;                    // frames delivered
        uint64_t emptyPackets = 0;
        uint64_t badPackets = 0;                // packets the bus reported as failed
        uint64_t underruns = 0;                 // times no transfer was left queued: sound was lost
        uint64_t requeueFailures = 0;           // finished transfers the kernel would not take again (each one is lost for good)
        bool silenceNoticed = false;
        double firstData = -1;                  // when the first sound arrived (seconds)
        double lastData = 0;
        uint64_t framesAtFirst = 0;

        AudioPort() = default;
        AudioPort (const AudioPort&) = delete;
        AudioPort& operator= (const AudioPort&) = delete;

        ~AudioPort()
        {
            if (fd < 0 || os == nullptr)
                return;

            // Setting 0 has no endpoint: the device stops sending and the transfers are cancelled.
            // Releasing the interfaces and closing the file would do the same; this is the tidy way.
            if (alt != 0)
            {
                usbdevfs_setinterface setting;
                std::memset (&setting, 0, sizeof setting);
                setting.interface = (unsigned) cand.stream.interfaceNumber;
                setting.altsetting = 0;
                os->ioctl (fd, USBDEVFS_SETINTERFACE, &setting);
            }

            for (auto it = claimed.rbegin(); it != claimed.rend(); ++it)
            {
                unsigned number = *it;
                os->ioctl (fd, USBDEVFS_RELEASEINTERFACE, &number);
            }

            os->close (fd);
        }

        int channels() const noexcept { return format.channels; }

        // The sample rate the device really delivers, from how much sound arrived in how long; 0
        // until there is enough to tell.
        double measuredRate() const noexcept
        {
            if (firstData < 0 || lastData - firstData < 0.5)
                return 0.0;

            return (double) (frames - framesAtFirst) / (lastData - firstData);
        }
    };

    inline std::string describe (const Candidate& c)
    {
        return "usb " + c.name + " (" + hex ((unsigned) c.vendor, 4) + ":" + hex ((unsigned) c.product, 4) + " " + c.title + ")";
    }

    // About 8 ms of sound in a transfer: 8 packets at full speed, 64 microframes at high speed.
    inline int packetsPerRequest (const Format& f, int speedMbps) noexcept
    {
        return std::clamp ((int) std::lround (busIntervalsPerSecond (f, speedMbps) * kSecondsPerRequest), 1, 128);
    }

    inline bool submitIso (AudioPort& port, IsoRequest& r)
    {
        usbdevfs_urb* urb = r.urb.get();
        std::memset (urb, 0, r.sizeBytes());
        urb->type = USBDEVFS_URB_TYPE_ISO;
        urb->endpoint = port.format.endpoint;
        urb->flags = USBDEVFS_URB_ISO_ASAP;
        urb->buffer = r.buffer.data();
        urb->buffer_length = (int) r.buffer.size();
        urb->number_of_packets = r.packets;
        urb->usercontext = &r;

        for (int i = 0; i < r.packets; ++i)
            urb->iso_frame_desc[i].length = (unsigned) r.packetBytes;

        if (port.os->ioctl (port.fd, USBDEVFS_SUBMITURB, urb) != 0)
            return false;

        ++port.inFlight;
        return true;
    }

    // Stops the stream: setting 0 cancels what is queued, and the cancelled transfers are fetched
    // (and ignored) so that none is left in the kernel's list.
    inline void stopStream (AudioPort& port)
    {
        if (port.alt != 0)
        {
            usbdevfs_setinterface setting;
            std::memset (&setting, 0, sizeof setting);
            setting.interface = (unsigned) port.cand.stream.interfaceNumber;
            setting.altsetting = 0;
            port.os->ioctl (port.fd, USBDEVFS_SETINTERFACE, &setting);
            port.alt = 0;
        }

        usbdevfs_urb* done = nullptr;

        for (int guard = 0; guard < 1000 && port.os->ioctl (port.fd, USBDEVFS_REAPURBNDELAY, &done) == 0; ++guard)
        {
        }

        port.requests.clear();
        port.inFlight = 0;
    }

    namespace detail
    {
        // `capacity` is what the buffer holds: a request for more is never sent (the device would
        // fill the buffer past its end).
        inline int control (AudioPort& p, const ControlRequest& r, void* data, size_t capacity)
        {
            if ((size_t) r.length > capacity)
            {
                errno = EINVAL;
                return -1;
            }

            return usbos::controlTransfer (*p.os, p.fd, r.type, r.request, r.value, r.index, data, r.length);
        }

        inline bool plausible (int rate) noexcept { return rate >= 8000 && rate <= 384000; }

        // Sets the sample rate and reads it back. Returns the rate the stream runs at, as far as that
        // can be known: what the device reports if it does, else what was asked for.
        inline int setRate (AudioPort& p, const Logger& log, const std::string& who)
        {
            const Stream& s = p.cand.stream;
            const Format& f = p.format;
            const int speed = p.cand.speed;

            if (s.version == 1)
            {
                int want = pickRate (rateOptions (f, speed));

                if (want == 0)
                    want = 48000;

                if (! f.rateControl)
                    return want;                        // the endpoint does not take a rate: it runs at the one it has

                uint8_t bytes[3];
                putRate (bytes, 3, (uint32_t) want);

                if (control (p, rateRequestV1 (true, f.endpoint), bytes, sizeof bytes) < 0)
                    log (who + "cannot set the rate on endpoint 0x" + hex (f.endpoint, 2) + ": " + std::strerror (errno) + " (carrying on at " + std::to_string (want) + " Hz)");

                uint8_t back[3] = {};

                if (control (p, rateRequestV1 (false, f.endpoint), back, sizeof back) == 3)
                {
                    const int got = (int) getRate (back, 3);

                    if (plausible (got) && got != want)
                    {
                        log (who + "asked for " + std::to_string (want) + " Hz, the device says " + std::to_string (got) + " Hz");
                        want = got;
                    }
                }

                return want;
            }

            // Version 2: the rate belongs to the clock source behind the stream.
            const int ci = s.controlInterface;

            if (ci < 0)
            {
                log (who + "no audio control interface: the rate is not known");
                return 48000;
            }

            const int clock = resolveClock (s, [&] (int selector)
            {
                uint8_t pin = 0;
                return control (p, selectorRequestV2 (selector, ci), &pin, sizeof pin) == 1 ? (int) pin : 0;
            });

            if (clock == 0)
            {
                log (who + "no clock source found: the rate is not known");
                return 48000;
            }

            const auto current = [&] () -> int
            {
                uint8_t bytes[4] = {};
                return control (p, rateRequestV2 (false, clock, ci), bytes, sizeof bytes) == 4 ? (int) getRate (bytes, 4) : 0;
            };

            const int reported = current();
            const int now = plausible (reported) ? reported : 0;

            // The rates the clock offers: first how many ranges there are, then the ranges.
            std::vector<int> options;
            uint8_t count[2] = {};

            if (control (p, rateRangeRequestV2 (clock, ci, 0), count, sizeof count) == 2)
            {
                const int ranges = count[0] | (count[1] << 8);

                if (ranges > 0)
                {
                    // At most 32 ranges are read (the request is cut to that as well).
                    std::vector<uint8_t> block ((size_t) 2 + 12 * (size_t) std::min (ranges, 32), 0);
                    const int got = control (p, rateRangeRequestV2 (clock, ci, ranges), block.data(), block.size());

                    if (got >= 2)
                        options = rangeRates (parseRanges (block.data(), (size_t) got));
                }
            }

            std::vector<int> fitting;

            for (const int r : options)
                if (carries (f, r, speed))
                    fitting.push_back (r);

            if (! fitting.empty())
                options = fitting;

            if (options.empty() && now > 0)
                options.push_back (now);

            int want = options.empty() ? 48000 : pickRate (options);
            const ClockNode* node = findClock (s, clock);
            const bool settable = node != nullptr && (node->controls & 3) == 3;

            if (settable && want != now)
            {
                uint8_t bytes[4];
                putRate (bytes, 4, (uint32_t) want);

                if (control (p, rateRequestV2 (true, clock, ci), bytes, sizeof bytes) < 0)
                {
                    log (who + "cannot set the clock " + hex ((unsigned) clock, 2) + " to " + std::to_string (want) + " Hz: " + std::strerror (errno));
                    want = now > 0 ? now : want;
                }
                else
                {
                    const int after = current();

                    if (plausible (after))
                    {
                        if (after != want)
                            log (who + "asked the clock for " + std::to_string (want) + " Hz, the device says " + std::to_string (after) + " Hz");

                        want = after;
                    }
                }
            }
            else if (! settable && now > 0)
            {
                want = now;                             // the rate is not for the program to choose
            }

            return want;
        }

        inline void claim (AudioPort& p, unsigned number, int* errorOut)
        {
            if (p.os->ioctl (p.fd, USBDEVFS_CLAIMINTERFACE, &number) < 0)
            {
                *errorOut = errno;
                return;
            }

            p.claimed.push_back (number);
            *errorOut = 0;
        }
    }

    // Opens the device, claims the interfaces, selects a format, sets the rate and queues the first
    // transfers. Returns nothing, after writing the reason to the log (and a short one to `reason`
    // for the screen), when any of that fails with every format that was worth trying.
    inline std::unique_ptr<AudioPort> openAudioPort (const Candidate& c, UsbOs& os, const Logger& log, std::string* reason = nullptr)
    {
        const std::string who = "audio: " + describe (c) + ": ";
        const auto fail = [&] (const std::string& why) -> std::unique_ptr<AudioPort>
        {
            if (reason != nullptr)
                *reason = why;

            return nullptr;
        };

        const int fd = usbos::openDeviceNode (os, c.bus, c.device, c.major, c.minor, who, log);

        if (fd < 0)
            return fail ("cannot open the USB device");

        auto port = std::make_unique<AudioPort>();
        port->os = &os;
        port->cand = c;
        port->fd = fd;      // from here on the destructor closes it

        int err = 0;

        // The AudioControl interface is where version 2 keeps its clock. Not every device needs it
        // (and nothing may have it), so a refusal is only noted.
        if (c.stream.controlInterface >= 0 && c.stream.controlInterface != c.stream.interfaceNumber)
        {
            detail::claim (*port, (unsigned) c.stream.controlInterface, &err);

            if (err != 0)
                log (who + "audio control interface " + std::to_string (c.stream.controlInterface) + " not claimed: " + std::strerror (err) + " (carrying on)");
        }

        detail::claim (*port, (unsigned) c.stream.interfaceNumber, &err);

        if (err != 0)
        {
            log (who + "cannot claim interface " + std::to_string (c.stream.interfaceNumber) + ": " + std::strerror (err)
                 + (err == EBUSY ? " (a kernel driver or another program is using it)" : ""));
            return fail (err == EBUSY ? "another driver or program is using this USB audio device" : "cannot claim the USB audio interface");
        }

        os.keepAwake (c.name);

        const auto order = rankFormats (c.stream);
        int tried = 0;
        std::string lastWhy = "the device has no format that Kerf can use";

        for (const int index : order)
        {
            if (tried++ >= kFormatsToTry)
                break;

            const Format& f = c.stream.formats[(size_t) index];
            port->format = f;

            usbdevfs_setinterface setting;
            std::memset (&setting, 0, sizeof setting);
            setting.interface = (unsigned) c.stream.interfaceNumber;
            setting.altsetting = (unsigned) f.alt;

            if (os.ioctl (fd, USBDEVFS_SETINTERFACE, &setting) < 0)
            {
                err = errno;
                log (who + "cannot select setting " + std::to_string (f.alt) + ": " + std::strerror (err) + (err == ENOSPC ? " (not enough USB bandwidth)" : ""));
                lastWhy = err == ENOSPC ? "not enough USB bandwidth for the audio" : "the USB audio device would not start";
                continue;
            }

            port->alt = (unsigned) f.alt;
            port->rate = detail::setRate (*port, log, who);
            port->decoder.configure (f.kind, f.channels, f.subslot);

            const int packets = packetsPerRequest (f, c.speed);
            bool queued = true;

            for (int k = 0; k < kRequestsInFlight && queued; ++k)
            {
                auto request = std::make_unique<IsoRequest> (packets, f.packet);

                if (! request->ok() || ! submitIso (*port, *request))
                {
                    err = request->ok() ? errno : ENOMEM;
                    log (who + "setting " + std::to_string (f.alt) + ": cannot queue a transfer on endpoint 0x" + hex (f.endpoint, 2) + ": " + std::strerror (err)
                         + (err == EMSGSIZE ? " (the packets are longer than the endpoint allows)" : ""));
                    lastWhy = "the USB audio device would not accept the transfers";
                    queued = false;
                    break;
                }

                port->requests.push_back (std::move (request));
            }

            if (! queued)
            {
                stopStream (*port);
                continue;
            }

            log (who + "recording: setting " + std::to_string (f.alt) + ", " + std::to_string (f.channels) + " channels, "
                 + std::to_string (f.subslot) + " bytes per sample, " + std::to_string (port->rate) + " Hz, "
                 + std::to_string (kRequestsInFlight) + " transfers of " + std::to_string (packets) + " packets of " + std::to_string (f.packet) + " bytes on endpoint 0x"
                 + hex (f.endpoint, 2) + (c.speed >= 480 ? " (high speed)" : (c.speed >= 12 ? " (full speed)" : (c.speed == 0 ? "" : " (low speed)"))));
            return port;
        }

        return fail (lastWhy);
    }

    // Handles the transfers that have finished: decodes the sound in them, hands it to `deliver`
    // and queues them again. `revents` is what poll() said. Returns false when the device is gone or
    // has failed too often; port.why then says which.
    inline bool serviceAudio (AudioPort& port, const Deliver& deliver, const Logger& log, short revents)
    {
        const std::string who = "audio: " + describe (port.cand) + ": ";

        if (revents & (POLLHUP | POLLERR | POLLNVAL))
        {
            port.dead = true;
            port.why = "unplugged";
        }

        // A cap, so that a device which answers without end cannot keep the thread here; poll()
        // reports whatever is left straight away.
        int reaped = 0;
        int lastRequeueError = 0;

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

            auto* request = static_cast<IsoRequest*> (done->usercontext);
            const int status = done->status;
            --port.inFlight;
            ++port.transfers;
            ++reaped;

            if (status == -ENODEV || status == -ESHUTDOWN)
            {
                port.dead = true;
                port.why = "unplugged";
            }
            else if (status != -ENOENT && status != -ECONNRESET)       // those two: cancelled, only queue it again
            {
                int failed = 0, empty = 0, withSound = 0;
                port.block.clear();
                const int channels = std::max (1, port.format.channels);

                for (int i = 0; i < request->packets; ++i)
                {
                    const auto& packet = done->iso_frame_desc[i];

                    if (packet.status != 0) { ++failed; continue; }
                    if (packet.actual_length == 0) { ++empty; continue; }

                    ++withSound;
                    const size_t n = std::min<size_t> (packet.actual_length, (size_t) request->packetBytes);
                    port.decoder.feed (request->buffer.data() + (size_t) i * (size_t) request->packetBytes, n, [&] (const float* data, int frames)
                    {
                        port.block.insert (port.block.end(), data, data + (size_t) frames * (size_t) channels);
                    });
                }

                port.badPackets += (uint64_t) failed;
                port.emptyPackets += (uint64_t) empty;
                port.errors = (failed == request->packets) ? port.errors + 1 : 0;

                const int frames = (int) (port.block.size() / (size_t) channels);

                if (frames > 0)
                {
                    const double now = port.clock();

                    if (port.firstData < 0)
                    {
                        port.firstData = now;
                        port.framesAtFirst = port.frames + (uint64_t) frames;
                    }

                    port.lastData = now;
                    port.frames += (uint64_t) frames;
                    deliver (port.block.data(), frames);
                }

                if (port.transfers <= (uint64_t) kRequestsToLog)
                    log (who + "transfer " + std::to_string (port.transfers) + ": " + std::to_string (request->packets) + " packets, "
                         + std::to_string (withSound) + " with sound, " + std::to_string (empty) + " empty, " + std::to_string (failed) + " failed, "
                         + std::to_string (frames) + " frames");

                if (! port.silenceNoticed && port.frames == 0 && port.transfers >= (uint64_t) kRequestsBeforeSilenceNotice)
                {
                    port.silenceNoticed = true;
                    log (who + "no sound has arrived in " + std::to_string (port.transfers) + " transfers: the device is not sending (is it muted, or in another mode?)");
                }

                if (port.errors > kMaxErrorsInARow)
                {
                    port.dead = true;
                    port.why = "stopped: the transfers kept failing";
                }
            }

            // A transfer the kernel will not take again is lost, and the stream goes on with one
            // fewer; the first one is written to the log.
            if (! port.dead && ! submitIso (port, *request))
            {
                const int err = errno;

                if (++port.requeueFailures == 1)
                    log (who + "a transfer could not be queued again: " + std::strerror (err) + " (" + std::to_string (port.inFlight) + " left)");

                if (err == ENODEV)
                {
                    port.dead = true;
                    port.why = "unplugged";
                }
                else
                {
                    lastRequeueError = err;
                }
            }
        }

        // With no transfer left in the kernel nothing more can come.
        if (! port.dead && port.inFlight <= 0)
        {
            port.dead = true;
            port.why = "stopped: no transfer could be queued again (" + std::string (std::strerror (lastRequeueError != 0 ? lastRequeueError : EIO)) + ")";
        }

        // Every transfer that was queued had finished before the first was queued again: for a
        // while the bus had nowhere to put the sound, which is lost.
        if (reaped >= kRequestsInFlight && ! port.dead)
            ++port.underruns;

        return ! port.dead;
    }

    // One line for the log when a stream ends.
    inline std::string summary (const AudioPort& port)
    {
        const double rate = port.measuredRate();
        return std::to_string (port.frames) + " frames in " + std::to_string (port.transfers) + " transfers"
               + (rate > 0 ? ", measured rate " + std::to_string ((long) std::lround (rate)) + " Hz" : "")
               + ", " + std::to_string (port.badPackets) + " bad packets, " + std::to_string (port.emptyPackets) + " empty, "
               + std::to_string (port.underruns) + " times the queue ran dry"
               + (port.requeueFailures > 0 ? ", " + std::to_string (port.requeueFailures) + " transfers lost" : std::string());
    }
}

#endif // __linux__
