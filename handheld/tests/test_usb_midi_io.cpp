// Tests for the part of the direct USB MIDI reading that talks to the kernel. A pretend kernel
// stands in for usbfs: it keeps the transfers that the program queues, completes them when the
// test says so, makes the file "writable" the way usbfs does, and can fail, stall or unplug.
// What the real kernel does is written down in usb_midi_io.h; the pretend one follows that.

#if defined(__linux__)

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "fake_usb_kernel.h"
#include "midi.h"
#include "midi_input.h"
#include "minitest.h"
#include "usb_fixtures.h"
#include "usb_midi_io.h"

using namespace kerf::ui;
using namespace kerf::ui::usbmidi;
using namespace kerf::ui::usbfake;
using usbfixtures::FakeSysfs;
using usbfixtures::keyboard;

namespace
{
    using std::chrono::milliseconds;

    // ----------------------------------------------------------------------------------------- helpers

    struct Message { int status, d1, d2; };

    Candidate keyboardCandidate (int bus = 4, int device = 2)
    {
        Candidate c;
        c.name = std::to_string (bus) + "-1";
        c.title = "M-Audio Oxygen Pro Mini";
        c.vendor = 0x0763;
        c.product = 0x103b;
        c.bus = bus;
        c.device = device;
        c.major = 189;
        c.minor = (bus - 1) * 128 + (device - 1);
        c.stream.interfaceNumber = 1;
        c.stream.alt = 0;
        Endpoint in;
        in.address = 0x81;
        in.attributes = 2;
        in.maxPacket = 64;
        c.stream.inputs.push_back (in);
        return c;
    }

    // Lets service() collect what the port decodes.
    struct Collected
    {
        std::vector<Message> messages;
        Deliver deliver() { return [this] (const kerf::MidiMessage& m) { messages.push_back ({ m.status, m.data1, m.data2 }); }; }
    };

    bool service (UsbPort& port, Collected& out, Lines& log, short revents = POLLOUT)
    {
        return serviceUsb (port, out.deliver(), log.logger(), revents);
    }

    const Bytes noteOn { 0x09, 0x90, 0x3C, 0x64 };
}

// ---------------------------------------------------------------------------------------- one device

KTEST (UsbIo, opens_the_node_claims_the_interface_and_queues_two_reads)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    const auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    EXPECT (port != nullptr);
    EXPECT (kernel.isClaimed (device));
    EXPECT_EQ (kernel.pendingCount (device), 2);
    EXPECT (kernel.callsOf (device) == (std::vector<std::string> { "open", "claim 1", "submit 81 bulk 64", "submit 81 bulk 64" }));
    EXPECT (kernel.awake == std::vector<std::string> { "4-1" });
}

KTEST (UsbIo, a_finished_read_is_decoded_and_queued_again)
{
    FakeKernel kernel;
    Lines log;
    Collected got;
    auto& device = kernel.add (4, 2);
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    EXPECT (kernel.deliver (device, { 0x09, 0x90, 0x3C, 0x64, 0x08, 0x80, 0x3C, 0x00 }));
    EXPECT_EQ (kernel.pendingCount (device), 1);                  // one finished, one still waiting
    EXPECT (service (*port, got, log));
    EXPECT_EQ (kernel.pendingCount (device), 2);                  // and the finished one is queued again

    EXPECT_EQ ((int) got.messages.size(), 2);
    EXPECT (got.messages[0].status == 0x90 && got.messages[0].d1 == 0x3C && got.messages[0].d2 == 0x64);
    EXPECT (got.messages[1].status == 0x80);
    EXPECT (log.has ("data 09 90 3c 64 08 80 3c 00"));
    EXPECT_EQ (kernel.callCount (device, "reap"), 1);
}

KTEST (UsbIo, nothing_finished_means_nothing_happens)
{
    FakeKernel kernel;
    Lines log;
    Collected got;
    auto& device = kernel.add (4, 2);
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    EXPECT (service (*port, got, log));      // poll() woke up for nothing
    EXPECT (got.messages.empty());
    EXPECT_EQ (kernel.pendingCount (device), 2);
}

KTEST (UsbIo, only_the_first_transfers_are_written_to_the_log)
{
    FakeKernel kernel;
    Lines log;
    Collected got;
    auto& device = kernel.add (4, 2);
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    for (int i = 0; i < 10; ++i)
    {
        EXPECT (kernel.deliver (device, noteOn));
        EXPECT (service (*port, got, log));
    }

    EXPECT_EQ ((int) got.messages.size(), 10);
    EXPECT_EQ (log.count (": data"), kTransfersToLog);
}

KTEST (UsbIo, a_zero_length_transfer_is_queued_again_and_says_nothing)
{
    FakeKernel kernel;
    Lines log;
    Collected got;
    auto& device = kernel.add (4, 2);
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    EXPECT (kernel.deliver (device, {}));
    EXPECT (service (*port, got, log));
    EXPECT (got.messages.empty());
    EXPECT (! log.has (": data"));
    EXPECT_EQ (kernel.pendingCount (device), 2);
}

KTEST (UsbIo, a_later_alternate_setting_is_selected_before_the_reads_are_queued)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    auto c = keyboardCandidate();
    c.stream.alt = 1;
    const auto port = openUsbPort (c, kernel, log.logger());

    EXPECT (port != nullptr);
    EXPECT (kernel.callsOf (device) == (std::vector<std::string> { "open", "claim 1", "alt 1 1", "submit 81 bulk 64", "submit 81 bulk 64" }));
}

KTEST (UsbIo, interrupt_endpoints_and_big_packets_get_the_right_type_and_size)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2, { { 0x82, USBDEVFS_URB_TYPE_INTERRUPT }, { 0x84, USBDEVFS_URB_TYPE_BULK } });
    auto c = keyboardCandidate();
    Endpoint slow;
    slow.address = 0x82;
    slow.attributes = 3;
    slow.maxPacket = 8;
    Endpoint fast;
    fast.address = 0x84;
    fast.attributes = 2;
    fast.maxPacket = 512;
    c.stream.inputs = { slow, fast };

    const auto port = openUsbPort (c, kernel, log.logger());
    EXPECT (port != nullptr);
    EXPECT (kernel.callsOf (device) == (std::vector<std::string> { "open", "claim 1", "submit 82 interrupt 64", "submit 82 interrupt 64",
                                                                   "submit 84 bulk 512", "submit 84 bulk 512" }));
    EXPECT_EQ (kernel.pendingCount (device), 4);
}

KTEST (UsbIo, an_interface_that_is_in_use_is_reported_and_not_released)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    device.claimError = EBUSY;

    EXPECT (openUsbPort (keyboardCandidate(), kernel, log.logger()) == nullptr);
    EXPECT (log.has ("cannot claim interface 1"));
    EXPECT (log.has ("another program or driver is using it"));
    EXPECT (kernel.callsOf (device) == (std::vector<std::string> { "open", "claim 1", "close" }));
}

KTEST (UsbIo, a_missing_node_is_made_from_the_numbers_in_sysfs)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    device.nodeExists = false;

    const auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());
    EXPECT (port != nullptr);
    EXPECT (kernel.madeNodes == std::vector<std::string> { "/dev/bus/usb/004/002 189:385" });
    EXPECT (log.has ("made the missing device node /dev/bus/usb/004/002"));
}

KTEST (UsbIo, a_missing_node_that_cannot_be_made_is_reported)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    device.nodeExists = false;
    device.makeNodeFails = true;

    EXPECT (openUsbPort (keyboardCandidate(), kernel, log.logger()) == nullptr);
    EXPECT (log.has ("there is no device node /dev/bus/usb/004/002 and it cannot be made"));
}

KTEST (UsbIo, a_node_that_cannot_be_opened_is_reported_with_a_hint_about_root)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    device.openError = EACCES;

    EXPECT (openUsbPort (keyboardCandidate(), kernel, log.logger()) == nullptr);
    EXPECT (log.has ("cannot open /dev/bus/usb/004/002: Permission denied"));
    EXPECT (log.has ("needs to run as root"));
}

KTEST (UsbIo, a_read_that_cannot_be_queued_gives_up_and_lets_go_of_the_interface)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    device.submitsAllowed = 1;
    device.submitError = ENOMEM;

    EXPECT (openUsbPort (keyboardCandidate(), kernel, log.logger()) == nullptr);
    EXPECT (log.has ("cannot queue a read on endpoint 0x81"));
    const auto calls = kernel.callsOf (device);
    EXPECT (calls.size() >= 2 && calls[calls.size() - 2] == "release 1" && calls.back() == "close");
    EXPECT (! kernel.isOpen (device));
}

KTEST (UsbIo, a_setting_that_cannot_be_selected_is_reported)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    device.setInterfaceError = EINVAL;
    auto c = keyboardCandidate();
    c.stream.alt = 2;

    EXPECT (openUsbPort (c, kernel, log.logger()) == nullptr);
    EXPECT (log.has ("cannot select setting 2"));
    EXPECT (! kernel.isOpen (device));
}

KTEST (UsbIo, a_stalled_endpoint_is_cleared_and_read_again)
{
    FakeKernel kernel;
    Lines log;
    Collected got;
    auto& device = kernel.add (4, 2);
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    EXPECT (kernel.fail (device, EPIPE));
    EXPECT (service (*port, got, log));
    EXPECT_EQ (kernel.callCount (device, "halt 81"), 1);
    EXPECT_EQ (kernel.pendingCount (device), 2);

    EXPECT (kernel.deliver (device, noteOn));       // and it works afterwards
    EXPECT (service (*port, got, log));
    EXPECT_EQ ((int) got.messages.size(), 1);
}

KTEST (UsbIo, a_cancelled_transfer_is_queued_again)
{
    FakeKernel kernel;
    Lines log;
    Collected got;
    auto& device = kernel.add (4, 2);
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    EXPECT (kernel.fail (device, ECONNRESET));
    EXPECT (service (*port, got, log));
    EXPECT_EQ (kernel.pendingCount (device), 2);
    EXPECT_EQ (kernel.callCount (device, "halt"), 0);
}

KTEST (UsbIo, errors_in_a_row_end_it_but_a_good_transfer_starts_the_count_again)
{
    FakeKernel kernel;
    Lines log;
    Collected got;
    auto& device = kernel.add (4, 2);
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    for (int i = 0; i < kMaxErrorsInARow - 1; ++i)
    {
        EXPECT (kernel.fail (device, EPROTO));
        EXPECT (service (*port, got, log));
    }

    EXPECT (kernel.deliver (device, noteOn));
    EXPECT (service (*port, got, log));
    EXPECT_EQ (port->errors, 0);

    for (int i = 0; i < kMaxErrorsInARow; ++i)      // the limit itself is still alive
    {
        EXPECT (kernel.fail (device, EPROTO));
        EXPECT (service (*port, got, log));
    }

    EXPECT (kernel.fail (device, EPROTO));          // one more
    EXPECT (! service (*port, got, log));
    EXPECT (port->why.find ("kept failing") != std::string::npos);
}

KTEST (UsbIo, unplugging_ends_it_however_it_is_noticed)
{
    {
        // poll() says the device has gone
        FakeKernel kernel;
        Lines log;
        Collected got;
        kernel.add (4, 2);
        auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());
        EXPECT (! service (*port, got, log, POLLHUP));
        EXPECT (port->why == "unplugged");
    }
    {
        // fetching a finished transfer says so (ENODEV)
        FakeKernel kernel;
        Lines log;
        Collected got;
        auto& device = kernel.add (4, 2);
        auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());
        kernel.unplug (device);
        EXPECT (! service (*port, got, log));
        EXPECT (port->why == "unplugged");
    }
    {
        // a transfer finishes with ESHUTDOWN
        FakeKernel kernel;
        Lines log;
        Collected got;
        auto& device = kernel.add (4, 2);
        auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());
        EXPECT (kernel.fail (device, ESHUTDOWN));
        EXPECT (! service (*port, got, log));
        EXPECT (port->why == "unplugged");
    }
    {
        // queueing it again says so (ENODEV)
        FakeKernel kernel;
        Lines log;
        Collected got;
        auto& device = kernel.add (4, 2);
        auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());
        EXPECT (kernel.deliver (device, noteOn));
        device.submitsAllowed = 0;
        device.submitError = ENODEV;
        EXPECT (! service (*port, got, log));
        EXPECT (port->why == "unplugged");
        EXPECT_EQ ((int) got.messages.size(), 1);       // what arrived before the device went is not lost
    }
}

KTEST (UsbIo, a_device_that_never_stops_answering_cannot_keep_the_thread)
{
    FakeKernel kernel;
    Lines log;
    Collected got;
    auto& device = kernel.add (4, 2);
    device.completeAtOnce = true;
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());

    EXPECT (port != nullptr);
    EXPECT (service (*port, got, log));
    EXPECT_EQ (kernel.callCount (device, "reap"), 64);
}

KTEST (UsbIo, closing_it_releases_the_interface_and_the_file_once)
{
    FakeKernel kernel;
    Lines log;
    auto& device = kernel.add (4, 2);
    auto port = openUsbPort (keyboardCandidate(), kernel, log.logger());
    EXPECT (kernel.isOpen (device));

    port.reset();
    EXPECT (! kernel.isOpen (device));
    EXPECT (! kernel.isClaimed (device));
    const auto calls = kernel.callsOf (device);
    EXPECT (calls.size() >= 2 && calls[calls.size() - 2] == "release 1" && calls.back() == "close");
    EXPECT_EQ (kernel.callCount (device, "release"), 1);
    EXPECT_EQ (kernel.callCount (device, "close"), 1);
}

// ----------------------------------------------------------------------------- the whole reader

namespace
{
    // A MidiInput that looks at a pretend sysfs and a pretend kernel, with a short interval.
    struct Rig
    {
        kerf::MidiInbox inbox;
        MidiInput midi { inbox };
        FakeKernel kernel;
        FakeSysfs sys;
        Lines log;

        Rig()
        {
            ::setenv ("KERF_USB_SYSFS", sys.root.string().c_str(), 1);
            midi.setUsbOs (&kernel);
            midi.setLogger (log.logger());
            midi.setScanInterval (milliseconds (20));
        }

        ~Rig()
        {
            midi.stop();
            ::unsetenv ("KERF_USB_SYSFS");
        }

        // The kernel's sysfs entry for a keyboard on bus 4, as device number `number`.
        void plugIn (int number, const std::string& name = "4-1") const
        {
            sys.device (name, keyboard(), 1, 4, number, "M-Audio", "Oxygen Pro Mini", "0763", "103b");
        }

        // Sends `bytes` as soon as a transfer is waiting for them.
        bool send (FakeDevice& device, const Bytes& bytes)
        {
            return waitFor ([&] { return kernel.deliver (device, bytes); });
        }

        std::vector<Message> drain()
        {
            std::vector<kerf::MidiMessage> pending;
            std::vector<Message> out;
            inbox.tryDrain (pending);

            for (const auto& m : pending)
                out.push_back ({ m.status, m.data1, m.data2 });

            return out;
        }
    };
}

KTEST (UsbReader, finds_a_keyboard_plays_it_and_notices_when_it_is_unplugged)
{
    Rig rig;
    auto& device = rig.kernel.add (4, 2);
    rig.plugIn (2);
    rig.midi.start();

    EXPECT (waitFor ([&] { return rig.midi.usbDirectCount() == 1; }));
    EXPECT (rig.midi.deviceCount() >= 1);
    EXPECT (rig.log.has ("usb 4-1 (0763:103b M-Audio Oxygen Pro Mini): interfaces (class.subclass) 1.1 1.3"));
    EXPECT (rig.log.has ("MIDI interface 1, input endpoint 0x81 (64 bytes), no kernel driver: reading it directly"));
    EXPECT (rig.log.has ("ready"));
    EXPECT_EQ (rig.kernel.openCount (device), 1);

    EXPECT (rig.send (device, { 0x09, 0x90, 0x3C, 0x64 }));
    EXPECT (rig.send (device, { 0x08, 0x80, 0x3C, 0x00 }));
    EXPECT (waitFor ([&] { return rig.midi.messageCount() == 2; }));
    EXPECT_EQ ((int) rig.midi.lastMessage(), 0x803C00);

    const auto got = rig.drain();
    EXPECT_EQ ((int) got.size(), 2);
    EXPECT (got.size() == 2 && got[0].status == 0x90 && got[0].d1 == 0x3C && got[0].d2 == 0x64);
    EXPECT (got.size() == 2 && got[1].status == 0x80);

    // Many more looks at the USB bus later, the open keyboard has not been opened a second time.
    std::this_thread::sleep_for (milliseconds (150));
    EXPECT_EQ (rig.kernel.openCount (device), 1);
    EXPECT_EQ (rig.midi.usbDirectCount(), 1);

    // unplugged: the kernel drops the device from sysfs and from usbfs
    rig.kernel.unplug (device);
    rig.sys.remove ("4-1");
    EXPECT (waitFor ([&] { return rig.midi.usbDirectCount() == 0; }));
    EXPECT (rig.log.has ("unplugged"));

    // plugged in again: a new device number, the same port
    auto& again = rig.kernel.add (4, 3);
    rig.plugIn (3);
    EXPECT (waitFor ([&] { return rig.midi.usbDirectCount() == 1; }));
    EXPECT (rig.send (again, { 0x09, 0x91, 0x40, 0x7F }));
    EXPECT (waitFor ([&] { return rig.midi.messageCount() == 3; }));
    EXPECT_EQ ((int) rig.midi.lastMessage(), 0x91407F);

    rig.midi.stop();
    EXPECT (! rig.kernel.isOpen (again));
    EXPECT_EQ (rig.midi.usbDirectCount(), 0);
}

KTEST (UsbReader, leaves_a_device_that_a_kernel_driver_has_taken)
{
    Rig rig;
    auto& device = rig.kernel.add (4, 2);
    rig.plugIn (2);

    if (! rig.sys.bind ("4-1:1.1", "snd-usb-audio"))
        return; // this file system cannot make symbolic links

    rig.midi.start();
    EXPECT (waitFor ([&] { return rig.log.has ("the kernel driver 'snd-usb-audio' has the MIDI interface"); }));
    std::this_thread::sleep_for (milliseconds (200));
    EXPECT_EQ (rig.kernel.openCount (device), 0);
    EXPECT_EQ (rig.midi.usbDirectCount(), 0);
}

KTEST (UsbReader, a_new_device_waits_one_look_so_that_a_kernel_driver_can_bind_first)
{
    Rig rig;
    auto& device = rig.kernel.add (4, 2);
    rig.plugIn (2);
    rig.midi.setScanInterval (milliseconds (300));
    rig.midi.start();

    // The first look has found the device; before the second one the kernel binds its driver.
    EXPECT (waitFor ([&] { return rig.log.has ("usb 4-1 (0763:103b M-Audio Oxygen Pro Mini): interfaces"); }));

    if (! rig.sys.bind ("4-1:1.1", "snd-usb-audio"))
        return; // this file system cannot make symbolic links

    EXPECT (waitFor ([&] { return rig.log.has ("the kernel driver 'snd-usb-audio' has the MIDI interface"); }));
    EXPECT_EQ (rig.kernel.openCount (device), 0);
}

KTEST (UsbReader, the_setting_can_turn_the_direct_reading_off)
{
    Rig rig;
    auto& device = rig.kernel.add (4, 2);
    rig.plugIn (2);
    rig.midi.setUsbDirect (false);
    rig.midi.start();

    EXPECT (waitFor ([&] { return rig.log.has ("off (usb_midi = 0)"); }));
    std::this_thread::sleep_for (milliseconds (200));
    EXPECT_EQ (rig.kernel.openCount (device), 0);
    EXPECT_EQ (rig.midi.usbDirectCount(), 0);
}

KTEST (UsbReader, a_device_that_cannot_be_opened_is_tried_four_times_and_then_left_alone)
{
    Rig rig;
    auto& device = rig.kernel.add (4, 2);
    device.openError = EACCES;
    rig.plugIn (2);
    rig.midi.start();

    EXPECT (waitFor ([&] { return rig.kernel.openCount (device) == 4; }));
    std::this_thread::sleep_for (milliseconds (300));
    EXPECT_EQ (rig.kernel.openCount (device), 4);
    EXPECT_EQ (rig.log.count ("cannot open /dev/bus/usb/004/002"), 4);
    EXPECT_EQ (rig.midi.usbDirectCount(), 0);

    // Plugged in again (another device number), it gets its four tries again.
    rig.kernel.unplug (device);
    rig.sys.remove ("4-1");
    auto& again = rig.kernel.add (4, 3);
    rig.plugIn (3);
    EXPECT (waitFor ([&] { return rig.midi.usbDirectCount() == 1; }));
    EXPECT (rig.log.has ("usb 4-1 (0763:103b M-Audio Oxygen Pro Mini): ready"));
    EXPECT_EQ (rig.kernel.openCount (again), 1);
}

KTEST (UsbReader, a_device_that_stopped_with_errors_is_opened_again)
{
    Rig rig;
    auto& device = rig.kernel.add (4, 2);
    rig.plugIn (2);
    rig.midi.start();
    EXPECT (waitFor ([&] { return rig.midi.usbDirectCount() == 1; }));

    // The transfers fail until the reader gives up on the device ...
    for (int i = 0; i < kMaxErrorsInARow + 1; ++i)
        EXPECT (waitFor ([&] { return rig.kernel.fail (device, EPROTO); }));

    EXPECT (waitFor ([&] { return rig.log.has ("stopped: the transfers kept failing"); }));

    // ... and, the device still being there, it is opened again and works.
    EXPECT (waitFor ([&] { return rig.kernel.openCount (device) == 2 && rig.midi.usbDirectCount() == 1; }));
    EXPECT (rig.log.has ("trying again (2 of 4)"));
    EXPECT (rig.send (device, { 0x09, 0x90, 0x30, 0x40 }));
    EXPECT (waitFor ([&] { return rig.midi.messageCount() == 1; }));
}

KTEST (UsbReader, two_midi_devices_are_read_side_by_side)
{
    Rig rig;
    auto& first = rig.kernel.add (4, 2);
    auto& second = rig.kernel.add (3, 5);
    rig.plugIn (2);
    rig.sys.device ("3-1.2", keyboard(), 1, 3, 5, "Arturia", "KeyStep", "1c75", "0287");
    rig.midi.start();

    EXPECT (waitFor ([&] { return rig.midi.usbDirectCount() == 2; }));
    EXPECT (rig.send (first, { 0x09, 0x90, 0x3C, 0x64 }));
    EXPECT (rig.send (second, { 0x09, 0x92, 0x40, 0x50 }));
    EXPECT (waitFor ([&] { return rig.midi.messageCount() == 2; }));

    // one goes away, the other goes on
    rig.kernel.unplug (first);
    rig.sys.remove ("4-1");
    EXPECT (waitFor ([&] { return rig.midi.usbDirectCount() == 1; }));
    EXPECT (rig.send (second, { 0x09, 0x92, 0x41, 0x50 }));
    EXPECT (waitFor ([&] { return rig.midi.messageCount() == 3; }));
}

#endif // __linux__
