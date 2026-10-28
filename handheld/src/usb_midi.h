#pragma once

// USB MIDI without a kernel driver.
//
// Some handheld kernels (muOS on the RG35XX H is one) are built without the USB audio / MIDI
// driver, so a class-compliant USB MIDI keyboard shows up on the USB bus but nothing reads it.
// Kerf then reads the keyboard itself, through the kernel's generic USB interface (usbfs).
//
// This header holds the parts that need no operating system calls, so the tests can run
// anywhere:
//   - parsing the descriptors of a USB device,
//   - finding its MIDI Streaming interfaces and their input endpoints,
//   - decoding the 4-byte "USB-MIDI event packets" the device sends,
//   - looking for such devices in (a copy of) /sys/bus/usb/devices.
// Claiming the interface and queueing the transfers is in midi_input.cpp (Linux only).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "midi.h"
#include "usb_sysfs.h"

namespace kerf::ui::usbmidi
{
    // ------------------------------------------------------------------------------ descriptors

    struct Endpoint
    {
        uint8_t address = 0;       // bit 7 set = IN (device to host)
        uint8_t attributes = 0;    // bits 1..0: 2 = bulk, 3 = interrupt
        uint16_t maxPacket = 0;

        bool isIn() const noexcept { return (address & 0x80) != 0; }
        int transferType() const noexcept { return attributes & 3; }
        bool carriesMidi() const noexcept { return isIn() && (transferType() == 2 || transferType() == 3); }
    };

    struct Interface            // one alternate setting of one interface
    {
        uint8_t number = 0;
        uint8_t alt = 0;
        uint8_t cls = 0;
        uint8_t sub = 0;
        uint8_t proto = 0;
        std::vector<Endpoint> endpoints;
    };

    struct Configuration
    {
        uint8_t value = 0;                  // bConfigurationValue
        std::vector<Interface> interfaces;
    };

    // The input side of one MIDI Streaming interface (audio class 1, subclass 3).
    struct Stream
    {
        int interfaceNumber = 0;
        int alt = 0;                        // the alternate setting that has the input endpoints
        std::vector<Endpoint> inputs;
    };

    // Parses what the kernel shows as /sys/bus/usb/devices/<device>/descriptors: the 18-byte
    // device descriptor, then every configuration descriptor followed by its interface and
    // endpoint descriptors. Anything that does not fit is ignored.
    inline std::vector<Configuration> parseDescriptors (const uint8_t* data, size_t size)
    {
        std::vector<Configuration> out;
        size_t at = 0;

        if (size >= 18 && data[0] == 18 && data[1] == 1)
            at = 18;

        while (at + 2 <= size)
        {
            const size_t length = data[at];
            const int type = data[at + 1];

            if (length < 2 || at + length > size)
                break;

            if (type == 2 && length >= 9)
            {
                Configuration c;
                c.value = data[at + 5];
                out.push_back (std::move (c));
            }
            else if (type == 4 && length >= 9 && ! out.empty())
            {
                Interface i;
                i.number = data[at + 2];
                i.alt = data[at + 3];
                i.cls = data[at + 5];
                i.sub = data[at + 6];
                i.proto = data[at + 7];
                out.back().interfaces.push_back (std::move (i));
            }
            else if (type == 5 && length >= 7 && ! out.empty() && ! out.back().interfaces.empty())
            {
                Endpoint e;
                e.address = data[at + 2];
                e.attributes = data[at + 3];
                e.maxPacket = (uint16_t) (data[at + 4] | (data[at + 5] << 8));
                out.back().interfaces.back().endpoints.push_back (e);
            }

            at += length;
        }

        return out;
    }

    inline const Configuration* findConfiguration (const std::vector<Configuration>& all, int value)
    {
        for (const auto& c : all)
            if (c.value == value)
                return &c;

        return all.empty() ? nullptr : &all.front();
    }

    // Every MIDI Streaming interface that has an input endpoint. When an interface has several
    // alternate settings the lowest one with an input endpoint is used (setting 0 is the
    // classic MIDI 1.0 one; a later one may be MIDI 2.0).
    inline std::vector<Stream> findMidiStreams (const Configuration& cfg)
    {
        auto hasInput = [] (const Interface& i)
        {
            return std::any_of (i.endpoints.begin(), i.endpoints.end(), [] (const Endpoint& e) { return e.carriesMidi(); });
        };

        std::vector<Stream> out;
        std::vector<int> seen;

        for (const auto& first : cfg.interfaces)
        {
            if (first.cls != 1 || first.sub != 3 || std::find (seen.begin(), seen.end(), (int) first.number) != seen.end())
                continue;

            seen.push_back (first.number);
            const Interface* best = nullptr;

            for (const auto& alt : cfg.interfaces)
                if (alt.number == first.number && alt.cls == 1 && alt.sub == 3 && hasInput (alt) && (best == nullptr || alt.alt < best->alt))
                    best = &alt;

            if (best == nullptr)
                continue;

            Stream s;
            s.interfaceNumber = best->number;
            s.alt = best->alt;

            for (const auto& e : best->endpoints)
                if (e.carriesMidi())
                    s.inputs.push_back (e);

            out.push_back (std::move (s));
        }

        return out;
    }

    // ---------------------------------------------------------------------------- event packets

    // USB-MIDI 1.0 sends 4-byte packets: the high nibble of the first byte is the cable (a
    // virtual MIDI port), the low nibble the "code index number", which says how many of the
    // next three bytes are MIDI.
    inline int packetBytes (int codeIndex) noexcept
    {
        static const int bytes[16] = { 0, 0, 2, 3, 3, 1, 2, 3, 3, 3, 3, 3, 2, 2, 3, 1 };
        return bytes[codeIndex & 15];
    }

    // Turns the packets of one finished transfer into channel messages. Each cable has its own
    // parser, so a SysEx dump on one cable cannot disturb the notes on another.
    class PacketDecoder
    {
    public:
        template <typename Deliver>
        void feed (const uint8_t* data, size_t size, Deliver&& deliver)
        {
            for (size_t at = 0; at + 4 <= size; at += 4)
            {
                const int cable = data[at] >> 4;
                const int count = packetBytes (data[at] & 15);
                kerf::MidiMessage m;

                for (int k = 0; k < count; ++k)
                    if (parsers_[cable].feed (data[at + 1 + (size_t) k], m))
                        deliver (m);
            }
        }

        void reset() noexcept
        {
            for (auto& p : parsers_)
                p.reset();
        }

    private:
        kerf::MidiParser parsers_[16];
    };

    // ------------------------------------------------------------------------- finding devices

    struct Candidate
    {
        std::string name;           // the kernel's name for the device, e.g. "4-1"
        std::string title;          // "M-Audio Oxygen Pro Mini"
        int vendor = 0;
        int product = 0;
        int bus = 0;                // -> /dev/bus/usb/<bus>/<device>
        int device = 0;
        int major = 0;              // from the device's "dev" attribute, to make the node when it is missing
        int minor = 0;
        Stream stream;
        std::string driver;         // the kernel driver bound to the interface; empty = none

        std::string key() const
        {
            return std::to_string (bus) + ":" + std::to_string (device) + ":" + std::to_string (stream.interfaceNumber);
        }
    };

    // Any USB device that was looked at, MIDI or not. Kerf writes one line about each to its log,
    // which is what tells why a keyboard is (or is not) picked up.
    struct Device
    {
        std::string name;
        std::string title;
        int vendor = 0;
        int product = 0;
        int bus = 0;
        int device = 0;
        std::string interfaces;     // "1.1 1.3": class.subclass of each interface, once each
        bool readable = true;       // false when the descriptors could not be read or understood
        bool midi = false;          // has a MIDI Streaming interface with an input

        std::string key() const { return std::to_string (bus) + ":" + std::to_string (device); }
    };

    struct Scan
    {
        std::vector<Device> devices;
        std::vector<Candidate> candidates;
    };

    namespace detail
    {
        using usbsys::readBytes;
        using usbsys::readLine;
        using usbsys::readNumber;
    }

    // Looks at every USB device in `devicesDir` (normally /sys/bus/usb/devices). `candidates` are
    // the MIDI Streaming interfaces; their `driver` tells whether the kernel already has a driver
    // for the interface, and only the ones without one need Kerf's own reading.
    inline Scan look (const std::filesystem::path& devicesDir)
    {
        namespace fs = std::filesystem;
        Scan out;
        std::error_code ec;

        for (const auto& entry : fs::directory_iterator (devicesDir, ec))
        {
            const std::string name = entry.path().filename().string();

            if (! usbsys::isDeviceName (name))
                continue;

            const fs::path base = devicesDir / name;
            const auto blob = detail::readBytes (base / "descriptors");
            const auto configs = parseDescriptors (blob.data(), blob.size());
            const usbsys::Identity id = usbsys::readIdentity (devicesDir, name);
            const std::string title = id.title;
            const int major = id.major, minor = id.minor;

            Device d;
            d.name = name;
            d.title = title.empty() ? "USB device" : title;
            d.vendor = id.vendor;
            d.product = id.product;
            d.bus = id.bus;
            d.device = id.device;

            const Configuration* cfg = nullptr;

            if (! configs.empty())
            {
                const int active = (int) detail::readNumber (base / "bConfigurationValue", 10, configs.front().value);
                cfg = findConfiguration (configs, active);
            }

            if (cfg == nullptr)
            {
                // Still worth a line in the log: it is the reason a keyboard is not picked up.
                d.readable = false;
                d.interfaces = "unknown (" + std::to_string (blob.size()) + " bytes of descriptors could be read)";
                out.devices.push_back (std::move (d));
                continue;
            }

            const auto streams = findMidiStreams (*cfg);
            d.midi = ! streams.empty();

            for (const auto& i : cfg->interfaces)
            {
                const std::string kind = std::to_string ((int) i.cls) + "." + std::to_string ((int) i.sub);

                if ((" " + d.interfaces + " ").find (" " + kind + " ") == std::string::npos)
                    d.interfaces += (d.interfaces.empty() ? "" : " ") + kind;
            }

            out.devices.push_back (d);

            if (streams.empty())
                continue;

            for (const auto& s : streams)
            {
                Candidate c;
                c.name = name;
                c.title = title.empty() ? "USB MIDI device" : title;
                c.vendor = d.vendor;
                c.product = d.product;
                c.bus = d.bus;
                c.device = d.device;
                c.major = major;
                c.minor = minor;
                c.stream = s;

                c.driver = usbsys::driverOf (devicesDir, name, (int) cfg->value, s.interfaceNumber);

                if (c.bus > 0 && c.device > 0)
                    out.candidates.push_back (std::move (c));
            }
        }

        const auto byKey = [] (const auto& a, const auto& b) { return a.key() < b.key(); };
        std::sort (out.candidates.begin(), out.candidates.end(), byKey);
        std::sort (out.devices.begin(), out.devices.end(), byKey);
        return out;
    }

    inline std::vector<Candidate> scan (const std::filesystem::path& devicesDir)
    {
        return look (devicesDir).candidates;
    }
}
