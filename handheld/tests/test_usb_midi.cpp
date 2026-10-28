// Tests for the USB MIDI reading that needs no operating system calls: descriptor parsing, the
// 4-byte USB-MIDI packets, and the search for devices in a (fake) /sys/bus/usb/devices.
// What happens after that - claiming the interface, queueing transfers - can only be tried on a
// machine that has a USB MIDI device; it is in midi_input.cpp.

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <random>
#include <string>
#include <vector>

#include "minitest.h"
#include "usb_fixtures.h"
#include "usb_midi.h"

using namespace kerf::ui::usbmidi;
using namespace usbfixtures;

namespace
{
    std::vector<Configuration> parse (const Bytes& b) { return parseDescriptors (b.data(), b.size()); }

    struct Message { int status, d1, d2; };

    std::vector<Message> decode (PacketDecoder& d, const Bytes& packets)
    {
        std::vector<Message> out;
        d.feed (packets.data(), packets.size(), [&] (const kerf::MidiMessage& m) { out.push_back ({ m.status, m.data1, m.data2 }); });
        return out;
    }

    std::vector<Message> decode (const Bytes& packets)
    {
        PacketDecoder d;
        return decode (d, packets);
    }
}

// ---------------------------------------------------------------------------------- descriptors

KTEST (UsbDescriptors, a_class_compliant_keyboard_has_one_midi_stream_with_one_input)
{
    const auto configs = parse (keyboard());
    EXPECT_EQ ((int) configs.size(), 1);
    EXPECT_EQ ((int) configs[0].value, 1);
    EXPECT_EQ ((int) configs[0].interfaces.size(), 2);
    EXPECT_EQ ((int) configs[0].interfaces[1].endpoints.size(), 2);

    const auto streams = findMidiStreams (configs[0]);
    EXPECT_EQ ((int) streams.size(), 1);
    EXPECT_EQ (streams[0].interfaceNumber, 1);
    EXPECT_EQ (streams[0].alt, 0);
    EXPECT_EQ ((int) streams[0].inputs.size(), 1);
    EXPECT_EQ ((int) streams[0].inputs[0].address, 0x81);
    EXPECT_EQ ((int) streams[0].inputs[0].maxPacket, 64);
    EXPECT (streams[0].inputs[0].isIn());
}

KTEST (UsbDescriptors, the_endpoint_list_skips_the_output_endpoint)
{
    const auto streams = findMidiStreams (parse (keyboard())[0]);
    EXPECT_EQ ((int) streams[0].inputs.size(), 1);

    for (const auto& e : streams[0].inputs)
        EXPECT (e.isIn());
}

KTEST (UsbDescriptors, interrupt_inputs_count_but_isochronous_and_output_do_not)
{
    Bytes interrupt = deviceDescriptor (1, 2);
    append (interrupt, configurationDescriptor (1, keyboardBody (1, 0x82, 3)));
    EXPECT_EQ ((int) findMidiStreams (parse (interrupt)[0]).size(), 1);
    EXPECT_EQ ((int) findMidiStreams (parse (interrupt)[0])[0].inputs[0].transferType(), 3);

    Bytes isochronous = deviceDescriptor (1, 2);
    append (isochronous, configurationDescriptor (1, keyboardBody (1, 0x81, 1)));
    EXPECT (findMidiStreams (parse (isochronous)[0]).empty());

    Bytes outputOnly;
    append (outputOnly, interfaceDescriptor (1, 0, 1, 1, 3));
    append (outputOnly, endpointDescriptor (0x01, 2, 64));
    Bytes device = deviceDescriptor (1, 2);
    append (device, configurationDescriptor (1, outputOnly, 1));
    EXPECT (findMidiStreams (parse (device)[0]).empty());
}

KTEST (UsbDescriptors, other_classes_are_not_midi)
{
    Bytes body;
    append (body, interfaceDescriptor (0, 0, 1, 3, 1));        // a keyboard or mouse (HID)
    append (body, endpointDescriptor (0x81, 3, 8, false));
    append (body, interfaceDescriptor (1, 0, 2, 255, 0));      // vendor specific
    append (body, endpointDescriptor (0x82, 2, 64, false));
    append (body, interfaceDescriptor (2, 0, 2, 8, 6));        // a memory stick
    append (body, endpointDescriptor (0x83, 2, 64, false));

    Bytes device = deviceDescriptor (1, 2);
    append (device, configurationDescriptor (1, body, 3));
    EXPECT (findMidiStreams (parse (device)[0]).empty());
}

KTEST (UsbDescriptors, the_lowest_alternate_setting_with_an_input_is_used)
{
    // Setting 0 is MIDI 1.0, setting 1 the same device as MIDI 2.0: use 0.
    Bytes both;
    append (both, interfaceDescriptor (1, 0, 2, 1, 3));
    append (both, endpointDescriptor (0x01, 2, 64));
    append (both, endpointDescriptor (0x81, 2, 64));
    append (both, interfaceDescriptor (1, 1, 2, 1, 3));
    append (both, endpointDescriptor (0x02, 2, 512));
    append (both, endpointDescriptor (0x82, 2, 512));
    Bytes device = deviceDescriptor (1, 2);
    append (device, configurationDescriptor (1, both, 1));

    auto streams = findMidiStreams (parse (device)[0]);
    EXPECT_EQ ((int) streams.size(), 1);
    EXPECT_EQ (streams[0].alt, 0);
    EXPECT_EQ ((int) streams[0].inputs[0].address, 0x81);

    // Setting 0 without endpoints (like the audio streaming interfaces do): the next one is used.
    Bytes late;
    append (late, interfaceDescriptor (1, 0, 0, 1, 3));
    append (late, interfaceDescriptor (1, 1, 2, 1, 3));
    append (late, endpointDescriptor (0x01, 2, 64));
    append (late, endpointDescriptor (0x81, 2, 64));
    Bytes device2 = deviceDescriptor (1, 2);
    append (device2, configurationDescriptor (1, late, 1));

    streams = findMidiStreams (parse (device2)[0]);
    EXPECT_EQ ((int) streams.size(), 1);
    EXPECT_EQ (streams[0].alt, 1);
}

KTEST (UsbDescriptors, two_midi_interfaces_give_two_streams)
{
    Bytes body;
    append (body, interfaceDescriptor (1, 0, 2, 1, 3));
    append (body, endpointDescriptor (0x01, 2, 64));
    append (body, endpointDescriptor (0x81, 2, 64));
    append (body, interfaceDescriptor (2, 0, 2, 1, 3));
    append (body, endpointDescriptor (0x02, 2, 64));
    append (body, endpointDescriptor (0x82, 2, 64));

    Bytes device = deviceDescriptor (1, 2);
    append (device, configurationDescriptor (1, body, 2));

    const auto streams = findMidiStreams (parse (device)[0]);
    EXPECT_EQ ((int) streams.size(), 2);
    EXPECT_EQ (streams[0].interfaceNumber, 1);
    EXPECT_EQ (streams[1].interfaceNumber, 2);
    EXPECT_EQ ((int) streams[1].inputs[0].address, 0x82);
}

KTEST (UsbDescriptors, a_configuration_is_found_by_its_value)
{
    Bytes device = deviceDescriptor (1, 2, 2);
    Bytes plain;
    append (plain, interfaceDescriptor (0, 0, 1, 3, 0));
    append (plain, endpointDescriptor (0x81, 3, 8, false));
    append (device, configurationDescriptor (1, plain, 1));
    append (device, configurationDescriptor (2, keyboardBody()));

    const auto configs = parse (device);
    EXPECT_EQ ((int) configs.size(), 2);
    EXPECT (findMidiStreams (*findConfiguration (configs, 1)).empty());
    EXPECT_EQ ((int) findMidiStreams (*findConfiguration (configs, 2)).size(), 1);
    EXPECT_EQ ((int) findConfiguration (configs, 9)->value, 1);     // unknown value: the first one
    EXPECT (findConfiguration ({}, 1) == nullptr);
}

KTEST (UsbDescriptors, truncated_empty_and_random_data_never_loops_or_crashes)
{
    EXPECT (parseDescriptors (nullptr, 0).empty());

    const Bytes good = keyboard();

    for (size_t cut = 0; cut <= good.size(); ++cut)
    {
        const auto configs = parseDescriptors (good.data(), cut);

        for (const auto& c : configs)
            findMidiStreams (c);
    }

    // A descriptor that says it is 0 or 1 byte long would never advance.
    Bytes zero = deviceDescriptor (1, 2);
    add (zero, { 0, 2, 0, 0 });
    EXPECT (parse (zero).empty());

    // A descriptor longer than what is left is dropped.
    Bytes tooLong = deviceDescriptor (1, 2);
    add (tooLong, { 9, 2, 40, 0, 1, 1, 0, 0x80 });
    EXPECT (parse (tooLong).empty());

    std::mt19937 random (7);

    for (int round = 0; round < 500; ++round)
    {
        Bytes noise ((size_t) (random() % 300));

        for (auto& b : noise)
            b = (uint8_t) random();

        for (const auto& c : parse (noise))
            findMidiStreams (c);
    }

    EXPECT (true);
}

KTEST (UsbDescriptors, endpoint_fields_are_read_little_endian)
{
    Bytes body;
    append (body, interfaceDescriptor (1, 0, 1, 1, 3));
    append (body, endpointDescriptor (0x81, 2, 0x0200));
    Bytes device = deviceDescriptor (1, 2);
    append (device, configurationDescriptor (1, body, 1));

    const auto streams = findMidiStreams (parse (device)[0]);
    EXPECT_EQ ((int) streams[0].inputs[0].maxPacket, 512);
}

// ---------------------------------------------------------------------------------- packets

KTEST (UsbMidiPackets, the_bytes_per_code_index_follow_the_usb_midi_table)
{
    const int expected[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };

    for (int cin = 0; cin < 16; ++cin)
        EXPECT_EQ (packetBytes (cin), expected[cin]);

    EXPECT_EQ (packetBytes (0x19), 3);    // only the low nibble counts
}

KTEST (UsbMidiPackets, notes_controllers_and_bends_come_out_as_midi_messages)
{
    const auto out = decode ({ 0x09, 0x90, 0x3C, 0x64,      // note on, C4, velocity 100
                               0x08, 0x80, 0x3C, 0x00,      // note off
                               0x0B, 0xB1, 0x07, 0x50,      // controller 7 on channel 2
                               0x0E, 0xE0, 0x00, 0x40 });   // pitch bend, centre

    EXPECT_EQ ((int) out.size(), 4);
    EXPECT (out[0].status == 0x90 && out[0].d1 == 0x3C && out[0].d2 == 0x64);
    EXPECT (out[1].status == 0x80 && out[1].d1 == 0x3C && out[1].d2 == 0x00);
    EXPECT (out[2].status == 0xB1 && out[2].d1 == 0x07 && out[2].d2 == 0x50);
    EXPECT (out[3].status == 0xE0 && out[3].d1 == 0x00 && out[3].d2 == 0x40);
}

KTEST (UsbMidiPackets, program_change_and_channel_pressure_have_one_data_byte)
{
    const auto out = decode ({ 0x0C, 0xC3, 0x05, 0x00, 0x0D, 0xD0, 0x44, 0x00 });
    EXPECT_EQ ((int) out.size(), 2);
    EXPECT (out[0].status == 0xC3 && out[0].d1 == 5 && out[0].d2 == 0);
    EXPECT (out[1].status == 0xD0 && out[1].d1 == 0x44);
}

KTEST (UsbMidiPackets, the_cable_is_not_part_of_the_message_and_cables_do_not_mix)
{
    const auto out = decode ({ 0x19, 0x90, 0x40, 0x7F,      // cable 1
                               0x09, 0x91, 0x41, 0x10 });   // cable 0
    EXPECT_EQ ((int) out.size(), 2);
    EXPECT (out[0].status == 0x90 && out[0].d1 == 0x40);
    EXPECT (out[1].status == 0x91 && out[1].d1 == 0x41);
}

KTEST (UsbMidiPackets, sysex_is_skipped_and_does_not_disturb_another_cable)
{
    PacketDecoder d;
    // SysEx F0 7E 7F 06 01 F7 on cable 0, in two packets, with a note on cable 1 in between.
    auto out = decode (d, { 0x04, 0xF0, 0x7E, 0x7F,
                            0x19, 0x90, 0x3C, 0x64,
                            0x07, 0x06, 0x01, 0xF7 });
    EXPECT_EQ ((int) out.size(), 1);
    EXPECT (out[0].status == 0x90 && out[0].d1 == 0x3C);

    // The sysex is over: a note on cable 0 is heard again.
    out = decode (d, { 0x09, 0x92, 0x30, 0x20 });
    EXPECT_EQ ((int) out.size(), 1);
    EXPECT (out[0].status == 0x92);

    // A sysex that ends with one or two bytes (code indexes 5 and 6).
    out = decode (d, { 0x04, 0xF0, 0x01, 0x02, 0x05, 0xF7, 0x00, 0x00, 0x09, 0x90, 0x3E, 0x40 });
    EXPECT_EQ ((int) out.size(), 1);
    EXPECT_EQ (out[0].d1, 0x3E);
}

KTEST (UsbMidiPackets, clock_reserved_codes_and_a_cut_off_packet_are_ignored)
{
    PacketDecoder d;
    auto out = decode (d, { 0x0F, 0xF8, 0x00, 0x00,         // timing clock
                            0x00, 0x90, 0x3C, 0x64,         // reserved code index 0
                            0x01, 0x90, 0x3C, 0x64,         // reserved code index 1
                            0x09, 0x90, 0x3C });            // cut off: only 3 of 4 bytes
    EXPECT (out.empty());

    out = decode (d, { 0x09, 0x90, 0x3C, 0x64 });
    EXPECT_EQ ((int) out.size(), 1);
}

KTEST (UsbMidiPackets, a_note_on_with_velocity_zero_is_passed_on_for_the_engine_to_treat_as_off)
{
    const auto out = decode ({ 0x09, 0x90, 0x3C, 0x00 });
    EXPECT_EQ ((int) out.size(), 1);
    EXPECT (out[0].status == 0x90 && out[0].d2 == 0);
}

KTEST (UsbMidiPackets, a_big_transfer_with_many_packets_is_decoded_completely)
{
    Bytes packets;

    for (int i = 0; i < 16; ++i)
        add (packets, { 0x09, 0x90, 36 + i, 100 });

    const auto out = decode (packets);
    EXPECT_EQ ((int) out.size(), 16);
    EXPECT_EQ (out[15].d1, 51);
}

// ---------------------------------------------------------------------------------- sysfs search

// /sys/bus/usb/devices exists on Linux only, and its interface folders have a ':' in their names.
#if defined(__linux__)

KTEST (UsbSysfs, finds_a_keyboard_that_has_no_driver_and_ignores_the_rest)
{
    FakeSysfs sys;
    sys.device ("4-1", keyboard(), 1, 4, 2, "M-Audio", "Oxygen Pro Mini", "0763", "0123");
    fs::create_directories (sys.root / "4-1:1.0");
    fs::create_directories (sys.root / "4-1:1.1");
    sys.device ("4-2", mouse(), 1, 4, 3, "Logitech", "USB Optical Mouse", "046d", "c077");
    sys.device ("usb4", keyboard(), 1, 4, 1, "Linux", "OHCI Host Controller", "1d6b", "0001");   // a root hub: skipped
    fs::create_directories (sys.root / "4-2:1.0");

    const auto found = look (sys.root);
    EXPECT_EQ ((int) found.candidates.size(), 1);
    EXPECT_EQ ((int) found.devices.size(), 2);

    const auto& c = found.candidates[0];
    EXPECT (c.name == "4-1");
    EXPECT (c.title == "M-Audio Oxygen Pro Mini");
    EXPECT_EQ (c.vendor, 0x0763);
    EXPECT_EQ (c.product, 0x0123);
    EXPECT_EQ (c.bus, 4);
    EXPECT_EQ (c.device, 2);
    EXPECT_EQ (c.major, 189);
    EXPECT_EQ (c.minor, 3 * 128 + 1);
    EXPECT_EQ (c.stream.interfaceNumber, 1);
    EXPECT (c.driver.empty());
    EXPECT (c.key() == "4:2:1");

    // The device list says what each one has, MIDI or not.
    EXPECT (found.devices[0].name == "4-1" && found.devices[0].midi);
    EXPECT (found.devices[0].interfaces == "1.1 1.3");
    EXPECT (found.devices[1].name == "4-2" && ! found.devices[1].midi);
    EXPECT (found.devices[1].interfaces == "3.1");
    EXPECT (found.devices[1].title == "Logitech USB Optical Mouse");
}

KTEST (UsbSysfs, a_bound_kernel_driver_is_reported_so_that_kerf_leaves_the_device_to_it)
{
    FakeSysfs sys;
    sys.device ("4-1", keyboard(), 1, 4, 2, "M-Audio", "Oxygen Pro Mini", "0763", "0123");

    if (! sys.bind ("4-1:1.1", "snd-usb-audio"))
        return; // this file system cannot make symbolic links

    const auto found = scan (sys.root);
    EXPECT_EQ ((int) found.size(), 1);
    EXPECT (found[0].driver == "snd-usb-audio");

    // Only the MIDI interface counts: a driver on the audio control interface does not.
    FakeSysfs other;
    other.device ("4-1", keyboard(), 1, 4, 2, "M-Audio", "Oxygen Pro Mini", "0763", "0123");
    other.bind ("4-1:1.0", "snd-usb-audio");
    EXPECT (scan (other.root)[0].driver.empty());
}

KTEST (UsbSysfs, our_own_claim_shows_up_as_the_usbfs_driver)
{
    FakeSysfs sys;
    sys.device ("4-1", keyboard(), 1, 4, 2, "M-Audio", "Oxygen Pro Mini", "0763", "0123");

    if (! sys.bind ("4-1:1.1", "usbfs"))
        return;

    EXPECT (scan (sys.root)[0].driver == "usbfs");
}

KTEST (UsbSysfs, the_active_configuration_decides)
{
    Bytes device = deviceDescriptor (1, 2, 2);
    Bytes plain;
    append (plain, interfaceDescriptor (0, 0, 1, 3, 0));
    append (plain, endpointDescriptor (0x81, 3, 8, false));
    append (device, configurationDescriptor (1, plain, 1));
    append (device, configurationDescriptor (2, keyboardBody()));

    FakeSysfs sys;
    sys.device ("3-1.4", device, 2, 3, 7, "Arturia", "Arturia KeyStep", "1c75", "0287");
    auto found = scan (sys.root);
    EXPECT_EQ ((int) found.size(), 1);
    EXPECT (found[0].title == "Arturia KeyStep");   // the maker's name is not repeated
    EXPECT (found[0].name == "3-1.4");

    FakeSysfs first;
    first.device ("3-1.4", device, 1, 3, 7, "Arturia", "Arturia KeyStep", "1c75", "0287");
    EXPECT (scan (first.root).empty());
}

KTEST (UsbSysfs, names_are_optional_and_a_missing_folder_is_not_an_error)
{
    FakeSysfs sys;
    sys.device ("1-1", keyboard(), 1, 1, 5, "", "", "ffff", "0001");
    const auto found = scan (sys.root);
    EXPECT_EQ ((int) found.size(), 1);
    EXPECT (found[0].title == "USB MIDI device");

    EXPECT (scan (sys.root / "nothing-here").empty());
    EXPECT (look (sys.root / "nothing-here").devices.empty());
}

KTEST (UsbSysfs, devices_without_numbers_are_not_offered_and_results_are_sorted)
{
    FakeSysfs sys;
    sys.device ("2-1", keyboard(), 1, 2, 9, "B", "Second", "0001", "0001");
    sys.device ("1-1", keyboard(), 1, 1, 4, "A", "First", "0002", "0002");
    sys.device ("5-1", keyboard(), 1, 0, 0, "C", "Broken", "0003", "0003");     // no bus / device number

    const auto found = scan (sys.root);
    EXPECT_EQ ((int) found.size(), 2);
    EXPECT (found[0].title == "A First");
    EXPECT (found[1].title == "B Second");
}

KTEST (UsbSysfs, the_bus_and_device_numbers_come_from_the_dev_attribute_when_the_two_files_are_missing)
{
    FakeSysfs sys;
    sys.device ("4-1", keyboard(), 1, 4, 2, "M-Audio", "Oxygen Pro Mini", "0763", "0123");
    fs::remove (sys.root / "4-1" / "busnum");
    fs::remove (sys.root / "4-1" / "devnum");

    // usbfs numbers its nodes as minor = 128 * (bus - 1) + (device - 1), and "dev" says "189:385".
    const auto found = look (sys.root);
    EXPECT_EQ ((int) found.candidates.size(), 1);
    EXPECT_EQ (found.candidates[0].bus, 4);
    EXPECT_EQ (found.candidates[0].device, 2);
    EXPECT (found.candidates[0].key() == "4:2:1");
    EXPECT_EQ (found.devices[0].bus, 4);
    EXPECT_EQ (found.devices[0].device, 2);

    // The two files, when they are there, are what counts.
    FakeSysfs files;
    files.device ("2-1", keyboard(), 1, 2, 9, "B", "Second", "0001", "0001");
    files.text (files.root / "2-1" / "dev", "189:5\n");   // would be bus 1, device 6
    const auto both = look (files.root);
    EXPECT_EQ ((int) both.candidates.size(), 1);
    EXPECT_EQ (both.candidates[0].bus, 2);
    EXPECT_EQ (both.candidates[0].device, 9);
}

KTEST (UsbSysfs, a_device_without_a_dev_attribute_is_still_found_but_has_no_numbers_for_making_its_node)
{
    FakeSysfs sys;
    sys.device ("4-1", keyboard(), 1, 4, 2, "M-Audio", "Oxygen Pro Mini", "0763", "0123");
    fs::remove (sys.root / "4-1" / "dev");

    const auto found = scan (sys.root);
    EXPECT_EQ ((int) found.size(), 1);
    EXPECT_EQ (found[0].bus, 4);
    EXPECT_EQ (found[0].device, 2);
    EXPECT_EQ (found[0].major, 0);   // openUsbPort then does not try to make the node
    EXPECT_EQ (found[0].minor, 0);

    // A nonsense "dev" gives no numbers either, and no bus or device number from it.
    FakeSysfs odd;
    odd.device ("5-1", keyboard(), 1, 0, 0, "C", "Broken", "0003", "0003");
    odd.text (odd.root / "5-1" / "dev", "not:numbers\n");
    EXPECT (scan (odd.root).empty());
}

KTEST (UsbSysfs, a_device_whose_descriptors_cannot_be_read_is_listed_for_the_log_but_not_offered)
{
    FakeSysfs sys;
    const std::string junk = "this is not a descriptor";

    fs::create_directories (sys.root / "7-1");
    sys.text (sys.root / "7-1" / "descriptors", junk);
    sys.text (sys.root / "7-1" / "idVendor", "0763\n");
    sys.text (sys.root / "7-1" / "idProduct", "103b\n");
    sys.text (sys.root / "7-1" / "manufacturer", "M-Audio\n");
    sys.text (sys.root / "7-1" / "product", "Oxygen Pro Mini\n");
    sys.text (sys.root / "7-1" / "busnum", "7\n");
    sys.text (sys.root / "7-1" / "devnum", "5\n");

    fs::create_directories (sys.root / "7-2");   // not even a descriptors file

    const auto found = look (sys.root);
    EXPECT (found.candidates.empty());
    EXPECT_EQ ((int) found.devices.size(), 2);

    const Device* named = nullptr;
    const Device* empty = nullptr;

    for (const auto& d : found.devices)
    {
        if (d.name == "7-1")
            named = &d;
        else if (d.name == "7-2")
            empty = &d;
    }

    EXPECT (named != nullptr && empty != nullptr);

    if (named != nullptr)
    {
        EXPECT (! named->readable);
        EXPECT (! named->midi);
        EXPECT_EQ (named->vendor, 0x0763);
        EXPECT_EQ (named->product, 0x103b);
        EXPECT_EQ (named->bus, 7);
        EXPECT_EQ (named->device, 5);
        EXPECT (named->title == "M-Audio Oxygen Pro Mini");
        EXPECT (named->interfaces == "unknown (" + std::to_string (junk.size()) + " bytes of descriptors could be read)");
    }

    if (empty != nullptr)
    {
        EXPECT (! empty->readable);
        EXPECT (empty->title == "USB device");
        EXPECT (empty->interfaces == "unknown (0 bytes of descriptors could be read)");
    }

    // A device that is understood is "readable", MIDI or not.
    FakeSysfs fine;
    fine.device ("4-2", mouse(), 1, 4, 3, "Logitech", "USB Optical Mouse", "046d", "c077");
    EXPECT (look (fine.root).devices[0].readable);
}

KTEST (UsbSysfs, garbage_in_a_device_folder_is_survived)
{
    FakeSysfs sys;
    fs::create_directories (sys.root / "7-1");
    sys.text (sys.root / "7-1" / "descriptors", "this is not a descriptor");
    sys.text (sys.root / "7-1" / "busnum", "x");

    fs::create_directories (sys.root / "7-2");                                   // no files at all
    sys.device ("7-3", keyboard(), 1, 7, 3, "M", "Fine", "0004", "0004");
    sys.text (sys.root / "7-3" / "bConfigurationValue", "nonsense");            // falls back to the first configuration

    const auto found = scan (sys.root);
    EXPECT_EQ ((int) found.size(), 1);
    EXPECT (found[0].name == "7-3");

    // The two broken ones are still listed (for the log), but only the good one is a candidate.
    EXPECT_EQ ((int) look (sys.root).devices.size(), 3);
}

#endif // __linux__
