#pragma once

// Pretend USB audio devices that answer the way a real one does, and the sound they send. They go
// with the pretend kernel (fake_usb_kernel.h) and with the descriptors of usb_audio_fixtures.h, for
// the tests of the audio reader (usb_audio_io.h) and of the Record screen's USB input.

#if defined(__linux__)

#include <cerrno>
#include <cstdint>
#include <memory>
#include <vector>

#include "fake_usb_kernel.h"
#include "usb_audio.h"
#include "usb_audio_fixtures.h"

namespace usbaudiofixtures
{
    using kerf::ui::usbfake::FakeDevice;
    using kerf::ui::usbfake::FakeKernel;

    inline constexpr int kIso = USBDEVFS_URB_TYPE_ISO;

    inline void put16 (Bytes& out, int value) { out.push_back ((uint8_t) (value & 0xFF)); out.push_back ((uint8_t) ((value >> 8) & 0xFF)); }
    inline void put24 (Bytes& out, int value) { put16 (out, value); out.push_back ((uint8_t) ((value >> 16) & 0xFF)); }

    // Frames of stereo 16 bit sound: frame k is (k, -k).
    inline Bytes stereoFrames (int first, int count)
    {
        Bytes b;

        for (int k = first; k < first + count; ++k)
        {
            put16 (b, k);
            put16 (b, -k);
        }

        return b;
    }

    inline std::vector<Bytes> stereoPackets (int firstFrame, int packets, int framesPerPacket)
    {
        std::vector<Bytes> out;

        for (int p = 0; p < packets; ++p)
            out.push_back (stereoFrames (firstFrame + p * framesPerPacket, framesPerPacket));

        return out;
    }

    // Frames of mono 16 bit sound: frame k is k.
    inline std::vector<Bytes> monoPackets (int firstFrame, int packets, int framesPerPacket)
    {
        std::vector<Bytes> out;

        for (int p = 0; p < packets; ++p)
        {
            Bytes b;

            for (int k = 0; k < framesPerPacket; ++k)
                put16 (b, firstFrame + p * framesPerPacket + k);

            out.push_back (b);
        }

        return out;
    }

    // ------------------------------------------------------------ a receiver with a microphone (version 1)

    struct Receiver
    {
        int rate = 0;               // what the program set
        int reported = -1;          // what the device answers when asked: -1 = what was set
        bool refuses = false;       // the device stalls the request that sets the rate
    };

    // The pretend device behind usb_audio_fixtures.h's receiver(): the interfaces 0 (control) and 1
    // (the microphone, endpoint 0x81, in the settings 1 to 4), which takes the rate with a request to
    // the endpoint and says it back.
    inline FakeDevice& addReceiver (FakeKernel& kernel, const std::shared_ptr<Receiver>& state, int bus = 4, int number = 2)
    {
        auto& d = kernel.add (bus, number, { { 0x81, kIso } });
        d.interfaces = { 0, 1 };
        d.endpointInterface[0x81] = 1;
        d.endpointAlt[0x81] = { 1, 2, 3, 4 };
        d.isoMaxPacket = 400;
        d.onControl = [state] (usbdevfs_ctrltransfer& t) -> int
        {
            auto* data = static_cast<uint8_t*> (t.data);

            if (t.bRequestType == 0x22 && t.bRequest == 0x01 && t.wLength == 3)         // SET_CUR of the sampling frequency, on the endpoint
            {
                if (state->refuses)
                    return -EPIPE;

                state->rate = (int) kerf::ui::usbaudio::getRate (data, 3);
                return 3;
            }

            if (t.bRequestType == 0xA2 && t.bRequest == 0x81 && t.wLength == 3)         // GET_CUR
            {
                kerf::ui::usbaudio::putRate (data, 3, (uint32_t) (state->reported >= 0 ? state->reported : state->rate));
                return 3;
            }

            return -EPIPE;
        };

        return d;
    }
}

#endif // __linux__
