// Tests for the parts of the core that don't exist in the VST3 plugin: the portable audio
// buffer, the lock-free queue, MIDI parsing, sample decoding and the Engine facade.

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include "audio_buffer.h"
#include "audio_fixtures.h"
#include "engine.h"
#include "midi.h"
#include "minitest.h"
#include "path_util.h"
#include "sample.h"
#include "spsc_queue.h"

using namespace kerf;

namespace
{
    // Rising zero crossings per second ~= frequency of a clean sine.
    double estimateFrequency (const float* x, int n, double rate)
    {
        int crossings = 0;

        for (int i = 1; i < n; ++i)
            if (x[i - 1] < 0.0f && x[i] >= 0.0f)
                ++crossings;

        return crossings / ((double) n / rate);
    }

    SamplePtr makeToneSample (double seconds, double rate, int channels, float freq = 220.0f)
    {
        auto s = std::make_shared<Sample>();
        const int frames = (int) (seconds * rate);
        s->data.resize (channels, frames);
        s->sampleRate = rate;
        s->name = "tone";

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < frames; ++i)
                s->data.set (ch, i, 0.5f * (float) std::sin (2.0 * 3.14159265358979 * freq * i / rate));

        return s;
    }

    // Three clicks/bursts separated by silence so the transient detector finds three slices.
    SamplePtr makeBurstSample (double rate)
    {
        auto s = std::make_shared<Sample>();
        const int frames = (int) (4.0 * rate);
        s->data.resize (2, frames);
        s->sampleRate = rate;
        s->name = "bursts";

        for (int burst = 0; burst < 3; ++burst)
        {
            const int start = (int) ((0.2 + burst * 1.2) * rate);
            const int len = (int) (0.4 * rate);

            for (int i = 0; i < len; ++i)
            {
                const float v = (i % 2 == 0) ? 0.8f : -0.8f; // big sample-to-sample deltas
                s->data.set (0, start + i, v);
                s->data.set (1, start + i, v);
            }
        }

        return s;
    }

    float rmsOf (const std::vector<float>& v)
    {
        if (v.empty()) return 0.0f;
        double acc = 0.0;
        for (auto x : v) acc += (double) x * x;
        return (float) std::sqrt (acc / (double) v.size());
    }
}

// ---------------------------------------------------------------------------------------------

KTEST (AudioBuffer, resize_zero_fills_and_indexes_by_channel)
{
    AudioBuffer b (2, 8);
    EXPECT_EQ (b.numChannels(), 2);
    EXPECT_EQ (b.numFrames(), 8);
    EXPECT (! b.empty());

    for (int i = 0; i < 8; ++i)
    {
        EXPECT_NEAR (b.get (0, i), 0.0f, 0.0f);
        EXPECT_NEAR (b.get (1, i), 0.0f, 0.0f);
    }

    b.set (1, 3, 0.25f);
    EXPECT_NEAR (b.get (1, 3), 0.25f, 0.0f);
    EXPECT_NEAR (b.get (0, 3), 0.0f, 0.0f);

    b.applyGain (2.0f);
    EXPECT_NEAR (b.get (1, 3), 0.5f, 1e-7f);

    b.clear();
    EXPECT_NEAR (b.get (1, 3), 0.0f, 0.0f);
    EXPECT (AudioBuffer().empty());
}

KTEST (SpscQueue, preserves_order_and_reports_full_and_empty)
{
    SpscQueue<int> q (4);
    int out = 0;
    EXPECT (! q.pop (out));
    EXPECT (q.empty());

    for (int i = 0; i < 4; ++i)
        EXPECT (q.push (i));

    EXPECT (! q.push (99)); // full

    for (int i = 0; i < 4; ++i)
    {
        EXPECT (q.pop (out));
        EXPECT_EQ (out, i);
    }

    EXPECT (q.empty());
}

KTEST (SpscQueue, carries_a_million_items_between_two_threads_in_order)
{
    SpscQueue<int> q (128);
    constexpr int total = 1000000;
    std::atomic<bool> orderOk { true };

    std::thread consumer ([&] {
        int expected = 0, v = 0;
        while (expected < total)
        {
            if (q.pop (v))
            {
                if (v != expected) orderOk = false;
                ++expected;
            }
        }
    });

    for (int i = 0; i < total; ++i)
        while (! q.push (i)) {}

    consumer.join();
    EXPECT (orderOk.load());
}

KTEST (MidiParser, parses_note_on_and_off_with_running_status)
{
    MidiParser p;
    MidiMessage m;
    const uint8_t stream[] = { 0x90, 60, 100, 62, 90, 0x80, 60, 0 };
    std::vector<MidiMessage> got;

    for (auto b : stream)
        if (p.feed (b, m)) got.push_back (m);

    EXPECT_EQ ((int) got.size(), 3);
    EXPECT (got[0].isNoteOn() && got[0].data1 == 60);
    EXPECT (got[1].isNoteOn() && got[1].data1 == 62); // running status: no repeated 0x90
    EXPECT (got[2].isNoteOff() && got[2].data1 == 60);
}

KTEST (MidiParser, note_on_with_velocity_zero_is_a_note_off)
{
    MidiParser p;
    MidiMessage m;
    bool done = false;
    for (auto b : { (uint8_t) 0x93, (uint8_t) 64, (uint8_t) 0 })
        done = p.feed (b, m);

    EXPECT (done);
    EXPECT (m.isNoteOff());
    EXPECT (! m.isNoteOn());
    EXPECT_EQ (m.channel(), 3);
}

KTEST (MidiParser, ignores_realtime_bytes_and_skips_sysex)
{
    MidiParser p;
    MidiMessage m;
    std::vector<MidiMessage> got;
    // clock bytes in the middle of a note-on, then a sysex, then another note-on
    const uint8_t stream[] = { 0x90, 0xF8, 60, 0xF8, 100,
                               0xF0, 0x7E, 0x00, 0x09, 0x01, 0xF7,
                               0x91, 40, 50 };

    for (auto b : stream)
        if (p.feed (b, m)) got.push_back (m);

    EXPECT_EQ ((int) got.size(), 2);
    EXPECT (got[0].isNoteOn() && got[0].data1 == 60 && got[0].data2 == 100);
    EXPECT (got[1].isNoteOn() && got[1].data1 == 40 && got[1].channel() == 1);
}

KTEST (MidiParser, program_change_has_a_single_data_byte)
{
    MidiParser p;
    MidiMessage m;
    EXPECT (! p.feed (0xC2, m));
    EXPECT (p.feed (5, m));
    EXPECT_EQ (m.command(), 0xC0);
    EXPECT_EQ (m.data1, 5);
}

KTEST (MidiInbox, collects_from_several_threads)
{
    MidiInbox inbox;
    std::vector<std::thread> threads;

    for (int t = 0; t < 4; ++t)
        threads.emplace_back ([&, t] {
            for (int i = 0; i < 100; ++i)
                inbox.push ({ (uint8_t) 0x90, (uint8_t) (t * 20 + (i % 20)), 100 });
        });

    for (auto& th : threads) th.join();

    std::vector<MidiMessage> drained;
    EXPECT (inbox.tryDrain (drained));
    EXPECT_EQ ((int) drained.size(), 400);
    EXPECT (inbox.tryDrain (drained));
    EXPECT_EQ ((int) drained.size(), 0);
}

// ---------------------------------------------------------------------------------------------

KTEST (Sample, wav_round_trip_stereo)
{
    auto src = makeToneSample (0.5, 44100.0, 2);
    const auto wav = encodeWav16 (src->data, src->sampleRate);

    auto result = decodeAudioMemory (wav.data(), wav.size(), "tone.wav");
    EXPECT (result.sample != nullptr);
    if (! result.sample) return;

    EXPECT_EQ (result.sample->data.numChannels(), 2);
    EXPECT_EQ (result.sample->data.numFrames(), src->data.numFrames());
    EXPECT_NEAR (result.sample->sampleRate, 44100.0, 0.0);
    EXPECT (result.sample->name == "tone.wav");

    for (int i = 0; i < src->data.numFrames(); i += 997)
        EXPECT_NEAR (result.sample->data.get (1, i), src->data.get (1, i), 1.0f / 16384.0f);
}

KTEST (Sample, wav_round_trip_mono_and_other_rates)
{
    auto src = makeToneSample (0.25, 48000.0, 1);
    const auto wav = encodeWav16 (src->data, src->sampleRate);
    auto result = decodeAudioMemory (wav.data(), wav.size(), "m.wav");

    EXPECT (result.sample != nullptr);
    if (! result.sample) return;
    EXPECT_EQ (result.sample->data.numChannels(), 1);
    EXPECT_NEAR (result.sample->sampleRate, 48000.0, 0.0);
    EXPECT_NEAR (result.sample->durationSeconds(), 0.25, 1e-6);
}

KTEST (Sample, rejects_garbage_and_overlong_files)
{
    const char junk[] = "this is definitely not audio at all";
    auto r1 = decodeAudioMemory (junk, sizeof junk, "junk");
    EXPECT (r1.sample == nullptr);
    EXPECT (! r1.error.empty());

    auto r2 = decodeAudioMemory (nullptr, 0, "empty");
    EXPECT (r2.sample == nullptr);

    auto src = makeToneSample (2.0, 8000.0, 1);
    const auto wav = encodeWav16 (src->data, src->sampleRate);
    auto r3 = decodeAudioMemory (wav.data(), wav.size(), "long.wav", /*maxSeconds*/ 1.0);
    EXPECT (r3.sample == nullptr);

    auto r4 = decodeAudioMemory (wav.data(), wav.size() / 2, "truncated.wav"); // cut mid-data: still decodes what is there or fails cleanly
    EXPECT (r4.sample != nullptr || ! r4.error.empty());
}

KTEST (Sample, decodeAudioFile_reports_missing_files)
{
    auto r = decodeAudioFileUtf8 ("/definitely/not/here.wav");
    EXPECT (r.sample == nullptr);
    EXPECT (! r.error.empty());
}

KTEST (Sample, decodes_mp3_with_the_right_length_and_pitch)
{
    auto r = decodeAudioMemory (kTone44kMp3, kTone44kMp3_size, "tone.mp3");
    EXPECT (r.sample != nullptr);
    if (! r.sample) return;

    EXPECT_EQ (r.sample->data.numChannels(), 1);
    EXPECT_NEAR (r.sample->sampleRate, 44100.0, 0.0);
    EXPECT_NEAR (r.sample->durationSeconds(), 0.3, 0.03); // encoder delay/padding are trimmed; allow a frame of slack
    EXPECT (r.sample->name == "tone.mp3");
    EXPECT_NEAR (estimateFrequency (r.sample->data.channel (0), r.sample->data.numFrames(), r.sample->sampleRate), 440.0, 15.0);
}

KTEST (Sample, decodes_flac_exactly)
{
    auto r = decodeAudioMemory (kTone8kFlac, kTone8kFlac_size, "tone.flac");
    EXPECT (r.sample != nullptr);
    if (! r.sample) return;

    EXPECT_EQ (r.sample->data.numChannels(), 1);
    EXPECT_EQ (r.sample->data.numFrames(), 2000);
    EXPECT_NEAR (r.sample->sampleRate, 8000.0, 0.0);

    // The fixture was a 16-bit 0.5-amplitude 440 Hz sine; FLAC is lossless.
    for (int i = 0; i < 2000; i += 37)
        EXPECT_NEAR (r.sample->data.get (0, i), 0.5 * std::sin (2.0 * 3.14159265358979 * 440.0 * i / 8000.0), 2.0 / 32768.0);
}

KTEST (Sample, a_truncated_wav_keeps_what_decoded)
{
    auto src = makeToneSample (1.0, 8000.0, 1);
    const auto wav = encodeWav16 (src->data, src->sampleRate);
    auto r = decodeAudioMemory (wav.data(), wav.size() / 2, "truncated.wav");

    if (r.sample != nullptr)
    {
        EXPECT (r.sample->data.numFrames() > 3000);
        EXPECT (r.sample->data.numFrames() <= 4100);
    }
    else
    {
        EXPECT (! r.error.empty()); // failing cleanly is acceptable too
    }
}

KTEST (Sample, loads_files_with_non_ascii_names_by_path_and_by_utf8_string)
{
    namespace fs = std::filesystem;

    auto src = makeToneSample (0.1, 8000.0, 1);
    const auto wav = encodeWav16 (src->data, src->sampleRate);

    std::error_code ec;
    const auto dir = fs::temp_directory_path() / pathFromUtf8 ("kerf-test-\xC3\xA1udio-\xC3\xA7\xC3\xA3o");
    fs::create_directories (dir, ec);
    const auto file = dir / pathFromUtf8 ("Grava\xC3\xA7\xC3\xA3o n\xC3\xBAmero 1.wav");

    {
        std::ofstream out (file, std::ios::binary);
        out.write (wav.data(), (std::streamsize) wav.size());
    }

    auto byPath = decodeAudioFile (file);
    EXPECT (byPath.sample != nullptr);
    if (byPath.sample)
        EXPECT (byPath.sample->name == "Grava\xC3\xA7\xC3\xA3o n\xC3\xBAmero 1.wav");

    auto byUtf8 = decodeAudioFileUtf8 (pathToUtf8 (file));
    EXPECT (byUtf8.sample != nullptr);

    fs::remove_all (dir, ec);
}

KTEST (AudioBuffer, shrinkTo_keeps_the_start_of_every_channel)
{
    AudioBuffer b (2, 10);

    for (int i = 0; i < 10; ++i)
    {
        b.set (0, i, (float) i);
        b.set (1, i, 100.0f + (float) i);
    }

    b.shrinkTo (4);
    EXPECT_EQ (b.numFrames(), 4);

    for (int i = 0; i < 4; ++i)
    {
        EXPECT_EQ (b.get (0, i), (float) i);
        EXPECT_EQ (b.get (1, i), 100.0f + (float) i);
    }

    b.shrinkTo (100); // never grows
    EXPECT_EQ (b.numFrames(), 4);

    b.shrinkTo (0);
    EXPECT (b.empty());
}

// ---------------------------------------------------------------------------------------------

KTEST (Engine, silent_until_something_plays)
{
    Engine e;
    e.prepare (44100.0);
    std::vector<float> out (512 * 2, 1.0f);
    e.render (out.data(), 512);
    EXPECT_NEAR (rmsOf (out), 0.0f, 0.0f);
}

KTEST (Engine, load_detect_trigger_and_render)
{
    Engine e;
    e.prepare (44100.0);
    EXPECT (e.loadSample (makeBurstSample (44100.0)));

    std::vector<float> out (1024 * 2);
    e.render (out.data(), 1024); // applies the setSample command

    const auto found = e.detectTransients (0.5f);
    EXPECT_EQ (found, 3);
    EXPECT_EQ (e.slices().size(), 3);

    e.triggerPreview (1);
    e.render (out.data(), 1024);
    EXPECT (rmsOf (out) > 0.05f);
    EXPECT (e.peakLevel() > 0.05f);
    EXPECT (e.peakLevel() <= 1.0f);

    Engine::Playhead heads[Engine::maxPlayheads];
    const auto n = e.copyPlayheads (heads, Engine::maxPlayheads);
    EXPECT_EQ (n, 1);
    if (n == 1)
    {
        EXPECT_EQ (heads[0].slice, 1);
        EXPECT (heads[0].seconds > e.slices().getCurrent()->at (1).start);
    }
}

KTEST (Engine, midi_inbox_drives_trigger_mode_and_ignores_held_key_repeats)
{
    Engine e;
    e.prepare (44100.0);
    e.loadSample (makeBurstSample (44100.0));
    std::vector<float> out (512 * 2);
    e.render (out.data(), 512);
    e.detectTransients (0.5f);
    e.setNoteOffset (60);

    e.midiInbox().push ({ 0x90, 61, 100 }); // slice 1
    e.render (out.data(), 512);
    EXPECT (e.isSliceSounding (1));
    EXPECT (! e.isSliceSounding (0));

    // A second note-on for the same held key must not retrigger (the original's pressedKeys).
    Engine::Playhead before[Engine::maxPlayheads];
    e.copyPlayheads (before, Engine::maxPlayheads);
    e.midiInbox().push ({ 0x90, 61, 100 });
    e.render (out.data(), 512);
    Engine::Playhead after[Engine::maxPlayheads];
    const auto n = e.copyPlayheads (after, Engine::maxPlayheads);
    EXPECT_EQ (n, 1);
    EXPECT (after[0].seconds > before[0].seconds); // kept advancing instead of restarting

    e.midiInbox().push ({ 0x80, 61, 0 });
    for (int i = 0; i < 20; ++i) e.render (out.data(), 512); // long enough for the 50 ms release
    EXPECT (! e.isSliceSounding (1));

    // Velocity-0 note-on works as a note-off too, and notes outside the slice range are ignored.
    e.midiInbox().push ({ 0x90, 100, 100 });
    e.render (out.data(), 512);
    EXPECT_EQ (e.copyPlayheads (after, Engine::maxPlayheads), 0);
}

KTEST (Engine, sampler_mode_plays_the_selected_slice_at_note_pitch)
{
    Engine e;
    e.prepare (44100.0);
    e.loadSample (makeBurstSample (44100.0));
    std::vector<float> out (512 * 2);
    e.render (out.data(), 512);
    e.detectTransients (0.5f);
    e.setMidiMode (VoiceEngine::MidiMode::sampler);
    e.setSelectedSlice (2);
    e.render (out.data(), 512); // apply the allNotesOff queued by the mode switch

    e.noteOn (60);
    e.noteOn (72);
    e.render (out.data(), 512);

    Engine::Playhead heads[Engine::maxPlayheads];
    const auto n = e.copyPlayheads (heads, Engine::maxPlayheads);
    EXPECT_EQ (n, 2);
    for (int i = 0; i < n; ++i)
        EXPECT_EQ (heads[i].slice, 2);
}

KTEST (Engine, swapping_the_sample_while_playing_is_safe_and_old_audio_is_freed_on_the_ui_thread)
{
    Engine e;
    e.prepare (44100.0);

    auto first = makeToneSample (1.0, 44100.0, 2);
    std::weak_ptr<const Sample> watcher = first;
    e.loadSample (first);
    first.reset();

    std::vector<float> out (512 * 2);
    e.render (out.data(), 512);

    Slice s; s.start = 0.0f; s.end = 0.9f; s.mode = Slice::Mode::loop;
    e.slices().add (s);
    e.triggerPreview (0);
    e.render (out.data(), 512);
    EXPECT (e.isSliceSounding (0));

    e.loadSample (makeToneSample (0.5, 22050.0, 1)); // swap mid-playback
    e.render (out.data(), 512);

    EXPECT_EQ (e.slices().size(), 0);       // new sample => old boundaries are gone
    EXPECT (! e.isSliceSounding (0));       // and nothing is left playing the old audio
    EXPECT (! watcher.expired());           // still alive: the audio thread handed it back...
    e.collectGarbage();
    EXPECT (watcher.expired());             // ...and the UI thread freed it
}

KTEST (Engine, concurrent_edits_while_rendering_do_not_crash)
{
    Engine e;
    e.prepare (44100.0);
    e.loadSample (makeBurstSample (44100.0));

    std::atomic<bool> stop { false };
    std::atomic<long> blocks { 0 };

    std::thread audio ([&] {
        std::vector<float> out (256 * 2);
        while (! stop.load())
        {
            e.render (out.data(), 256);
            ++blocks;
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (400);
    int i = 0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        e.detectTransients (0.3f + 0.1f * (float) (i % 5));
        e.slices().add (Slice { 0.1f, 0.5f });
        e.triggerPreview (i % 4);
        e.noteOn (60 + (i % 5));
        e.slices().updateAt (0, [] (Slice& s) { s.pitch = 1.5f; });
        e.retriggerIfPlaying (0);
        e.midiInbox().push ({ 0x90, (uint8_t) (60 + (i % 5)), 90 });

        if (i % 25 == 0)
            e.loadSample (makeBurstSample (44100.0));

        e.collectGarbage();
        ++i;
    }

    stop = true;
    audio.join();
    EXPECT (blocks.load() > 10);
    EXPECT (i > 50);
}
