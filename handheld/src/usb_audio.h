#pragma once

// USB audio input without a kernel driver.
//
// The same situation as with MIDI (usb_midi.h): the handheld's kernel has no USB audio driver, so
// a USB microphone, a wireless-microphone receiver or a mixer such as the Tascam Model 12 shows up
// on the USB bus but nothing reads it. Kerf then speaks "USB Audio Class" itself, through usbfs.
//
// This header holds the parts that need no operating system calls, so the tests can run anywhere:
//   - parsing the audio descriptors of a device (Audio Class 1.0 and 2.0),
//   - choosing the alternate setting, the sample rate and the packet layout to use,
//   - the class requests that set and read the sample rate,
//   - turning the bytes of a packet into floating point samples,
//   - looking for such devices in (a copy of) /sys/bus/usb/devices.
// Opening the device and queueing the isochronous transfers is in usb_audio_io.h (Linux only).
//
// What an audio device looks like, in short. An "AudioControl" interface (class 1, subclass 1)
// describes the inside of the device: terminals (a microphone, the USB connection...), and in
// version 2 also clocks. Every direction of sound has an "AudioStreaming" interface (class 1,
// subclass 2) with an alternate setting 0 that has no endpoint (so it takes no bandwidth) and
// further settings that each have an isochronous endpoint and one format: number of channels,
// bytes per sample, sample rates. Recording is the endpoint that points to the host (IN).

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "usb_sysfs.h"

namespace kerf::ui::usbaudio
{
    // ------------------------------------------------------------------------------------ model

    enum class Kind { other, pcm, pcm8, floating };    // pcm8 is the one that is unsigned

    // One alternate setting of an AudioStreaming interface that has an isochronous data endpoint.
    struct Format
    {
        int alt = 0;
        Kind kind = Kind::other;
        int channels = 0;
        int subslot = 0;                // bytes per sample on the bus
        int bits = 0;                   // how many of the bits carry sound
        std::vector<int> rates;         // version 1 with a list of rates
        int rateMin = 0;                // version 1 with a range of rates
        int rateMax = 0;
        uint8_t endpoint = 0;           // the isochronous data endpoint; 0 = none
        uint8_t attributes = 0;
        int packet = 0;                 // the most bytes the endpoint moves in one bus interval
        int interval = 1;               // bInterval: 2^(n-1) frames (full speed) or microframes (high speed)
        bool rateControl = false;       // version 1: the rate is set on the endpoint
        std::string note;               // why the format cannot be used; empty = it can

        int frameBytes() const noexcept { return channels * subslot; }
        bool usable() const noexcept { return note.empty() && endpoint != 0; }
        bool isInput() const noexcept { return (endpoint & 0x80) != 0; }
    };

    // An entity of version 2 that has to do with the clock: a source (0x0A), a selector (0x0B) or a
    // multiplier (0x0C).
    struct ClockNode
    {
        int id = 0;
        int subtype = 0;
        int controls = 0;               // sources: bits 1..0 are the rate control (1 = read only, 3 = can be set)
        std::vector<int> inputs;        // selectors: the clocks it can choose from; multipliers: the one it multiplies
    };

    // One AudioStreaming interface.
    struct Stream
    {
        int interfaceNumber = 0;
        int version = 1;                // 1, 2 (3 is not supported)
        bool capture = true;            // sound goes to the host
        int terminalLink = 0;
        int controlInterface = -1;      // the AudioControl interface it belongs to (class requests of version 2 name it)
        int clockRoot = 0;              // version 2: the clock entity behind the terminal
        std::vector<ClockNode> clocks;  // version 2: every clock entity of that AudioControl interface
        std::vector<Format> formats;    // the alternate settings that stream
    };

    struct Parsed
    {
        int configuration = 0;
        std::vector<Stream> streams;    // recording and playback; Stream::capture says which
        std::string note;               // when nothing could be made of the descriptors
    };

    // ----------------------------------------------------------------------------- descriptors

    // The most bytes an endpoint moves in a bus interval: bits 10..0 of wMaxPacketSize, times the
    // 1 to 3 transactions that bits 12..11 allow in a high speed microframe.
    inline int effectivePacket (unsigned wMaxPacketSize) noexcept
    {
        const unsigned more = std::min (2u, (wMaxPacketSize >> 11) & 3u);
        return (int) ((wMaxPacketSize & 0x7FFu) * (1u + more));
    }

    // Parses what the kernel shows as /sys/bus/usb/devices/<device>/descriptors (the device
    // descriptor and then every configuration) and returns the audio streams of one configuration:
    // `configuration` if there is such a one, else the first. Anything that does not fit is ignored.
    inline Parsed parseAudio (const uint8_t* data, size_t size, int configuration = -1)
    {
        struct Entity { int id = 0; int subtype = 0; int terminalType = 0; int clock = 0; int controls = 0; std::vector<int> inputs; };
        struct Control { int number = 0; int version = 1; std::vector<int> streams; std::vector<Entity> entities; };
        struct Alt
        {
            int number = 0;
            int alt = 0;
            int version = 1;
            int terminalLink = 0;
            bool haveGeneral = false;
            bool haveFormat = false;
            bool formatTypeOne = true;
            Format f;
        };

        Parsed out;
        const auto b = [] (const uint8_t* d, size_t len, size_t i) -> int { return i < len ? d[i] : 0; };
        const auto w = [&] (const uint8_t* d, size_t len, size_t i) -> int { return b (d, len, i) | (b (d, len, i + 1) << 8); };
        const auto t = [&] (const uint8_t* d, size_t len, size_t i) -> int { return w (d, len, i) | (b (d, len, i + 2) << 16); };
        const auto q = [&] (const uint8_t* d, size_t len, size_t i) -> uint32_t { return (uint32_t) t (d, len, i) | ((uint32_t) b (d, len, i + 3) << 24); };

        size_t start = (size >= 18 && data[0] == 18 && data[1] == 1) ? 18 : 0;

        // Where each configuration begins and ends.
        struct Range { int value; size_t from; size_t to; };
        std::vector<Range> ranges;

        for (size_t p = start; p + 2 <= size;)
        {
            const size_t length = data[p];

            if (length < 2 || p + length > size)
                break;

            if (data[p + 1] == 2 && length >= 9)
            {
                if (! ranges.empty())
                    ranges.back().to = p;

                ranges.push_back ({ data[p + 5], p, size });
            }

            p += length;
        }

        if (ranges.empty())
        {
            out.note = "no configuration";
            return out;
        }

        const Range* chosen = &ranges.front();

        for (const auto& r : ranges)
            if (r.value == configuration)
                chosen = &r;

        out.configuration = chosen->value;

        std::vector<Control> controls;
        std::vector<Alt> alts;
        int controlAt = -1, altAt = -1;         // what the interface descriptor above this point opened
        bool lastWasData = false;               // the endpoint descriptor above is the data endpoint of an alt

        for (size_t p = chosen->from; p + 2 <= chosen->to;)
        {
            const uint8_t* d = data + p;
            const size_t length = d[0];

            if (length < 2 || p + length > chosen->to)
                break;

            const int type = d[1];
            p += length;

            if (type == 4)
            {
                controlAt = altAt = -1;
                lastWasData = false;

                if (length < 9 || d[5] != 1)
                    continue;

                const int version = d[7] == 0x20 ? 2 : (d[7] >= 0x30 ? 3 : 1);

                if (d[6] == 1)             // AudioControl
                {
                    for (size_t i = 0; i < controls.size(); ++i)
                        if (controls[i].number == d[2])
                            controlAt = (int) i;

                    if (controlAt < 0)
                    {
                        Control c;
                        c.number = d[2];
                        c.version = version;
                        controls.push_back (std::move (c));
                        controlAt = (int) controls.size() - 1;
                    }
                }
                else if (d[6] == 2)        // AudioStreaming
                {
                    Alt a;
                    a.number = d[2];
                    a.alt = d[3];
                    a.version = version;
                    alts.push_back (std::move (a));
                    altAt = (int) alts.size() - 1;
                }

                continue;
            }

            if (type == 0x24 && length >= 3)
            {
                const int sub = d[2];

                if (controlAt >= 0)
                {
                    Control& c = controls[(size_t) controlAt];
                    Entity e;

                    if (sub == 1 && c.version == 1)                     // HEADER: the AudioStreaming interfaces are listed
                    {
                        for (int k = 0; k < b (d, length, 7); ++k)
                            if (8 + (size_t) k < length)
                                c.streams.push_back (d[8 + (size_t) k]);
                    }
                    else if (sub == 2 || sub == 3)                      // INPUT_TERMINAL, OUTPUT_TERMINAL
                    {
                        e.id = b (d, length, 3);
                        e.subtype = sub;
                        e.terminalType = w (d, length, 4);

                        if (c.version == 2)
                            e.clock = sub == 2 ? b (d, length, 7) : b (d, length, 8);

                        c.entities.push_back (std::move (e));
                    }
                    else if (c.version == 2 && sub == 0x0A)             // CLOCK_SOURCE
                    {
                        e.id = b (d, length, 3);
                        e.subtype = sub;
                        e.controls = b (d, length, 5);
                        c.entities.push_back (std::move (e));
                    }
                    else if (c.version == 2 && sub == 0x0B)             // CLOCK_SELECTOR
                    {
                        e.id = b (d, length, 3);
                        e.subtype = sub;

                        for (int k = 0; k < b (d, length, 4); ++k)
                            if (5 + (size_t) k < length)
                                e.inputs.push_back (d[5 + (size_t) k]);

                        c.entities.push_back (std::move (e));
                    }
                    else if (c.version == 2 && sub == 0x0C)             // CLOCK_MULTIPLIER
                    {
                        e.id = b (d, length, 3);
                        e.subtype = sub;
                        e.inputs.push_back (b (d, length, 4));
                        c.entities.push_back (std::move (e));
                    }
                }
                else if (altAt >= 0)
                {
                    Alt& a = alts[(size_t) altAt];
                    Format& f = a.f;

                    if (sub == 1)                                       // AS_GENERAL
                    {
                        a.haveGeneral = true;
                        a.terminalLink = b (d, length, 3);

                        if (a.version == 1)
                        {
                            const int tag = w (d, length, 5);
                            f.kind = tag == 1 ? Kind::pcm : (tag == 2 ? Kind::pcm8 : (tag == 3 ? Kind::floating : Kind::other));
                        }
                        else
                        {
                            const uint32_t formats = q (d, length, 6);
                            f.channels = b (d, length, 10);
                            f.kind = (formats & 1) ? Kind::pcm : ((formats & 4) ? Kind::floating : ((formats & 2) ? Kind::pcm8 : Kind::other));
                            a.formatTypeOne = b (d, length, 5) == 1;
                        }
                    }
                    else if (sub == 2)                                  // FORMAT_TYPE
                    {
                        a.haveFormat = true;

                        if (b (d, length, 3) != 1)
                        {
                            a.formatTypeOne = false;
                        }
                        else if (a.version == 1)
                        {
                            f.channels = b (d, length, 4);
                            f.subslot = b (d, length, 5);
                            f.bits = b (d, length, 6);
                            const int count = b (d, length, 7);

                            if (count == 0)
                            {
                                f.rateMin = t (d, length, 8);
                                f.rateMax = t (d, length, 11);
                            }
                            else
                            {
                                for (int k = 0; k < count && 8 + 3 * (size_t) k + 3 <= length; ++k)
                                    f.rates.push_back (t (d, length, 8 + 3 * (size_t) k));
                            }
                        }
                        else
                        {
                            f.subslot = b (d, length, 4);
                            f.bits = b (d, length, 5);
                        }
                    }
                }

                continue;
            }

            if (type == 5 && altAt >= 0 && length >= 7)
            {
                lastWasData = false;
                const int attributes = d[3];

                if ((attributes & 3) != 1)      // not isochronous
                    continue;

                const int usage = (attributes >> 4) & 3;    // 0 data, 1 feedback, 2 data that is also the feedback

                if (usage == 1)
                    continue;

                Format& f = alts[(size_t) altAt].f;

                if (f.endpoint == 0)
                {
                    f.endpoint = d[2];
                    f.attributes = (uint8_t) attributes;
                    f.packet = effectivePacket ((unsigned) w (d, length, 4));
                    f.interval = std::max (1, (int) d[6]);
                    lastWasData = true;
                }

                continue;
            }

            if (type == 0x25 && altAt >= 0 && lastWasData && length >= 4 && d[2] == 1)
            {
                // EP_GENERAL: in version 1 bit 0 of the attributes says that the endpoint takes the sample rate.
                if (alts[(size_t) altAt].version == 1)
                    alts[(size_t) altAt].f.rateControl = (d[3] & 1) != 0;

                continue;
            }
        }

        // ---- group the alternate settings by interface

        std::vector<int> order;

        for (const auto& a : alts)
            if (std::find (order.begin(), order.end(), a.number) == order.end())
                order.push_back (a.number);

        for (const int number : order)
        {
            Stream s;
            s.interfaceNumber = number;
            bool haveDirection = false;

            for (Alt& a : alts)
            {
                if (a.number != number)
                    continue;

                s.version = a.version;

                if (a.f.endpoint == 0)          // alternate setting 0, or one without a data endpoint
                    continue;

                const bool input = (a.f.endpoint & 0x80) != 0;

                if (! haveDirection)
                {
                    s.capture = input;
                    haveDirection = true;
                }
                else if (input != s.capture)
                {
                    continue;
                }

                Format f = a.f;
                f.alt = a.alt;

                if (a.version >= 3)                         f.note = "USB audio class 3 is not supported";
                else if (! a.haveGeneral || ! a.haveFormat) f.note = "no format descriptor";
                else if (! a.formatTypeOne || f.kind == Kind::other) f.note = "not uncompressed PCM";
                else if (f.channels < 1 || f.channels > 64) f.note = "bad channel count";
                else if (f.subslot < 1 || f.subslot > 4)    f.note = "unsupported sample size";
                else if (f.kind == Kind::floating && f.subslot != 4) f.note = "unsupported floating point size";
                else if (f.bits > f.subslot * 8)            f.note = "bad bit resolution";
                else if (f.packet < 1)                      f.note = "no packet size";

                if (f.bits <= 0)
                    f.bits = f.subslot * 8;

                s.formats.push_back (std::move (f));

                if (a.terminalLink != 0)
                    s.terminalLink = a.terminalLink;
            }

            if (s.formats.empty())
                continue;

            // The AudioControl interface it belongs to: the one whose header lists it, else the first
            // of its version (version 2 has no list).
            const Control* owner = nullptr;

            for (const auto& c : controls)
                if (std::find (c.streams.begin(), c.streams.end(), number) != c.streams.end())
                    owner = &c;

            if (owner == nullptr)
                for (const auto& c : controls)
                    if (c.version == s.version) { owner = &c; break; }

            if (owner != nullptr)
            {
                s.controlInterface = owner->number;

                if (s.version == 2)
                {
                    for (const auto& e : owner->entities)
                    {
                        if (e.id == s.terminalLink && (e.subtype == 2 || e.subtype == 3))
                            s.clockRoot = e.clock;

                        if (e.subtype >= 0x0A && e.subtype <= 0x0C)
                            s.clocks.push_back ({ e.id, e.subtype, e.controls, e.inputs });
                    }
                }
            }

            out.streams.push_back (std::move (s));
        }

        return out;
    }

    // --------------------------------------------------------------------------- what to use

    inline double busIntervalsPerSecond (const Format& f, int speedMbps) noexcept
    {
        const bool high = speedMbps >= 480 || (speedMbps == 0 && f.packet > 1023);
        return (high ? 8000.0 : 1000.0) / (double) (1 << std::clamp (f.interval - 1, 0, 15));
    }

    // Does one packet of the endpoint have room for the frames that `rate` needs per bus interval?
    inline bool carries (const Format& f, int rate, int speedMbps) noexcept
    {
        if (rate <= 0 || f.frameBytes() <= 0)
            return false;

        const long frames = (long) std::ceil ((double) rate / busIntervalsPerSecond (f, speedMbps));
        return frames * f.frameBytes() <= f.packet;
    }

    // The sample rates a version 1 format offers (a version 2 one does not say: the clock does),
    // those that fit in the packets first. A range yields the usual rates inside it.
    inline std::vector<int> rateOptions (const Format& f, int speedMbps)
    {
        static const int usual[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
        std::vector<int> all = f.rates;

        if (all.empty() && f.rateMax > 0 && f.rateMax >= f.rateMin)
        {
            for (const int r : usual)
                if (r >= f.rateMin && r <= f.rateMax)
                    all.push_back (r);

            if (all.empty())
                all.push_back (f.rateMax);
        }

        std::vector<int> fits;

        for (const int r : all)
            if (r > 0 && carries (f, r, speedMbps))
                fits.push_back (r);

        return fits.empty() ? all : fits;
    }

    // 48000, else 44100, else the highest rate below those (24000 beats 16000), else the lowest of
    // the higher ones (88200 before 96000), else 0 when there is none.
    inline int pickRate (const std::vector<int>& options) noexcept
    {
        for (const int p : { 48000, 44100 })
            if (std::find (options.begin(), options.end(), p) != options.end())
                return p;

        int below = 0, above = 0;

        for (const int r : options)
        {
            if (r <= 0)
                continue;

            if (r < 44100)
                below = std::max (below, r);
            else if (above == 0 || r < above)
                above = r;
        }

        return below != 0 ? below : above;
    }

    // The order in which the formats of a stream are tried: most channels first (a mixer's
    // channels can be chosen later, a missing one cannot), then 24 bit, 16 bit, the 32 bit ones and
    // 8 bit last, then the lower alternate setting.
    inline std::vector<int> rankFormats (const Stream& s)
    {
        const auto score = [] (const Format& f)
        {
            const int bits = f.kind == Kind::floating ? 32 : f.bits;
            return (f.kind == Kind::pcm8 || bits <= 8) ? 0 : (bits == 24 ? 4 : (bits == 16 ? 3 : 2));
        };

        std::vector<int> order;

        for (size_t i = 0; i < s.formats.size(); ++i)
            if (s.formats[i].usable() && s.formats[i].isInput() == s.capture)
                order.push_back ((int) i);

        std::stable_sort (order.begin(), order.end(), [&] (int x, int y)
        {
            const Format& a = s.formats[(size_t) x];
            const Format& b = s.formats[(size_t) y];

            if (a.channels != b.channels) return a.channels > b.channels;
            if (score (a) != score (b)) return score (a) > score (b);
            return a.alt < b.alt;
        });

        return order;
    }

    // ----------------------------------------------------------------- class requests

    struct ControlRequest
    {
        uint8_t type = 0;           // bmRequestType
        uint8_t request = 0;        // bRequest
        uint16_t value = 0;
        uint16_t index = 0;
        uint16_t length = 0;
    };

    constexpr uint8_t kSetCur = 0x01;
    constexpr uint8_t kGetCur = 0x81;           // version 1
    constexpr uint8_t kCur = 0x01;              // version 2 (the direction is in bmRequestType)
    constexpr uint8_t kRange = 0x02;            // version 2

    // Version 1: the sample rate is set on the isochronous endpoint, as three bytes.
    inline ControlRequest rateRequestV1 (bool set, uint8_t endpoint) noexcept
    {
        return { (uint8_t) (set ? 0x22 : 0xA2), set ? kSetCur : kGetCur, 0x0100, endpoint, 3 };
    }

    // Version 2: the sample rate belongs to a clock source and is four bytes. The index is the
    // entity in the high byte and the AudioControl interface in the low one.
    inline ControlRequest rateRequestV2 (bool set, int clockId, int controlInterface) noexcept
    {
        return { (uint8_t) (set ? 0x21 : 0xA1), kCur, 0x0100, (uint16_t) ((clockId << 8) | (controlInterface & 0xFF)), 4 };
    }

    inline ControlRequest rateRangeRequestV2 (int clockId, int controlInterface, int subRanges) noexcept
    {
        return { 0xA1, kRange, 0x0100, (uint16_t) ((clockId << 8) | (controlInterface & 0xFF)), (uint16_t) (2 + 12 * std::clamp (subRanges, 0, 32)) };
    }

    // Which input a clock selector has chosen (one byte, counted from 1).
    inline ControlRequest selectorRequestV2 (int selectorId, int controlInterface) noexcept
    {
        return { 0xA1, kCur, 0x0100, (uint16_t) ((selectorId << 8) | (controlInterface & 0xFF)), 1 };
    }

    inline void putRate (uint8_t* out, int bytes, uint32_t rate) noexcept
    {
        for (int i = 0; i < bytes; ++i)
            out[i] = (uint8_t) (rate >> (8 * i));
    }

    inline uint32_t getRate (const uint8_t* in, int bytes) noexcept
    {
        uint32_t v = 0;

        for (int i = 0; i < bytes && i < 4; ++i)
            v |= (uint32_t) in[i] << (8 * i);

        return v;
    }

    struct SubRange { uint32_t low = 0, high = 0, step = 0; };

    // The answer to a RANGE request: a count, then (low, high, step) of 32 bits each.
    inline std::vector<SubRange> parseRanges (const uint8_t* data, size_t size)
    {
        std::vector<SubRange> out;

        if (size < 2)
            return out;

        const size_t count = (size_t) (data[0] | (data[1] << 8));

        for (size_t i = 0; i < count && 2 + 12 * (i + 1) <= size; ++i)
            out.push_back ({ getRate (data + 2 + 12 * i, 4), getRate (data + 6 + 12 * i, 4), getRate (data + 10 + 12 * i, 4) });

        return out;
    }

    inline std::vector<int> rangeRates (const std::vector<SubRange>& ranges)
    {
        static const int usual[] = { 8000, 11025, 16000, 22050, 24000, 32000, 44100, 48000, 88200, 96000, 176400, 192000 };
        std::vector<int> out;

        for (const auto& r : ranges)
        {
            for (const int rate : usual)
            {
                const bool inside = (uint32_t) rate >= r.low && (uint32_t) rate <= r.high;
                const bool onStep = r.step == 0 || ((uint32_t) rate - r.low) % r.step == 0;

                if (inside && onStep && std::find (out.begin(), out.end(), rate) == out.end())
                    out.push_back (rate);
            }

            // A fixed rate that is not one of the usual ones.
            if (r.low == r.high && r.low > 0 && std::find (out.begin(), out.end(), (int) r.low) == out.end())
                out.push_back ((int) r.low);
        }

        std::sort (out.begin(), out.end());
        return out;
    }

    // The clock source behind a version 2 stream. Selectors are asked which input they have chosen
    // (`activePin` returns it, counted from 1; anything else means "could not ask" and the first
    // input is taken); multipliers are followed to the clock they multiply. Returns 0 when the chain
    // is broken.
    inline int resolveClock (const Stream& s, const std::function<int (int)>& activePin = {})
    {
        int id = s.clockRoot;

        for (int hops = 0; hops < 8 && id != 0; ++hops)
        {
            const ClockNode* node = nullptr;

            for (const auto& n : s.clocks)
                if (n.id == id)
                    node = &n;

            if (node == nullptr)
                return 0;

            if (node->subtype == 0x0A)
                return id;

            if (node->inputs.empty())
                return 0;

            int pin = 1;

            if (node->subtype == 0x0B && activePin)
            {
                const int asked = activePin (id);

                if (asked >= 1 && asked <= (int) node->inputs.size())
                    pin = asked;
            }

            id = node->inputs[(size_t) pin - 1];
        }

        return 0;
    }

    inline const ClockNode* findClock (const Stream& s, int id) noexcept
    {
        for (const auto& n : s.clocks)
            if (n.id == id)
                return &n;

        return nullptr;
    }

    // --------------------------------------------------------------------------- the samples

    // Turns the bytes of the isochronous packets into floating point samples, whole frames at a
    // time. A frame that is cut between two packets (a device that splits one) is finished with the
    // next packet's bytes.
    class PcmDecoder
    {
    public:
        void configure (Kind kind, int channels, int subslot) noexcept
        {
            kind_ = kind;
            channels_ = std::clamp (channels, 1, 64);
            subslot_ = kind == Kind::floating ? 4 : (kind == Kind::pcm8 ? 1 : std::clamp (subslot, 1, 4));
            carried_ = 0;
        }

        int channels() const noexcept { return channels_; }
        int frameBytes() const noexcept { return channels_ * subslot_; }

        // `sink (const float* interleaved, int frames)` is called at most once per feed.
        template <typename Sink>
        void feed (const uint8_t* data, size_t size, Sink&& sink)
        {
            const size_t frame = (size_t) frameBytes();
            size_t at = 0;
            out_.clear();

            if (carried_ > 0)
            {
                while (carried_ < frame && at < size)
                    pending_[carried_++] = data[at++];

                if (carried_ == frame)
                {
                    decode (pending_, 1);
                    carried_ = 0;
                }
            }

            const size_t whole = (size - at) / frame;

            if (whole > 0)
            {
                decode (data + at, whole);
                at += whole * frame;
            }

            while (at < size && carried_ < frame)
                pending_[carried_++] = data[at++];

            if (! out_.empty())
                sink (out_.data(), (int) (out_.size() / (size_t) channels_));
        }

        void reset() noexcept { carried_ = 0; }

    private:
        void decode (const uint8_t* in, size_t frames)
        {
            const size_t samples = frames * (size_t) channels_;
            const size_t base = out_.size();
            out_.resize (base + samples);
            float* out = out_.data() + base;

            switch (kind_)
            {
                case Kind::floating:
                    for (size_t i = 0; i < samples; ++i)
                    {
                        float v;
                        std::memcpy (&v, in + 4 * i, 4);      // little endian machines only; Kerf's targets all are
                        out[i] = std::isfinite (v) ? v : 0.0f;
                    }
                    break;

                case Kind::pcm8:
                    for (size_t i = 0; i < samples; ++i)
                        out[i] = ((int) in[i] - 128) * (1.0f / 128.0f);
                    break;

                default:
                    // Signed, little endian, the sound in the high bits of the slot.
                    for (size_t i = 0; i < samples; ++i)
                    {
                        const uint8_t* s = in + (size_t) subslot_ * i;

                        switch (subslot_)
                        {
                            case 1: out[i] = (int8_t) s[0] * (1.0f / 128.0f); break;
                            case 2: out[i] = (int16_t) (s[0] | (s[1] << 8)) * (1.0f / 32768.0f); break;
                            case 3: out[i] = (int32_t) (((uint32_t) s[0] << 8) | ((uint32_t) s[1] << 16) | ((uint32_t) s[2] << 24)) * (1.0f / 2147483648.0f); break;
                            default: out[i] = (int32_t) ((uint32_t) s[0] | ((uint32_t) s[1] << 8) | ((uint32_t) s[2] << 16) | ((uint32_t) s[3] << 24)) * (1.0f / 2147483648.0f); break;
                        }
                    }
                    break;
            }
        }

        Kind kind_ = Kind::pcm;
        int channels_ = 1;
        int subslot_ = 2;
        uint8_t pending_[256] = {};         // up to 64 channels of 4 bytes
        size_t carried_ = 0;
        std::vector<float> out_;
    };

    // ----------------------------------------------------------------------------- the log

    inline std::string describe (const Format& f)
    {
        const char* kind = f.kind == Kind::floating ? "float" : (f.kind == Kind::pcm8 ? "unsigned pcm" : (f.kind == Kind::pcm ? "pcm" : "other format"));
        std::string out = "alt " + std::to_string (f.alt) + ": " + std::to_string (f.channels) + " ch, " + std::to_string (f.subslot) + " B x "
                          + std::to_string (f.bits) + " bit " + kind;

        if (! f.rates.empty())
        {
            out += ", rates";

            for (const int r : f.rates)
                out += " " + std::to_string (r);
        }
        else if (f.rateMax > 0)
        {
            out += ", rates " + std::to_string (f.rateMin) + "-" + std::to_string (f.rateMax);
        }
        else
        {
            out += ", rate set by the clock";
        }

        out += ", ep 0x" + usbsys::hexBytes (&f.endpoint, 1) + " packet " + std::to_string (f.packet) + " interval " + std::to_string (f.interval);
        out += f.rateControl ? ", rate control on the endpoint" : "";
        out += f.note.empty() ? "" : " (cannot use: " + f.note + ")";
        return out;
    }

    inline std::string describe (const Stream& s)
    {
        std::string out = std::string (s.capture ? "recording" : "playback") + " interface " + std::to_string (s.interfaceNumber)
                          + " (audio class " + std::to_string (s.version) + ")";

        for (const auto& f : s.formats)
            out += "; " + describe (f);

        return out;
    }

    // ----------------------------------------------------------------------- finding devices

    // A recording stream that can be used.
    struct Candidate
    {
        std::string name;           // the kernel's name for the device, e.g. "4-1"
        std::string title;          // "TEAC Model 12"
        int vendor = 0;
        int product = 0;
        int bus = 0;                // -> /dev/bus/usb/<bus>/<device>
        int device = 0;
        int major = 0;              // to make the node when it is missing
        int minor = 0;
        int speed = 0;              // Mbit/s: 1, 12, 480 ...; 0 unknown
        Stream stream;
        std::string driver;         // the kernel driver bound to the stream's interface; empty = none

        std::string key() const
        {
            return std::to_string (bus) + ":" + std::to_string (device) + ":" + std::to_string (stream.interfaceNumber);
        }
    };

    // A USB device that has audio streaming interfaces, whether or not one of them can be used.
    // Kerf writes what it found out about each to its log: it is what tells why a microphone is (or
    // is not) offered.
    struct Device
    {
        std::string name;
        std::string title;
        int vendor = 0;
        int product = 0;
        int bus = 0;
        int device = 0;
        int speed = 0;
        std::string summary;        // the streams, in words
        std::string descriptors;    // the raw bytes of the configuration, in hex
        bool usable = false;        // has a recording stream that can be used

        std::string key() const { return std::to_string (bus) + ":" + std::to_string (device); }
    };

    struct Scan
    {
        std::vector<Device> devices;
        std::vector<Candidate> candidates;
    };

    // Looks at every USB device in `devicesDir` (normally /sys/bus/usb/devices). `candidates` are the
    // recording streams that can be used; their `driver` tells whether the kernel already has a
    // driver for the interface, and only the ones without one need Kerf's own reading.
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
            const auto blob = usbsys::readBytes (base / "descriptors");

            if (blob.size() < 18)
                continue;

            const int active = (int) usbsys::readNumber (base / "bConfigurationValue", 10, -1);
            const Parsed parsed = parseAudio (blob.data(), blob.size(), active);

            if (parsed.streams.empty())
                continue;

            const usbsys::Identity id = usbsys::readIdentity (devicesDir, name);

            Device d;
            d.name = name;
            d.title = id.title.empty() ? "USB audio device" : id.title;
            d.vendor = id.vendor;
            d.product = id.product;
            d.bus = id.bus;
            d.device = id.device;
            d.speed = id.speed;
            const size_t skip = (blob[0] == 18 && blob[1] == 1) ? 18 : 0;
            d.descriptors = usbsys::hexBytes (blob.data() + skip, blob.size() - skip, 4096);

            for (const auto& s : parsed.streams)
                d.summary += (d.summary.empty() ? "" : " | ") + describe (s);

            for (const auto& s : parsed.streams)
            {
                if (! s.capture || rankFormats (s).empty())
                    continue;

                Candidate c;
                c.name = name;
                c.title = d.title;
                c.vendor = id.vendor;
                c.product = id.product;
                c.bus = id.bus;
                c.device = id.device;
                c.major = id.major;
                c.minor = id.minor;
                c.speed = id.speed;
                c.stream = s;
                c.driver = usbsys::driverOf (devicesDir, name, parsed.configuration, s.interfaceNumber);

                if (c.bus > 0 && c.device > 0)
                {
                    d.usable = true;
                    out.candidates.push_back (std::move (c));
                }
            }

            if (d.bus > 0 && d.device > 0)
                out.devices.push_back (std::move (d));
        }

        const auto byKey = [] (const auto& a, const auto& b) { return a.key() < b.key(); };
        std::sort (out.candidates.begin(), out.candidates.end(), byKey);
        std::sort (out.devices.begin(), out.devices.end(), byKey);
        return out;
    }
}
