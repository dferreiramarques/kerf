#pragma once

// The system's own audio inputs, through SDL: microphones, line inputs and USB audio interfaces
// that the operating system has a driver for (Windows, macOS, Linux desktops).
//
// Sound is captured as 32 bit floating point at the device's own rate and number of channels, so a
// mixer with a driver keeps all its channels. SDL calls back on its own thread; the callback only
// writes to the CaptureRing.

#include <memory>

#include "audio_input.h"

namespace kerf::ui
{
    std::unique_ptr<InputBackend> makeSdlAudioInput();
}
