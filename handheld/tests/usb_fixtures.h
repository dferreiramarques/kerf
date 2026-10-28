#pragma once

// Pretend USB devices for the tests: the descriptors a class-compliant MIDI keyboard (or a mouse)
// reports, and a copy of /sys/bus/usb/devices in a temporary folder.

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <system_error>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace usbfixtures
{
    namespace fs = std::filesystem;

    using Bytes = std::vector<uint8_t>;

    void add (Bytes& out, std::initializer_list<int> values)
    {
        for (int v : values)
            out.push_back ((uint8_t) v);
    }

    void append (Bytes& out, const Bytes& more)
    {
        out.insert (out.end(), more.begin(), more.end());
    }

    Bytes deviceDescriptor (int vendor, int product, int configurations = 1)
    {
        Bytes b;
        add (b, { 18, 1, 0x00, 0x02, 0, 0, 0, 64, vendor & 0xFF, vendor >> 8, product & 0xFF, product >> 8, 0x00, 0x01, 1, 2, 0, configurations });
        return b;
    }

    Bytes interfaceDescriptor (int number, int alt, int endpoints, int cls, int sub)
    {
        Bytes b;
        add (b, { 9, 4, number, alt, endpoints, cls, sub, 0, 0 });
        return b;
    }

    // Audio-class endpoints are 9 bytes long (bRefresh, bSynchAddress); plain ones are 7.
    Bytes endpointDescriptor (int address, int attributes, int maxPacket, bool audioClass = true)
    {
        Bytes b;
        add (b, { audioClass ? 9 : 7, 5, address, attributes, maxPacket & 0xFF, maxPacket >> 8, 0 });

        if (audioClass)
            add (b, { 0, 0 });

        return b;
    }

    // Class-specific descriptors (type 0x24 / 0x25) sit between the standard ones and must be skipped.
    Bytes classSpecific (int type, int size)
    {
        Bytes b;
        add (b, { size, type });

        for (int i = 2; i < size; ++i)
            b.push_back ((uint8_t) (i * 7));

        return b;
    }

    Bytes configurationDescriptor (int value, const Bytes& body, int interfaces = 2)
    {
        const int total = 9 + (int) body.size();
        Bytes b;
        add (b, { 9, 2, total & 0xFF, total >> 8, interfaces, value, 0, 0x80, 50 });
        append (b, body);
        return b;
    }

    // What a class-compliant keyboard shows: an Audio Control interface, then a MIDI Streaming
    // interface with one bulk OUT and one bulk IN endpoint.
    Bytes keyboardBody (int midiInterface = 1, int inAddress = 0x81, int attributes = 2)
    {
        Bytes body;
        append (body, interfaceDescriptor (0, 0, 0, 1, 1));
        append (body, classSpecific (0x24, 9));
        append (body, interfaceDescriptor (midiInterface, 0, 2, 1, 3));
        append (body, classSpecific (0x24, 7));
        append (body, classSpecific (0x24, 6));
        append (body, endpointDescriptor (0x01, 2, 64));
        append (body, classSpecific (0x25, 5));
        append (body, endpointDescriptor (inAddress, attributes, 64));
        append (body, classSpecific (0x25, 5));
        return body;
    }

    Bytes keyboard()
    {
        Bytes all = deviceDescriptor (0x0763, 0x0123);
        append (all, configurationDescriptor (1, keyboardBody()));
        return all;
    }

    struct FakeSysfs
    {
        fs::path root;

        FakeSysfs()
        {
            // The process number is in the name: test programs that run at the same time (ctest -j) must not share a folder.
            static int counter = 0;
#if defined(_WIN32)
            const long process = (long) _getpid();
#else
            const long process = (long) ::getpid();
#endif
            std::error_code ec;
            root = fs::temp_directory_path() / ("kerf-usb-sysfs-test-" + std::to_string (process) + "-" + std::to_string (++counter));
            fs::remove_all (root, ec);
            fs::create_directories (root, ec);
        }

        ~FakeSysfs()
        {
            std::error_code ec;
            fs::remove_all (root, ec);
        }

        void text (const fs::path& file, const std::string& content) const
        {
            std::ofstream out (file, std::ios::binary);
            out << content;
        }

        // A device folder like the kernel's: descriptors, numbers and names. It is built next to the
        // others under a name with a ':' in it (which the search skips) and renamed when it is
        // complete, so that a search running at the same time never sees half of it.
        void device (const std::string& name, const Bytes& descriptors, int configuration, int bus, int number,
                     const std::string& maker, const std::string& product, const std::string& vendor, const std::string& productId) const
        {
            const fs::path dir = root / ("building:" + name);
            std::error_code ec;
            fs::remove_all (dir, ec);
            fs::create_directories (dir);
            std::ofstream out (dir / "descriptors", std::ios::binary);
            out.write ((const char*) descriptors.data(), (std::streamsize) descriptors.size());
            out.close();
            text (dir / "bConfigurationValue", std::to_string (configuration) + "\n");
            text (dir / "busnum", std::to_string (bus) + "\n");
            text (dir / "devnum", std::to_string (number) + "\n");
            text (dir / "idVendor", vendor + "\n");
            text (dir / "idProduct", productId + "\n");
            text (dir / "dev", "189:" + std::to_string ((bus - 1) * 128 + number - 1) + "\n");

            if (! maker.empty())
                text (dir / "manufacturer", maker + "\n");

            if (! product.empty())
                text (dir / "product", product + "\n");

            fs::rename (dir, root / name);
        }

        // The kernel removes the device and its interfaces from sysfs when it is unplugged.
        void remove (const std::string& name) const
        {
            std::error_code ec;
            std::vector<fs::path> gone;

            for (const auto& entry : fs::directory_iterator (root, ec))
            {
                const std::string entryName = entry.path().filename().string();

                if (entryName == name || entryName.rfind (name + ":", 0) == 0)
                    gone.push_back (entry.path());
            }

            // Renamed first (to a name the search skips), then deleted: it vanishes in one step.
            int n = 0;

            for (const auto& path : gone)
            {
                const fs::path away = root / ("gone:" + std::to_string (n++) + ":" + path.filename().string());
                fs::rename (path, away, ec);
                fs::remove_all (away, ec);
            }
        }

        // Binds a (made-up) kernel driver to an interface: the kernel shows it as a link named "driver".
        // False when this file system cannot make a symbolic link that reads back (Windows without the
        // privilege, Wine): a test that needs one then has nothing to look at.
        bool bind (const std::string& interfaceName, const std::string& driver) const
        {
            std::error_code ec;
            fs::create_directories (root / interfaceName, ec);
            const fs::path link = root / interfaceName / "driver";
            fs::create_symlink (fs::path ("../../../bus/usb/drivers") / driver, link, ec);

            if (ec)
                return false;

            std::error_code readError;
            return fs::is_symlink (link, readError) && ! readError && ! fs::read_symlink (link, readError).empty() && ! readError;
        }
    };

    Bytes mouse()
    {
        Bytes body;
        append (body, interfaceDescriptor (0, 0, 1, 3, 1));
        append (body, endpointDescriptor (0x81, 3, 8, false));
        Bytes all = deviceDescriptor (0x046d, 0xc077);
        append (all, configurationDescriptor (1, body, 1));
        return all;
    }
}
