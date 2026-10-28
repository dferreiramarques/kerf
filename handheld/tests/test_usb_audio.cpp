// Tests for the parts of the direct USB audio input that need no operating system: reading the
// audio descriptors (Audio Class 1 and 2), choosing a format and a rate, the class requests, the
// decoding of samples, and the search in a copy of /sys/bus/usb/devices.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "minitest.h"
#include "usb_audio.h"
#include "usb_audio_fixtures.h"
#include "usb_fixtures.h"

using namespace kerf::ui::usbaudio;
using namespace usbaudiofixtures;
using usbfixtures::FakeSysfs;

namespace
{
    Parsed parse (const Bytes& all, int configuration = -1)
    {
        return parseAudio (all.data(), all.size(), configuration);
    }

    bool near (float a, float b, float tolerance = 1e-6f) { return std::fabs (a - b) <= tolerance; }

    struct Decoded
    {
        std::vector<float> samples;
        int frames = 0;
        int calls = 0;
    };

    Decoded decode (Kind kind, int channels, int subslot, const Bytes& bytes)
    {
        PcmDecoder decoder;
        decoder.configure (kind, channels, subslot);
        Decoded out;

        decoder.feed (bytes.data(), bytes.size(), [&] (const float* data, int frames)
        {
            out.samples.insert (out.samples.end(), data, data + (size_t) frames * (size_t) channels);
            out.frames += frames;
            ++out.calls;
        });

        return out;
    }

    // The same interface descriptors, but with another protocol (0x30 is Audio Class 3).
    Bytes withProtocol (Bytes all, int protocol)
    {
        size_t at = 18;

        while (at + 2 <= all.size() && all[at] >= 2)
        {
            if (all[at + 1] == 4 && all[at] >= 9 && all[at + 5] == 1 && all[at + 6] == 2)
                all[at + 7] = (uint8_t) protocol;

            at += all[at];
        }

        return all;
    }

    // A tiny deterministic generator, so that a failure can be repeated.
    struct Random
    {
        uint32_t state = 12345;
        uint32_t next() { state = state * 1664525u + 1013904223u; return state >> 8; }
    };
}

// ------------------------------------------------------------------------------ descriptors

KTEST (UsbAudio, a_wireless_receiver_is_read_from_its_descriptors)
{
    const auto parsed = parse (receiver());

    EXPECT (parsed.configuration == 1);
    EXPECT (parsed.streams.size() == 1);

    if (parsed.streams.size() != 1)
        return;

    const Stream& s = parsed.streams[0];
    EXPECT (s.interfaceNumber == 1);
    EXPECT (s.version == 1);
    EXPECT (s.capture);
    EXPECT (s.terminalLink == 3);
    EXPECT (s.controlInterface == 0);
    EXPECT (s.formats.size() == 2);

    if (s.formats.size() != 2)
        return;

    const Format& mono = s.formats[0];
    EXPECT (mono.alt == 1);
    EXPECT (mono.kind == Kind::pcm);
    EXPECT (mono.channels == 1 && mono.subslot == 2 && mono.bits == 16);
    EXPECT (mono.rates == std::vector<int> ({ 44100, 48000 }));
    EXPECT (mono.endpoint == 0x81);
    EXPECT (mono.isInput());
    EXPECT (mono.packet == 100 && mono.interval == 1);
    EXPECT (mono.rateControl);
    EXPECT (mono.usable());
    EXPECT (mono.frameBytes() == 2);

    EXPECT (s.formats[1].alt == 2 && s.formats[1].channels == 2 && s.formats[1].packet == 196);
}

KTEST (UsbAudio, the_endpoint_does_not_take_a_rate_unless_it_says_so)
{
    const auto parsed = parse (receiver (0x1234, 0x5678, { { 1, 1, 2, 16, { 48000 }, 0, 0, 1, 1, 100, false } }));
    EXPECT (parsed.streams.size() == 1 && parsed.streams[0].formats.size() == 1);
    EXPECT (! parsed.streams[0].formats[0].rateControl);
}

KTEST (UsbAudio, formats_are_tried_with_the_most_channels_first_then_the_deepest)
{
    const auto parsed = parse (receiver (0x1234, 0x5678,
                                         { { 1, 1, 2, 16, { 48000 }, 0, 0, 1, 1, 100, true },
                                           { 2, 2, 2, 16, { 48000 }, 0, 0, 1, 1, 196, true },
                                           { 3, 2, 3, 24, { 48000 }, 0, 0, 1, 1, 300, true },
                                           { 4, 2, 4, 32, { 48000 }, 0, 0, 1, 1, 400, true } }));
    EXPECT (parsed.streams.size() == 1);

    if (parsed.streams.empty())
        return;

    const auto order = rankFormats (parsed.streams[0]);
    EXPECT (order.size() == 4);

    if (order.size() == 4)
    {
        EXPECT (parsed.streams[0].formats[(size_t) order[0]].alt == 3);     // 2 channels, 24 bit
        EXPECT (parsed.streams[0].formats[(size_t) order[1]].alt == 2);     // 2 channels, 16 bit
        EXPECT (parsed.streams[0].formats[(size_t) order[2]].alt == 4);     // 2 channels, 32 bit
        EXPECT (parsed.streams[0].formats[(size_t) order[3]].alt == 1);     // 1 channel
    }
}

KTEST (UsbAudio, a_mixer_has_a_recording_stream_apart_from_the_playback_one_with_its_feedback_endpoint)
{
    const auto parsed = parse (mixer());
    EXPECT (parsed.streams.size() == 2);

    if (parsed.streams.size() != 2)
        return;

    const Stream& playback = parsed.streams[0];
    EXPECT (playback.interfaceNumber == 1);
    EXPECT (! playback.capture);
    EXPECT (playback.version == 2);
    EXPECT (playback.formats.size() == 1 && playback.formats[0].endpoint == 0x01);   // the data endpoint, not the 0x81 feedback one
    EXPECT (playback.formats[0].channels == 10);
    EXPECT (rankFormats (playback).size() == 1);

    const Stream& recording = parsed.streams[1];
    EXPECT (recording.interfaceNumber == 2);
    EXPECT (recording.capture);
    EXPECT (recording.version == 2);
    EXPECT (recording.terminalLink == 0x22);
    EXPECT (recording.controlInterface == 0);
    EXPECT (recording.clockRoot == 0x28);
    EXPECT (recording.formats.size() == 1);

    if (recording.formats.size() == 1)
    {
        const Format& f = recording.formats[0];
        EXPECT (f.channels == 12 && f.subslot == 3 && f.bits == 24 && f.kind == Kind::pcm);
        EXPECT (f.endpoint == 0x82 && f.packet == 360 && f.interval == 1);
        EXPECT (f.rates.empty() && f.rateMax == 0);                      // the clock says which rates there are
        EXPECT (! f.rateControl);
        EXPECT (f.usable());
    }

    EXPECT (recording.clocks.size() == 2);
    EXPECT (findClock (recording, 0x28) != nullptr && findClock (recording, 0x28)->controls == 0x07);
    EXPECT (findClock (recording, 0x2A) != nullptr && findClock (recording, 0x2A)->controls == 0x05);
    EXPECT (findClock (recording, 0x99) == nullptr);
}

KTEST (UsbAudio, a_feedback_endpoint_is_never_taken_for_the_data_endpoint)
{
    // The feedback endpoint comes FIRST in this one.
    Bytes body = controlV2 (false);
    Bytes stream;
    append (stream, interfaceWith (1, 0, 0, 1, 2, 0x20));
    append (stream, interfaceWith (1, 1, 2, 1, 2, 0x20));
    append (stream, desc (0x24, { 1, 0x22, 0, 1, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0 }));
    append (stream, desc (0x24, { 2, 1, 3, 24 }));
    Bytes feedback, data;
    add (feedback, { 7, 5, 0x81, 0x11, 4, 0, 4 });
    add (data, { 7, 5, 0x82, 0x05, 0x2c, 0x01, 1 });
    append (stream, feedback);
    append (stream, data);
    append (body, stream);

    Bytes all = usbfixtures::deviceDescriptor (1, 2);
    append (all, usbfixtures::configurationDescriptor (1, body, 2));

    const auto parsed = parse (all);
    EXPECT (parsed.streams.size() == 1);

    if (! parsed.streams.empty() && ! parsed.streams[0].formats.empty())
        EXPECT (parsed.streams[0].formats[0].endpoint == 0x82);
}

KTEST (UsbAudio, an_endpoint_that_is_its_own_feedback_is_a_data_endpoint)
{
    // Usage "implicit feedback data endpoint" (bits 5..4 = 2): some mixers send their recording this way.
    Bytes body = controlV2 (false);
    Bytes stream;
    append (stream, interfaceWith (2, 0, 0, 1, 2, 0x20));
    append (stream, interfaceWith (2, 1, 1, 1, 2, 0x20));
    append (stream, desc (0x24, { 1, 0x22, 0, 1, 1, 0, 0, 0, 2, 0, 0, 0, 0, 0 }));
    append (stream, desc (0x24, { 2, 1, 3, 24 }));
    Bytes data;
    add (data, { 7, 5, 0x82, 0x25, 0x2c, 0x01, 1 });
    append (stream, data);
    append (body, stream);

    Bytes all = usbfixtures::deviceDescriptor (1, 2);
    append (all, usbfixtures::configurationDescriptor (1, body, 2));

    const auto parsed = parse (all);
    EXPECT (parsed.streams.size() == 1 && parsed.streams[0].capture);
    EXPECT (! parsed.streams.empty() && ! parsed.streams[0].formats.empty() && parsed.streams[0].formats[0].endpoint == 0x82);
}

KTEST (UsbAudio, high_bandwidth_endpoints_count_every_transaction)
{
    EXPECT (effectivePacket (0x0064) == 100);
    EXPECT (effectivePacket (0x0400) == 1024);
    EXPECT (effectivePacket (0x0C00) == 2048);      // bits 12..11 = 1: two transactions per microframe
    EXPECT (effectivePacket (0x1400) == 3072);      // = 2: three
    EXPECT (effectivePacket (0x1C00) == 3072);      // = 3 is reserved: no more than three
    EXPECT (effectivePacket (0xE064) == 100);       // bits 15..13 are not part of the size
    EXPECT (effectivePacket (0xF864) == 100 * 3);   // bits 12..11 = 3 together with those
}

KTEST (UsbAudio, formats_that_cannot_be_decoded_are_marked_and_not_offered)
{
    const auto tryFormat = [] (Format1 f)
    {
        const auto parsed = parse (receiver (1, 2, { f }));
        return parsed.streams.empty() || parsed.streams[0].formats.empty() ? Format() : parsed.streams[0].formats[0];
    };

    Format1 good;
    EXPECT (tryFormat (good).note.empty());

    Format1 mpeg = good; mpeg.tag = 0x1001;
    EXPECT (tryFormat (mpeg).note == "not uncompressed PCM");

    Format1 typeTwo = good; typeTwo.formatType = 2;
    EXPECT (tryFormat (typeTwo).note == "not uncompressed PCM");

    Format1 fiveBytes = good; fiveBytes.subslot = 5; fiveBytes.bits = 32;
    EXPECT (tryFormat (fiveBytes).note == "unsupported sample size");

    Format1 tooManyBits = good; tooManyBits.bits = 24;
    EXPECT (tryFormat (tooManyBits).note == "bad bit resolution");

    Format1 halfFloat = good; halfFloat.tag = 3;
    EXPECT (tryFormat (halfFloat).note == "unsupported floating point size");

    Format1 noChannels = good; noChannels.channels = 0;
    EXPECT (tryFormat (noChannels).note == "bad channel count");

    Format1 noPacket = good; noPacket.packet = 0;
    EXPECT (tryFormat (noPacket).note == "no packet size");

    Format1 floating = good; floating.tag = 3; floating.subslot = 4; floating.bits = 32;
    EXPECT (tryFormat (floating).note.empty() && tryFormat (floating).kind == Kind::floating);

    Format1 unsignedEight = good; unsignedEight.tag = 2; unsignedEight.subslot = 1; unsignedEight.bits = 8;
    EXPECT (tryFormat (unsignedEight).note.empty() && tryFormat (unsignedEight).kind == Kind::pcm8);

    // A stream with nothing usable offers nothing.
    const auto parsed = parse (receiver (1, 2, { mpeg }));
    EXPECT (parsed.streams.size() == 1 && rankFormats (parsed.streams[0]).empty());
}

KTEST (UsbAudio, audio_class_3_is_marked_as_not_supported)
{
    const auto parsed = parse (withProtocol (receiver(), 0x30));
    EXPECT (parsed.streams.size() == 1 && ! parsed.streams[0].formats.empty());

    if (! parsed.streams.empty() && ! parsed.streams[0].formats.empty())
    {
        EXPECT (parsed.streams[0].version == 3);
        EXPECT (parsed.streams[0].formats[0].note == "USB audio class 3 is not supported");
        EXPECT (rankFormats (parsed.streams[0]).empty());
    }
}

KTEST (UsbAudio, a_stream_without_a_format_descriptor_is_marked)
{
    // Interface and endpoint, nothing class specific in between.
    Bytes body = controlV1 (0x0201, 1, { 1 });
    append (body, interfaceWith (1, 0, 0, 1, 2, 0));
    append (body, interfaceWith (1, 1, 1, 1, 2, 0));
    append (body, audioEndpoint (0x81, 0x05, 100));

    Bytes all = usbfixtures::deviceDescriptor (1, 2);
    append (all, usbfixtures::configurationDescriptor (1, body, 2));
    const auto parsed = parse (all);
    EXPECT (parsed.streams.size() == 1);
    EXPECT (! parsed.streams.empty() && ! parsed.streams[0].formats.empty() && parsed.streams[0].formats[0].note == "no format descriptor");
}

KTEST (UsbAudio, the_active_configuration_is_the_one_used)
{
    Bytes first;
    append (first, usbfixtures::interfaceDescriptor (0, 0, 1, 3, 1));          // a mouse, no audio
    Bytes second = controlV1 (0x0201, 1, { 1 });
    append (second, streamV1 (1, { Format1() }));

    Bytes all = usbfixtures::deviceDescriptor (1, 2, 2);
    append (all, usbfixtures::configurationDescriptor (1, first, 1));
    append (all, usbfixtures::configurationDescriptor (2, second, 2));

    EXPECT (parse (all, 1).streams.empty());
    EXPECT (parse (all, 1).configuration == 1);
    EXPECT (parse (all, 2).streams.size() == 1);
    EXPECT (parse (all, 2).configuration == 2);
    EXPECT (parse (all, -1).streams.empty());             // none asked for: the first
    EXPECT (parse (all, 9).configuration == 1);           // one that does not exist: the first
}

KTEST (UsbAudio, a_device_with_no_audio_gives_nothing)
{
    EXPECT (parse (usbfixtures::mouse()).streams.empty());
    EXPECT (parse (usbfixtures::keyboard()).streams.empty());      // its streaming interface is MIDI (subclass 3)
    EXPECT (parse (Bytes()).streams.empty());
    EXPECT (! parse (Bytes()).note.empty());
}

KTEST (UsbAudio, descriptors_that_are_cut_or_wrong_are_survived)
{
    const Bytes blobs[] = { receiver(), mixer(), mixer (true, 0x29), speaker(), usbfixtures::keyboard() };
    int checked = 0;

    for (const auto& blob : blobs)
    {
        // Cut anywhere.
        for (size_t n = 0; n <= blob.size(); ++n)
        {
            const auto parsed = parseAudio (blob.data(), n, -1);

            for (const auto& s : parsed.streams)
                for (const auto& f : s.formats)
                {
                    EXPECT (f.channels >= 0 && f.channels <= 255);
                    EXPECT (f.packet >= 0);
                    ++checked;
                }
        }

        // Bytes changed at random.
        Random random;

        for (int round = 0; round < 400; ++round)
        {
            Bytes mutated = blob;

            for (int k = 0; k < 1 + (int) (random.next() % 6); ++k)
                mutated[random.next() % mutated.size()] = (uint8_t) random.next();

            const auto parsed = parseAudio (mutated.data(), mutated.size(), (int) (random.next() % 3));

            for (const auto& s : parsed.streams)
            {
                const auto order = rankFormats (s);

                for (const int i : order)
                {
                    const Format& f = s.formats[(size_t) i];
                    EXPECT (f.usable() && f.channels >= 1 && f.channels <= 64 && f.subslot >= 1 && f.subslot <= 4);
                    EXPECT (f.packet >= 1 && f.bits >= 1 && f.bits <= f.subslot * 8);
                    ++checked;
                }

                (void) resolveClock (s);
                (void) describe (s);
            }
        }
    }

    EXPECT (checked > 1000);
}

// ----------------------------------------------------------------------------- what to use

KTEST (UsbAudio, a_rate_needs_room_in_the_packets)
{
    Format f;                       // stereo, 16 bit: 4 bytes a frame; 100 bytes a millisecond
    f.channels = 2; f.subslot = 2; f.bits = 16; f.packet = 100; f.interval = 1;

    EXPECT (carries (f, 24000, 12));            // 24 frames: 96 bytes
    EXPECT (carries (f, 22050, 12));            // 23 frames: 92 bytes
    EXPECT (! carries (f, 44100, 12));          // 45 frames: 180 bytes
    EXPECT (! carries (f, 48000, 12));
    EXPECT (! carries (f, 0, 12));

    // 44.1 kHz needs 45 frames in some of the milliseconds: room for 44 is not enough.
    f.packet = 178;                                 // 44 frames of 4 bytes and a little more
    EXPECT (! carries (f, 44100, 12));
    EXPECT (carries (f, 44000, 12));
    f.packet = 180;
    EXPECT (carries (f, 44100, 12));
    f.packet = 100;

    f.rates = { 8000, 16000, 32000, 48000 };
    EXPECT (rateOptions (f, 12) == std::vector<int> ({ 8000, 16000 }));
    EXPECT (pickRate (rateOptions (f, 12)) == 16000);

    f.rates = { 32000, 48000 };                 // none fits: all are offered, to try
    EXPECT (rateOptions (f, 12) == std::vector<int> ({ 32000, 48000 }));
    EXPECT (pickRate (rateOptions (f, 12)) == 48000);
}

KTEST (UsbAudio, a_range_of_rates_gives_the_usual_rates_inside_it)
{
    Format f;
    f.channels = 1; f.subslot = 2; f.bits = 16; f.packet = 200; f.interval = 1;
    f.rateMin = 8000; f.rateMax = 48000;
    EXPECT (rateOptions (f, 12) == std::vector<int> ({ 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000 }));
    EXPECT (pickRate (rateOptions (f, 12)) == 48000);

    f.rateMin = 44100; f.rateMax = 44100;
    EXPECT (rateOptions (f, 12) == std::vector<int> ({ 44100 }));

    f.rateMin = 12345; f.rateMax = 12345;       // not a usual rate, but the only one
    EXPECT (rateOptions (f, 12) == std::vector<int> ({ 12345 }));

    f.rateMin = 0; f.rateMax = 0;               // nothing said at all (version 2)
    EXPECT (rateOptions (f, 12).empty());
}

KTEST (UsbAudio, high_speed_has_eight_bus_intervals_in_a_millisecond)
{
    const auto parsed = parse (mixer());
    EXPECT (parsed.streams.size() == 2);

    if (parsed.streams.size() != 2)
        return;

    const Format& f = parsed.streams[1].formats[0];          // 12 channels of 3 bytes = 36 bytes a frame, packet 360
    EXPECT (busIntervalsPerSecond (f, 480) == 8000.0);
    EXPECT (busIntervalsPerSecond (f, 12) == 1000.0);
    EXPECT (carries (f, 48000, 480));                        // 6 frames per microframe: 216 bytes
    EXPECT (carries (f, 44100, 480));                        // 6 frames (5.5 rounded up)
    EXPECT (! carries (f, 96000, 480));                      // 12 frames: 432 bytes
    EXPECT (! carries (f, 48000, 12));                       // the same endpoint at full speed would need 1728 bytes

    Format slow = f;
    slow.interval = 4;                                       // every eighth microframe: 1000 a second
    EXPECT (busIntervalsPerSecond (slow, 480) == 1000.0);

    Format big = f;
    big.packet = 1024;
    EXPECT (busIntervalsPerSecond (big, 0) == 8000.0);       // speed unknown: a packet that large can only be high speed
    EXPECT (busIntervalsPerSecond (f, 0) == 1000.0);
}

KTEST (UsbAudio, the_rate_is_48000_if_there_is_one_then_44100_then_what_is_nearest_to_everyday_use)
{
    EXPECT (pickRate ({ 44100, 48000 }) == 48000);
    EXPECT (pickRate ({ 8000, 44100 }) == 44100);
    EXPECT (pickRate ({ 96000, 192000 }) == 96000);
    EXPECT (pickRate ({ 88200, 176400 }) == 88200);
    EXPECT (pickRate ({ 44100, 96000 }) == 44100);
    EXPECT (pickRate ({ 32000, 8000 }) == 32000);
    EXPECT (pickRate ({ 12000, 24000 }) == 24000);
    EXPECT (pickRate ({ 11025 }) == 11025);
    EXPECT (pickRate ({ 8000, 16000, 24000 }) == 24000);         // the best of the lower ones, not the first in a list
    EXPECT (pickRate ({ 96000, 22050, 16000 }) == 22050);        // lower ones are preferred to higher ones
    EXPECT (pickRate ({ 192000, 96000, 88200 }) == 88200);
    EXPECT (pickRate ({ 0, -1, 16000 }) == 16000);               // nonsense is not picked
    EXPECT (pickRate ({}) == 0);
}

// ---------------------------------------------------------------------------------- clocks

KTEST (UsbAudio, the_clock_behind_a_stream_is_found_through_selectors_and_multipliers)
{
    const auto plain = parse (mixer());
    EXPECT (plain.streams.size() == 2);

    if (plain.streams.size() != 2)
        return;

    EXPECT (resolveClock (plain.streams[1]) == 0x28);

    // The recording terminal is on a selector that chooses between the clocks 0x28 and 0x2A.
    const auto chosen = parse (mixer (true, 0x29));
    EXPECT (chosen.streams.size() == 2 && chosen.streams[1].clockRoot == 0x29);

    if (chosen.streams.size() != 2)
        return;

    const Stream& s = chosen.streams[1];
    EXPECT (resolveClock (s) == 0x28);                                          // not asked: the first input
    EXPECT (resolveClock (s, [] (int) { return 2; }) == 0x2A);                  // the device says input 2
    EXPECT (resolveClock (s, [] (int) { return 1; }) == 0x28);
    EXPECT (resolveClock (s, [] (int) { return 0; }) == 0x28);                  // could not be asked
    EXPECT (resolveClock (s, [] (int) { return 7; }) == 0x28);                  // an answer that makes no sense
    int asked = -1;
    resolveClock (s, [&] (int id) { asked = id; return 1; });
    EXPECT (asked == 0x29);

    Stream chain;                                                               // a multiplier in front of a source
    chain.clockRoot = 5;
    chain.clocks = { { 5, 0x0C, 0, { 6 } }, { 6, 0x0A, 3, {} } };
    EXPECT (resolveClock (chain) == 6);

    Stream loop;                                                                // a selector that chooses itself
    loop.clockRoot = 1;
    loop.clocks = { { 1, 0x0B, 0, { 1 } } };
    EXPECT (resolveClock (loop) == 0);
    int questions = 0;
    resolveClock (loop, [&] (int) { ++questions; return 1; });
    EXPECT (questions > 0 && questions <= 8);                                   // a chain that goes round is given up after a few steps

    Stream missing;                                                             // a clock that is not there
    missing.clockRoot = 3;
    missing.clocks = { { 1, 0x0A, 3, {} } };
    EXPECT (resolveClock (missing) == 0);

    Stream none;
    EXPECT (resolveClock (none) == 0);
}

KTEST (UsbAudio, the_class_requests_have_the_values_of_the_specification)
{
    // Version 1: SET_CUR / GET_CUR of the sampling frequency control, on the endpoint, three bytes.
    const auto set1 = rateRequestV1 (true, 0x81);
    EXPECT (set1.type == 0x22 && set1.request == 0x01 && set1.value == 0x0100 && set1.index == 0x0081 && set1.length == 3);
    const auto get1 = rateRequestV1 (false, 0x81);
    EXPECT (get1.type == 0xA2 && get1.request == 0x81 && get1.value == 0x0100 && get1.index == 0x0081 && get1.length == 3);

    // Version 2: CUR of the clock source's sampling frequency control, on the interface, four bytes;
    // the clock is in the high byte of the index and the AudioControl interface in the low one.
    const auto set2 = rateRequestV2 (true, 0x28, 0);
    EXPECT (set2.type == 0x21 && set2.request == 0x01 && set2.value == 0x0100 && set2.index == 0x2800 && set2.length == 4);
    const auto get2 = rateRequestV2 (false, 0x28, 2);
    EXPECT (get2.type == 0xA1 && get2.request == 0x01 && get2.value == 0x0100 && get2.index == 0x2802 && get2.length == 4);

    const auto range = rateRangeRequestV2 (0x28, 0, 3);
    EXPECT (range.type == 0xA1 && range.request == 0x02 && range.value == 0x0100 && range.index == 0x2800 && range.length == 2 + 36);
    EXPECT (rateRangeRequestV2 (0x28, 0, 0).length == 2);
    EXPECT (rateRangeRequestV2 (0x28, 0, 1000).length == 2 + 12 * 32);

    const auto selector = selectorRequestV2 (0x29, 0);
    EXPECT (selector.type == 0xA1 && selector.request == 0x01 && selector.value == 0x0100 && selector.index == 0x2900 && selector.length == 1);

    uint8_t three[3], four[4];
    putRate (three, 3, 48000);
    putRate (four, 4, 44100);
    EXPECT (three[0] == 0x80 && three[1] == 0xBB && three[2] == 0x00);
    EXPECT (four[0] == 0x44 && four[1] == 0xAC && four[2] == 0x00 && four[3] == 0x00);
    EXPECT (getRate (three, 3) == 48000 && getRate (four, 4) == 44100);
}

KTEST (UsbAudio, the_answer_to_a_range_request_is_read_and_turned_into_rates)
{
    Bytes answer;
    const auto sub = [&] (uint32_t low, uint32_t high, uint32_t step)
    {
        uint8_t block[12];
        putRate (block, 4, low);
        putRate (block + 4, 4, high);
        putRate (block + 8, 4, step);
        answer.insert (answer.end(), block, block + 12);
    };

    add (answer, { 4, 0 });
    sub (44100, 44100, 0);
    sub (8000, 96000, 8000);            // 8000, 16000 ... 96000: not 44100, which is not on the steps
    sub (12345, 12345, 0);
    sub (200000, 200000, 0);            // outside the usual rates but fixed

    const auto ranges = parseRanges (answer.data(), answer.size());
    EXPECT (ranges.size() == 4);
    EXPECT (ranges[1].low == 8000 && ranges[1].high == 96000 && ranges[1].step == 8000);

    EXPECT (rangeRates (ranges) == std::vector<int> ({ 8000, 12345, 16000, 24000, 32000, 44100, 48000, 96000, 200000 }));

    // The count says 3 but the device sent one block: only that one is believed.
    Bytes cut = { 3, 0 };
    cut.insert (cut.end(), answer.begin() + 2, answer.begin() + 2 + 12);
    cut.insert (cut.end(), answer.begin() + 14, answer.begin() + 14 + 5);
    EXPECT (parseRanges (cut.data(), cut.size()).size() == 1);

    EXPECT (parseRanges (answer.data(), 1).empty());
    EXPECT (parseRanges (nullptr, 0).empty());
    EXPECT (rangeRates ({}).empty());
}

// ------------------------------------------------------------------------------ the samples

KTEST (UsbAudio, sixteen_bit_samples_are_decoded)
{
    const auto d = decode (Kind::pcm, 2, 2, { 0xFF, 0x7F, 0x00, 0x80, 0x00, 0x00, 0x01, 0x00 });
    EXPECT (d.frames == 2 && d.samples.size() == 4);

    if (d.samples.size() == 4)
    {
        EXPECT (near (d.samples[0], 32767.0f / 32768.0f));
        EXPECT (d.samples[1] == -1.0f);
        EXPECT (d.samples[2] == 0.0f);
        EXPECT (near (d.samples[3], 1.0f / 32768.0f));
    }
}

KTEST (UsbAudio, twenty_four_bit_samples_are_decoded)
{
    const auto d = decode (Kind::pcm, 1, 3, { 0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0x01, 0x00, 0x00, 0xFF, 0xFF, 0xFF });
    EXPECT (d.frames == 4 && d.samples.size() == 4);

    if (d.samples.size() == 4)
    {
        EXPECT (near (d.samples[0], 8388607.0f / 8388608.0f));
        EXPECT (d.samples[1] == -1.0f);
        EXPECT (near (d.samples[2], 1.0f / 8388608.0f));
        EXPECT (near (d.samples[3], -1.0f / 8388608.0f));
    }
}

KTEST (UsbAudio, thirty_two_bit_and_floating_point_samples_are_decoded)
{
    const auto big = decode (Kind::pcm, 1, 4, { 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x40, 0xFF, 0xFF, 0xFF, 0x7F });
    EXPECT (big.frames == 3 && big.samples.size() == 3);

    if (big.samples.size() == 3)
    {
        EXPECT (big.samples[0] == -1.0f);
        EXPECT (near (big.samples[1], 0.5f));
        EXPECT (big.samples[2] <= 1.0f && big.samples[2] > 0.999f);
    }

    // 24 bit sound in a 4 byte slot sits in the high bits: the lowest byte is 0 and does not matter.
    const auto slot = decode (Kind::pcm, 1, 4, { 0x00, 0xFF, 0xFF, 0x7F });
    EXPECT (slot.samples.size() == 1 && near (slot.samples[0], 8388607.0f / 8388608.0f));

    float values[4] = { 0.25f, -0.5f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity() };
    Bytes bytes (sizeof values);
    std::memcpy (bytes.data(), values, sizeof values);
    const auto f = decode (Kind::floating, 2, 4, bytes);
    EXPECT (f.frames == 2 && f.samples.size() == 4);

    if (f.samples.size() == 4)
    {
        EXPECT (f.samples[0] == 0.25f && f.samples[1] == -0.5f);
        EXPECT (f.samples[2] == 0.0f && f.samples[3] == 0.0f);           // a number that is not a sample becomes silence
    }
}

KTEST (UsbAudio, eight_bit_samples_are_unsigned_in_pcm8_and_signed_in_pcm)
{
    const auto u = decode (Kind::pcm8, 1, 1, { 0x80, 0xFF, 0x00 });
    EXPECT (u.samples.size() == 3);

    if (u.samples.size() == 3)
        EXPECT (u.samples[0] == 0.0f && near (u.samples[1], 127.0f / 128.0f) && u.samples[2] == -1.0f);

    const auto s = decode (Kind::pcm, 1, 1, { 0x80, 0x7F, 0x00 });
    EXPECT (s.samples.size() == 3);

    if (s.samples.size() == 3)
        EXPECT (s.samples[0] == -1.0f && near (s.samples[1], 127.0f / 128.0f) && s.samples[2] == 0.0f);

    // A float or an 8 bit format ignores a wrong sample size in the call: it is what the format says.
    PcmDecoder fixed;
    fixed.configure (Kind::floating, 1, 2);
    EXPECT (fixed.frameBytes() == 4);
    fixed.configure (Kind::pcm8, 2, 3);
    EXPECT (fixed.frameBytes() == 2);
}

KTEST (UsbAudio, a_frame_cut_between_two_packets_is_put_together)
{
    // Stereo, 16 bit: a frame is 4 bytes. The samples are 1, 2, 3, 4, 5, 6 (as 16 bit numbers).
    const Bytes all = { 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0 };

    PcmDecoder decoder;
    decoder.configure (Kind::pcm, 2, 2);
    std::vector<float> out;
    int calls = 0;
    const auto sink = [&] (const float* data, int frames) { out.insert (out.end(), data, data + frames * 2); ++calls; };

    decoder.feed (all.data(), 6, sink);                 // one frame and half of the next
    EXPECT (out.size() == 2 && calls == 1);
    decoder.feed (all.data() + 6, 6, sink);             // the rest of that frame, then one more frame, then half a frame
    EXPECT (out.size() == 6 && calls == 2);
    decoder.feed (all.data() + 12 - 0, 0, sink);        // nothing: nothing is delivered
    EXPECT (calls == 2);

    for (size_t i = 0; i < out.size(); ++i)
        EXPECT (near (out[i] * 32768.0f, (float) (i + 1)));

    // The same bytes one at a time.
    PcmDecoder slow;
    slow.configure (Kind::pcm, 2, 2);
    std::vector<float> one;

    for (const uint8_t byte : all)
        slow.feed (&byte, 1, [&] (const float* data, int frames) { one.insert (one.end(), data, data + frames * 2); });

    EXPECT (one == out);

    // reset() drops the half frame that is waiting.
    PcmDecoder cut;
    cut.configure (Kind::pcm, 2, 2);
    std::vector<float> after;
    const auto keep = [&] (const float* data, int frames) { after.insert (after.end(), data, data + frames * 2); };
    cut.feed (all.data(), 3, keep);
    cut.reset();
    cut.feed (all.data(), 4, keep);
    EXPECT (after.size() == 2 && near (after[0] * 32768.0f, 1.0f) && near (after[1] * 32768.0f, 2.0f));
}

KTEST (UsbAudio, the_samples_do_not_depend_on_how_the_bytes_are_cut_into_packets)
{
    Random random;

    for (const Kind kind : { Kind::pcm, Kind::pcm8, Kind::floating })
    {
        for (int channels : { 1, 2, 3, 12 })
        {
            for (int subslot : { 1, 2, 3, 4 })
            {
                if (kind == Kind::floating && subslot != 4)
                    continue;

                if (kind == Kind::pcm8 && subslot != 1)
                    continue;

                const int frameBytes = channels * subslot;
                Bytes bytes ((size_t) frameBytes * 50);

                for (auto& b : bytes)
                    b = (uint8_t) random.next();

                if (kind == Kind::floating)
                    for (size_t i = 0; i + 3 < bytes.size(); i += 4)
                        bytes[i + 3] &= 0x3F;             // keep the exponent small: finite numbers

                const auto whole = decode (kind, channels, subslot, bytes);
                EXPECT (whole.frames == 50);

                PcmDecoder chopped;
                chopped.configure (kind, channels, subslot);
                std::vector<float> samples;

                for (size_t at = 0; at < bytes.size();)
                {
                    const size_t n = std::min<size_t> (1 + random.next() % 37, bytes.size() - at);
                    chopped.feed (bytes.data() + at, n, [&] (const float* data, int frames)
                    {
                        samples.insert (samples.end(), data, data + (size_t) frames * (size_t) channels);
                    });
                    at += n;
                }

                EXPECT (samples == whole.samples);
            }
        }
    }
}

// ----------------------------------------------------------------------- the log, the search

KTEST (UsbAudio, the_log_says_what_a_stream_offers)
{
    const auto old = parse (receiver());
    EXPECT (old.streams.size() == 1);

    if (old.streams.empty())
        return;

    const std::string a = describe (old.streams[0]);
    EXPECT (a.find ("recording interface 1 (audio class 1)") != std::string::npos);
    EXPECT (a.find ("alt 1: 1 ch, 2 B x 16 bit pcm, rates 44100 48000, ep 0x81 packet 100 interval 1, rate control on the endpoint") != std::string::npos);
    EXPECT (a.find ("alt 2: 2 ch") != std::string::npos);

    const auto mix = parse (mixer());
    EXPECT (mix.streams.size() == 2);

    if (mix.streams.size() != 2)
        return;

    const std::string b = describe (mix.streams[1]);
    EXPECT (b.find ("recording interface 2 (audio class 2)") != std::string::npos);
    EXPECT (b.find ("12 ch, 3 B x 24 bit pcm, rate set by the clock, ep 0x82 packet 360") != std::string::npos);
    EXPECT (describe (mix.streams[0]).find ("playback interface 1") != std::string::npos);

    Format1 mpeg; mpeg.tag = 0x1001;
    const auto odd = parse (receiver (1, 2, { mpeg }));
    EXPECT (! odd.streams.empty() && describe (odd.streams[0]).find ("(cannot use: not uncompressed PCM)") != std::string::npos);

    Format1 ranged; ranged.rates.clear(); ranged.rateLow = 8000; ranged.rateHigh = 48000;
    const auto range = parse (receiver (1, 2, { ranged }));
    EXPECT (! range.streams.empty() && describe (range.streams[0]).find ("rates 8000-48000") != std::string::npos);
}

KTEST (UsbAudio, the_devices_with_a_recording_stream_are_found_in_sysfs)
{
    FakeSysfs sys;
    sys.device ("4-1", receiver(), 1, 4, 2, "Shenzhen", "Wireless Mic Receiver", "1234", "5678");
    sys.device ("1-2", mixer(), 1, 1, 5, "TASCAM", "Model 12", "0644", "805f");
    sys.text (sys.root / "1-2" / "speed", "480\n");
    sys.text (sys.root / "4-1" / "speed", "12\n");
    sys.device ("3-1", usbfixtures::keyboard(), 1, 3, 7, "M-Audio", "Oxygen", "0763", "0123");      // MIDI, not audio streaming
    sys.device ("2-3", usbfixtures::mouse(), 1, 2, 3, "", "Mouse", "046d", "c077");
    sys.device ("2-4", speaker(), 1, 2, 4, "Generic", "USB Speaker", "0d8c", "000c");               // playback only

    const auto found = look (sys.root);
    EXPECT (found.devices.size() == 3);                         // the keyboard and the mouse have no audio streaming
    EXPECT (found.candidates.size() == 2);

    if (found.devices.size() != 3 || found.candidates.size() != 2)
        return;

    EXPECT (found.devices[0].name == "1-2" && found.devices[0].usable);
    EXPECT (found.devices[1].name == "2-4" && ! found.devices[1].usable);
    EXPECT (found.devices[2].name == "4-1" && found.devices[2].usable);
    EXPECT (found.devices[0].speed == 480 && found.devices[0].title == "TASCAM Model 12");
    EXPECT (found.devices[0].summary.find ("recording interface 2") != std::string::npos);
    EXPECT (found.devices[0].summary.find ("playback interface 1") != std::string::npos);
    EXPECT (found.devices[1].summary.find ("playback interface 1") != std::string::npos);
    EXPECT (! found.devices[0].descriptors.empty() && found.devices[0].descriptors.rfind ("09 02", 0) == 0);   // starts at the configuration, in hex

    const Candidate& m = found.candidates[0];
    EXPECT (m.name == "1-2" && m.vendor == 0x0644 && m.product == 0x805f);
    EXPECT (m.bus == 1 && m.device == 5 && m.major == 189 && m.minor == 4);
    EXPECT (m.speed == 480 && m.driver.empty());
    EXPECT (m.stream.interfaceNumber == 2 && m.stream.capture && m.stream.formats[0].channels == 12);
    EXPECT (m.key() == "1:5:2");

    const Candidate& r = found.candidates[1];
    EXPECT (r.name == "4-1" && r.title == "Shenzhen Wireless Mic Receiver" && r.speed == 12);
    EXPECT (r.stream.interfaceNumber == 1 && r.stream.formats.size() == 2);
}

KTEST (UsbAudio, a_driver_that_the_kernel_has_bound_to_the_stream_is_reported)
{
    FakeSysfs sys;
    sys.device ("4-1", receiver(), 1, 4, 2, "", "Receiver", "1234", "5678");

    if (! sys.bind ("4-1:1.1", "snd-usb-audio"))               // the streaming interface
        return;                                                // this file system cannot make symbolic links

    EXPECT (sys.bind ("4-1:1.0", "snd-usb-audio"));            // the control one

    const auto found = look (sys.root);
    EXPECT (found.candidates.size() == 1);
    EXPECT (! found.candidates.empty() && found.candidates[0].driver == "snd-usb-audio");
}

KTEST (UsbAudio, a_device_the_search_cannot_make_sense_of_is_skipped)
{
    FakeSysfs sys;
    sys.device ("4-1", Bytes { 1, 2, 3 }, 1, 4, 2, "", "Broken", "1234", "5678");
    sys.device ("4-2", Bytes(), 1, 4, 3, "", "Empty", "1234", "5679");
    sys.device ("4-3", receiver(), 1, 0, 0, "", "No numbers", "1234", "567a");          // bus and device 0: no way to open it
    sys.device ("4-4", receiver(), 1, 4, 5, "", "Good", "1234", "567b");

    const auto found = look (sys.root);
    EXPECT (found.candidates.size() == 1 && found.candidates[0].name == "4-4");

    EXPECT (look (sys.root / "does-not-exist").candidates.empty());
}

KTEST (UsbAudio, the_active_configuration_in_sysfs_decides_which_streams_are_offered)
{
    Bytes first;
    append (first, usbfixtures::interfaceDescriptor (0, 0, 1, 3, 1));
    Bytes second = controlV1 (0x0201, 1, { 1 });
    append (second, streamV1 (1, { Format1() }));

    Bytes all = usbfixtures::deviceDescriptor (1, 2, 2);
    append (all, usbfixtures::configurationDescriptor (1, first, 1));
    append (all, usbfixtures::configurationDescriptor (2, second, 2));

    FakeSysfs sys;
    sys.device ("4-1", all, 1, 4, 2, "", "Two configurations", "1234", "5678");
    EXPECT (look (sys.root).candidates.empty());

    sys.remove ("4-1");
    sys.device ("4-1", all, 2, 4, 2, "", "Two configurations", "1234", "5678");
    const auto found = look (sys.root);
    EXPECT (found.candidates.size() == 1);
}
