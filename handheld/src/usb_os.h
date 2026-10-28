#pragma once

// What the direct USB readers have in common (Linux only): MIDI is in usb_midi_io.h and audio in
// usb_audio_io.h.
//
// Every system call that reaches the kernel goes through UsbOs, so that the tests can put a pretend
// kernel in its place (tests/fake_usb_kernel.h). A device is used through usbfs, the way libusb
// does it: open /dev/bus/usb/<bus>/<device>, claim an interface, queue transfers ("URBs"), and
// when one has finished, fetch it. usbfs says that a transfer has finished by making the file
// "writable" (POLLOUT); the finished transfer is then fetched with REAPURBNDELAY.

#if defined(__linux__)

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>
#include <linux/usbdevice_fs.h>

namespace kerf::ui::usbos
{
    class UsbOs
    {
    public:
        virtual ~UsbOs() = default;

        // Like open(2), close(2) and ioctl(2): -1 and errno when they fail.
        virtual int open (const char* path, int flags) = 0;
        virtual int close (int fd) = 0;
        virtual int ioctl (int fd, unsigned long request, void* argument) = 0;

        // Makes /dev/bus/usb/<bus>/<device> when /dev has no entry for the device. 0, or -1 and errno.
        virtual int makeNode (int bus, int device, int major, int minor) = 0;

        // Asks the kernel not to put the device to sleep (power/control = "on"). Best effort.
        virtual void keepAwake (const std::string& deviceName) = 0;
    };

    class SystemUsbOs : public UsbOs
    {
    public:
        int open (const char* path, int flags) override { return ::open (path, flags); }
        int close (int fd) override { return ::close (fd); }
        int ioctl (int fd, unsigned long request, void* argument) override { return ::ioctl (fd, request, argument); }

        int makeNode (int bus, int device, int major, int minor) override
        {
            // The kernel knows the device but /dev has no node for it (no udev or devtmpfs entry).
            // The numbers come from the device's own "dev" attribute in sysfs.
            char folder[48], path[96];
            std::snprintf (folder, sizeof folder, "/dev/bus/usb/%03d", bus);
            std::snprintf (path, sizeof path, "/dev/bus/usb/%03d/%03d", bus, device);
            ::mkdir ("/dev/bus", 0755);
            ::mkdir ("/dev/bus/usb", 0755);
            ::mkdir (folder, 0755);
            return ::mknod (path, S_IFCHR | 0660, makedev ((unsigned) major, (unsigned) minor));
        }

        void keepAwake (const std::string& deviceName) override
        {
            const std::string file = "/sys/bus/usb/devices/" + deviceName + "/power/control";
            const int fd = ::open (file.c_str(), O_WRONLY | O_CLOEXEC);

            if (fd >= 0)
            {
                const ssize_t written = ::write (fd, "on", 2);
                (void) written;
                ::close (fd);
            }
        }
    };

    using Logger = std::function<void (const std::string&)>;

    inline std::string hex (unsigned value, int digits)
    {
        char text[16];
        std::snprintf (text, sizeof text, "%0*x", digits, value);
        return text;
    }

    inline std::string devicePath (int bus, int device)
    {
        char path[96];
        std::snprintf (path, sizeof path, "/dev/bus/usb/%03d/%03d", bus, device);
        return path;
    }

    // Opens the device for reading and writing (usbfs reports finished transfers only to a file that
    // was opened for writing), making its node first when /dev has none. Returns the file, or -1
    // after writing the reason to the log; every line starts with `who`.
    inline int openDeviceNode (UsbOs& os, int bus, int device, int major, int minor, const std::string& who, const Logger& log)
    {
        const std::string path = devicePath (bus, device);
        int fd = os.open (path.c_str(), O_RDWR | O_CLOEXEC);
        int err = errno;

        if (fd < 0 && err == ENOENT && major > 0)
        {
            if (os.makeNode (bus, device, major, minor) == 0)
            {
                log (who + "made the missing device node " + path);
                fd = os.open (path.c_str(), O_RDWR | O_CLOEXEC);
                err = errno;
            }
            else
            {
                err = errno;
                log (who + "there is no device node " + path + " and it cannot be made: " + std::strerror (err));
                return -1;
            }
        }

        if (fd < 0)
        {
            log (who + "cannot open " + path + ": " + std::strerror (err)
                 + ((err == EACCES || err == EPERM) ? " (Kerf needs to run as root for this)" : ""));
            return -1;
        }

        return fd;
    }

    // A transfer on the control pipe (endpoint 0). Returns the number of bytes moved, or -1 and
    // errno. The kernel limits `length` to one page, and the interface or endpoint that a class
    // request is aimed at has to have been claimed first.
    inline int controlTransfer (UsbOs& os, int fd, uint8_t requestType, uint8_t request, uint16_t value, uint16_t index,
                                void* data, uint16_t length, unsigned timeoutMs = 300)
    {
        usbdevfs_ctrltransfer transfer;
        std::memset (&transfer, 0, sizeof transfer);
        transfer.bRequestType = requestType;
        transfer.bRequest = request;
        transfer.wValue = value;
        transfer.wIndex = index;
        transfer.wLength = length;
        transfer.timeout = timeoutMs;
        transfer.data = data;
        return os.ioctl (fd, USBDEVFS_CONTROL, &transfer);
    }
}

#endif // __linux__
