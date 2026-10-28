#pragma once

// Reading what the kernel shows about USB devices under /sys/bus/usb/devices (or a copy of it, which
// is what the tests use). Shared by the MIDI (usb_midi.h) and the audio (usb_audio.h) search.
// Needs no operating system calls, so it builds and is tested everywhere.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace kerf::ui::usbsys
{
    inline std::vector<uint8_t> readBytes (const std::filesystem::path& file)
    {
        std::ifstream in (file, std::ios::binary);
        return std::vector<uint8_t> ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
    }

    inline std::string readLine (const std::filesystem::path& file)
    {
        std::ifstream in (file);
        std::string line;
        std::getline (in, line);

        while (! line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\n'))
            line.pop_back();

        return line;
    }

    inline long readNumber (const std::filesystem::path& file, int base, long fallback)
    {
        const auto text = readLine (file);
        char* end = nullptr;
        const long v = std::strtol (text.c_str(), &end, base);
        return (end == text.c_str()) ? fallback : v;
    }

    // Who a device is, from the small files in its folder.
    struct Identity
    {
        std::string name;           // the kernel's name for the device, e.g. "4-1"
        std::string title;          // "TEAC Model 12"; empty when the device gives no names
        int vendor = 0;
        int product = 0;
        int bus = 0;                // -> /dev/bus/usb/<bus>/<device>
        int device = 0;
        int major = 0;              // from the "dev" attribute, to make the node when it is missing
        int minor = 0;
        int speed = 0;              // Mbit/s: 1 (1.5), 12, 480, 5000 ...; 0 when unknown
    };

    // Where the kernel lists the USB devices. KERF_USB_SYSFS points the search at another folder (for tests).
    inline std::filesystem::path devicesDir()
    {
        const char* root = std::getenv ("KERF_USB_SYSFS");
        return (root != nullptr && *root != '\0') ? std::filesystem::path (root) : std::filesystem::path ("/sys/bus/usb/devices");
    }

    // Devices are "<bus>-<port>[.<port>...]"; interfaces ("4-1:1.1") and root hubs ("usb4") are not.
    inline bool isDeviceName (const std::string& name)
    {
        return ! name.empty() && name.find (':') == std::string::npos && name.rfind ("usb", 0) != 0;
    }

    inline Identity readIdentity (const std::filesystem::path& devicesDir, const std::string& name)
    {
        const std::filesystem::path base = devicesDir / name;
        const std::string maker = readLine (base / "manufacturer");
        const std::string product = readLine (base / "product");

        Identity id;
        id.name = name;
        id.title = product;

        if (! maker.empty() && product.rfind (maker, 0) != 0)
            id.title = maker + " " + product;

        id.vendor = (int) readNumber (base / "idVendor", 16, 0);
        id.product = (int) readNumber (base / "idProduct", 16, 0);
        id.bus = (int) readNumber (base / "busnum", 10, 0);
        id.device = (int) readNumber (base / "devnum", 10, 0);
        id.speed = (int) readNumber (base / "speed", 10, 0);

        // The "dev" attribute is "<major>:<minor>", and usbfs numbers its nodes as
        // minor = 128 * (bus - 1) + (device - 1): that gives the bus and the device number when
        // the two attributes above are missing.
        const auto devNumbers = readLine (base / "dev");
        std::sscanf (devNumbers.c_str(), "%d:%d", &id.major, &id.minor);

        if ((id.bus <= 0 || id.device <= 0) && id.major > 0 && id.minor >= 0)
        {
            id.bus = id.minor / 128 + 1;
            id.device = id.minor % 128 + 1;
        }

        return id;
    }

    // The kernel driver that is bound to an interface ("snd-usb-audio", "usbhid"...), empty when
    // there is none. The interface folder is "<device>:<configuration>.<interface>".
    inline std::string driverOf (const std::filesystem::path& devicesDir, const std::string& deviceName, int configuration, int interfaceNumber)
    {
        namespace fs = std::filesystem;
        const fs::path interfaceDir = devicesDir / (deviceName + ":" + std::to_string (configuration) + "." + std::to_string (interfaceNumber));
        std::error_code linkError;
        const auto link = fs::read_symlink (interfaceDir / "driver", linkError);
        return linkError ? std::string() : link.filename().string();
    }

    // "01 0a ff ..." for the log, at most `limit` bytes.
    inline std::string hexBytes (const uint8_t* data, size_t size, size_t limit = 1024)
    {
        std::string out;
        char one[8];

        for (size_t i = 0; i < size && i < limit; ++i)
        {
            std::snprintf (one, sizeof one, "%02x", (unsigned) data[i]);
            out += (i == 0 ? "" : " ");
            out += one;
        }

        if (size > limit)
            out += " ...";

        return out;
    }
}
