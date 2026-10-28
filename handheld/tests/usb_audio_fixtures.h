#pragma once

// Pretend USB audio devices for the tests: the descriptors a wireless-microphone receiver (Audio
// Class 1) and a mixer with a USB interface (Audio Class 2, like the Tascam Model 12) report.
// Built from the layouts in "Universal Serial Bus Device Class Definition for Audio Devices" 1.0
// and 2.0, so that the tests do not depend on the parser agreeing with itself.

#include <cstdint>
#include <initializer_list>
#include <vector>

#include "usb_fixtures.h"

namespace usbaudiofixtures
{
    using usbfixtures::Bytes;
    using usbfixtures::add;
    using usbfixtures::append;

    // A descriptor whose length byte is filled in: { length, type, body... }.
    inline Bytes desc (int type, std::initializer_list<int> body)
    {
        Bytes b;
        b.push_back ((uint8_t) (2 + body.size()));
        b.push_back ((uint8_t) type);

        for (int v : body)
            b.push_back ((uint8_t) v);

        return b;
    }

    inline Bytes interfaceWith (int number, int alt, int endpoints, int cls, int sub, int protocol)
    {
        Bytes b;
        add (b, { 9, 4, number, alt, endpoints, cls, sub, protocol, 0 });
        return b;
    }

    // An audio endpoint: 9 bytes, with bRefresh and bSynchAddress.
    inline Bytes audioEndpoint (int address, int attributes, int maxPacket, int interval = 1, int syncAddress = 0)
    {
        Bytes b;
        add (b, { 9, 5, address, attributes, maxPacket & 0xFF, maxPacket >> 8, interval, 0, syncAddress });
        return b;
    }

    // ----------------------------------------------------------------------------- version 1

    struct Format1
    {
        int alt = 1;
        int channels = 1;
        int subslot = 2;
        int bits = 16;
        std::vector<int> rates { 44100, 48000 };    // empty = a range, rateLow to rateHigh
        int rateLow = 8000;
        int rateHigh = 48000;
        int tag = 1;                                // wFormatTag: 1 PCM, 2 PCM8, 3 IEEE float, 0x1001 MPEG
        int formatType = 1;
        int packet = 100;
        bool rateControl = true;
        int interval = 1;                           // bInterval of the endpoint
    };

    inline Bytes controlV1 (int terminalType, int channels, std::initializer_list<int> streams, int terminalId = 1)
    {
        Bytes b;
        append (b, interfaceWith (0, 0, 0, 1, 1, 0));

        std::vector<int> header { 0x00, 0x01, 0x00, 0x00, (int) streams.size() };

        for (int s : streams)
            header.push_back (s);

        Bytes h;
        h.push_back ((uint8_t) (3 + header.size()));
        h.push_back (0x24);
        h.push_back (1);

        for (int v : header)
            h.push_back ((uint8_t) v);

        append (b, h);
        append (b, desc (0x24, { 2, terminalId, terminalType & 0xFF, terminalType >> 8, 0, channels, 0x03, 0x00, 0, 0 }));      // INPUT_TERMINAL
        append (b, desc (0x24, { 6, 2, terminalId, 1, 1, 0, 0 }));                                                              // FEATURE_UNIT
        append (b, desc (0x24, { 3, 3, 0x01, 0x01, 0, 2, 0 }));                                                                 // OUTPUT_TERMINAL, USB streaming
        return b;
    }

    // One AudioStreaming interface: alternate setting 0 (no endpoint) and one setting per format.
    inline Bytes streamV1 (int number, const std::vector<Format1>& formats, int endpoint = 0x81, int attributes = 0x05, int terminalLink = 3)
    {
        Bytes b;
        append (b, interfaceWith (number, 0, 0, 1, 2, 0));

        for (const auto& f : formats)
        {
            append (b, interfaceWith (number, f.alt, 1, 1, 2, 0));
            append (b, desc (0x24, { 1, terminalLink, 1, f.tag & 0xFF, f.tag >> 8 }));                                         // AS_GENERAL

            std::vector<int> format { 2, f.formatType, f.channels, f.subslot, f.bits };

            if (f.rates.empty())
            {
                format.insert (format.end(), { 0, f.rateLow & 0xFF, (f.rateLow >> 8) & 0xFF, f.rateLow >> 16,
                                               f.rateHigh & 0xFF, (f.rateHigh >> 8) & 0xFF, f.rateHigh >> 16 });
            }
            else
            {
                format.push_back ((int) f.rates.size());

                for (int r : f.rates)
                    format.insert (format.end(), { r & 0xFF, (r >> 8) & 0xFF, r >> 16 });
            }

            Bytes ft;
            ft.push_back (0);                       // the length, below
            ft.push_back (0x24);

            for (int v : format)
                ft.push_back ((uint8_t) v);

            ft[0] = (uint8_t) ft.size();
            append (b, ft);
            append (b, audioEndpoint (endpoint, attributes, f.packet, f.interval));
            append (b, desc (0x25, { 1, f.rateControl ? 1 : 0, 0, 0, 0 }));                                                    // EP_GENERAL
        }

        return b;
    }

    // A receiver for a wireless lavalier: one microphone, mono and stereo settings.
    inline Bytes receiver (int vendor = 0x1234, int product = 0x5678, std::vector<Format1> formats = { { 1, 1, 2, 16, { 44100, 48000 }, 0, 0, 1, 1, 100, true },
                                                                                                         { 2, 2, 2, 16, { 44100, 48000 }, 0, 0, 1, 1, 196, true } })
    {
        Bytes body = controlV1 (0x0201, 2, { 1 });
        append (body, streamV1 (1, formats));
        Bytes all = usbfixtures::deviceDescriptor (vendor, product);
        append (all, usbfixtures::configurationDescriptor (1, body, 2));
        return all;
    }

    // ----------------------------------------------------------------------------- version 2

    struct Format2
    {
        int alt = 1;
        int channels = 2;
        int subslot = 3;
        int bits = 24;
        int formats = 1;                            // bmFormats: bit 0 PCM, bit 1 PCM8, bit 2 float
        int formatType = 1;
        int packet = 300;
        int interval = 1;
    };

    // The AudioControl interface of a mixer: a clock source (id 0x28, rate can be set), optionally
    // a second one (0x2A) and a selector (0x29) that chooses between them; an input terminal for
    // the playback stream and an output terminal for the recording stream, both on the clock.
    inline Bytes controlV2 (bool withSelector, int clockForRecording = 0x28)
    {
        Bytes b;
        append (b, interfaceWith (0, 0, 0, 1, 1, 0x20));
        append (b, desc (0x24, { 1, 0x00, 0x02, 0x08, 0x00, 0x00, 0x00 }));                                                     // HEADER
        append (b, desc (0x24, { 0x0A, 0x28, 0x01, 0x07, 0, 0 }));                                                              // CLOCK_SOURCE, programmable
        append (b, desc (0x24, { 0x0A, 0x2A, 0x01, 0x05, 0, 0 }));                                                              // CLOCK_SOURCE, read only

        if (withSelector)
            append (b, desc (0x24, { 0x0B, 0x29, 2, 0x28, 0x2A, 0x03, 0 }));                                                    // CLOCK_SELECTOR

        append (b, desc (0x24, { 2, 0x10, 0x01, 0x01, 0, 0x28, 10, 0, 0, 0, 0, 0, 0, 0, 0 }));                                  // INPUT_TERMINAL, USB streaming (playback)
        append (b, desc (0x24, { 3, 0x22, 0x01, 0x01, 0, 0x30, clockForRecording, 0, 0, 0 }));                                  // OUTPUT_TERMINAL, USB streaming (recording)
        return b;
    }

    inline Bytes streamV2 (int number, const std::vector<Format2>& formats, int terminalLink, int endpoint, int attributes,
                           int feedbackEndpoint = 0)
    {
        Bytes b;
        append (b, interfaceWith (number, 0, 0, 1, 2, 0x20));

        for (const auto& f : formats)
        {
            append (b, interfaceWith (number, f.alt, feedbackEndpoint != 0 ? 2 : 1, 1, 2, 0x20));
            append (b, desc (0x24, { 1, terminalLink, 0, f.formatType, f.formats & 0xFF, 0, 0, 0, f.channels, 0, 0, 0, 0, 0 })); // AS_GENERAL
            append (b, desc (0x24, { 2, f.formatType, f.subslot, f.bits }));                                                    // FORMAT_TYPE
            Bytes e;
            add (e, { 7, 5, endpoint, attributes, f.packet & 0xFF, f.packet >> 8, f.interval });
            append (b, e);
            append (b, desc (0x25, { 1, 0, 0, 0, 0, 0 }));                                                                     // EP_GENERAL

            if (feedbackEndpoint != 0)
            {
                Bytes fb;
                add (fb, { 7, 5, feedbackEndpoint, 0x11, 4, 0, 4 });                                                           // isochronous, feedback
                append (b, fb);
            }
        }

        return b;
    }

    // A Model-12-like mixer: playback with a feedback endpoint (interface 1), recording of 12
    // channels (interface 2).
    inline Bytes mixer (bool withSelector = false, int clockForRecording = 0x28, int vendor = 0x0644, int product = 0x805f,
                        int recordPacket = 360, int recordInterval = 1)
    {
        Bytes body = controlV2 (withSelector, clockForRecording);
        append (body, streamV2 (1, { { 1, 10, 3, 24, 1, 1, 300, 1 } }, 0x10, 0x01 /* out */, 0x05, 0x81));
        append (body, streamV2 (2, { { 1, 12, 3, 24, 1, 1, recordPacket, recordInterval } }, 0x22, 0x82, 0x05));
        Bytes all = usbfixtures::deviceDescriptor (vendor, product);
        append (all, usbfixtures::configurationDescriptor (1, body, 3));
        return all;
    }

    // A speaker: playback only.
    inline Bytes speaker()
    {
        Bytes body = controlV1 (0x0301, 2, { 1 });
        Bytes out = streamV1 (1, { { 1, 2, 2, 16, { 48000 }, 0, 0, 1, 1, 196, false } }, 0x01, 0x09);
        append (body, out);
        Bytes all = usbfixtures::deviceDescriptor (0x0d8c, 0x000c);
        append (all, usbfixtures::configurationDescriptor (1, body, 2));
        return all;
    }
}
