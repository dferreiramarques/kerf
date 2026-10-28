// Tests for the part of the direct USB audio input that talks to the kernel. A pretend kernel
// stands in for usbfs (tests/fake_usb_kernel.h): it keeps the isochronous transfers that the program
// queues, completes them when the test says so, answers the class requests the way an audio device
// would, and can fail, run out of bandwidth or unplug. What the real kernel does is written down in
// usb_audio_io.h; the pretend one follows that.

#if defined(__linux__)

#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <poll.h>

#include "fake_usb_kernel.h"
#include "minitest.h"
#include "usb_audio_devices.h"
#include "usb_audio_fixtures.h"
#include "usb_audio_io.h"
#include "usb_fixtures.h"

using namespace kerf::ui::usbaudio;
using namespace kerf::ui::usbfake;
using namespace usbaudiofixtures;
using usbfixtures::FakeSysfs;

namespace
{
    // Where the decoded sound goes.
    struct Sink
    {
        int channels = 2;
        std::vector<float> samples;
        int frames = 0;
        int calls = 0;

        explicit Sink (int channelCount = 2) : channels (channelCount) {}

        Deliver deliver()
        {
            return [this] (const float* data, int count)
            {
                samples.insert (samples.end(), data, data + (size_t) count * (size_t) channels);
                frames += count;
                ++calls;
            };
        }
    };

    bool near (float a, float b, float tolerance = 1e-6f) { return std::fabs (a - b) <= tolerance; }

    bool contains (const std::vector<std::string>& calls, const std::string& wanted)
    {
        for (const auto& c : calls)
            if (c == wanted)
                return true;

        return false;
    }

    // The kernel's view of everything a test needs for a receiver.
    struct Rig
    {
        FakeSysfs sys;
        FakeKernel kernel;
        Lines log;
        std::shared_ptr<Receiver> state = std::make_shared<Receiver>();
        FakeDevice* device = nullptr;
        Candidate cand;
        std::unique_ptr<AudioPort> port;
        std::string reason;

        explicit Rig (const Bytes& descriptors = receiver(), int speed = 12)
        {
            sys.device ("4-1", descriptors, 1, 4, 2, "", "Receiver", "1234", "5678");
            sys.text (sys.root / "4-1" / "speed", std::to_string (speed) + "\n");
            const auto found = look (sys.root);
            EXPECT (found.candidates.size() == 1);

            if (! found.candidates.empty())
                cand = found.candidates[0];

            device = &addReceiver (kernel, state);
        }

        bool open()
        {
            port = openAudioPort (cand, kernel, log.logger(), &reason);
            return port != nullptr;
        }

        bool service (Sink& sink, short revents = POLLOUT)
        {
            return serviceAudio (*port, sink.deliver(), log.logger(), revents);
        }

        // The device finishes one transfer with these packets.
        bool send (const std::vector<Bytes>& packets, const std::vector<int>& statuses = {})
        {
            return kernel.deliverIso (*device, packets, 0x81, statuses);
        }
    };

    // ------------------------------------------------------------------------ a mixer (version 2)

    struct ClockState
    {
        int rate = 44100;
        bool programmable = true;               // false: the device stalls the request that sets the rate
        int takes = -1;                         // the rate the clock really runs at after it was set (-1 = the one asked for)
        std::vector<std::pair<int, int>> ranges { { 44100, 44100 }, { 48000, 48000 } };
    };

    struct Mixer2
    {
        std::map<int, ClockState> clocks;
        int selectorPin = 1;
        int selectorFails = false;
    };

    struct MixerRig
    {
        FakeSysfs sys;
        FakeKernel kernel;
        Lines log;
        std::shared_ptr<Mixer2> state = std::make_shared<Mixer2>();
        FakeDevice* device = nullptr;
        Candidate cand;
        std::unique_ptr<AudioPort> port;
        std::string reason;

        explicit MixerRig (bool withSelector = false, int recordingClock = 0x28, int recordPacket = 360, int recordInterval = 1)
        {
            state->clocks[0x28] = ClockState();
            state->clocks[0x2A] = ClockState();
            state->clocks[0x2A].programmable = false;
            state->clocks[0x2A].rate = 48000;

            sys.device ("1-2", mixer (withSelector, recordingClock, 0x0644, 0x805f, recordPacket, recordInterval), 1, 1, 5, "TASCAM", "Model 12", "0644", "805f");
            sys.text (sys.root / "1-2" / "speed", "480\n");
            const auto found = look (sys.root);
            EXPECT (found.candidates.size() == 1);

            if (! found.candidates.empty())
                cand = found.candidates[0];

            device = &kernel.add (1, 5, { { 0x01, kIso }, { 0x81, kIso }, { 0x82, kIso } });
            device->interfaces = { 0, 1, 2 };
            device->endpointInterface = { { 0x01, 1 }, { 0x81, 1 }, { 0x82, 2 } };
            device->endpointAlt = { { 0x01, { 1 } }, { 0x81, { 1 } }, { 0x82, { 1 } } };
            device->isoMaxPacket = 360;

            const auto s = state;
            device->onControl = [s] (usbdevfs_ctrltransfer& t) -> int
            {
                auto* data = static_cast<uint8_t*> (t.data);
                const int entity = t.wIndex >> 8;

                if ((t.wIndex & 0xFF) != 0)
                    return -EPIPE;

                if (entity == 0x29 && t.bRequestType == 0xA1 && t.bRequest == 0x01 && t.wValue == 0x0100 && t.wLength == 1)
                {
                    if (s->selectorFails)
                        return -EPIPE;

                    data[0] = (uint8_t) s->selectorPin;
                    return 1;
                }

                const auto it = s->clocks.find (entity);

                if (it == s->clocks.end() || t.wValue != 0x0100)
                    return -EPIPE;

                ClockState& clock = it->second;

                if (t.bRequestType == 0xA1 && t.bRequest == 0x01 && t.wLength == 4)           // CUR
                {
                    putRate (data, 4, (uint32_t) clock.rate);
                    return 4;
                }

                if (t.bRequestType == 0x21 && t.bRequest == 0x01 && t.wLength == 4)
                {
                    if (! clock.programmable)
                        return -EPIPE;

                    clock.rate = clock.takes >= 0 ? clock.takes : (int) getRate (data, 4);
                    return 4;
                }

                if (t.bRequestType == 0xA1 && t.bRequest == 0x02)                              // RANGE
                {
                    Bytes block;
                    block.push_back ((uint8_t) clock.ranges.size());
                    block.push_back (0);

                    for (const auto& r : clock.ranges)
                    {
                        uint8_t three[4];
                        putRate (three, 4, (uint32_t) r.first);
                        block.insert (block.end(), three, three + 4);
                        putRate (three, 4, (uint32_t) r.second);
                        block.insert (block.end(), three, three + 4);
                        putRate (three, 4, 0);
                        block.insert (block.end(), three, three + 4);
                    }

                    const size_t n = std::min<size_t> (block.size(), t.wLength);       // a device answers with no more than was asked for
                    std::memcpy (data, block.data(), n);
                    return (int) n;
                }

                return -EPIPE;
            };
        }

        bool open()
        {
            port = openAudioPort (cand, kernel, log.logger(), &reason);
            return port != nullptr;
        }
    };
}

// ----------------------------------------------------------------------------------- opening

KTEST (UsbAudioIo, a_microphone_is_opened_set_to_a_rate_and_started)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    // Stereo comes before mono, so the second setting (alt 2) is the one used.
    const auto calls = r.kernel.callsOf (*r.device);
    const std::vector<std::string> expected = { "open", "claim 0", "claim 1", "alt 1 2",
                                                "control 22 01 0100 0081 3", "control a2 81 0100 0081 3" };
    EXPECT (calls.size() >= expected.size());
    EXPECT (std::equal (expected.begin(), expected.end(), calls.begin()));
    EXPECT (r.kernel.callCount (*r.device, "submit 81 iso 1568") == 8);       // 8 transfers of 8 packets of 196 bytes
    EXPECT (r.kernel.pendingCount (*r.device) == 8);

    int packetLength = 0;
    EXPECT (r.kernel.waitingPackets (*r.device, 0x81, &packetLength) == 8 && packetLength == 196);
    EXPECT ((r.kernel.lastFlags (*r.device) & USBDEVFS_URB_ISO_ASAP) != 0);
    EXPECT (r.kernel.autoClaimCount (*r.device) == 0);                        // both interfaces were claimed before use
    EXPECT (r.kernel.awake == std::vector<std::string> ({ "4-1" }));
    EXPECT (r.kernel.altOf (*r.device, 1) == 2);

    const auto controls = r.kernel.controlsOf (*r.device);
    EXPECT (controls.size() == 2);
    EXPECT (controls.size() == 2 && controls[0].sent == Bytes ({ 0x80, 0xBB, 0x00 }));       // 48000 Hz, three bytes, low byte first

    EXPECT (r.port->rate == 48000);
    EXPECT (r.state->rate == 48000);
    EXPECT (r.port->format.alt == 2 && r.port->format.channels == 2 && r.port->channels() == 2);
    EXPECT (r.log.has ("recording: setting 2, 2 channels, 2 bytes per sample, 48000 Hz, 8 transfers of 8 packets of 196 bytes on endpoint 0x81 (full speed)"));
}

KTEST (UsbAudioIo, the_sound_that_arrives_is_decoded_and_the_transfer_is_queued_again)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    pollfd waiting { r.port->fd, POLLOUT, 0 };
    EXPECT (::poll (&waiting, 1, 0) == 0);                                    // nothing has finished: usbfs is not "writable"

    EXPECT (r.send (stereoPackets (0, 8, 48)));
    waiting.revents = 0;
    EXPECT (::poll (&waiting, 1, 0) == 1 && (waiting.revents & POLLOUT));     // one has

    Sink sink;
    EXPECT (r.service (sink));
    EXPECT (sink.frames == 384 && sink.calls == 1);
    EXPECT (sink.samples.size() == 768);

    for (size_t i = 0; i < sink.samples.size() / 2 && i < 384; ++i)
    {
        EXPECT (near (sink.samples[2 * i], (float) i / 32768.0f));
        EXPECT (near (sink.samples[2 * i + 1], -(float) i / 32768.0f));
    }

    EXPECT (r.kernel.pendingCount (*r.device) == 8);                          // the transfer went back in the queue
    EXPECT (r.port->inFlight == 8);                                           // and the count of what is queued kept up
    EXPECT (r.port->frames == 384 && r.port->transfers == 1);

    // The next transfer continues where the first ended.
    EXPECT (r.send (stereoPackets (384, 8, 48)));
    EXPECT (r.service (sink));
    EXPECT (sink.frames == 768);
    EXPECT (near (sink.samples[2 * 400], 400.0f / 32768.0f));
    EXPECT (r.log.has ("transfer 1: 8 packets, 8 with sound, 0 empty, 0 failed, 384 frames"));

    pollfd after { r.port->fd, POLLOUT, 0 };
    EXPECT (::poll (&after, 1, 0) == 0);                                      // everything has been fetched
}

KTEST (UsbAudioIo, empty_and_failed_packets_are_counted_and_the_rest_is_kept)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    std::vector<Bytes> packets (8);
    packets[0] = stereoFrames (0, 48);
    packets[2] = stereoFrames (48, 48);                     // this one failed on the bus: its bytes are not trusted
    packets[3] = stereoFrames (96, 24);                     // half a packet
    std::vector<int> statuses (8, 0);
    statuses[2] = -EXDEV;

    EXPECT (r.send (packets, statuses));
    Sink sink;
    EXPECT (r.service (sink));
    EXPECT (sink.frames == 72);
    EXPECT (r.port->badPackets == 1);
    EXPECT (r.port->emptyPackets == 5);                     // packets 1 and 4..7
    EXPECT (r.log.has ("transfer 1: 8 packets, 2 with sound, 5 empty, 1 failed, 72 frames"));
    EXPECT (near (sink.samples[2 * 48], 96.0f / 32768.0f));  // the frame after packet 0 is the first of packet 3
}

KTEST (UsbAudioIo, frames_cut_between_packets_are_put_together_even_across_transfers)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    // 24 frames of 4 bytes, cut into packets of 3 and 5 bytes: 8 packets (32 bytes) to a transfer.
    const Bytes all = stereoFrames(0, 24);
    std::vector<std::vector<Bytes>> transfers (3);
    size_t at = 0;

    for (auto& transfer : transfers)
        for (int p = 0; p < 8; ++p)
        {
            const size_t n = (p % 2 == 0) ? 3 : 5;
            transfer.emplace_back (all.begin() + (std::ptrdiff_t) at, all.begin() + (std::ptrdiff_t) (at + n));
            at += n;
        }

    Sink sink;

    for (const auto& transfer : transfers)
    {
        EXPECT (r.send (transfer));
        EXPECT (r.service (sink));
    }

    EXPECT (sink.frames == 24);

    for (size_t i = 0; i < sink.samples.size() / 2; ++i)
        EXPECT (near (sink.samples[2 * i], (float) i / 32768.0f));
}

KTEST (UsbAudioIo, many_transfers_in_which_every_packet_failed_end_the_stream_but_a_good_one_in_between_does_not)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    Sink sink;
    const std::vector<int> allFail (8, -EPROTO);

    for (int i = 0; i < 20; ++i)
    {
        EXPECT (r.send ({}, allFail));
        EXPECT (r.service (sink));
    }

    EXPECT (r.send (stereoPackets (0, 8, 48)));
    EXPECT (r.service (sink));
    EXPECT (r.port->errors == 0);

    int served = 0;
    bool alive = true;

    for (int i = 0; i < 40 && alive; ++i)
    {
        EXPECT (r.send ({}, allFail));
        alive = r.service (sink);
        ++served;
    }

    EXPECT (! alive && served == 26);
    EXPECT (r.port->why == "stopped: the transfers kept failing");
    EXPECT (r.port->badPackets == 20 * 8 + 26 * 8);
}

KTEST (UsbAudioIo, a_transfer_that_was_cancelled_is_queued_again_and_one_from_a_dead_device_ends_the_stream)
{
    {
        Rig r;
        EXPECT (r.open());

        if (! r.port)
            return;

        Sink sink;
        EXPECT (r.kernel.fail (*r.device, ECONNRESET, 0x81));
        EXPECT (r.service (sink));
        EXPECT (r.kernel.fail (*r.device, ENOENT, 0x81));
        EXPECT (r.service (sink));
        EXPECT (r.kernel.pendingCount (*r.device) == 8);
        EXPECT (r.port->why.empty());

        // A cancelled transfer says nothing about the sound: its packets are not looked at.
        EXPECT (r.port->transfers == 2 && r.port->inFlight == 8);
        EXPECT (r.port->emptyPackets == 0 && r.port->badPackets == 0 && r.port->errors == 0 && sink.calls == 0);
        EXPECT (r.log.count ("): transfer ") == 0);
    }

    for (const int error : { ENODEV, ESHUTDOWN })
    {
        Rig r;
        EXPECT (r.open());

        if (! r.port)
            return;

        Sink sink;
        EXPECT (r.kernel.fail (*r.device, error, 0x81));
        EXPECT (! r.service (sink));
        EXPECT (r.port->dead && r.port->why == "unplugged");
    }
}

KTEST (UsbAudioIo, unplugging_the_device_ends_the_stream)
{
    {
        Rig r;
        EXPECT (r.open());

        if (! r.port)
            return;

        Sink sink;
        r.kernel.unplug (*r.device);
        EXPECT (! r.service (sink, POLLERR | POLLHUP));
        EXPECT (r.port->why == "unplugged");
    }

    {
        // Even if poll() said nothing: fetching a transfer from a device that has gone fails.
        Rig r;
        EXPECT (r.open());

        if (! r.port)
            return;

        Sink sink;
        r.kernel.unplug (*r.device);
        EXPECT (! r.service (sink, 0));
        EXPECT (r.port->why == "unplugged");
    }
}

KTEST (UsbAudioIo, an_error_or_a_hangup_that_poll_reports_ends_the_stream_by_itself)
{
    // The device is still there as far as the pretend kernel is concerned; only poll() complains.
    for (const int flag : { (int) POLLERR, (int) POLLHUP, (int) POLLNVAL })
    {
        Rig r;
        EXPECT (r.open());

        if (! r.port)
            return;

        Sink sink;
        EXPECT (! r.service (sink, (short) flag));
        EXPECT (r.port->dead && r.port->why == "unplugged");
    }

    // The ordinary events are not complaints.
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    Sink sink;
    EXPECT (r.service (sink, POLLOUT));
    EXPECT (r.service (sink, POLLIN));
    EXPECT (r.service (sink, 0));
    EXPECT (! r.port->dead);
}

KTEST (UsbAudioIo, a_transfer_that_ends_the_stream_is_not_queued_again)
{
    {
        Rig r;
        EXPECT (r.open());

        if (! r.port)
            return;

        Sink sink;
        EXPECT (r.kernel.fail (*r.device, ENODEV, 0x81));
        EXPECT (! r.service (sink));
        EXPECT (r.kernel.pendingCount (*r.device) == 7 && r.port->inFlight == 7);
        EXPECT (r.kernel.callCount (*r.device, "submit 81") == 8);              // the eight of the start, none after
    }

    {
        Rig r;
        EXPECT (r.open());

        if (! r.port)
            return;

        Sink sink;
        const std::vector<int> allFail (8, -EPROTO);
        bool alive = true;
        int served = 0;

        for (int i = 0; i < 40 && alive; ++i)
        {
            EXPECT (r.send ({}, allFail));
            alive = r.service (sink);
            ++served;
        }

        EXPECT (! alive && served == 26);
        EXPECT (r.kernel.pendingCount (*r.device) == 7);
        EXPECT (r.kernel.callCount (*r.device, "submit 81") == 8 + 25);         // the last transfer was not queued again
    }
}

KTEST (UsbAudioIo, a_transfer_the_kernel_will_not_take_again_is_lost_and_the_stream_ends_when_none_is_left)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    r.device->submitError = ENOMEM;
    r.device->submitsAllowed = 0;                                               // from now on the kernel takes no more transfers
    Sink sink;

    for (int i = 0; i < 7; ++i)
    {
        EXPECT (r.send (stereoPackets (0, 8, 48)));
        EXPECT (r.service (sink));                                              // still alive, with one transfer fewer each time
        EXPECT (r.port->inFlight == 7 - i && r.kernel.pendingCount (*r.device) == 7 - i);
        EXPECT (r.port->requeueFailures == (uint64_t) (i + 1));
    }

    EXPECT (sink.frames == 7 * 384);                                            // all the sound was kept
    EXPECT (r.log.count ("a transfer could not be queued again") == 1);         // said once
    EXPECT (r.log.has ("a transfer could not be queued again: Cannot allocate memory (7 left)"));

    EXPECT (r.send (stereoPackets (0, 8, 48)));
    EXPECT (! r.service (sink));                                                // the last one: nothing is left to bring sound
    EXPECT (r.port->dead && r.port->why == "stopped: no transfer could be queued again (Cannot allocate memory)");
    EXPECT (sink.frames == 8 * 384);                                            // and its sound was kept too
    EXPECT (r.port->inFlight == 0 && r.port->underruns == 0);
    EXPECT (summary (*r.port).find ("8 transfers lost") != std::string::npos);

    // A device that has gone says so at once, however many transfers are left.
    Rig gone;
    EXPECT (gone.open());

    if (! gone.port)
        return;

    gone.device->submitError = ENODEV;
    gone.device->submitsAllowed = 0;
    Sink other;
    EXPECT (gone.send (stereoPackets (0, 8, 48)));
    EXPECT (! gone.service (other));
    EXPECT (gone.port->why == "unplugged" && gone.port->inFlight == 7);
    EXPECT (other.frames == 384);

    // The same when every transfer has finished at once: none is left, and the reason stays "unplugged".
    Rig allGone;
    EXPECT (allGone.open());

    if (! allGone.port)
        return;

    allGone.device->submitError = ENODEV;
    allGone.device->submitsAllowed = 0;
    Sink everything;

    for (int i = 0; i < 8; ++i)
        EXPECT (allGone.send (stereoPackets (0, 8, 48)));

    EXPECT (! allGone.service (everything));
    EXPECT (allGone.port->dead && allGone.port->inFlight == 0);
    EXPECT (allGone.port->why == "unplugged");
    EXPECT (everything.frames == 8 * 384);

    // No lines about lost transfers while all goes well.
    Rig fine;
    EXPECT (fine.open());

    if (! fine.port)
        return;

    Sink third;

    for (int i = 0; i < 5; ++i)
    {
        EXPECT (fine.send (stereoPackets (0, 8, 48)));
        EXPECT (fine.service (third));
    }

    EXPECT (fine.port->requeueFailures == 0 && ! fine.log.has ("could not be queued again"));
    EXPECT (summary (*fine.port).find ("lost") == std::string::npos);
}

KTEST (UsbAudioIo, a_stream_with_a_bad_packet_in_every_transfer_goes_on)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    Sink sink;
    std::vector<int> oneBad (8, 0);
    oneBad[3] = -EXDEV;
    std::vector<int> sevenBad (8, -EXDEV);
    sevenBad[5] = 0;

    for (int i = 0; i < 40; ++i)
    {
        EXPECT (r.send (stereoPackets (0, 8, 48), oneBad));
        EXPECT (r.service (sink));
        EXPECT (r.send (stereoPackets (0, 8, 48), sevenBad));
        EXPECT (r.service (sink));
    }

    EXPECT (! r.port->dead && r.port->errors == 0);
    EXPECT (r.port->badPackets == 40 * 1 + 40 * 7);
    EXPECT (sink.frames == 40 * 7 * 48 + 40 * 1 * 48);
}

KTEST (UsbAudioIo, a_packet_that_claims_more_bytes_than_its_slot_holds_is_cut_to_the_slot)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    EXPECT (r.send (stereoPackets (0, 8, 48)));
    EXPECT (r.kernel.editLastFinished (*r.device, [] (usbdevfs_urb& urb) { urb.iso_frame_desc[0].actual_length = 300; }));

    Sink sink;
    EXPECT (r.service (sink));
    EXPECT (sink.frames == 49 + 7 * 48);                    // the slot of packet 0 holds 196 bytes: 49 frames, not 75
    EXPECT (r.port->emptyPackets == 0 && r.port->badPackets == 0);
}

KTEST (UsbAudioIo, only_the_first_few_transfers_get_a_line_in_the_log)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    Sink sink;

    for (int i = 0; i < 6; ++i)
    {
        EXPECT (r.send (stereoPackets (0, 8, 48)));
        EXPECT (r.service (sink));
    }

    EXPECT (r.log.count ("): transfer ") == 4);
    EXPECT (r.log.has ("transfer 4: 8 packets, 8 with sound"));
    EXPECT (! r.log.has ("transfer 5:"));
}

KTEST (UsbAudioIo, a_device_that_does_send_sound_is_never_reported_as_silent)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    Sink sink;

    for (int i = 0; i < 70; ++i)
    {
        EXPECT (r.send (stereoPackets (0, 8, 48)));
        EXPECT (r.service (sink));
    }

    EXPECT (r.log.count ("no sound has arrived") == 0);
    EXPECT (! r.port->silenceNoticed);

    // Nor one that was quiet for a while and then began (the notice is for a device that never does).
    Rig late;
    EXPECT (late.open());

    if (! late.port)
        return;

    Sink other;

    for (int i = 0; i < 63; ++i)
    {
        EXPECT (late.send ({}));
        EXPECT (late.service (other));
    }

    EXPECT (late.send (stereoPackets (0, 8, 48)));
    EXPECT (late.service (other));
    EXPECT (late.log.count ("no sound has arrived") == 0);
}

KTEST (UsbAudioIo, a_queue_that_ran_dry_is_counted)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    Sink sink;

    for (int i = 0; i < 7; ++i)
        EXPECT (r.send (stereoPackets (0, 8, 48)));

    EXPECT (r.service (sink));
    EXPECT (r.port->underruns == 0);                    // one transfer was still queued: the bus never ran out

    for (int i = 0; i < 8; ++i)                         // all of them finish before the program gets to look
        EXPECT (r.send (stereoPackets (0, 8, 48)));

    EXPECT (r.service (sink));
    EXPECT (r.port->underruns == 1);
    EXPECT (r.kernel.pendingCount (*r.device) == 8);
}

KTEST (UsbAudioIo, a_device_that_sends_nothing_is_noticed_once)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    Sink sink;

    for (int i = 0; i < 70; ++i)
    {
        EXPECT (r.send ({}));
        EXPECT (r.service (sink));
    }

    EXPECT (r.log.count ("no sound has arrived") == 1);
    EXPECT (r.log.has ("no sound has arrived in 64 transfers"));
    EXPECT (r.port->frames == 0 && r.port->emptyPackets == 70 * 8);

    EXPECT (r.send (stereoPackets (0, 8, 48)));
    EXPECT (r.service (sink));
    EXPECT (sink.frames == 384);
}

KTEST (UsbAudioIo, the_rate_the_device_really_delivers_is_measured)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    double now = 10.0;
    r.port->clock = [&] { return now; };
    Sink sink;
    EXPECT (r.port->measuredRate() == 0.0);

    for (int i = 0; i < 101; ++i)
    {
        now += 0.008;
        EXPECT (r.send (stereoPackets (0, 8, 48)));
        EXPECT (r.service (sink));

        if (i == 10)
            EXPECT (r.port->measuredRate() == 0.0);         // not enough time has passed to tell
    }

    EXPECT (std::fabs (r.port->measuredRate() - 48000.0) < 1.0);
    EXPECT (summary (*r.port).find ("measured rate 48000 Hz") != std::string::npos);
    EXPECT (summary (*r.port).find ("38784 frames in 101 transfers") != std::string::npos);

    // A device that is a little fast shows it.
    Rig fast;
    EXPECT (fast.open());

    if (! fast.port)
        return;

    now = 100.0;
    fast.port->clock = [&] { return now; };
    Sink other;

    for (int i = 0; i < 101; ++i)
    {
        now += 0.008;
        EXPECT (fast.send (stereoPackets (0, 8, 49)));          // 49 frames a millisecond: 49000 Hz
        EXPECT (fast.service (other));
    }

    EXPECT (std::fabs (fast.port->measuredRate() - 49000.0) < 1.0);
}

// ----------------------------------------------------------------------------- the sample rate

KTEST (UsbAudioIo, the_rate_is_read_back_and_what_the_device_says_is_believed)
{
    Rig r;
    r.state->reported = 44100;                              // asked for 48000, the device answers with 44100
    EXPECT (r.open());
    EXPECT (r.port && r.port->rate == 44100);
    EXPECT (r.log.has ("asked for 48000 Hz, the device says 44100 Hz"));

    Rig silly;
    silly.state->reported = 0x00FFFFFF;                     // nonsense: not believed
    EXPECT (silly.open());
    EXPECT (silly.port && silly.port->rate == 48000);
}

KTEST (UsbAudioIo, a_device_that_stalls_the_rate_is_used_anyway)
{
    Rig r;
    r.state->refuses = true;
    EXPECT (r.open());
    EXPECT (r.port && r.port->rate == 48000);
    EXPECT (r.log.has ("cannot set the rate on endpoint 0x81"));
    EXPECT (r.log.has ("carrying on at 48000 Hz"));
}

KTEST (UsbAudioIo, an_endpoint_that_does_not_take_a_rate_is_not_sent_one)
{
    Rig r (receiver (1, 2, { { 1, 2, 2, 16, { 44100, 48000 }, 0, 0, 1, 1, 196, false } }));
    EXPECT (r.open());
    EXPECT (r.kernel.callCount (*r.device, "control") == 0);
    EXPECT (r.port && r.port->rate == 48000);
}

KTEST (UsbAudioIo, when_the_packets_are_too_small_for_48000_the_rate_that_fits_is_asked_for)
{
    // Stereo 16 bit with room for 100 bytes a millisecond: 24 kHz at most.
    Rig r (receiver (1, 2, { { 1, 2, 2, 16, { 8000, 16000, 24000, 32000, 44100, 48000 }, 0, 0, 1, 1, 100, true } }));
    EXPECT (r.open());
    EXPECT (r.port && r.port->rate == 24000);
    EXPECT (r.state->rate == 24000);
}

// ------------------------------------------------------------------------- the formats, the failures

KTEST (UsbAudioIo, when_the_bandwidth_is_not_there_the_next_setting_is_tried)
{
    Rig r;
    r.device->altErrors[{ 1, 2 }] = ENOSPC;                 // stereo does not fit on the bus
    EXPECT (r.open());

    if (! r.port)
        return;

    const auto calls = r.kernel.callsOf (*r.device);
    EXPECT (contains (calls, "alt 1 2") && contains (calls, "alt 1 1"));
    EXPECT (r.port->format.alt == 1 && r.port->format.channels == 1);
    EXPECT (r.kernel.callCount (*r.device, "submit 81 iso 800") == 8);        // mono: 8 packets of 100 bytes
    EXPECT (r.log.has ("cannot select setting 2: No space left on device (not enough USB bandwidth)"));
}

KTEST (UsbAudioIo, when_no_setting_works_the_open_fails_with_a_reason_and_leaves_nothing_open)
{
    Rig r;
    r.device->altErrors[{ 1, 1 }] = ENOSPC;
    r.device->altErrors[{ 1, 2 }] = ENOSPC;
    EXPECT (! r.open());
    EXPECT (r.reason == "not enough USB bandwidth for the audio");
    EXPECT (! r.kernel.isOpen (*r.device));
    EXPECT (r.log.count ("cannot select setting") == 2);
}

KTEST (UsbAudioIo, only_a_few_formats_are_tried)
{
    Rig r (receiver (1, 2, { { 1, 1, 2, 16, { 48000 }, 0, 0, 1, 1, 100, true }, { 2, 2, 2, 16, { 48000 }, 0, 0, 1, 1, 196, true },
                             { 3, 3, 2, 16, { 48000 }, 0, 0, 1, 1, 292, true }, { 4, 4, 2, 16, { 48000 }, 0, 0, 1, 1, 388, true } }));

    for (unsigned alt = 1; alt <= 4; ++alt)
        r.device->altErrors[{ 1, alt }] = ENOSPC;

    EXPECT (! r.open());
    EXPECT (r.kernel.callCount (*r.device, "alt") == 3);
}

KTEST (UsbAudioIo, a_transfer_the_kernel_will_not_queue_makes_it_try_the_next_setting_and_then_give_up)
{
    Rig r;
    r.device->submitsAllowed = 3;
    r.device->submitError = ENOMEM;
    EXPECT (! r.open());
    EXPECT (r.reason == "the USB audio device would not accept the transfers");
    EXPECT (r.log.has ("cannot queue a transfer on endpoint 0x81: Cannot allocate memory"));
    EXPECT (r.kernel.pendingCount (*r.device) == 0);
    EXPECT (! r.kernel.isOpen (*r.device));

    // Each setting that failed was switched off at once (not left running until the port is closed),
    // and the three transfers of the first one that had got in were fetched.
    EXPECT (r.kernel.callCount (*r.device, "alt 1 0") == 2);
    EXPECT (r.kernel.callCount (*r.device, "reap") == 3);
}

KTEST (UsbAudioIo, a_setting_that_could_only_be_half_queued_is_cleaned_up_before_the_next_one_is_tried)
{
    Rig r;
    r.device->submitError = ENOMEM;
    r.device->submitsAllowed = 3;                           // three transfers of the stereo setting get in, the fourth is refused ...
    r.device->submitsFailing = 1;                           // ... and after that the kernel takes transfers again
    EXPECT (r.open());

    if (! r.port)
        return;

    EXPECT (r.port->format.alt == 1 && r.port->format.channels == 1);        // the mono setting runs
    EXPECT (r.kernel.altOf (*r.device, 1) == 1 && r.port->alt == 1);
    EXPECT (r.kernel.callCount (*r.device, "submit 81 iso 1568") == 4);       // the stereo setting: four tries
    EXPECT (r.kernel.callCount (*r.device, "submit 81 iso 800") == 8);        // the mono one: eight transfers of 8 packets of 100 bytes

    // Nothing of the first attempt is left: its three transfers were cancelled and fetched, so the
    // first look at the queue finds only the new ones.
    EXPECT (r.kernel.callCount (*r.device, "reap") == 3);
    EXPECT (r.kernel.completedCount (*r.device) == 0);
    EXPECT (r.kernel.pendingCount (*r.device) == 8);
    EXPECT (r.port->requests.size() == 8 && r.port->inFlight == 8);

    EXPECT (r.send (monoPackets (0, 8, 50)));
    Sink sink (1);
    EXPECT (r.service (sink));
    EXPECT (! r.port->dead && r.port->why.empty());
    EXPECT (sink.frames == 400 && r.port->transfers == 1 && r.port->inFlight == 8);
}

KTEST (UsbAudioIo, stopping_the_stream_switches_the_device_off_and_fetches_every_cancelled_transfer)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    EXPECT (r.kernel.pendingCount (*r.device) == 8 && r.port->requests.size() == 8 && r.port->inFlight == 8 && r.port->alt == 2);
    stopStream (*r.port);

    EXPECT (r.port->alt == 0 && r.kernel.altOf (*r.device, 1) == 0);
    EXPECT (r.port->requests.empty() && r.port->inFlight == 0);
    EXPECT (r.kernel.pendingCount (*r.device) == 0);
    EXPECT (r.kernel.completedCount (*r.device) == 0);                        // none of the cancelled transfers is left in the kernel
    EXPECT (r.kernel.callCount (*r.device, "reap") == 8);

    r.port.reset();                                                           // closing finds the interface in setting 0 already
    EXPECT (r.kernel.callCount (*r.device, "alt 1 0") == 1);
}

KTEST (UsbAudioIo, packets_longer_than_the_endpoint_allows_make_it_fall_back_to_the_smaller_setting)
{
    Rig r;
    r.device->isoMaxPacket = 100;                           // the stereo setting says 196: the kernel refuses (EMSGSIZE)
    EXPECT (r.open());
    EXPECT (r.port && r.port->format.channels == 1);
    EXPECT (r.log.has ("(the packets are longer than the endpoint allows)"));
    EXPECT (r.kernel.pendingCount (*r.device) == 8);        // none of the refused setting's transfers are left
}

KTEST (UsbAudioIo, an_interface_that_a_driver_has_is_reported)
{
    Rig r;
    r.device->claimErrors[1] = EBUSY;
    EXPECT (! r.open());
    EXPECT (r.reason == "another driver or program is using this USB audio device");
    EXPECT (r.log.has ("cannot claim interface 1: Device or resource busy (a kernel driver or another program is using it)"));
    EXPECT (! r.kernel.isOpen (*r.device));
}

KTEST (UsbAudioIo, the_audio_control_interface_may_be_refused)
{
    Rig r;
    r.device->claimErrors[0] = EBUSY;
    EXPECT (r.open());
    EXPECT (r.log.has ("audio control interface 0 not claimed"));
    EXPECT (r.port && r.port->rate == 48000);               // a version 1 rate is set on the endpoint, which needs no control interface
}

KTEST (UsbAudioIo, a_device_that_cannot_be_opened_is_reported)
{
    Rig denied;
    denied.device->openError = EACCES;
    EXPECT (! denied.open());
    EXPECT (denied.reason == "cannot open the USB device");
    EXPECT (denied.log.has ("(Kerf needs to run as root for this)"));

    Rig missing;
    missing.device->nodeExists = false;
    EXPECT (missing.open());
    EXPECT (missing.kernel.madeNodes == std::vector<std::string> ({ "/dev/bus/usb/004/002 189:385" }));
    EXPECT (missing.log.has ("made the missing device node /dev/bus/usb/004/002"));

    Rig cannot;
    cannot.device->nodeExists = false;
    cannot.device->makeNodeFails = true;
    EXPECT (! cannot.open());
    EXPECT (cannot.log.has ("there is no device node /dev/bus/usb/004/002 and it cannot be made"));
}

KTEST (UsbAudioIo, closing_the_port_stops_the_device_and_releases_everything)
{
    Rig r;
    EXPECT (r.open());
    r.port.reset();

    const auto calls = r.kernel.callsOf (*r.device);
    EXPECT (calls.size() >= 4);

    if (calls.size() >= 4)
    {
        const std::vector<std::string> tail (calls.end() - 4, calls.end());
        EXPECT (tail == std::vector<std::string> ({ "alt 1 0", "release 1", "release 0", "close" }));
    }

    EXPECT (! r.kernel.isOpen (*r.device));
    EXPECT (r.kernel.pendingCount (*r.device) == 0);
}

KTEST (UsbAudioIo, thirty_two_bit_floating_point_sound_goes_through_the_whole_path)
{
    Format1 f;
    f.channels = 1; f.subslot = 4; f.bits = 32; f.tag = 3; f.rates = { 48000 }; f.packet = 196; f.rateControl = true;
    Rig r (receiver (1, 2, { f }));
    EXPECT (r.open());

    if (! r.port)
        return;

    float values[4] = { 0.5f, -0.25f, std::numeric_limits<float>::quiet_NaN(), 1.0f };
    Bytes bytes (sizeof values);
    std::memcpy (bytes.data(), values, sizeof values);

    EXPECT (r.send ({ bytes }));
    Sink sink (1);
    EXPECT (r.service (sink));
    EXPECT (sink.frames == 4 && sink.samples.size() == 4);

    if (sink.samples.size() == 4)
        EXPECT (sink.samples[0] == 0.5f && sink.samples[1] == -0.25f && sink.samples[2] == 0.0f && sink.samples[3] == 1.0f);
}

// ----------------------------------------------------------------------- the version 2 mixer

KTEST (UsbAudioIo, a_mixer_is_set_up_through_the_clock_of_its_recording_stream)
{
    MixerRig m;
    EXPECT (m.open());

    if (! m.port)
        return;

    const auto calls = m.kernel.callsOf (*m.device);
    const std::vector<std::string> expected = { "open", "claim 0", "claim 2", "alt 2 1",
                                                "control a1 01 0100 2800 4",        // CUR of the clock: 44100 for now
                                                "control a1 02 0100 2800 2",        // RANGE: how many ranges
                                                "control a1 02 0100 2800 26",       // RANGE: the two ranges
                                                "control 21 01 0100 2800 4",        // SET_CUR: 48000
                                                "control a1 01 0100 2800 4" };      // CUR again
    EXPECT (calls.size() >= expected.size());
    EXPECT (std::equal (expected.begin(), expected.end(), calls.begin()));
    EXPECT (! contains (calls, "claim 1"));                                         // the playback interface is left alone
    EXPECT (m.kernel.autoClaimCount (*m.device) == 0);
    EXPECT (m.state->clocks[0x28].rate == 48000);

    EXPECT (m.port->rate == 48000);
    EXPECT (m.port->format.channels == 12 && m.port->format.subslot == 3);
    EXPECT (m.kernel.callCount (*m.device, "submit 82 iso 23040") == 8);            // 64 microframes of 360 bytes
    EXPECT (m.log.has ("recording: setting 1, 12 channels, 3 bytes per sample, 48000 Hz, 8 transfers of 64 packets of 360 bytes on endpoint 0x82 (high speed)"));

    // 12 channels of 24 bit, 6 frames in a microframe: frame f, channel c is (c + 1) * 1000 + f.
    const auto packet = [] (int first)
    {
        Bytes b;

        for (int f = 0; f < 6; ++f)
            for (int c = 0; c < 12; ++c)
                put24 (b, (c + 1) * 1000 + first + f);

        return b;
    };

    std::vector<Bytes> packets;

    for (int p = 0; p < 64; ++p)
        packets.push_back (packet (p * 6));

    EXPECT (m.kernel.deliverIso (*m.device, packets, 0x82));
    Sink sink (12);
    EXPECT (serviceAudio (*m.port, sink.deliver(), m.log.logger(), POLLOUT));
    EXPECT (sink.frames == 384 && sink.samples.size() == 384 * 12);

    if (sink.samples.size() == 384 * 12)
    {
        EXPECT (near (sink.samples[0], 1000.0f / 8388608.0f));
        EXPECT (near (sink.samples[11], 12000.0f / 8388608.0f));
        EXPECT (near (sink.samples[12 * 100 + 5], (6 * 1000.0f + 100.0f) / 8388608.0f));
    }
}

KTEST (UsbAudioIo, a_clock_that_cannot_be_set_is_used_as_it_is)
{
    MixerRig m (false, 0x2A);                               // the recording runs on the read-only clock
    m.state->clocks[0x2A].rate = 44100;
    EXPECT (m.open());

    if (! m.port)
        return;

    EXPECT (m.port->rate == 44100);
    EXPECT (m.kernel.callCount (*m.device, "control 21") == 0);                     // no SET_CUR was sent
    EXPECT (m.kernel.callCount (*m.device, "control a1 01 0100 2a00 4") == 1);
}

KTEST (UsbAudioIo, a_clock_selector_is_asked_which_input_it_has_chosen)
{
    MixerRig m (true, 0x29);
    m.state->selectorPin = 2;                               // the second input: the clock 0x2A, read only at 48000
    EXPECT (m.open());

    if (! m.port)
        return;

    const auto calls = m.kernel.callsOf (*m.device);
    EXPECT (contains (calls, "control a1 01 0100 2900 1"));
    EXPECT (contains (calls, "control a1 01 0100 2a00 4"));
    EXPECT (m.kernel.callCount (*m.device, "control 21") == 0);
    EXPECT (m.port->rate == 48000);

    MixerRig stalled (true, 0x29);
    stalled.state->selectorFails = true;                    // not answered: the first input, the clock 0x28, which can be set
    EXPECT (stalled.open());
    EXPECT (stalled.port && stalled.state->clocks[0x28].rate == 48000);
}

KTEST (UsbAudioIo, a_mixer_that_stalls_every_clock_request_is_still_opened)
{
    MixerRig m;
    m.device->onControl = nullptr;                          // every control transfer stalls
    EXPECT (m.open());
    EXPECT (m.port && m.port->rate == 48000);               // nothing is known: the usual rate is assumed
}

KTEST (UsbAudioIo, a_clock_that_already_runs_at_the_wanted_rate_is_not_set_again)
{
    MixerRig m;
    m.state->clocks[0x28].rate = 48000;
    EXPECT (m.open());
    EXPECT (m.port && m.port->rate == 48000);
    EXPECT (m.kernel.callCount (*m.device, "control 21") == 0);
    EXPECT (m.kernel.callCount (*m.device, "control a1 01") == 1);      // the rate was only read
}

KTEST (UsbAudioIo, the_rate_a_clock_reports_after_it_was_set_is_the_one_used)
{
    MixerRig m;
    EXPECT (m.open());
    EXPECT (m.port && m.port->rate == 48000);
    EXPECT (! m.log.has ("asked the clock"));                           // it took what it was asked: nothing to say

    MixerRig stays;
    stays.state->clocks[0x28].takes = 44100;                            // asked for 48000, it stays where it is
    EXPECT (stays.open());
    EXPECT (stays.port && stays.port->rate == 44100);
    EXPECT (stays.log.has ("asked the clock for 48000 Hz, the device says 44100 Hz"));

    MixerRig silly;
    silly.state->clocks[0x28].takes = 0x00FFFFFF;                       // nonsense is not believed
    EXPECT (silly.open());
    EXPECT (silly.port && silly.port->rate == 48000);
    EXPECT (! silly.log.has ("the device says"));
}

KTEST (UsbAudioIo, a_clock_that_refuses_to_be_set_is_used_at_the_rate_it_has)
{
    MixerRig m;
    m.state->clocks[0x28].programmable = false;             // the descriptor says it can be set, the device stalls the request
    EXPECT (m.open());
    EXPECT (m.port && m.port->rate == 44100);
    EXPECT (m.log.has ("cannot set the clock 28 to 48000 Hz"));
    EXPECT (m.state->clocks[0x28].rate == 44100);
}

KTEST (UsbAudioIo, a_rate_the_packets_cannot_carry_is_not_chosen_when_another_one_fits)
{
    // 12 channels of 24 bit are 36 bytes a frame. A packet of 200 bytes holds 5 frames: 32000 Hz needs
    // 4 in each microframe, 48000 Hz needs 6.
    MixerRig m (false, 0x28, 200);
    m.state->clocks[0x28].ranges = { { 32000, 32000 }, { 48000, 48000 } };
    EXPECT (m.open());
    EXPECT (m.port && m.port->rate == 32000);
    EXPECT (m.state->clocks[0x28].rate == 32000);

    // When none of them fits, they are all offered: better to try than to give up.
    MixerRig none (false, 0x28, 200);
    none.state->clocks[0x28].ranges = { { 48000, 48000 } };
    EXPECT (none.open());
    EXPECT (none.port && none.port->rate == 48000);

    // With room for both, the usual choice holds.
    MixerRig roomy (false, 0x28, 360);
    roomy.state->clocks[0x28].ranges = { { 32000, 32000 }, { 48000, 48000 } };
    EXPECT (roomy.open());
    EXPECT (roomy.port && roomy.port->rate == 48000);
}

KTEST (UsbAudioIo, the_ranges_a_clock_offers_are_all_read_up_to_the_most_the_buffer_holds)
{
    // Only the third range has 48000 Hz.
    MixerRig three;
    three.state->clocks[0x28].ranges = { { 8000, 8000 }, { 16000, 16000 }, { 48000, 48000 } };
    EXPECT (three.open());
    EXPECT (three.port && three.port->rate == 48000);

    // A clock that claims forty ranges is read as far as the thirty-second and no further.
    MixerRig many;
    auto& ranges = many.state->clocks[0x28].ranges;
    ranges.clear();

    for (int i = 1; i <= 32; ++i)
        ranges.push_back ({ 1000 * i, 1000 * i });

    for (int i = 0; i < 8; ++i)
        ranges.push_back ({ 44100, 44100 });                // out of reach

    EXPECT (many.open());
    EXPECT (many.port && many.port->rate == 32000);          // nothing of 44100 or 48000 among the ones that were read

    for (const auto& c : many.kernel.controlsOf (*many.device))
        EXPECT (c.length <= 2 + 12 * 32);                    // no request asks for more than the buffer holds
}

KTEST (UsbAudioIo, a_control_request_longer_than_its_buffer_is_never_sent)
{
    Rig r;
    EXPECT (r.open());

    if (! r.port)
        return;

    const int before = r.kernel.callCount (*r.device, "control");
    uint8_t tooSmall[2] = {};
    errno = 0;
    EXPECT (detail::control (*r.port, rateRequestV1 (false, 0x81), tooSmall, sizeof tooSmall) < 0 && errno == EINVAL);
    EXPECT (r.kernel.callCount (*r.device, "control") == before);               // it never reached the device

    uint8_t enough[3] = {};
    EXPECT (detail::control (*r.port, rateRequestV1 (false, 0x81), enough, sizeof enough) == 3);
    EXPECT (r.kernel.callCount (*r.device, "control") == before + 1);
}

KTEST (UsbAudioIo, a_version_2_stream_without_a_control_interface_is_opened_without_asking_for_a_rate)
{
    MixerRig m;
    m.cand.stream.controlInterface = -1;
    EXPECT (m.open());
    EXPECT (m.port && m.port->rate == 48000);
    EXPECT (m.log.has ("no audio control interface: the rate is not known"));
    EXPECT (m.kernel.callCount (*m.device, "control") == 0);
    EXPECT (! contains (m.kernel.callsOf (*m.device), "claim 0"));      // there is nothing to claim
}

// ------------------------------------------------------------------------- how much a transfer holds

KTEST (UsbAudioIo, a_transfer_holds_about_eight_milliseconds_whatever_the_interval_of_the_endpoint)
{
    Format f;
    f.packet = 392;

    f.interval = 1;  EXPECT (packetsPerRequest (f, 12) == 8);       // full speed: a packet every millisecond
    f.interval = 2;  EXPECT (packetsPerRequest (f, 12) == 4);       // every other millisecond
    f.interval = 4;  EXPECT (packetsPerRequest (f, 12) == 1);
    f.interval = 16; EXPECT (packetsPerRequest (f, 12) == 1);       // never fewer than one

    f.interval = 1;  EXPECT (packetsPerRequest (f, 480) == 64);     // high speed: a packet every 125 microseconds
    f.interval = 2;  EXPECT (packetsPerRequest (f, 480) == 32);
    f.interval = 4;  EXPECT (packetsPerRequest (f, 480) == 8);

    f.interval = 1;                                                 // speed not known: big packets mean high speed
    EXPECT (packetsPerRequest (f, 0) == 8);
    f.packet = 1024;
    EXPECT (packetsPerRequest (f, 0) == 64);
}

KTEST (UsbAudioIo, an_endpoint_with_a_longer_interval_gets_fewer_packets_in_a_transfer)
{
    // Full speed, a packet every other millisecond: 96 frames of 4 bytes.
    Format1 f;
    f.channels = 2;
    f.rates = { 48000 };
    f.packet = 384;
    f.interval = 2;
    Rig r (receiver (1, 2, { f }));
    EXPECT (r.open());

    if (! r.port)
        return;

    int packetLength = 0;
    EXPECT (r.kernel.callCount (*r.device, "submit 81 iso 1536") == 8);
    EXPECT (r.kernel.waitingPackets (*r.device, 0x81, &packetLength) == 4 && packetLength == 384);
    EXPECT (r.port->rate == 48000);
    EXPECT (r.log.has ("8 transfers of 4 packets of 384 bytes"));

    // High speed, a packet every 250 microseconds.
    MixerRig m (false, 0x28, 440, 2);
    m.device->isoMaxPacket = 0;
    EXPECT (m.open());

    if (! m.port)
        return;

    EXPECT (m.kernel.callCount (*m.device, "submit 82 iso 14080") == 8);        // 32 packets of 440 bytes
    EXPECT (m.log.has ("8 transfers of 32 packets of 440 bytes"));
}

#endif // __linux__
