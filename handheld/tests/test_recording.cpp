// Tests for recording: the queue between the capture thread and the screen, the choice of
// channels, the take and the WAV file, the inputs (a pretend microphone through the InputHub) and
// the RecordSession that the Record screen is built on.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "audio_input.h"
#include "capture_ring.h"
#include "fake_input.h"
#include "minitest.h"
#include "record_session.h"
#include "recorder.h"
#include "sample.h"

namespace fs = std::filesystem;
using namespace kerf::ui;

namespace
{
    struct TempDir
    {
        fs::path path;

        TempDir()
        {
            static int counter = 0;
            path = fs::temp_directory_path() / ("kerf-recording-test-" + std::to_string (std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string (++counter));
            fs::create_directories (path);
        }

        ~TempDir()
        {
            std::error_code ec;
            fs::remove_all (path, ec);
        }
    };

    bool near (float a, float b, float tolerance = 1e-4f) { return std::fabs (a - b) <= tolerance; }

    using Gen = std::function<float (int channel, int frame)>;

    std::vector<float> frames (int channels, int count, const Gen& f)
    {
        std::vector<float> out ((size_t) channels * (size_t) count);

        for (int i = 0; i < count; ++i)
            for (int c = 0; c < channels; ++c)
                out[(size_t) i * (size_t) channels + (size_t) c] = f (c, i);

        return out;
    }

    std::vector<float> constant (int channels, int count, std::vector<float> perChannel)
    {
        return frames (channels, count, [&] (int c, int) { return perChannel[(size_t) c]; });
    }

    // Saves the take and reads the file back, as a person's later "load" would.
    kerf::SamplePtr saveAndRead (const Recorder& rec, const fs::path& file)
    {
        std::string error;

        if (! rec.saveWav16 (file, error))
            return nullptr;

        return kerf::decodeAudioFile (file).sample;
    }

    float peakOf (const kerf::Sample& s, int channel)
    {
        float p = 0.0f;
        const float* d = s.data.channel (channel);

        for (int i = 0; i < s.data.numFrames(); ++i)
            p = std::max (p, std::fabs (d[i]));

        return p;
    }

    // Reads everything that is waiting.
    std::vector<float> drain (InputHub& hub, int channels)
    {
        std::vector<float> out;
        std::vector<float> block ((size_t) 4096 * (size_t) channels);

        for (int guard = 0; guard < 1000; ++guard)
        {
            const int n = hub.read (block.data(), 4096);

            if (n <= 0)
                break;

            out.insert (out.end(), block.begin(), block.begin() + (std::ptrdiff_t) n * channels);
        }

        return out;
    }

    // The bytes of a file, for looking at a WAV header that a decoder would forgive.
    std::vector<unsigned char> readBytes (const fs::path& file)
    {
        std::ifstream in (file, std::ios::binary);
        std::vector<unsigned char> out;
        char c = 0;

        while (in.get (c))
            out.push_back ((unsigned char) c);

        return out;
    }

    // `count` bytes from `at`, least significant first, as a WAV file stores its numbers.
    uint32_t littleEndian (const std::vector<unsigned char>& bytes, size_t at, int count)
    {
        uint32_t v = 0;

        for (int i = 0; i < count && at + (size_t) i < bytes.size(); ++i)
            v |= (uint32_t) bytes[at + (size_t) i] << (8 * i);

        return v;
    }

    std::string textAt (const std::vector<unsigned char>& bytes, size_t at, size_t count)
    {
        std::string s;

        for (size_t i = 0; i < count && at + i < bytes.size(); ++i)
            s += (char) bytes[at + i];

        return s;
    }
}

// ------------------------------------------------------------------------------ the queue

KTEST (CaptureRing, frames_come_out_in_the_order_they_went_in_across_the_wrap)
{
    CaptureRing ring;
    ring.configure (2, 8);

    std::vector<float> in (12), out (16);

    for (int i = 0; i < 12; ++i)
        in[(size_t) i] = (float) i;

    EXPECT (ring.write (in.data(), 6) == 6);
    EXPECT (ring.available() == 6);
    EXPECT (ring.read (out.data(), 4) == 4);

    for (int i = 0; i < 8; ++i)
        EXPECT (out[(size_t) i] == (float) i);

    std::vector<float> more (12);

    for (int i = 0; i < 12; ++i)
        more[(size_t) i] = 100.0f + (float) i;

    EXPECT (ring.write (more.data(), 6) == 6);          // 2 frames were left, 6 more make 8: exactly full
    EXPECT (ring.available() == 8);
    EXPECT (ring.read (out.data(), 8) == 8);

    const float expected[16] = { 8, 9, 10, 11, 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111 };

    for (int i = 0; i < 16; ++i)
        EXPECT (out[(size_t) i] == expected[i]);

    EXPECT (ring.available() == 0);
    EXPECT (ring.read (out.data(), 8) == 0);
}

KTEST (CaptureRing, what_does_not_fit_is_dropped_and_counted)
{
    CaptureRing ring;
    ring.configure (1, 4);
    const float a[6] = { 1, 2, 3, 4, 5, 6 };
    float out[8] = {};

    EXPECT (ring.write (a, 6) == 4);
    EXPECT (ring.dropped() == 2);
    EXPECT (ring.written() == 4);
    EXPECT (ring.read (out, 8) == 4);
    EXPECT (out[0] == 1.0f && out[3] == 4.0f);
    EXPECT (ring.write (a, 2) == 2);
    EXPECT (ring.dropped() == 2);                       // the room is back, nothing more is lost
    EXPECT (ring.write (a, 0) == 0);
    EXPECT (ring.write (a, -3) == 0);
}

KTEST (CaptureRing, a_read_gives_what_is_there_and_no_more)
{
    CaptureRing ring;
    ring.configure (1, 16);
    const float a[3] = { 7, 8, 9 };
    float out[16] = {};

    ring.write (a, 3);
    EXPECT (ring.read (out, 16) == 3);
    EXPECT (ring.read (out, 0) == 0);
    EXPECT (ring.read (out, -1) == 0);
}

KTEST (CaptureRing, configuring_starts_afresh)
{
    CaptureRing ring;
    ring.configure (2, 4);
    const float a[16] = {};
    ring.write (a, 8);
    EXPECT (ring.dropped() == 4);

    ring.configure (3, 16);
    EXPECT (ring.channels() == 3 && ring.capacity() == 16);
    EXPECT (ring.available() == 0 && ring.dropped() == 0 && ring.written() == 0);

    ring.configure (0, 0);                              // nonsense is made safe
    EXPECT (ring.channels() == 1 && ring.capacity() == 1);
}

KTEST (CaptureRing, one_thread_can_write_while_another_reads)
{
    CaptureRing ring;
    ring.configure (2, 1024);
    constexpr int total = 300000;

    std::thread writer ([&]
    {
        int next = 0;
        int block = 1;
        std::vector<float> data (2 * 300);

        while (next < total)
        {
            const int n = std::min ({ block, total - next, 300 });

            // Only write when there is room, so that nothing is dropped: the reader only ever makes more.
            if (ring.capacity() - ring.available() < n)
            {
                std::this_thread::yield();
                continue;
            }

            for (int i = 0; i < n; ++i)
            {
                data[(size_t) i * 2] = (float) (next + i);
                data[(size_t) i * 2 + 1] = -(float) (next + i);
            }

            ring.write (data.data(), n);
            next += n;
            block = block % 299 + 1;
        }
    });

    int got = 0;
    bool inOrder = true;
    std::vector<float> out (2 * 77);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);

    while (got < total && std::chrono::steady_clock::now() < deadline)
    {
        const int n = ring.read (out.data(), 77);

        if (n == 0)
        {
            std::this_thread::yield();
            continue;
        }

        for (int i = 0; i < n; ++i)
            inOrder = inOrder && out[(size_t) i * 2] == (float) (got + i) && out[(size_t) i * 2 + 1] == -(float) (got + i);

        got += n;
    }

    writer.join();
    EXPECT (got == total);
    EXPECT (inOrder);
    EXPECT (ring.dropped() == 0);
}

// ------------------------------------------------------------------------ channel choices

KTEST (ChannelChoices, one_channel_can_only_be_recorded_as_mono)
{
    const auto c = channelChoices (1);
    EXPECT (c.size() == 1);

    if (c.size() == 1)
    {
        EXPECT (c[0].left == 0 && c[0].right == -1 && ! c[0].mix);
        EXPECT (c[0].takeChannels() == 1);
        EXPECT (c[0].label == "1  mono");
    }
}

KTEST (ChannelChoices, a_stereo_stream_offers_the_pair_each_channel_and_a_mix)
{
    const auto c = channelChoices (2);
    EXPECT (c.size() == 4);

    if (c.size() != 4)
        return;

    EXPECT (c[0].label == "1+2  stereo" && c[0].left == 0 && c[0].right == 1 && c[0].takeChannels() == 2);
    EXPECT (c[1].label == "1  mono" && c[1].left == 0 && c[1].right == -1 && c[1].takeChannels() == 1);
    EXPECT (c[2].label == "2  mono" && c[2].left == 1 && c[2].right == -1 && c[2].takeChannels() == 1);
    EXPECT (c[3].label == "1+2  mono mix" && c[3].mix && c[3].takeChannels() == 1);
}

KTEST (ChannelChoices, a_mixer_offers_its_pairs_first_and_then_every_channel)
{
    const auto c = channelChoices (12);
    EXPECT (c.size() == 6 + 12);

    if (c.size() != 18)
        return;

    EXPECT (c[0].label == "1+2  stereo");
    EXPECT (c[5].label == "11+12  stereo" && c[5].left == 10 && c[5].right == 11);
    EXPECT (c[6].label == "1  mono");
    EXPECT (c[17].label == "12  mono" && c[17].left == 11);
}

KTEST (ChannelChoices, an_odd_channel_has_no_pair_but_is_still_offered)
{
    const auto c = channelChoices (3);
    EXPECT (c.size() == 1 + 3);

    if (c.size() == 4)
        EXPECT (c[0].label == "1+2  stereo" && c[3].label == "3  mono" && c[3].left == 2);
}

KTEST (ChannelChoices, nothing_or_too_many)
{
    EXPECT (channelChoices (0).empty());
    EXPECT (channelChoices (-3).empty());
    EXPECT (channelChoices (100).size() == 16 + 32);    // limited to 32 channels
}

// --------------------------------------------------------------------------- the recorder

KTEST (Recorder, a_stream_sets_the_channels_and_the_first_pair_is_chosen)
{
    Recorder rec;
    EXPECT (! rec.configured());
    rec.configure (6, 48000, 10.0);
    EXPECT (rec.configured() && rec.channels() == 6 && rec.sampleRate() == 48000);
    EXPECT (rec.choice().label == "1+2  stereo");
    EXPECT (! rec.recording() && ! rec.hasTake());

    rec.reset();
    EXPECT (! rec.configured());
}

KTEST (Recorder, the_longest_take_is_between_a_second_and_an_hour)
{
    Recorder rec;
    rec.configure (1, 8000, 0.2);
    EXPECT (rec.maxSeconds() == 1.0);
    rec.configure (1, 8000, -5.0);
    EXPECT (rec.maxSeconds() == 1.0);
    rec.configure (1, 8000, 90.0);
    EXPECT (rec.maxSeconds() == 90.0);
    rec.configure (1, 8000, 1.0e9);
    EXPECT (rec.maxSeconds() == 3600.0);

    // ... and a stream is never more than 32 channels wide, nor has a negative rate.
    rec.configure (100, -5, 10.0);
    EXPECT (rec.channels() == Recorder::kMaxChannels && rec.sampleRate() == 0);
    EXPECT (! rec.start());
}

KTEST (Recorder, the_meters_follow_the_peaks_and_fall_back_by_24_dB_a_second)
{
    Recorder rec;
    rec.configure (2, 48000, 10.0);
    const auto in = constant (2, 100, { 0.5f, -0.25f });
    rec.feed (in.data(), 100);

    EXPECT (near (rec.level (0), 0.5f) && near (rec.level (1), 0.25f));
    EXPECT (near (rec.hold (0), 0.5f));
    EXPECT (rec.level (2) == 0.0f && rec.level (-1) == 0.0f && rec.hold (7) == 0.0f);

    for (int i = 0; i < 2; ++i)
        rec.tick (0.25);

    const float drop = 20.0f * std::log10 (rec.level (0) / 0.5f);
    EXPECT (std::fabs (drop - (-12.0f)) < 0.2f);       // half a second: 12 dB
    EXPECT (near (rec.hold (0), 0.5f));                 // a peak is held for a second

    rec.tick (0.25);
    EXPECT (near (rec.hold (0), 0.5f));
    rec.tick (0.25);                                     // the second is over: the hold starts to fall too
    EXPECT (rec.hold (0) < 0.5f);

    for (int i = 0; i < 8; ++i)
        rec.tick (0.25);

    EXPECT (rec.level (0) < 0.01f && rec.hold (0) < 0.05f);
    EXPECT (rec.hold (0) >= rec.level (0));
}

KTEST (Recorder, a_louder_peak_replaces_the_hold_and_starts_the_second_again)
{
    Recorder rec;
    rec.configure (1, 48000, 10.0);
    const auto quiet = constant (1, 10, { 0.1f });
    const auto loud = constant (1, 10, { 0.8f });
    const auto louder = constant (1, 10, { 0.9f });

    rec.feed (loud.data(), 10);
    rec.tick (0.5);
    rec.feed (quiet.data(), 10);
    EXPECT (near (rec.hold (0), 0.8f));                 // the quiet one does not lower it

    rec.feed (louder.data(), 10);
    EXPECT (near (rec.hold (0), 0.9f));

    for (int i = 0; i < 3; ++i)
        rec.tick (0.25);

    EXPECT (near (rec.hold (0), 0.9f));                 // a second from the louder peak, not from the first
    rec.tick (0.25);
    EXPECT (rec.hold (0) < 0.9f);
}

KTEST (Recorder, a_meter_shows_the_highest_value_of_a_block_and_a_quieter_block_does_not_lower_it)
{
    Recorder rec;
    rec.configure (1, 8000, 10.0);
    const std::vector<float> block { 0.2f, -0.9f, 0.4f, 0.1f };      // the peak is not the last frame
    rec.feed (block.data(), 4);
    EXPECT (near (rec.level (0), 0.9f));
    EXPECT (near (rec.hold (0), 0.9f));

    const std::vector<float> softer { 0.3f, 0.1f };
    rec.feed (softer.data(), 2);
    EXPECT (near (rec.level (0), 0.9f));                 // a meter falls with time, not with a quieter block
}

KTEST (Recorder, the_gain_applies_to_the_meters_and_to_the_take)
{
    Recorder rec;
    rec.configure (1, 8000, 10.0);
    rec.setGainDb (6);
    EXPECT (rec.gainDb() == 6);
    const auto in = constant (1, 100, { 0.25f });
    rec.start();
    rec.feed (in.data(), 100);
    EXPECT (near (rec.level (0), 0.25f * 1.99526f, 1e-3f));

    TempDir dir;
    const auto s = saveAndRead (rec, dir.path / "gain.wav");
    EXPECT (s != nullptr);

    if (s)
        EXPECT (near (peakOf (*s, 0), 0.25f * 1.99526f, 2e-3f));

    rec.setGainDb (500);
    EXPECT (rec.gainDb() == 60);
    rec.setGainDb (-500);
    EXPECT (rec.gainDb() == -60);
}

KTEST (Recorder, the_gain_is_a_setting_that_stays_when_a_stream_starts_or_ends)
{
    Recorder rec;
    rec.setGainDb (6);
    rec.configure (2, 48000, 10.0);
    EXPECT (rec.gainDb() == 6);

    const auto in = constant (2, 10, { 0.25f, 0.25f });
    rec.feed (in.data(), 10);
    EXPECT (near (rec.level (0), 0.25f * 1.99526f, 1e-3f));          // the new stream is metered with it

    rec.configure (1, 44100, 10.0);                      // another input
    EXPECT (rec.gainDb() == 6);
    rec.reset();
    EXPECT (rec.gainDb() == 6);
}

KTEST (Recorder, both_channels_of_a_stereo_take_and_the_mix_of_two_get_the_gain)
{
    TempDir dir;
    Recorder rec;
    rec.configure (2, 8000, 10.0);
    rec.setGainDb (6);
    const float gain = 1.99526f;
    const auto in = constant (2, 100, { 0.1f, 0.2f });

    rec.start();
    rec.feed (in.data(), 100);
    rec.stop();
    auto s = saveAndRead (rec, dir.path / "pair.wav");
    EXPECT (s && s->data.numChannels() == 2);

    if (s)
    {
        EXPECT (near (s->data.get (0, 5), 0.1f * gain, 1e-3f));
        EXPECT (near (s->data.get (1, 5), 0.2f * gain, 1e-3f));
    }

    rec.setChoice (channelChoices (2)[3]);               // the two mixed to mono
    rec.start();
    rec.feed (in.data(), 100);
    rec.stop();
    s = saveAndRead (rec, dir.path / "mix.wav");
    EXPECT (s && s->data.numChannels() == 1);

    if (s)
        EXPECT (near (s->data.get (0, 5), 0.15f * gain, 1e-3f));
}

KTEST (Recorder, a_take_holds_the_chosen_pair_of_a_many_channel_stream)
{
    Recorder rec;
    rec.configure (6, 44100, 10.0);
    const auto choices = channelChoices (6);
    EXPECT (choices.size() >= 2);
    rec.setChoice (choices[1]);                          // 3+4
    EXPECT (rec.choice().label == "3+4  stereo");
    EXPECT (rec.start());

    const auto in = constant (6, 1000, { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f });
    rec.feed (in.data(), 1000);
    rec.stop();

    EXPECT (rec.hasTake() && rec.takeFrames() == 1000 && rec.takeChannels() == 2);
    EXPECT (near (rec.takeSeconds(), 1000.0 / 44100.0, 1e-6));

    TempDir dir;
    const auto s = saveAndRead (rec, dir.path / "pair.wav");
    EXPECT (s != nullptr);

    if (! s)
        return;

    EXPECT (s->data.numChannels() == 2 && s->data.numFrames() == 1000);
    EXPECT (near (s->sampleRate, 44100.0, 0.5));
    EXPECT (near (s->data.get (0, 10), 0.3f, 1e-3f));
    EXPECT (near (s->data.get (1, 10), 0.4f, 1e-3f));
}

KTEST (Recorder, a_mono_take_is_one_channel_or_the_mix_of_two)
{
    TempDir dir;
    Recorder rec;
    rec.configure (2, 8000, 10.0);
    const auto in = constant (2, 400, { 0.5f, -0.1f });

    rec.setChoice (channelChoices (2)[2]);               // "2  mono"
    rec.start();
    rec.feed (in.data(), 400);
    rec.stop();
    EXPECT (rec.takeChannels() == 1);
    auto s = saveAndRead (rec, dir.path / "right.wav");
    EXPECT (s && s->data.numChannels() == 1);

    if (s)
        EXPECT (near (s->data.get (0, 5), -0.1f, 1e-3f));

    rec.setChoice (channelChoices (2)[3]);               // mix
    rec.start();
    rec.feed (in.data(), 400);
    rec.stop();
    s = saveAndRead (rec, dir.path / "mix.wav");
    EXPECT (s && s->data.numChannels() == 1);

    if (s)
        EXPECT (near (s->data.get (0, 5), 0.2f, 1e-3f));
}

KTEST (Recorder, only_the_recorded_channels_can_clip_and_the_take_never_goes_past_full_scale)
{
    Recorder rec;
    rec.configure (4, 8000, 10.0);                       // recording 1+2
    rec.start();

    auto elsewhere = constant (4, 50, { 0.1f, 0.1f, 1.5f, 1.5f });
    rec.feed (elsewhere.data(), 50);
    EXPECT (! rec.clipping() && ! rec.clipped());
    EXPECT (rec.level (2) > 1.0f);                       // the meter still shows it

    auto over = constant (4, 50, { 1.5f, -2.0f, 0.0f, 0.0f });
    rec.feed (over.data(), 50);
    EXPECT (rec.clipping() && rec.clipped());

    rec.tick (1.0);
    EXPECT (rec.clipping());
    rec.tick (0.6);
    EXPECT (! rec.clipping());
    EXPECT (rec.clipped());                              // ... but this take did clip

    rec.stop();
    TempDir dir;
    const auto s = saveAndRead (rec, dir.path / "clip.wav");
    EXPECT (s != nullptr);

    if (s)
    {
        EXPECT (near (s->data.get (0, 60), 1.0f, 1e-3f));
        EXPECT (near (s->data.get (1, 60), -1.0f, 1e-3f));
    }
}

KTEST (Recorder, clipping_is_only_remembered_for_a_take_that_was_under_way)
{
    Recorder rec;
    rec.configure (1, 8000, 10.0);
    const auto over = constant (1, 10, { 1.2f });
    rec.feed (over.data(), 10);
    EXPECT (rec.clipping() && ! rec.clipped());

    rec.start();
    EXPECT (! rec.clipped());                            // a new take starts clean
    rec.feed (over.data(), 10);
    EXPECT (rec.clipped());
    rec.discard();
    EXPECT (! rec.clipped());
}

KTEST (Recorder, clipping_means_full_scale_after_the_gain_and_not_a_loud_signal)
{
    Recorder rec;
    rec.configure (1, 8000, 10.0);
    rec.start();

    const auto loud = constant (1, 10, { 0.6f });
    rec.feed (loud.data(), 10);
    EXPECT (! rec.clipping() && ! rec.clipped());

    const auto nearly = constant (1, 10, { 0.999f });
    rec.feed (nearly.data(), 10);
    EXPECT (! rec.clipping() && ! rec.clipped());

    const auto full = constant (1, 10, { -1.0f });
    rec.feed (full.data(), 10);
    EXPECT (rec.clipping() && rec.clipped());

    // The gain is part of it: 0.6 with 6 dB more is past full scale.
    Recorder boosted;
    boosted.configure (1, 8000, 10.0);
    boosted.setGainDb (6);
    boosted.feed (loud.data(), 10);
    EXPECT (boosted.clipping());
}

KTEST (Recorder, the_take_stops_by_itself_when_it_is_as_long_as_allowed)
{
    Recorder rec;
    rec.configure (1, 1000, 1.0);                        // one second at 1000 Hz
    rec.start();
    const auto in = constant (1, 600, { 0.5f });

    rec.feed (in.data(), 600);
    EXPECT (rec.recording() && ! rec.full() && rec.takeFrames() == 600);

    rec.feed (in.data(), 600);
    EXPECT (! rec.recording() && rec.full() && rec.takeFrames() == 1000);
    EXPECT (near ((float) rec.takeSeconds(), 1.0f));

    rec.feed (in.data(), 100);
    EXPECT (rec.takeFrames() == 1000);                   // nothing more is added
    EXPECT (rec.hasTake());
}

KTEST (Recorder, the_choice_cannot_change_under_way_and_a_wrong_one_is_ignored)
{
    Recorder rec;
    rec.configure (4, 8000, 10.0);
    const auto choices = channelChoices (4);
    rec.setChoice (choices[1]);
    EXPECT (rec.choice().label == "3+4  stereo");

    rec.start();
    rec.setChoice (choices[0]);
    EXPECT (rec.choice().label == "3+4  stereo");        // recording: no change
    rec.stop();

    ChannelChoice wrong;
    wrong.left = 9;
    wrong.right = 10;
    rec.setChoice (wrong);
    EXPECT (rec.choice().label == "3+4  stereo");
    wrong.left = 0;
    wrong.right = 4;
    rec.setChoice (wrong);
    EXPECT (rec.choice().label == "3+4  stereo");
    wrong.left = -1;
    wrong.right = -1;
    rec.setChoice (wrong);
    EXPECT (rec.choice().label == "3+4  stereo");
    wrong.left = 7;                                      // a mono choice of a channel the stream does not have
    wrong.right = -1;
    rec.setChoice (wrong);
    EXPECT (rec.choice().label == "3+4  stereo");
}

KTEST (Recorder, a_new_take_forgets_the_old_one_and_starting_needs_a_stream)
{
    Recorder rec;
    EXPECT (! rec.start());                               // no stream

    rec.configure (1, 8000, 10.0);
    const auto in = constant (1, 100, { 0.5f });
    EXPECT (rec.start());
    EXPECT (! rec.start());                               // already recording
    rec.feed (in.data(), 100);
    rec.stop();
    EXPECT (rec.takeFrames() == 100);

    EXPECT (rec.start());
    EXPECT (rec.takeFrames() == 0);
    rec.discard();
    EXPECT (! rec.hasTake() && ! rec.recording());

    Recorder broken;
    broken.configure (2, 0, 10.0);                        // no sample rate
    EXPECT (! broken.start());
}

KTEST (Recorder, frames_that_are_not_recorded_only_move_the_meters)
{
    Recorder rec;
    rec.configure (1, 8000, 10.0);
    const auto in = constant (1, 100, { 0.5f });
    rec.feed (in.data(), 100);
    EXPECT (! rec.hasTake());
    rec.feed (nullptr, 10);
    rec.feed (in.data(), 0);
    EXPECT (near (rec.level (0), 0.5f));
}

KTEST (Recorder, a_take_is_written_as_a_wav_that_reads_back_and_leaves_no_temporary_file)
{
    TempDir dir;
    Recorder rec;
    rec.configure (2, 22050, 10.0);
    rec.start();
    const auto ramp = frames (2, 1000, [] (int c, int i) { return (c == 0 ? 1.0f : -1.0f) * (float) i / 1000.0f; });
    rec.feed (ramp.data(), 1000);
    rec.stop();

    const auto file = dir.path / "take.wav";
    std::string error;
    EXPECT (rec.saveWav16 (file, error));
    EXPECT (error.empty());
    EXPECT (fs::exists (file));
    EXPECT (! fs::exists (dir.path / "take.wav.part"));
    EXPECT (fs::file_size (file) == 44u + 1000u * 2u * 2u);

    const auto s = kerf::decodeAudioFile (file).sample;
    EXPECT (s != nullptr);

    if (! s)
        return;

    EXPECT (s->data.numChannels() == 2 && s->data.numFrames() == 1000);
    EXPECT (near (s->sampleRate, 22050.0, 0.5));

    bool same = true;

    for (int i = 0; i < 1000; ++i)
        same = same && near (s->data.get (0, i), ramp[(size_t) i * 2], 1e-4f)
                    && near (s->data.get (1, i), ramp[(size_t) i * 2 + 1], 1e-4f);

    EXPECT (same);
}

KTEST (Recorder, the_wav_header_says_what_the_file_holds)
{
    TempDir dir;

    // What a decoder forgives and a sound editor may not: every number of the header, byte by byte.
    const auto check = [&dir] (int channels, int rate, int count, const char* name)
    {
        Recorder rec;
        rec.configure (channels, rate, 10.0);
        rec.start();
        const auto in = frames (channels, count, [] (int c, int) { return c == 0 ? 0.5f : -0.5f; });
        rec.feed (in.data(), count);
        rec.stop();

        std::string error;
        EXPECT (rec.saveWav16 (dir.path / name, error));
        const auto b = readBytes (dir.path / name);
        const uint32_t dataBytes = (uint32_t) (count * channels * 2);

        EXPECT (b.size() == 44u + dataBytes);
        EXPECT (textAt (b, 0, 4) == "RIFF");
        EXPECT (littleEndian (b, 4, 4) == 36u + dataBytes);                  // the rest of the file
        EXPECT (textAt (b, 8, 8) == "WAVEfmt ");
        EXPECT (littleEndian (b, 16, 4) == 16u);                             // the size of the "fmt " part
        EXPECT (littleEndian (b, 20, 2) == 1u);                              // PCM
        EXPECT (littleEndian (b, 22, 2) == (uint32_t) channels);
        EXPECT (littleEndian (b, 24, 4) == (uint32_t) rate);
        EXPECT (littleEndian (b, 28, 4) == (uint32_t) (rate * channels * 2));    // bytes a second
        EXPECT (littleEndian (b, 32, 2) == (uint32_t) (channels * 2));           // bytes a frame
        EXPECT (littleEndian (b, 34, 2) == 16u);
        EXPECT (textAt (b, 36, 4) == "data");
        EXPECT (littleEndian (b, 40, 4) == dataBytes);

        // The first frame: +0.5 on the left (and -0.5 on the right), low byte first.
        EXPECT (littleEndian (b, 44, 2) == 0x4000u);

        if (channels == 2)
            EXPECT (littleEndian (b, 46, 2) == 0xC000u);
    };

    check (1, 8000, 1000, "mono.wav");
    check (2, 44100, 500, "stereo.wav");
}

#if defined(__linux__)
// /dev/full takes a file name and then refuses every byte: a disk that fills up, without filling one.
KTEST (Recorder, a_write_that_fails_says_so_and_leaves_no_temporary_file)
{
    if (! fs::exists ("/dev/full"))
        return;

    TempDir dir;
    Recorder rec;
    rec.configure (1, 8000, 10.0);
    rec.start();
    const auto in = constant (1, 100, { 0.5f });
    rec.feed (in.data(), 100);
    rec.stop();

    const fs::path target = dir.path / "full.wav";
    fs::path part = target;
    part += ".part";
    std::error_code ec;
    fs::create_symlink ("/dev/full", part, ec);

    if (ec)
        return;

    std::string error;
    EXPECT (! rec.saveWav16 (target, error));
    EXPECT (error == "cannot write full.wav (is the disk full?)");
    EXPECT (! fs::exists (target));
    EXPECT (fs::symlink_status (part, ec).type() == fs::file_type::not_found);        // the half-written file is taken away
}
#endif

KTEST (Recorder, a_long_take_is_written_in_pieces)
{
    TempDir dir;
    Recorder rec;
    rec.configure (1, 48000, 30.0);
    rec.start();
    const auto in = frames (1, 48000, [] (int, int i) { return 0.5f * std::sin ((float) i * 0.05f); });

    for (int k = 0; k < 5; ++k)
        rec.feed (in.data(), 48000);

    rec.stop();
    EXPECT (rec.takeFrames() == 240000);                 // more than a few of the writer's chunks

    const auto s = saveAndRead (rec, dir.path / "long.wav");
    EXPECT (s && s->data.numFrames() == 240000 && s->data.numChannels() == 1);

    if (s)
    {
        EXPECT (near (s->data.get (0, 239999), 0.5f * std::sin ((float) 47999 * 0.05f), 1e-3f));
        EXPECT (near (s->data.get (0, 16384), 0.5f * std::sin ((float) 16384 * 0.05f), 1e-3f));    // the first frame of the second piece
    }
}

KTEST (Recorder, saving_says_what_went_wrong)
{
    TempDir dir;
    Recorder rec;
    rec.configure (1, 8000, 10.0);
    std::string error;

    EXPECT (! rec.saveWav16 (dir.path / "none.wav", error));
    EXPECT (error == "nothing has been recorded");

    const auto in = constant (1, 100, { 0.5f });
    rec.start();
    rec.feed (in.data(), 100);
    rec.stop();

    error.clear();
    EXPECT (! rec.saveWav16 (dir.path / "missing-folder" / "x.wav", error));
    EXPECT (error == "cannot create x.wav");
    EXPECT (! fs::exists (dir.path / "missing-folder"));
}

KTEST (Recorder, the_next_take_is_numbered_after_the_highest_one_there)
{
    TempDir dir;
    EXPECT (Recorder::nextTakePath (dir.path) == dir.path / "rec-001.wav");

    // Only rec-<number>.wav counts: not another word before the number, not another kind of file.
    for (const char* name : { "rec-001.wav", "rec-005.wav", "REC-007.WAV", "rec-abc.wav", "rec-.wav", "notes.wav", "rec-0002.mp3", "xrec-020.wav",
                              "take050.wav", "rec-050.mp3" })
        std::ofstream (dir.path / name).put ('x');

    EXPECT (Recorder::nextTakePath (dir.path) == dir.path / "rec-008.wav");

    std::ofstream (dir.path / "rec-999.wav").put ('x');
    EXPECT (Recorder::nextTakePath (dir.path) == dir.path / "rec-1000.wav");

    EXPECT (Recorder::nextTakePath (dir.path / "does-not-exist") == dir.path / "does-not-exist" / "rec-001.wav");
}

// ---------------------------------------------------------------------- inputs and the hub

KTEST (InputHub, inputs_of_all_backends_are_listed_with_the_ones_kerf_reads_first)
{
    InputHub hub;
    auto system = std::make_unique<FakeInput>();
    system->add ({ "", "System microphone", 1, 48000 });
    auto usb = std::make_unique<FakeInput>();
    FakeDevice receiver;
    receiver.name = "Wireless receiver";
    receiver.direct = true;
    usb->add (receiver);
    hub.add (std::move (system));
    hub.add (std::move (usb));

    const auto list = hub.sources();
    EXPECT (list.size() == 2);

    if (list.size() != 2)
        return;

    EXPECT (list[0].name == "Wireless receiver" && list[0].direct && list[0].backend == 1);
    EXPECT (list[1].name == "System microphone" && ! list[1].direct && list[1].backend == 0);
    EXPECT (list[0].id == "fake:Wireless receiver");
}

KTEST (InputHub, an_open_input_delivers_sound_as_the_clock_runs)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();
    fake->add ({ "", "Mic", 2, 48000 });
    hub.add (std::move (fake));

    EXPECT (hub.state() == InputState::idle);
    EXPECT (hub.open (hub.sources()[0]));
    EXPECT (hub.isOpen() && hub.current().name == "Mic");
    EXPECT (hub.state() == InputState::running);

    const auto st = hub.status();
    EXPECT (st.channels == 2 && st.sampleRate == 48000 && st.state == InputState::running);

    hub.poll (0.5);
    const auto sound = drain (hub, 2);
    EXPECT (sound.size() == 2u * 24000u);
    EXPECT (hub.status().frames == 24000);

    float peak0 = 0.0f, peak1 = 0.0f;

    for (size_t i = 0; i + 1 < sound.size(); i += 2)
    {
        peak0 = std::max (peak0, std::fabs (sound[i]));
        peak1 = std::max (peak1, std::fabs (sound[i + 1]));
    }

    EXPECT (near (peak0, fakeAmplitude (0), 2e-3f));
    EXPECT (near (peak1, fakeAmplitude (1), 2e-3f));
    EXPECT (fakeAmplitude (1) > fakeAmplitude (0));
}

KTEST (InputHub, time_that_is_not_a_whole_number_of_frames_is_carried_over)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();
    fake->add ({ "", "Mic", 1, 44100 });
    hub.add (std::move (fake));
    hub.open (hub.sources()[0]);

    for (int i = 0; i < 60; ++i)
        hub.poll (1.0 / 60.0);

    EXPECT (std::abs ((int) drain (hub, 1).size() - 44100) <= 1);
}

KTEST (InputHub, an_input_that_cannot_be_opened_says_why)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();
    FakeDevice d;
    d.name = "Busy";
    d.failWith = "another program is using it";
    fake->add (d);
    hub.add (std::move (fake));

    EXPECT (hub.open (hub.sources()[0]));
    const auto st = hub.status();
    EXPECT (st.state == InputState::failed && st.message == "another program is using it");
    hub.poll (1.0);
    EXPECT (hub.read (nullptr, 0) == 0);
    std::vector<float> block (64);
    EXPECT (hub.read (block.data(), 32) == 0);
}

KTEST (InputHub, an_input_nobody_lists_is_refused)
{
    InputHub hub;
    InputSource nobody;
    nobody.id = "x";
    nobody.backend = 3;
    EXPECT (! hub.open (nobody));
    EXPECT (! hub.isOpen());
    EXPECT (hub.status().state == InputState::idle);

    hub.add (std::make_unique<FakeInput>());
    EXPECT (! hub.open (nobody));                          // there is a backend, but not number 3
    nobody.backend = 1;
    EXPECT (! hub.open (nobody));                          // ... nor one past the last
    nobody.backend = -1;
    EXPECT (! hub.open (nobody));
    nobody.backend = 0;                                    // a backend that does not have it
    EXPECT (hub.open (nobody));
    EXPECT (hub.status().state == InputState::failed);
}

KTEST (InputHub, closing_stops_the_input)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();
    fake->add ({ "", "Mic", 1, 8000 });
    hub.add (std::move (fake));
    hub.open (hub.sources()[0]);
    hub.poll (0.1);
    hub.close();

    EXPECT (! hub.isOpen() && hub.state() == InputState::idle);
    std::vector<float> block (1000);
    EXPECT (hub.read (block.data(), 1000) == 0);
    EXPECT (hub.current().id.empty());
    hub.poll (0.1);                                        // nothing is open: nothing happens
}

KTEST (InputHub, sound_that_arrived_before_the_input_ended_can_still_be_read)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();
    FakeDevice d;
    d.name = "Short";
    d.channels = 1;
    d.rate = 48000;
    d.endsAfter = 0.2;
    fake->add (d);
    hub.add (std::move (fake));
    hub.open (hub.sources()[0]);

    hub.poll (0.25);
    EXPECT (hub.state() == InputState::ended);
    EXPECT (hub.status().message == "unplugged");
    EXPECT (drain (hub, 1).size() == 9600);                 // 0.2 s, not 0.25
    hub.poll (0.25);
    EXPECT (drain (hub, 1).empty());
}

KTEST (InputHub, sound_nobody_reads_is_lost_and_counted)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();
    fake->add ({ "", "Mic", 1, 48000 });
    hub.add (std::move (fake));
    hub.open (hub.sources()[0]);

    for (int i = 0; i < 10; ++i)
        hub.poll (0.25);                                    // 2.5 s into a ring of 2 s

    EXPECT (hub.status().dropped == 24000);
    EXPECT (drain (hub, 1).size() == 96000);
}

KTEST (InputHub, a_silent_input_sends_zeros_and_one_that_sends_nothing_sends_nothing)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();
    FakeDevice silent;
    silent.name = "Silent";
    silent.channels = 1;
    silent.silent = true;
    FakeDevice nothing;
    nothing.name = "Nothing";
    nothing.channels = 1;
    nothing.sendsNothing = true;
    fake->add (silent);
    fake->add (nothing);
    hub.add (std::move (fake));
    const auto list = hub.sources();

    hub.open (list[0]);
    hub.poll (0.1);
    const auto zeros = drain (hub, 1);
    EXPECT (zeros.size() == 4800);
    EXPECT (std::all_of (zeros.begin(), zeros.end(), [] (float v) { return v == 0.0f; }));

    hub.open (list[1]);
    hub.poll (0.1);
    EXPECT (drain (hub, 1).empty());
    EXPECT (hub.state() == InputState::running);
}

namespace
{
    // An input that does nothing, except that it can say something to the logger it was given.
    class TalkingInput : public InputBackend
    {
    public:
        std::vector<InputSource> list() override { return {}; }
        void start (const InputSource&, CaptureRing&) override {}
        void stop() override {}
        InputState state() const override { return InputState::idle; }
        InputStatus status() const override { return {}; }
        void say (const std::string& text) const { log (text); }
    };
}

KTEST (InputHub, the_logger_reaches_the_backends_added_before_and_after_it_was_set)
{
    std::vector<std::string> lines;
    InputHub hub;

    auto early = std::make_unique<TalkingInput>();
    TalkingInput* first = early.get();
    hub.add (std::move (early));

    hub.setLogger ([&lines] (const std::string& line) { lines.push_back (line); });

    auto late = std::make_unique<TalkingInput>();
    TalkingInput* second = late.get();
    hub.add (std::move (late));

    first->say ("one");
    second->say ("two");
    EXPECT (lines.size() == 2 && lines[0] == "one" && lines[1] == "two");
}

KTEST (InputHub, a_backend_that_is_not_there_is_not_added)
{
    InputHub hub;
    hub.add (nullptr);
    EXPECT (hub.empty());
    EXPECT (hub.sources().empty());
    EXPECT (! hub.open (InputSource {}));
}

KTEST (InputHub, the_inputs_of_each_group_stay_in_the_order_they_were_listed_in)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();

    for (int i = 0; i < 40; ++i)
    {
        FakeDevice d;
        d.name = "in" + std::to_string (100 + i);
        d.direct = (i % 3 == 0);
        fake->add (d);
    }

    hub.add (std::move (fake));

    std::vector<std::string> expected;

    for (int i = 0; i < 40; ++i)
        if (i % 3 == 0)
            expected.push_back ("fake:in" + std::to_string (100 + i));

    for (int i = 0; i < 40; ++i)
        if (i % 3 != 0)
            expected.push_back ("fake:in" + std::to_string (100 + i));

    const auto list = hub.sources();
    bool same = list.size() == expected.size();

    for (size_t i = 0; same && i < list.size(); ++i)
        same = list[i].id == expected[i];

    EXPECT (same);
}

KTEST (InputHub, opening_or_closing_stops_the_input_that_was_running)
{
    InputHub hub;
    auto system = std::make_unique<FakeInput>();
    system->add ({ "", "System microphone", 1, 48000 });
    auto usb = std::make_unique<FakeInput>();
    FakeDevice receiver;
    receiver.name = "Wireless receiver";
    receiver.direct = true;
    usb->add (receiver);
    FakeInput* systemBackend = system.get();
    FakeInput* usbBackend = usb.get();
    hub.add (std::move (system));
    hub.add (std::move (usb));

    const auto list = hub.sources();                       // the receiver first, then the system's microphone
    EXPECT (list.size() == 2);

    if (list.size() != 2)
        return;

    EXPECT (hub.open (list[1]));
    EXPECT (systemBackend->state() == InputState::running);
    EXPECT (hub.open (list[0]));                           // from the other backend
    EXPECT (systemBackend->state() == InputState::idle);   // ... and the first one is stopped
    EXPECT (usbBackend->state() == InputState::running);

    hub.close();
    EXPECT (usbBackend->state() == InputState::idle);
}

KTEST (InputHub, what_an_earlier_stream_left_behind_is_not_shown_for_an_input_that_failed)
{
    InputHub hub;
    auto fake = std::make_unique<FakeInput>();
    fake->add ({ "", "Mic", 1, 48000 });
    FakeDevice busy;
    busy.name = "Busy";
    busy.failWith = "another program is using it";
    fake->add (busy);
    hub.add (std::move (fake));
    const auto list = hub.sources();

    hub.open (list[0]);

    for (int i = 0; i < 10; ++i)
        hub.poll (0.25);                                   // 2.5 s into a ring of 2 s: some is lost, some is waiting

    EXPECT (hub.status().dropped == 24000);

    hub.open (list[1]);                                    // fails before the ring is set up again
    EXPECT (hub.state() == InputState::failed);
    EXPECT (hub.status().dropped == 0);
    std::vector<float> block (4096);
    EXPECT (hub.read (block.data(), 4096) == 0);
}

// --------------------------------------------------------------------------- the session

namespace
{
    struct Rig
    {
        InputHub hub;
        FakeInput* fake = nullptr;
        RecordSession session;
        TempDir dir;

        Rig()
        {
            auto f = std::make_unique<FakeInput>();
            fake = f.get();
            hub.add (std::move (f));
            session.attach (&hub);
        }

        void with (const std::string& name, int channels, int rate = 48000)
        {
            FakeDevice d;
            d.name = name;
            d.channels = channels;
            d.rate = rate;
            fake->add (d);
        }

        // Frames of 1/60 s, as the program runs them.
        void run (int frameCount, double dt = 1.0 / 60.0)
        {
            for (int i = 0; i < frameCount; ++i)
                session.update (dt);
        }
    };
}

KTEST (RecordSession, the_input_used_last_time_is_opened_and_the_first_one_if_it_is_gone)
{
    Rig r;
    r.with ("A", 2);
    r.with ("B", 6);
    r.session.begin ("fake:B", 3, 60.0);

    EXPECT (r.session.active() && r.session.ready());
    EXPECT (r.session.sourceId() == "fake:B" && r.session.sourceName() == "B");
    EXPECT (r.session.sourceIndex() == 1 && r.session.sources().size() == 2);
    EXPECT (r.session.recorder().channels() == 6);
    EXPECT (r.session.choices().size() == 3 + 6);
    EXPECT (r.session.gainDb() == 3);

    r.session.end();
    r.session.begin ("fake:Gone", 0, 60.0);
    EXPECT (r.session.sourceId() == "fake:A" && r.session.sourceIndex() == 0);
    EXPECT (r.session.recorder().channels() == 2);

    r.session.end();
    r.session.begin ("", 0, 60.0);
    EXPECT (r.session.sourceId() == "fake:A");
}

KTEST (RecordSession, without_an_input_it_says_so_and_picks_one_up_when_it_is_plugged_in)
{
    Rig r;
    r.session.begin ("", 0, 60.0);
    EXPECT (r.session.active() && ! r.session.ready());
    EXPECT (r.session.statusText() == "No audio input found");
    EXPECT (r.session.sourceIndex() == -1 && r.session.sourceName().empty());
    EXPECT (! r.session.toggleRecording());

    r.with ("Mic", 1);
    r.session.update (0.5);
    EXPECT (! r.session.ready());                            // the list is looked at every 1.5 s
    r.session.update (1.1);
    EXPECT (r.session.ready() && r.session.sourceName() == "Mic");
    EXPECT (r.session.recorder().channels() == 1);
}

KTEST (RecordSession, a_session_without_a_hub_does_nothing)
{
    RecordSession s;
    EXPECT (! s.available());
    s.begin ("", 0, 60.0);
    s.update (0.1);
    EXPECT (s.statusText() == "No audio input found");
    EXPECT (! s.toggleRecording());
    s.chooseSource (1);
    s.chooseChannels (1);
    s.adjustGain (1);
    s.end();

    InputHub empty;
    s.attach (&empty);
    EXPECT (! s.available());
}

KTEST (RecordSession, recording_collects_the_chosen_channels_for_as_long_as_it_runs)
{
    Rig r;
    r.with ("Mic", 2);
    r.session.begin ("", 0, 60.0);
    r.run (10);

    EXPECT (r.session.phase() == RecordSession::Phase::idle);
    EXPECT (r.session.recorder().level (1) > 0.1f);          // the meters move before recording starts
    EXPECT (r.session.toggleRecording());
    EXPECT (r.session.phase() == RecordSession::Phase::recording);

    r.run (120);
    EXPECT (std::fabs (r.session.recorder().takeSeconds() - 2.0) < 0.02);

    EXPECT (r.session.toggleRecording());
    EXPECT (r.session.phase() == RecordSession::Phase::stopped);
    const double length = r.session.recorder().takeSeconds();
    r.run (30);
    EXPECT (r.session.recorder().takeSeconds() == length);   // stopped: nothing is added
    EXPECT (! r.session.toggleRecording());                  // a take is kept until it is made new

    fs::path saved;
    std::string error;
    EXPECT (r.session.save (r.dir.path, saved, error));
    const auto s = kerf::decodeAudioFile (saved).sample;
    EXPECT (s && s->data.numChannels() == 2);

    if (s)
    {
        EXPECT (std::fabs (s->durationSeconds() - length) < 1e-3);
        EXPECT (near (peakOf (*s, 0), fakeAmplitude (0), 3e-3f));
        EXPECT (near (peakOf (*s, 1), fakeAmplitude (1), 3e-3f));
    }
}

KTEST (RecordSession, a_take_that_is_too_short_is_thrown_away)
{
    Rig r;
    r.with ("Mic", 1);
    r.session.begin ("", 0, 60.0);
    r.run (3);
    r.session.toggleRecording();
    r.run (2);
    r.session.toggleRecording();

    EXPECT (r.session.phase() == RecordSession::Phase::idle);
    EXPECT (! r.session.recorder().hasTake());
    EXPECT (r.session.takeNotice() == "Too short - nothing was kept");
    EXPECT (r.session.takeNotice().empty());                 // a notice is given once
}

KTEST (RecordSession, the_settings_are_locked_while_recording_and_while_a_take_exists)
{
    Rig r;
    r.with ("A", 4);
    r.with ("B", 2);
    r.session.begin ("fake:A", 0, 60.0);
    r.run (5);

    r.session.toggleRecording();
    r.run (30);
    r.session.chooseChannels (1);
    r.session.chooseSource (1);
    r.session.adjustGain (1);
    EXPECT (r.session.choiceIndex() == 0 && r.session.sourceId() == "fake:A" && r.session.gainDb() == 0);
    EXPECT (! r.session.canChange());

    r.session.toggleRecording();
    EXPECT (r.session.phase() == RecordSession::Phase::stopped);
    r.session.chooseChannels (1);
    r.session.chooseSource (1);
    r.session.adjustGain (1);
    EXPECT (r.session.choiceIndex() == 0 && r.session.sourceId() == "fake:A" && r.session.gainDb() == 0);
    EXPECT (! r.session.canChange());

    r.session.newTake();
    EXPECT (r.session.canChange() && r.session.phase() == RecordSession::Phase::idle);
    r.session.chooseChannels (1);
    EXPECT (r.session.choiceIndex() == 1);
    r.session.adjustGain (2);
    EXPECT (r.session.gainDb() == 6);
}

KTEST (RecordSession, the_channels_that_are_chosen_are_the_ones_recorded)
{
    Rig r;
    r.with ("Mixer", 6);
    r.session.begin ("", 0, 60.0);
    r.run (3);
    r.session.chooseChannels (1);                            // 3+4
    EXPECT (r.session.choices()[(size_t) r.session.choiceIndex()].label == "3+4  stereo");

    r.session.toggleRecording();
    r.run (30);
    r.session.toggleRecording();

    fs::path saved;
    std::string error;
    EXPECT (r.session.save (r.dir.path, saved, error));
    const auto s = kerf::decodeAudioFile (saved).sample;
    EXPECT (s && s->data.numChannels() == 2);

    if (s)
    {
        EXPECT (near (peakOf (*s, 0), fakeAmplitude (2), 3e-3f));
        EXPECT (near (peakOf (*s, 1), fakeAmplitude (3), 3e-3f));
    }
}

KTEST (RecordSession, the_choice_of_channels_is_kept_for_the_next_input_that_has_it)
{
    Rig r;
    r.with ("A", 6);
    r.with ("B", 6);
    r.with ("C", 2);
    r.session.begin ("fake:A", 0, 60.0);
    r.session.chooseChannels (2);                            // 5+6
    const std::string label = r.session.choices()[(size_t) r.session.choiceIndex()].label;
    EXPECT (label == "5+6  stereo");

    r.session.chooseSource (1);                              // B has six channels too
    EXPECT (r.session.sourceId() == "fake:B");
    EXPECT (r.session.choices()[(size_t) r.session.choiceIndex()].label == label);

    r.session.chooseSource (1);                              // C has two: the first choice
    EXPECT (r.session.sourceId() == "fake:C" && r.session.choiceIndex() == 0);
    EXPECT (r.session.recorder().channels() == 2);

    r.session.chooseSource (1);                              // around
    EXPECT (r.session.sourceId() == "fake:A");
    r.session.chooseSource (-1);
    EXPECT (r.session.sourceId() == "fake:C");
}

KTEST (RecordSession, the_channels_taken_last_time_are_taken_again_when_the_input_has_them)
{
    Rig r;
    r.with ("Mixer", 6);
    r.with ("Mic", 2);

    // Nothing was picked before: the first choice, and nothing to keep.
    r.session.begin ("fake:Mixer", 0, 60.0);
    EXPECT (r.session.choiceIndex() == 0);
    EXPECT (r.session.chosenChannels().empty());

    // What was picked last time (the program keeps the label in its settings).
    r.session.begin ("fake:Mixer", 0, 60.0, "5+6  stereo");
    EXPECT (r.session.choices()[(size_t) r.session.choiceIndex()].label == "5+6  stereo");
    EXPECT (r.session.recorder().choice().label == "5+6  stereo");
    EXPECT (r.session.chosenChannels() == "5+6  stereo");

    // An input without them starts with its first choice - and the wish is not lost.
    r.session.chooseSource (1);
    EXPECT (r.session.sourceId() == "fake:Mic" && r.session.choiceIndex() == 0);
    EXPECT (r.session.chosenChannels() == "5+6  stereo");
    r.session.chooseSource (1);
    EXPECT (r.session.sourceId() == "fake:Mixer");
    EXPECT (r.session.choices()[(size_t) r.session.choiceIndex()].label == "5+6  stereo");

    // Picking another pair replaces the wish; the program takes it out when the screen is left and
    // gives it back when the screen is entered again.
    r.session.chooseChannels (-1);
    EXPECT (r.session.chosenChannels() == "3+4  stereo");
    const std::string wish = r.session.chosenChannels();
    r.session.end();
    r.session.begin ("fake:Mixer", 0, 60.0, wish);
    EXPECT (r.session.choices()[(size_t) r.session.choiceIndex()].label == "3+4  stereo");

    // A label that no input has leaves the first choice.
    r.session.begin ("fake:Mixer", 0, 60.0, "9+10  stereo");
    EXPECT (r.session.choiceIndex() == 0);
    EXPECT (r.session.chosenChannels() == "9+10  stereo");
}

KTEST (RecordSession, the_gain_moves_in_steps_of_three_and_stays_in_range)
{
    Rig r;
    r.with ("Mic", 1);
    r.session.begin ("", 100, 60.0);
    EXPECT (r.session.gainDb() == 30);                       // limited when the screen is entered
    r.session.adjustGain (1);
    EXPECT (r.session.gainDb() == 30);
    r.session.adjustGain (-2);
    EXPECT (r.session.gainDb() == 24);
    r.session.adjustGain (-100);
    EXPECT (r.session.gainDb() == -12);
    EXPECT (r.session.recorder().gainDb() == -12);

    r.session.end();
    r.session.begin ("", -50, 60.0);
    EXPECT (r.session.gainDb() == -12);
}

KTEST (RecordSession, a_take_that_reaches_the_longest_allowed_stops_and_says_so)
{
    Rig r;
    r.with ("Mic", 1);
    r.session.begin ("", 0, 1.0);                            // one second at most
    r.run (3);
    r.session.toggleRecording();
    r.run (70);

    EXPECT (r.session.phase() == RecordSession::Phase::stopped);
    EXPECT (r.session.recorder().full());
    EXPECT (r.session.recorder().takeSeconds() == 1.0);
    EXPECT (r.session.takeNotice() == "The take is full (1.0s)");
    r.run (10);
    EXPECT (r.session.takeNotice().empty());
}

KTEST (RecordSession, an_input_that_goes_away_during_a_take_keeps_what_was_recorded)
{
    Rig r;
    FakeDevice d;
    d.name = "Flaky";
    d.channels = 1;
    d.endsAfter = 0.5;
    r.fake->add (d);
    r.session.begin ("", 0, 60.0);
    r.run (1);
    EXPECT (r.session.toggleRecording());
    r.run (60);

    EXPECT (r.session.phase() == RecordSession::Phase::stopped);
    EXPECT (std::fabs (r.session.recorder().takeSeconds() - 0.5) < 0.03);
    EXPECT (r.session.takeNotice() == "The input stopped - the take so far is kept");
    EXPECT (r.session.input().state == InputState::ended);
    EXPECT (r.session.statusText() == "The input stopped: unplugged");

    fs::path saved;
    std::string error;
    EXPECT (r.session.save (r.dir.path, saved, error));          // the take is good to keep
}

KTEST (RecordSession, an_input_that_goes_away_at_the_start_of_a_take_leaves_nothing)
{
    Rig r;
    FakeDevice d;
    d.name = "Flaky";
    d.channels = 1;
    d.endsAfter = 0.01;
    r.fake->add (d);
    r.session.begin ("", 0, 60.0);
    r.session.toggleRecording();
    r.run (3);

    EXPECT (r.session.phase() == RecordSession::Phase::idle);
    EXPECT (r.session.takeNotice() == "The input stopped - nothing was kept");
}

KTEST (RecordSession, an_input_that_stopped_is_opened_again_only_after_it_was_unplugged_and_plugged_back)
{
    Rig r;
    FakeDevice d;
    d.name = "Mic";
    d.channels = 1;
    d.endsAfter = 0.2;
    r.fake->add (d);
    r.session.begin ("", 0, 60.0);
    EXPECT (r.fake->starts() == 1);

    r.run (30);                                              // it stops by itself, and is still listed
    r.session.update (1.6);
    r.session.update (1.6);
    EXPECT (r.session.input().state == InputState::ended);
    EXPECT (r.fake->starts() == 1);                          // not tried again: it would end the same way

    r.fake->remove ("fake:Mic");
    r.session.update (1.6);                                  // gone
    EXPECT (r.fake->starts() == 1);
    EXPECT (r.session.sources().empty());

    d.endsAfter = 0.0;
    r.fake->add (d);
    r.session.update (1.6);                                  // back
    EXPECT (r.fake->starts() == 2);
    EXPECT (r.session.ready() && r.session.sourceName() == "Mic");
}

KTEST (RecordSession, an_input_that_failed_can_be_tried_again)
{
    Rig r;
    FakeDevice d;
    d.name = "Busy";
    d.channels = 1;
    d.failWith = "another driver or program is using this USB audio device";
    r.fake->add (d);
    r.session.begin ("", 0, 60.0);

    EXPECT (! r.session.ready());
    EXPECT (r.session.statusText() == "another driver or program is using this USB audio device");
    EXPECT (! r.session.toggleRecording());

    r.fake->edit ("fake:Busy", [] (FakeDevice& dev) { dev.failWith.clear(); });
    r.session.update (1.6);
    EXPECT (! r.session.ready());                            // a refresh does not retry a failure by itself
    r.session.reopen();
    EXPECT (r.session.ready());
    EXPECT (r.session.statusText() == "48000 Hz, 1 channel");
}

KTEST (RecordSession, a_device_that_sends_nothing_is_noticed_after_a_moment)
{
    Rig r;
    FakeDevice d;
    d.name = "Dead";
    d.channels = 2;
    d.sendsNothing = true;
    r.fake->add (d);
    r.with ("Live", 2);
    r.session.begin ("fake:Dead", 0, 60.0);

    r.session.update (1.0);
    EXPECT (! r.session.quiet());
    EXPECT (r.session.statusText() == "48000 Hz, 2 channels");
    r.session.update (0.6);
    EXPECT (r.session.quiet() && ! r.session.soundSeen());
    EXPECT (r.session.statusText() == "Nothing is coming from the input yet");

    r.session.end();
    r.session.begin ("fake:Live", 0, 60.0);
    r.session.update (0.3);
    r.session.update (0.25);
    r.session.update (0.25);
    r.session.update (0.25);
    r.session.update (0.25);
    EXPECT (r.session.soundSeen() && ! r.session.quiet());

    r.run (120);                                             // sound that keeps coming is never "nothing"
    EXPECT (r.session.soundSeen() && ! r.session.quiet());
    EXPECT (r.session.statusText() == "48000 Hz, 2 channels");
}

KTEST (RecordSession, the_status_line_counts_what_was_lost)
{
    Rig r;
    r.with ("Mic", 1);
    r.session.begin ("", 0, 60.0);

    for (int i = 0; i < 10; ++i)
        r.hub.poll (0.25);                                   // sound nobody reads

    r.session.update (0.0);
    EXPECT (r.session.statusText().find ("frames lost") != std::string::npos);
}

KTEST (RecordSession, takes_are_saved_with_the_next_number_and_the_take_is_kept)
{
    Rig r;
    r.with ("Mic", 1);
    r.session.begin ("", 0, 60.0);
    r.run (3);
    r.session.toggleRecording();
    r.run (30);
    r.session.toggleRecording();

    fs::path first, second;
    std::string error;
    EXPECT (r.session.save (r.dir.path / "samples", first, error));        // the folder is made
    EXPECT (first == r.dir.path / "samples" / "rec-001.wav" && fs::exists (first));
    EXPECT (r.session.save (r.dir.path / "samples", second, error));
    EXPECT (second == r.dir.path / "samples" / "rec-002.wav");
    EXPECT (r.session.recorder().hasTake());

    r.session.newTake();
    fs::path none;
    EXPECT (! r.session.save (r.dir.path, none, error));
    EXPECT (error == "nothing has been recorded");
}

KTEST (RecordSession, leaving_closes_the_input_and_forgets_everything)
{
    Rig r;
    r.with ("Mic", 1);
    r.session.begin ("", 0, 60.0);
    r.run (3);
    r.session.toggleRecording();
    r.run (10);
    EXPECT (r.hub.isOpen());

    r.session.end();
    EXPECT (! r.hub.isOpen() && r.hub.state() == InputState::idle);
    EXPECT (! r.session.active() && ! r.session.recorder().hasTake() && ! r.session.recorder().configured());
    EXPECT (r.session.sources().empty() && r.session.sourceIndex() == -1);
    r.session.update (1.0);                                  // inactive: nothing happens
    EXPECT (r.hub.state() == InputState::idle);
}

KTEST (RecordSession, the_inputs_are_not_looked_at_while_recording)
{
    Rig r;
    r.with ("A", 1);
    r.session.begin ("", 0, 60.0);
    r.run (3);
    r.session.toggleRecording();
    r.with ("B", 1);
    r.session.update (1.6);
    EXPECT (r.session.sources().size() == 1);                // B is not listed until the take is over
    r.session.toggleRecording();
    r.session.newTake();
    r.session.update (1.6);
    EXPECT (r.session.sources().size() == 2);
}

KTEST (RecordSession, ending_a_session_that_is_not_active_leaves_the_hub_alone)
{
    Rig r;
    r.with ("Mic", 1);
    EXPECT (r.hub.open (r.hub.sources()[0]));
    r.session.end();                                         // it never began: the input is not this session's
    EXPECT (r.hub.isOpen());
}

KTEST (RecordSession, the_meters_keep_falling_while_the_screen_runs)
{
    Rig r;
    FakeDevice d;
    d.name = "Short";
    d.channels = 1;
    d.endsAfter = 0.1;
    r.fake->add (d);
    r.session.begin ("", 0, 60.0);
    r.run (8);                                               // the 0.1 s of sound is in and the input has ended

    const float before = r.session.recorder().level (0);
    EXPECT (before > 0.0f);
    r.session.update (0.5);
    EXPECT (r.session.recorder().level (0) < before * 0.3f);       // half a second: 12 dB down
}

KTEST (RecordSession, an_input_that_comes_back_does_not_take_the_place_of_a_take)
{
    Rig r;
    FakeDevice d;
    d.name = "Mic";
    d.channels = 1;
    d.endsAfter = 0.5;
    r.fake->add (d);
    r.session.begin ("", 0, 60.0);
    r.run (1);
    EXPECT (r.session.toggleRecording());
    r.run (60);
    EXPECT (r.session.phase() == RecordSession::Phase::stopped);
    const int64_t kept = r.session.recorder().takeFrames();
    EXPECT (kept > 0);

    r.fake->remove ("fake:Mic");
    r.session.update (1.6);                                  // unplugged
    d.endsAfter = 0.0;
    r.fake->add (d);
    r.session.update (1.6);                                  // plugged in again, with the take still there
    EXPECT (r.fake->starts() == 1);
    EXPECT (r.session.phase() == RecordSession::Phase::stopped && r.session.recorder().takeFrames() == kept);

    r.session.newTake();                                     // once the take is let go the input is opened again
    r.session.update (1.6);
    EXPECT (r.fake->starts() == 2);
    EXPECT (r.session.ready());
}

KTEST (RecordSession, the_gain_stays_when_the_input_changes_and_when_a_stream_starts)
{
    Rig r;
    r.with ("A", 1);
    r.with ("B", 1);
    r.session.begin ("fake:A", 6, 60.0);
    r.run (5);
    EXPECT (r.session.recorder().gainDb() == 6);
    EXPECT (near (r.session.recorder().level (0), fakeAmplitude (0) * 1.99526f, 5e-3f));

    r.session.chooseSource (1);
    EXPECT (r.session.sourceId() == "fake:B");
    EXPECT (r.session.recorder().gainDb() == 6);
    r.run (5);
    EXPECT (near (r.session.recorder().level (0), fakeAmplitude (0) * 1.99526f, 5e-3f));
}

KTEST (RecordSession, when_the_open_input_is_unplugged_the_buttons_go_to_the_first_or_to_the_last_that_is_listed)
{
    const auto idAfterUnplugging = [] (int delta)
    {
        Rig r;
        r.with ("A", 1);
        r.with ("B", 1);
        r.with ("C", 1);
        r.session.begin ("fake:A", 0, 60.0);
        r.fake->remove ("fake:A");
        r.session.update (1.6);                              // the list is looked at again: A is gone
        EXPECT (r.session.sources().size() == 2 && r.session.sourceIndex() == -1);
        EXPECT (r.session.sourceId() == "fake:A");           // ... but it is still the one that is wanted

        r.session.chooseSource (delta);
        return r.session.sourceId();
    };

    EXPECT (idAfterUnplugging (1) == "fake:B");              // forwards: the first of what is there
    EXPECT (idAfterUnplugging (-1) == "fake:C");             // backwards: the last
}

KTEST (RecordSession, the_input_that_was_wanted_is_remembered_while_it_is_not_plugged_in)
{
    Rig r;
    r.session.begin ("fake:Mic", 0, 60.0);                   // nothing is listed
    EXPECT (r.session.sourceIndex() == -1);
    EXPECT (r.session.sourceId() == "fake:Mic");             // leaving the screen does not forget it

    r.with ("Other", 1);
    r.with ("Mic", 1);
    r.session.update (1.6);
    EXPECT (r.session.sourceId() == "fake:Mic" && r.session.ready());
    EXPECT (r.session.sourceIndex() == 1);
}

KTEST (RecordSession, the_channels_go_around_in_both_directions)
{
    Rig r;
    r.with ("Mic", 2);
    r.session.begin ("", 0, 60.0);
    EXPECT (r.session.choices().size() == 4);                // 1+2, 1, 2 and the two mixed

    r.session.chooseChannels (-1);
    EXPECT (r.session.choiceIndex() == 3 && r.session.choices()[3].mix);
    r.session.chooseChannels (1);
    EXPECT (r.session.choiceIndex() == 0);
    r.session.chooseChannels (5);
    EXPECT (r.session.choiceIndex() == 1);
    r.session.chooseChannels (-6);
    EXPECT (r.session.choiceIndex() == 3);
}

KTEST (RecordSession, trying_again_opens_the_input_once_and_falls_back_to_the_first_when_the_wanted_one_is_gone)
{
    {
        Rig r;
        r.session.begin ("", 0, 60.0);                       // nothing is plugged in yet
        r.with ("Mic", 1);
        r.session.reopen();
        EXPECT (r.session.ready());
        EXPECT (r.fake->starts() == 1);                      // not opened by the look at the list and again by the retry
    }

    {
        Rig r;
        r.with ("A", 1);
        r.with ("B", 1);
        r.session.begin ("fake:B", 0, 60.0);
        r.fake->remove ("fake:B");
        r.session.reopen();
        EXPECT (r.session.sourceId() == "fake:A" && r.session.ready());
    }
}

KTEST (RecordSession, a_take_cannot_be_started_from_an_input_that_has_stopped)
{
    Rig r;
    FakeDevice d;
    d.name = "Flaky";
    d.channels = 1;
    d.endsAfter = 0.2;
    r.fake->add (d);
    r.session.begin ("", 0, 60.0);
    r.run (30);

    EXPECT (r.session.input().state == InputState::ended);
    EXPECT (! r.session.ready());
    EXPECT (! r.session.toggleRecording());
    EXPECT (r.session.phase() == RecordSession::Phase::idle);
}

KTEST (RecordSession, sound_from_before_the_button_was_pressed_is_not_in_the_take)
{
    Rig r;
    r.with ("Mic", 1);
    r.session.begin ("", 0, 60.0);
    r.run (3);
    r.hub.poll (1.0);                                        // a second that the screen has not looked at yet

    EXPECT (r.session.toggleRecording());
    r.session.update (0.0);
    EXPECT (r.session.recorder().takeSeconds() < 0.01);

    r.run (30);
    EXPECT (std::fabs (r.session.recorder().takeSeconds() - 0.5) < 0.02);       // only what came after
}

KTEST (RecordSession, a_second_of_sound_that_arrives_while_the_screen_is_busy_is_taken_in_one_frame)
{
    Rig r;
    r.with ("Mic", 1);
    r.session.begin ("", 0, 60.0);
    r.run (3);
    EXPECT (r.session.toggleRecording());

    r.hub.poll (1.0);
    r.session.update (0.0);
    EXPECT (std::fabs (r.session.recorder().takeSeconds() - 1.0) < 0.001);
}
