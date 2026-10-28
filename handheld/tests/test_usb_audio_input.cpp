// Tests for the Record screen's USB input: Kerf's own reading of a USB microphone receiver or
// mixer (usb_audio_input.cpp), seen from where the screen sees it, the InputHub. A pretend /sys lists
// the devices, a pretend kernel stands in for usbfs (tests/fake_usb_kernel.h) and the test plays the
// device: it finishes the transfers that Kerf queued with sound in them, the way a receiver would.
// What the reader does with the kernel, packet by packet, is tested in test_usb_audio_io.cpp.

#if defined(__linux__)

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio_input.h"
#include "fake_usb_kernel.h"
#include "minitest.h"
#include "usb_audio_devices.h"
#include "usb_audio_fixtures.h"
#include "usb_audio_input.h"
#include "usb_fixtures.h"

using namespace kerf::ui;
using namespace kerf::ui::usbfake;
using namespace usbaudiofixtures;
using usbfixtures::FakeSysfs;

namespace
{
    constexpr int kFramesPerPacket = 48;            // 48 kHz at full speed: one packet each millisecond
    constexpr int kFramesPerTransfer = 8 * kFramesPerPacket;

    // What the pretend microphone hears: a saw tooth that carries on from one packet to the next.
    Bytes sawPacket (long n)
    {
        Bytes b;

        for (int f = 0; f < kFramesPerPacket; ++f)
        {
            const int v = (int) ((n * kFramesPerPacket + f) % 1000) - 500;
            put16 (b, v);
            put16 (b, -v);
        }

        return b;
    }

    // Does the sound follow that saw tooth, frame after frame, from whichever frame it starts at?
    bool followsTheSaw (const std::vector<float>& s)
    {
        if (s.size() < 4 || s.size() % 2 != 0)
            return false;

        const int start = (int) std::lround (s[0] * 32768.0f) + 500;

        for (size_t i = 0; i < s.size(); i += 2)
        {
            const int v = ((start + (int) (i / 2)) % 1000) - 500;

            if (s[i] != (float) v / 32768.0f || s[i + 1] != (float) (-v) / 32768.0f)
                return false;
        }

        return true;
    }

    // One pretend receiver in a pretend /sys, and the hub the Record screen has.
    struct Rig
    {
        FakeSysfs sys;
        FakeKernel kernel;
        Lines log;
        std::shared_ptr<Receiver> state = std::make_shared<Receiver>();
        FakeDevice* device = nullptr;
        long packet = 0;                            // the packets of the saw tooth that the device has sent
        InputHub hub;                               // last: it stops the reading thread before the rest goes away

        explicit Rig (bool plugged = true)
        {
            if (plugged)
                plug();

            UsbAudioInputOptions options;
            options.os = &kernel;
            options.sysfs = sys.root;
            hub.setLogger (log.logger());
            hub.add (makeUsbAudioInput (options));
        }

        // A receiver as USB device `number` of bus 4, in the port "4-<number - 1>".
        FakeDevice& plug (int number = 2, const char* product = "Receiver")
        {
            const std::string port = "4-" + std::to_string (number - 1);
            sys.device (port, receiver(), 1, 4, number, "", product, "1234", "5678");
            sys.text (sys.root / port / "speed", "12\n");
            auto& d = addReceiver (kernel, state, 4, number);

            if (device == nullptr)
                device = &d;

            return d;
        }

        // The one receiver, opened.
        bool open()
        {
            const auto listed = hub.sources();

            if (listed.size() != 1 || ! hub.open (listed[0]))
                return false;

            return waitFor ([&] { return hub.state() != InputState::starting; });
        }

        // The device sends `count` transfers' worth of the saw tooth, each as soon as Kerf has queued it.
        bool send (int count)
        {
            const auto end = std::chrono::steady_clock::now() + std::chrono::seconds (10);

            for (int sent = 0; sent < count;)
            {
                if (kernel.deliverIsoWith (*device, [this] (int) { return sawPacket (packet++); }, 0x81))
                    ++sent;
                else if (std::chrono::steady_clock::now() > end)
                    return false;
                else
                    std::this_thread::sleep_for (std::chrono::microseconds (100));
            }

            return true;
        }

        // Reads from the hub, as the screen does, until `frames` frames have been collected.
        bool collect (std::vector<float>& out, int frames, int channels = 2, int timeoutMs = 5000)
        {
            std::vector<float> block ((size_t) channels * 512);

            return waitFor ([&]
            {
                for (;;)
                {
                    const int n = hub.read (block.data(), 512);

                    if (n <= 0)
                        break;

                    out.insert (out.end(), block.begin(), block.begin() + (std::ptrdiff_t) n * channels);
                }

                return (int) (out.size() / (size_t) channels) >= frames;
            }, timeoutMs);
        }

        bool framesAtLeast (uint64_t n) { return waitFor ([&] { return hub.status().frames >= n; }); }
    };
}

// ---------------------------------------------------------------------------------------- the list

KTEST (UsbAudioInput, a_receiver_is_listed_with_a_name_an_id_that_stays_and_what_it_is)
{
    Rig r;
    const auto listed = r.hub.sources();
    EXPECT (listed.size() == 1);

    if (listed.size() != 1)
        return;

    EXPECT (listed[0].id == "usb:1234:5678@4-1#1");
    EXPECT (listed[0].name == "Receiver");
    EXPECT (listed[0].detail == "USB, 2 channels, read by Kerf");
    EXPECT (listed[0].direct);
    EXPECT (listed[0].backend == 0);

    // Unplugged, nothing is listed; plugged in to the same port again, it has the same id.
    r.sys.remove ("4-1");
    EXPECT (r.hub.sources().empty());
    r.sys.device ("4-1", receiver(), 1, 4, 5, "", "Receiver", "1234", "5678");
    const auto again = r.hub.sources();
    EXPECT (again.size() == 1 && again[0].id == listed[0].id);
}

KTEST (UsbAudioInput, two_receivers_of_the_same_kind_are_told_apart_by_their_port)
{
    Rig r;
    r.plug (3);
    const auto listed = r.hub.sources();
    EXPECT (listed.size() == 2);

    if (listed.size() != 2)
        return;

    EXPECT (listed[0].name == "Receiver (4-1)" && listed[1].name == "Receiver (4-2)");
    EXPECT (listed[0].id == "usb:1234:5678@4-1#1" && listed[1].id == "usb:1234:5678@4-2#1");

    // A receiver of another kind in the same company keeps its plain name.
    r.sys.device ("4-3", receiver (0x1234, 0x9999), 1, 4, 4, "", "Other receiver", "1234", "9999");
    const auto three = r.hub.sources();
    EXPECT (three.size() == 3);

    if (three.size() == 3)
    {
        EXPECT (three[2].name == "Other receiver");
        EXPECT (three[2].id == "usb:1234:9999@4-3#1");
    }
}

KTEST (UsbAudioInput, a_mixer_is_offered_with_all_the_channels_of_its_recording_stream)
{
    Rig r (false);
    r.sys.device ("3-2", mixer(), 1, 3, 5, "TASCAM", "Model 12", "0644", "805f");
    const auto listed = r.hub.sources();
    EXPECT (listed.size() == 1);

    if (listed.size() != 1)
        return;

    EXPECT (listed[0].name == "TASCAM Model 12");
    EXPECT (listed[0].id == "usb:0644:805f@3-2#2");                 // the recording interface is the second one
    EXPECT (listed[0].detail == "USB, 12 channels, read by Kerf");
}

KTEST (UsbAudioInput, a_receiver_with_one_microphone_says_one_channel)
{
    Rig r (false);
    r.sys.device ("4-1", receiver (0x1234, 0x5678, { { 1, 1, 2, 16, { 44100, 48000 }, 0, 0, 1, 1, 100, true } }), 1, 4, 2, "", "Mono receiver", "1234", "5678");
    const auto listed = r.hub.sources();
    EXPECT (listed.size() == 1);

    if (listed.size() == 1)
        EXPECT (listed[0].detail == "USB, 1 channel, read by Kerf");
}

KTEST (UsbAudioInput, a_speaker_is_not_a_microphone_and_the_log_says_so)
{
    Rig r (false);
    r.sys.device ("5-1", speaker(), 1, 5, 2, "", "Speaker", "0d8c", "000c");
    EXPECT (r.hub.sources().empty());
    EXPECT (r.log.has ("(0d8c:000c Speaker): playback interface 1"));
    EXPECT (r.log.has ("no recording stream that Kerf can use"));
}

KTEST (UsbAudioInput, nothing_plugged_in_lists_nothing_and_logs_nothing)
{
    Rig r (false);
    EXPECT (r.hub.sources().empty());
    EXPECT (r.log.count ("audio:") == 0);
}

KTEST (UsbAudioInput, every_audio_device_is_described_in_the_log_once_with_its_descriptors)
{
    Rig r;

    for (int i = 0; i < 4; ++i)
        r.hub.sources();

    EXPECT (r.log.count ("audio: usb 4-1 (1234:5678 Receiver): recording interface 1 (audio class 1)") == 1);
    EXPECT (r.log.count ("audio: usb 4-1 (1234:5678 Receiver): descriptors 09 02 ") == 1);

    r.plug (3);
    r.hub.sources();
    r.hub.sources();
    EXPECT (r.log.count ("): descriptors ") == 2);                  // the new one only
    EXPECT (r.log.count ("audio: usb 4-2 (1234:5678 Receiver): descriptors ") == 1);
}

KTEST (UsbAudioInput, a_receiver_that_the_kernel_has_a_driver_for_is_left_to_it_and_said_once)
{
    Rig r;
    EXPECT (r.sys.bind ("4-1:1.1", "snd-usb-audio"));

    for (int i = 0; i < 3; ++i)
        EXPECT (r.hub.sources().empty());

    EXPECT (r.log.count ("the kernel driver 'snd-usb-audio' has the interface, Kerf leaves it to the driver") == 1);
    EXPECT (r.log.count ("(another program is reading it)") == 0);

    // The same for a program that has the device through usbfs.
    Rig other;
    EXPECT (other.sys.bind ("4-1:1.1", "usbfs"));
    EXPECT (other.hub.sources().empty());
    EXPECT (other.hub.sources().empty());
    EXPECT (other.log.count ("the kernel driver 'usbfs' has the interface (another program is reading it)") == 1);
}

KTEST (UsbAudioInput, every_receiver_that_the_kernel_has_a_driver_for_is_said_once)
{
    Rig r;
    r.plug (3);
    EXPECT (r.sys.bind ("4-1:1.1", "snd-usb-audio"));
    EXPECT (r.sys.bind ("4-2:1.1", "snd-usb-audio"));

    for (int i = 0; i < 3; ++i)
        EXPECT (r.hub.sources().empty());

    EXPECT (r.log.count ("the kernel driver 'snd-usb-audio' has the interface, Kerf leaves it to the driver") == 2);   // one for each

    // One that comes later is said when it comes, and the first two are not said again.
    r.plug (4);
    EXPECT (r.sys.bind ("4-3:1.1", "snd-usb-audio"));
    EXPECT (r.hub.sources().empty());
    EXPECT (r.hub.sources().empty());
    EXPECT (r.log.count ("the kernel driver 'snd-usb-audio' has the interface, Kerf leaves it to the driver") == 3);
}

KTEST (UsbAudioInput, the_search_folder_can_be_given_by_an_environment_variable)
{
    FakeSysfs sys;
    sys.device ("4-1", receiver(), 1, 4, 2, "", "Receiver", "1234", "5678");
    ::setenv ("KERF_USB_SYSFS", sys.root.c_str(), 1);

    const auto backend = makeUsbAudioInput();
    const auto listed = backend ? backend->list() : std::vector<InputSource> {};
    ::unsetenv ("KERF_USB_SYSFS");

    EXPECT (backend != nullptr);
    EXPECT (listed.size() == 1);
}

KTEST (UsbAudioInput, the_devices_are_looked_for_in_sys_unless_a_variable_says_otherwise)
{
    ::unsetenv ("KERF_USB_SYSFS");
    EXPECT (usbsys::devicesDir() == std::filesystem::path ("/sys/bus/usb/devices"));
    ::setenv ("KERF_USB_SYSFS", "", 1);
    EXPECT (usbsys::devicesDir() == std::filesystem::path ("/sys/bus/usb/devices"));         // an empty one says nothing
    ::setenv ("KERF_USB_SYSFS", "/somewhere/else", 1);
    EXPECT (usbsys::devicesDir() == std::filesystem::path ("/somewhere/else"));
    ::unsetenv ("KERF_USB_SYSFS");
}

// ------------------------------------------------------------------------------ the stream

KTEST (UsbAudioInput, a_receiver_streams_its_sound_to_the_screen_without_losing_a_frame)
{
    Rig r;
    EXPECT (r.open());
    EXPECT (r.hub.state() == InputState::running);

    const InputStatus first = r.hub.status();
    EXPECT (first.channels == 2 && first.sampleRate == 48000);
    EXPECT (first.message == "listening");
    EXPECT (first.frames == 0 && first.dropped == 0);
    EXPECT (r.hub.current().id == "usb:1234:5678@4-1#1");
    EXPECT (r.state->rate == 48000);

    // A device that sends twice as fast as the clock says: it is the pace that is measured.
    IsoPump pump (r.kernel, *r.device, 0x81, [] (long n) { return sawPacket (n); }, 4);

    std::vector<float> heard;
    EXPECT (r.collect (heard, 60000, 2, 20000));
    EXPECT (heard.size() >= 120000);
    EXPECT (followsTheSaw (heard));

    EXPECT (r.framesAtLeast (60000));                                // the figures follow the sound a moment later
    const InputStatus now = r.hub.status();
    EXPECT (now.state == InputState::running);
    EXPECT (now.frames >= 60000);
    EXPECT (now.dropped == 0);
    EXPECT (now.measuredRate > 10000.0 && now.measuredRate < 400000.0);

    pump.stop();
    r.hub.close();
    EXPECT (r.hub.state() == InputState::idle);
    EXPECT (! r.kernel.isOpen (*r.device));                          // the file is closed ...
    EXPECT (! r.kernel.isClaimed (*r.device));                       // ... the interfaces are given back ...
    EXPECT (r.log.has ("(1234:5678 Receiver): closed: "));           // ... and the log has the totals
    EXPECT (r.log.has (" frames in "));
    EXPECT (r.log.has ("recording: setting 2, 2 channels, 2 bytes per sample, 48000 Hz"));
}

KTEST (UsbAudioInput, the_ring_holds_two_seconds_and_what_does_not_fit_is_counted)
{
    Rig r;
    EXPECT (r.open());

    // 160 transfers are 1.28 s of sound: the screen has not read any of it yet, and it all stays.
    EXPECT (r.send (160));
    EXPECT (r.framesAtLeast (160 * kFramesPerTransfer));
    EXPECT (r.hub.status().dropped == 0);

    std::vector<float> heard;
    EXPECT (r.collect (heard, 160 * kFramesPerTransfer));
    EXPECT (heard.size() == (size_t) 160 * kFramesPerTransfer * 2 && followsTheSaw (heard));

    // 300 transfers are 2.4 s: the first two seconds are kept and the last 0.4 s is counted as lost.
    EXPECT (r.send (300));
    EXPECT (r.framesAtLeast ((160 + 300) * kFramesPerTransfer));
    const InputStatus full = r.hub.status();
    EXPECT (full.dropped == 50 * (uint64_t) kFramesPerTransfer);

    std::vector<float> rest;
    EXPECT (r.collect (rest, 250 * kFramesPerTransfer));
    EXPECT (rest.size() == (size_t) 250 * kFramesPerTransfer * 2 && followsTheSaw (rest));
    EXPECT (r.hub.read (rest.data(), 16) == 0);
}

KTEST (UsbAudioInput, a_receiver_that_is_unplugged_ends_the_stream_but_what_it_sent_can_still_be_read)
{
    Rig r;
    EXPECT (r.open());
    EXPECT (r.send (10));
    EXPECT (r.framesAtLeast (10 * kFramesPerTransfer));

    r.kernel.unplug (*r.device);
    EXPECT (waitFor ([&] { return r.hub.state() == InputState::ended; }));

    const InputStatus status = r.hub.status();
    EXPECT (status.state == InputState::ended);
    EXPECT (status.message == "unplugged");
    EXPECT (r.log.has ("(1234:5678 Receiver): stopped (unplugged): 3840 frames in 10 transfers, "));      // the log has the totals

    std::vector<float> heard;
    EXPECT (r.collect (heard, 10 * kFramesPerTransfer));
    EXPECT (heard.size() == (size_t) 10 * kFramesPerTransfer * 2 && followsTheSaw (heard));

    r.hub.close();
    EXPECT (r.hub.state() == InputState::idle);
}

KTEST (UsbAudioInput, an_error_on_the_file_ends_the_stream_even_when_the_kernel_still_answers)
{
    Rig r;
    EXPECT (r.open());
    EXPECT (r.send (4));
    EXPECT (r.framesAtLeast (4 * kFramesPerTransfer));

    r.kernel.hangUp (*r.device);
    EXPECT (waitFor ([&] { return r.hub.state() == InputState::ended; }));
    EXPECT (r.hub.status().message == "unplugged");
    EXPECT (r.log.has ("(1234:5678 Receiver): stopped (unplugged): "));

    r.hub.close();
    EXPECT (r.hub.state() == InputState::idle);
}

KTEST (UsbAudioInput, the_screen_sees_the_input_starting_while_the_device_is_being_set_up)
{
    Rig r;
    const auto before = r.device->onControl;
    r.device->onControl = [before] (usbdevfs_ctrltransfer& t) -> int
    {
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
        return before(t);
    };

    const auto listed = r.hub.sources();
    EXPECT (listed.size() == 1);

    if (listed.empty())
        return;

    EXPECT (r.hub.open (listed[0]));
    EXPECT (r.hub.state() == InputState::starting);
    EXPECT (r.hub.status().state == InputState::starting);

    std::vector<float> nothing (16);
    EXPECT (r.hub.read (nothing.data(), 8) == 0);                    // no sound yet, and the ring is not touched

    EXPECT (waitFor ([&] { return r.hub.state() == InputState::running; }));
    EXPECT (r.hub.status().channels == 2);
}

KTEST (UsbAudioInput, the_device_being_read_stays_in_the_list_though_usbfs_has_its_interface_now)
{
    Rig r;
    EXPECT (r.open());
    EXPECT (r.sys.bind ("4-1:1.1", "usbfs"));                        // what the kernel shows for an interface that a program claimed

    for (int i = 0; i < 3; ++i)
    {
        const auto listed = r.hub.sources();
        EXPECT (listed.size() == 1 && listed[0].id == "usb:1234:5678@4-1#1");
    }

    EXPECT (r.log.count ("another program is reading it") == 0);

    // Closed, the interface is nobody's: the kernel would show no driver then, and the pretend /sys still does.
    r.hub.close();
    EXPECT (r.hub.sources().empty());
    EXPECT (r.log.count ("another program is reading it") == 1);
}

KTEST (UsbAudioInput, opening_again_starts_a_new_stream_with_nothing_left_from_the_old_one)
{
    Rig r;
    EXPECT (r.open());
    EXPECT (r.send (5));
    EXPECT (r.framesAtLeast (5 * kFramesPerTransfer));
    r.hub.close();
    EXPECT (r.kernel.openCount (*r.device) == 1);

    EXPECT (r.open());
    EXPECT (r.kernel.openCount (*r.device) == 2);
    EXPECT (r.hub.state() == InputState::running);
    EXPECT (r.hub.status().frames == 0 && r.hub.status().dropped == 0);
    EXPECT (r.hub.status().message == "listening");

    std::vector<float> none (16);
    EXPECT (r.hub.read (none.data(), 8) == 0);                       // the first stream's sound is not there any more

    EXPECT (r.send (3));
    std::vector<float> heard;
    EXPECT (r.collect (heard, 3 * kFramesPerTransfer));
    EXPECT (heard.size() == (size_t) 3 * kFramesPerTransfer * 2 && followsTheSaw (heard));
}

KTEST (UsbAudioInput, opening_another_input_stops_the_one_that_was_open)
{
    Rig r;
    auto& second = r.plug (3);
    const auto listed = r.hub.sources();
    EXPECT (listed.size() == 2);

    if (listed.size() != 2)
        return;

    EXPECT (r.hub.open (listed[0]));
    EXPECT (waitFor ([&] { return r.hub.state() == InputState::running; }));
    EXPECT (r.kernel.isOpen (*r.device));

    EXPECT (r.hub.open (listed[1]));
    EXPECT (waitFor ([&] { return r.hub.state() == InputState::running; }));
    EXPECT (! r.kernel.isOpen (*r.device));
    EXPECT (r.kernel.isOpen (second));
    EXPECT (r.hub.current().id == "usb:1234:5678@4-2#1");
}

KTEST (UsbAudioInput, starting_an_input_while_another_is_open_replaces_it_and_sets_up_the_new_ring)
{
    Rig r;
    auto& second = r.plug (3);
    UsbAudioInputOptions options;
    options.os = &r.kernel;
    options.sysfs = r.sys.root;
    const auto backend = makeUsbAudioInput (options);
    const auto listed = backend->list();
    EXPECT (listed.size() == 2);

    if (listed.size() != 2)
        return;

    CaptureRing firstRing, secondRing;
    backend->start (listed[0], firstRing);
    EXPECT (waitFor ([&] { return backend->state() == InputState::running; }));
    EXPECT (firstRing.channels() == 2 && firstRing.capacity() == 96000);          // two seconds at 48 kHz
    EXPECT (r.kernel.isOpen (*r.device));

    backend->start (listed[1], secondRing);                                       // no stop() in between
    EXPECT (backend->state() == InputState::starting);
    EXPECT (waitFor ([&] { return backend->state() == InputState::running; }));
    EXPECT (! r.kernel.isOpen (*r.device) && r.kernel.isOpen (second));
    EXPECT (secondRing.channels() == 2 && secondRing.capacity() == 96000);

    backend->stop();
    EXPECT (backend->state() == InputState::idle);
    EXPECT (! r.kernel.isOpen (second));
    backend->stop();                                                              // and again: nothing is open
    EXPECT (backend->state() == InputState::idle);
}

KTEST (UsbAudioInput, stopping_is_quick_even_when_the_device_is_silent)
{
    Rig r;
    EXPECT (r.open());

    const auto begin = std::chrono::steady_clock::now();
    r.hub.close();
    const auto taken = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - begin).count();

    EXPECT (taken < 1000);
    EXPECT (r.hub.state() == InputState::idle);
    EXPECT (! r.kernel.isOpen (*r.device));
}

// ------------------------------------------------------------------------------------ failures

KTEST (UsbAudioInput, an_input_that_has_gone_between_the_list_and_the_open_is_reported_as_gone)
{
    Rig r;
    const auto listed = r.hub.sources();
    EXPECT (listed.size() == 1);

    if (listed.empty())
        return;

    r.sys.remove ("4-1");
    EXPECT (r.hub.open (listed[0]));
    EXPECT (r.hub.state() == InputState::failed);                    // said at once: no thread was started
    EXPECT (r.hub.status().message == "the input is gone");
    EXPECT (r.kernel.openCount (*r.device) == 0);

    std::vector<float> none (16);
    EXPECT (r.hub.read (none.data(), 8) == 0);
    r.hub.close();
    EXPECT (r.hub.state() == InputState::idle);
}

namespace
{
    // Opens the receiver and waits for the verdict.
    InputStatus openAndWait (Rig& r)
    {
        const auto listed = r.hub.sources();

        if (listed.size() != 1 || ! r.hub.open (listed[0]))
            return {};

        waitFor ([&] { return r.hub.state() != InputState::starting; });
        return r.hub.status();
    }
}

KTEST (UsbAudioInput, a_device_that_cannot_be_opened_says_why_on_the_screen_and_in_the_log)
{
    {
        Rig r;
        r.device->openError = EACCES;
        const InputStatus s = openAndWait (r);
        EXPECT (s.state == InputState::failed);
        EXPECT (s.message == "cannot open the USB device");
        EXPECT (r.log.has ("Permission denied (Kerf needs to run as root for this)"));
        EXPECT (s.channels == 0 && s.frames == 0);

        std::vector<float> none (16);
        EXPECT (r.hub.read (none.data(), 8) == 0);
        r.hub.close();
        EXPECT (r.hub.state() == InputState::idle);
    }

    {
        Rig r;
        r.device->claimErrors[1] = EBUSY;
        const InputStatus s = openAndWait (r);
        EXPECT (s.state == InputState::failed);
        EXPECT (s.message == "another driver or program is using this USB audio device");
        EXPECT (r.log.has ("cannot claim interface 1"));
    }

    {
        Rig r;
        r.device->setInterfaceError = ENOSPC;
        const InputStatus s = openAndWait (r);
        EXPECT (s.state == InputState::failed);
        EXPECT (s.message == "not enough USB bandwidth for the audio");
        EXPECT (r.log.has ("(not enough USB bandwidth)"));
    }

    {
        Rig r;
        r.device->submitError = EMSGSIZE;
        r.device->submitsAllowed = 0;
        const InputStatus s = openAndWait (r);
        EXPECT (s.state == InputState::failed);
        EXPECT (s.message == "the USB audio device would not accept the transfers");
    }
}

KTEST (UsbAudioInput, a_failed_input_can_be_opened_again_once_the_fault_is_gone)
{
    Rig r;
    r.device->claimErrors[1] = EBUSY;
    EXPECT (openAndWait (r).state == InputState::failed);

    r.device->claimErrors.clear();
    EXPECT (r.open());
    EXPECT (r.hub.state() == InputState::running);
    EXPECT (r.hub.status().message == "listening");
}

KTEST (UsbAudioInput, an_input_that_fails_shows_nothing_of_the_stream_that_ran_before_it)
{
    Rig r;
    EXPECT (r.open());
    EXPECT (r.send (4));
    EXPECT (r.framesAtLeast (4 * kFramesPerTransfer));
    EXPECT (r.hub.status().channels == 2 && r.hub.status().sampleRate == 48000);
    r.hub.close();

    r.device->claimErrors[1] = EBUSY;
    const InputStatus s = openAndWait (r);
    EXPECT (s.state == InputState::failed);
    EXPECT (s.channels == 0 && s.sampleRate == 0 && s.frames == 0 && s.dropped == 0 && s.measuredRate == 0.0);
}

// ------------------------------------------------------------------------------------ stress

KTEST (UsbAudioInput, opening_and_closing_over_and_over_while_sound_flows_is_safe)
{
    Rig r;
    IsoPump pump (r.kernel, *r.device, 0x81, [] (long n) { return sawPacket (n); }, 1);

    for (int i = 0; i < 12; ++i)
    {
        EXPECT (r.open());
        EXPECT (r.hub.state() == InputState::running);

        std::vector<float> heard;
        EXPECT (r.collect (heard, 500));
        EXPECT (followsTheSaw (heard));

        if (i % 2 == 0)
            r.hub.close();                                           // the other half is closed by the next open
    }

    // The last one is left open: the hub's destructor stops it, with the pump still sending.
}

#endif // __linux__
