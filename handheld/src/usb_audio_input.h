#pragma once

// Kerf's own reading of USB audio devices, as an InputBackend for the Record screen.
//
// The handheld's kernel has no driver for USB audio (no snd-usb-audio), so a wireless-microphone
// receiver or a mixer plugged into its USB-C port shows up as a USB device and nothing more. This
// backend finds such devices in sysfs (usb_audio.h), opens them through usbfs and reads the
// isochronous stream itself (usb_audio_io.h) on a thread of its own, which writes the decoded sound
// to the CaptureRing.
//
// Only devices that no kernel driver has are listed; where the kernel does have a driver the
// system's own audio (SDL) offers the device. The device being read is listed too, although its
// interface now shows the "usbfs" driver, so that the screen keeps pointing at it.
//
// Every device found is written to the log once, with its descriptors in hex, so that a device
// that is not recognised can be understood from kerf-log.txt.

#include <filesystem>
#include <memory>

#include "audio_input.h"

namespace kerf::ui
{
    namespace usbos { class UsbOs; }

    struct UsbAudioInputOptions
    {
        usbos::UsbOs* os = nullptr;                 // tests put a pretend kernel here
        std::filesystem::path sysfs;                // where the USB devices are listed; empty = $KERF_USB_SYSFS, else /sys/bus/usb/devices
    };

    // Returns nullptr where there is no usbfs (anything but Linux).
    std::unique_ptr<InputBackend> makeUsbAudioInput (const UsbAudioInputOptions& options = {});
}
