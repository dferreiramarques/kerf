// Tests for the 16-step pattern sequencer: the clock on its own (StepSequencer), and the clock
// driving the slices through the Engine.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>
#include <vector>

#include "engine.h"
#include "minitest.h"
#include "step_sequencer.h"

using namespace kerf;

namespace
{
    constexpr double kRate = 44100.0;

    struct Hit
    {
        long long frame = 0;   // absolute frame the step started on
        int step = 0;
        int scene = 0;
    };

    // Runs the clock for `totalFrames`, handing it out in blocks of `block` frames (the last one
    // may be shorter), and returns every step it fired.
    std::vector<Hit> runClock (StepSequencer& seq, long long totalFrames, int block, double rate = kRate)
    {
        std::vector<Hit> hits;
        StepSequencer::Event events[StepSequencer::kMaxEvents];
        long long position = 0;

        while (position < totalFrames)
        {
            const int n = (int) std::min<long long> (block, totalFrames - position);
            bool stopped = false;
            const int count = seq.process (n, rate, events, StepSequencer::kMaxEvents, stopped);

            for (int i = 0; i < count; ++i)
            {
                EXPECT (events[i].frame >= 0 && events[i].frame < n);
                hits.push_back ({ position + events[i].frame, events[i].step, events[i].scene });
            }

            position += n;
        }

        return hits;
    }

    // A sample made of constant-level stretches, so the level of what comes out says which slice
    // played: slice i holds the value 0.1 * (i + 1).
    SamplePtr makeLevelsSample (int slices, double sliceSeconds, double rate = kRate)
    {
        auto s = std::make_shared<Sample>();
        const int per = (int) (sliceSeconds * rate);
        s->data.resize (2, per * slices);
        s->sampleRate = rate;
        s->name = "levels";

        for (int i = 0; i < slices; ++i)
            for (int k = 0; k < per; ++k)
            {
                s->data.set (0, i * per + k, 0.1f * (float) (i + 1));
                s->data.set (1, i * per + k, 0.1f * (float) (i + 1));
            }

        return s;
    }

    // `slices` slices of `sliceSeconds` each, exactly covering makeLevelsSample(), with no fades
    // so a note starts and ends cleanly.
    void addSlices (Engine& e, int slices, double sliceSeconds, Slice::Mode mode = Slice::Mode::shot)
    {
        for (int i = 0; i < slices; ++i)
        {
            Slice s;
            s.start = (float) (i * sliceSeconds);
            s.end = (float) ((i + 1) * sliceSeconds);
            s.fadeInMs = 0.0f;
            s.fadeOutMs = 0.0f;
            s.mode = mode;
            e.slices().add (s);
        }
    }

    // A fresh engine with the levels sample loaded, full volume, centre pan law accounted for by
    // the caller (a centred voice is cos(pi/4) = 0.7071 in each channel).
    struct Rig
    {
        Engine e;

        explicit Rig (int slices, double sliceSeconds, Slice::Mode mode = Slice::Mode::shot)
        {
            e.prepare (kRate);
            e.setMasterVolume (1.0f);
            e.loadSample (makeLevelsSample (slices, sliceSeconds));
            std::vector<float> out (64 * 2);
            e.render (out.data(), 64);   // applies the setSample command
            addSlices (e, slices, sliceSeconds, mode);
        }

        std::vector<float> render (int totalFrames, int block)
        {
            std::vector<float> all ((size_t) totalFrames * 2);
            int done = 0;

            while (done < totalFrames)
            {
                const int n = std::min (block, totalFrames - done);
                e.render (all.data() + (size_t) done * 2, n);
                done += n;
            }

            return all;
        }
    };

    constexpr float kCentre = 0.70710678f;   // a centred voice: cos(pi/4) in each channel

    struct Burst
    {
        long long start = 0;
        float level = 0.0f;
    };

    // Stretches of non-silence in the left channel (what the sequencer's one-shots look like).
    std::vector<Burst> findBursts (const std::vector<float>& interleaved)
    {
        std::vector<Burst> bursts;
        bool inside = false;

        for (size_t i = 0; i * 2 < interleaved.size(); ++i)
        {
            const float v = interleaved[i * 2];

            if (! inside && std::fabs (v) > 1.0e-4f)
            {
                inside = true;
                bursts.push_back ({ (long long) i, 0.0f });
            }
            else if (inside && std::fabs (v) <= 1.0e-4f)
            {
                inside = false;
            }

            if (inside && bursts.back().level == 0.0f && (long long) i == bursts.back().start + 8)
                bursts.back().level = v;
        }

        return bursts;
    }
}

// ---------------------------------------------------------------------------------------------
// the clock

KTEST (StepSequencer, fires_sixteenth_notes_at_the_tempo)
{
    StepSequencer seq;
    seq.setTempo (120.0f);     // a sixteenth is 5512.5 frames at 44.1 kHz
    seq.play();

    const auto hits = runClock (seq, 44100 * 2, 512);

    EXPECT_EQ ((int) hits.size(), 16);   // 2 s at 120 BPM = 4 beats x 4 steps (the 17th starts at exactly 2.0 s)

    for (size_t k = 0; k < hits.size(); ++k)
    {
        EXPECT_EQ (hits[k].frame, (long long) std::floor ((double) k * 5512.5));
        EXPECT_EQ (hits[k].step, (int) (k % 16));
        EXPECT_EQ (hits[k].scene, 0);
    }
}

KTEST (StepSequencer, starts_on_the_very_first_frame_even_when_started_late)
{
    StepSequencer seq;
    StepSequencer::Event ev[StepSequencer::kMaxEvents];
    bool stopped = false;

    // Time passes while stopped: nothing fires.
    for (int i = 0; i < 10; ++i)
        EXPECT_EQ (seq.process (512, kRate, ev, StepSequencer::kMaxEvents, stopped), 0);

    seq.play();
    const int n = seq.process (300, kRate, ev, StepSequencer::kMaxEvents, stopped);
    EXPECT_EQ (n, 1);
    EXPECT_EQ (ev[0].frame, 0);
    EXPECT_EQ (ev[0].step, 0);
    EXPECT_EQ (seq.currentStep(), 0);
}

KTEST (StepSequencer, the_steps_do_not_depend_on_the_block_size)
{
    StepSequencer reference;
    reference.setTempo (133.3f);   // not a round number: the fractions have to accumulate right
    reference.setSwing (0.6f);
    reference.play();
    const auto want = runClock (reference, 44100 * 20, 512);
    EXPECT (want.size() > 100);

    for (int block : { 64, 100, 333, 480, 1000, 1024, 4096 })
    {
        StepSequencer seq;
        seq.setTempo (133.3f);
        seq.setSwing (0.6f);
        seq.play();
        const auto got = runClock (seq, 44100 * 20, block);

        EXPECT_EQ ((int) got.size(), (int) want.size());

        for (size_t i = 0; i < std::min (got.size(), want.size()); ++i)
        {
            EXPECT_EQ (got[i].frame, want[i].frame);
            EXPECT_EQ (got[i].step, want[i].step);
        }
    }

    // One frame at a time, and seven at a time, for the first two seconds.
    StepSequencer a, b, c;
    for (auto* s : { &a, &b, &c }) { s->setTempo (133.3f); s->setSwing (0.6f); s->play(); }
    const auto h1 = runClock (a, 44100 * 2, 1);
    const auto h7 = runClock (b, 44100 * 2, 7);
    const auto h512 = runClock (c, 44100 * 2, 512);
    EXPECT_EQ ((int) h1.size(), (int) h512.size());
    EXPECT_EQ ((int) h7.size(), (int) h512.size());

    for (size_t i = 0; i < std::min (h512.size(), std::min (h1.size(), h7.size())); ++i)
    {
        EXPECT_EQ (h1[i].frame, h512[i].frame);
        EXPECT_EQ (h7[i].frame, h512[i].frame);
    }
}

KTEST (StepSequencer, does_not_drift_over_ten_minutes)
{
    StepSequencer seq;
    seq.setTempo (137.0f);
    seq.play();

    const long long total = (long long) (kRate * 600.0);
    const auto hits = runClock (seq, total, 512);
    const double sixteenth = kRate * 15.0 / 137.0;

    // Step k starts at exactly k * sixteenth; check the last one against the formula.
    EXPECT (hits.size() > 4000);
    const auto k = (long long) hits.size() - 1;
    EXPECT_EQ (hits.back().frame, (long long) std::floor ((double) k * sixteenth));
    EXPECT_EQ (hits.back().step, (int) (k % 16));
}

KTEST (StepSequencer, swing_lengthens_the_even_steps_and_shortens_the_odd_ones)
{
    StepSequencer seq;
    seq.setTempo (120.0f);
    seq.setSwing (2.0f / 3.0f);   // triplet feel
    seq.play();

    const auto hits = runClock (seq, 44100 * 2, 512);
    EXPECT (hits.size() >= 8);

    const double pair = 2.0 * 5512.5;   // two sixteenths

    for (size_t k = 0; k + 1 < hits.size(); k += 2)
    {
        // The pair always starts on a pair boundary...
        EXPECT_NEAR ((double) hits[k].frame, (double) (k / 2) * pair, 1.0);
        // ...and its second step comes two thirds of the way through it.
        EXPECT_NEAR ((double) hits[k + 1].frame - (double) hits[k].frame, pair * 2.0 / 3.0, 1.0);
    }

    // Straight again.
    StepSequencer straight;
    straight.setSwing (0.5f);
    straight.play();
    const auto s = runClock (straight, 44100, 512);
    EXPECT_NEAR ((double) s[1].frame - (double) s[0].frame, 5512.5, 1.0);
}

KTEST (StepSequencer, a_shorter_pattern_wraps_early)
{
    StepSequencer seq;
    seq.setLength (5);
    seq.play();

    const auto hits = runClock (seq, 44100 * 3, 512);
    EXPECT (hits.size() > 10);

    for (size_t k = 0; k < hits.size(); ++k)
        EXPECT_EQ (hits[k].step, (int) (k % 5));

    // Making it shorter while it runs takes effect at once, and a length of one is a metronome.
    StepSequencer s2;
    s2.play();
    runClock (s2, 20000, 512);
    s2.setLength (1);
    const auto after = runClock (s2, 30000, 512);

    for (const auto& h : after)
        EXPECT_EQ (h.step, 0);
}

KTEST (StepSequencer, a_scene_change_waits_for_the_top_of_the_pattern)
{
    StepSequencer seq;
    seq.setScene (0);
    seq.play();

    long long position = 0;
    std::vector<Hit> hits;

    auto run = [&] (long long frames)
    {
        for (auto h : runClock (seq, frames, 512))
        {
            h.frame += position;
            hits.push_back (h);
        }

        position += frames;
    };

    run (22050);                        // steps 1-4 of the pattern
    EXPECT_EQ (seq.activeScene(), 0);
    seq.setScene (4);                   // asked for in the middle of the pattern
    EXPECT_EQ (seq.scene(), 4);
    EXPECT_EQ (seq.activeScene(), 0);   // still the old one sounding
    run (44100 * 3);

    bool changed = false;

    for (const auto& h : hits)
    {
        if (h.frame < 88200)
            EXPECT_EQ (h.scene, 0);     // the rest of the pattern is still scene 1

        if (h.scene == 4 && ! changed)
        {
            changed = true;
            EXPECT_EQ (h.step, 0);                 // it lands on a downbeat...
            EXPECT_EQ (h.frame, 88200);            // ...the first one after the request
        }
        else if (changed)
        {
            EXPECT_EQ (h.scene, 4);                // and stays
        }
    }

    EXPECT (changed);
    EXPECT_EQ (seq.activeScene(), 4);
}

KTEST (StepSequencer, stopping_is_reported_once_and_play_restarts_at_the_top)
{
    StepSequencer seq;
    seq.play();
    runClock (seq, 20000, 512);
    EXPECT (seq.currentStep() >= 0);

    seq.stop();
    StepSequencer::Event ev[StepSequencer::kMaxEvents];
    bool stopped = false;

    EXPECT_EQ (seq.process (512, kRate, ev, StepSequencer::kMaxEvents, stopped), 0);
    EXPECT (stopped);
    EXPECT_EQ (seq.currentStep(), -1);

    EXPECT_EQ (seq.process (512, kRate, ev, StepSequencer::kMaxEvents, stopped), 0);
    EXPECT (! stopped);      // only the one block says so

    seq.play();
    EXPECT_EQ (seq.process (512, kRate, ev, StepSequencer::kMaxEvents, stopped), 1);
    EXPECT_EQ (ev[0].step, 0);
    EXPECT_EQ (ev[0].frame, 0);
}

KTEST (StepSequencer, a_tempo_change_applies_from_the_next_step)
{
    StepSequencer seq;
    seq.setTempo (60.0f);      // a sixteenth is 11025 frames
    seq.play();

    auto hits = runClock (seq, 30000, 512);
    EXPECT_EQ ((int) hits.size(), 3);   // 0, 11025, 22050

    seq.setTempo (240.0f);     // 2756.25 frames
    const auto more = runClock (seq, 30000, 512);
    EXPECT (more.size() > 8);

    // Frames below are relative to the second run; the first interval still has the old tempo's
    // remainder, the rest are fast.
    for (size_t k = 2; k < more.size(); ++k)
        EXPECT_NEAR ((double) more[k].frame - (double) more[k - 1].frame, 2756.25, 1.0);
}

KTEST (StepSequencer, the_pattern_api_checks_its_ranges)
{
    StepSequencer seq;
    EXPECT (! seq.cell (0, 0, 0));
    seq.setCell (0, 3, 5, true);
    seq.setCell (7, 127, 15, true);
    EXPECT (seq.cell (0, 3, 5));
    EXPECT (seq.cell (7, 127, 15));
    EXPECT (! seq.cell (0, 3, 4));
    EXPECT_EQ (seq.rowBits (0, 3), 1 << 5);
    EXPECT_EQ (seq.cellCount (0), 1);
    EXPECT_EQ (seq.cellCount (7), 1);
    EXPECT_EQ (seq.cellCount (7, 100), 0);   // row 127 is beyond the first 100

    // Out of range is ignored, never a crash.
    seq.setCell (-1, 0, 0, true);
    seq.setCell (8, 0, 0, true);
    seq.setCell (0, 128, 0, true);
    seq.setCell (0, 0, 16, true);
    seq.setCell (0, -1, 0, true);
    EXPECT (! seq.cell (0, 128, 0));
    EXPECT (! seq.cell (8, 0, 0));
    EXPECT_EQ (seq.rowBits (9, 9), 0);

    seq.setCell (0, 3, 5, false);
    EXPECT (! seq.cell (0, 3, 5));

    seq.setRowBits (2, 10, 0xA5A5);
    EXPECT_EQ (seq.rowBits (2, 10), 0xA5A5);
    EXPECT_EQ (seq.cellCount (2), 8);

    // Settings are clamped.
    seq.setTempo (5.0f);   EXPECT_EQ (seq.tempo(), StepSequencer::kMinTempo);
    seq.setTempo (999.0f); EXPECT_EQ (seq.tempo(), StepSequencer::kMaxTempo);
    seq.setSwing (0.1f);   EXPECT_EQ (seq.swing(), StepSequencer::kMinSwing);
    seq.setSwing (0.9f);   EXPECT_EQ (seq.swing(), StepSequencer::kMaxSwing);
    seq.setLength (0);     EXPECT_EQ (seq.length(), 1);
    seq.setLength (99);    EXPECT_EQ (seq.length(), 16);
    seq.setScene (-3);     EXPECT_EQ (seq.scene(), 0);
    seq.setScene (42);     EXPECT_EQ (seq.scene(), 7);
}

KTEST (StepSequencer, rows_follow_their_slices_when_slices_come_and_go)
{
    StepSequencer seq;

    for (int s = 0; s < 8; ++s)
    {
        seq.setRowBits (s, 0, 0x0001);
        seq.setRowBits (s, 1, 0x0002);
        seq.setRowBits (s, 2, 0x0004);
        seq.setRowBits (s, 127, 0x8000);
    }

    // A slice is inserted before row 1: the old row 1 and 2 become 2 and 3, row 1 is empty, and the
    // last row falls off the end.
    seq.insertRow (1);

    for (int s = 0; s < 8; ++s)
    {
        EXPECT_EQ (seq.rowBits (s, 0), 0x0001);
        EXPECT_EQ (seq.rowBits (s, 1), 0);
        EXPECT_EQ (seq.rowBits (s, 2), 0x0002);
        EXPECT_EQ (seq.rowBits (s, 3), 0x0004);
        EXPECT_EQ (seq.rowBits (s, 127), 0);
    }

    // And it is removed again: everything is back (except what fell off the end).
    seq.removeRow (1);

    for (int s = 0; s < 8; ++s)
    {
        EXPECT_EQ (seq.rowBits (s, 0), 0x0001);
        EXPECT_EQ (seq.rowBits (s, 1), 0x0002);
        EXPECT_EQ (seq.rowBits (s, 2), 0x0004);
        EXPECT_EQ (seq.rowBits (s, 127), 0);
    }

    // Removing a row drops its pattern.
    seq.removeRow (0);
    EXPECT_EQ (seq.rowBits (3, 0), 0x0002);
    EXPECT_EQ (seq.rowBits (3, 1), 0x0004);

    // Bad indices do nothing.
    seq.insertRow (-1);
    seq.removeRow (500);
    EXPECT_EQ (seq.rowBits (3, 0), 0x0002);
}

KTEST (StepSequencer, snapshot_restore_and_copy_scene)
{
    StepSequencer seq;
    seq.setRowBits (0, 5, 0x00F0);
    seq.setRowBits (3, 9, 0x1234);

    const auto grid = seq.snapshot();
    seq.clearAll();
    EXPECT_EQ (seq.cellCount (0), 0);

    seq.restore (grid);
    EXPECT_EQ (seq.rowBits (0, 5), 0x00F0);
    EXPECT_EQ (seq.rowBits (3, 9), 0x1234);

    seq.copyScene (3, 6);
    EXPECT_EQ (seq.rowBits (6, 9), 0x1234);
    EXPECT_EQ (seq.rowBits (3, 9), 0x1234);

    seq.copyScene (3, 3);      // onto itself: nothing happens
    seq.copyScene (3, 99);     // out of range: nothing happens
    EXPECT_EQ (seq.rowBits (3, 9), 0x1234);

    seq.clearScene (6);
    EXPECT_EQ (seq.cellCount (6), 0);
    EXPECT_EQ (seq.cellCount (3), 5);

    // reset(): stops, forgets the patterns and goes back to scene 1, but keeps the person's settings.
    seq.setTempo (90.0f);
    seq.setSwing (0.6f);
    seq.setLength (12);
    seq.setScene (5);
    seq.play();
    seq.reset();
    EXPECT (! seq.isPlaying());
    EXPECT_EQ (seq.cellCount (3), 0);
    EXPECT_EQ (seq.scene(), 0);
    EXPECT_EQ (seq.tempo(), 90.0f);
    EXPECT_EQ (seq.length(), 12);
}

// ---------------------------------------------------------------------------------------------
// the clock driving the slices

KTEST (EngineSequencer, each_lit_step_starts_its_slice_on_the_exact_frame)
{
    Rig r (4, 0.05);   // four slices of 50 ms: 2205 frames, shorter than a step
    auto& seq = r.e.sequencer();

    seq.setTempo (120.0f);
    seq.setCell (0, 0, 0, true);  seq.setCell (0, 0, 8, true);                          // slice 1: steps 1 and 9
    seq.setCell (0, 1, 4, true);                                                         // slice 2: step 5
    for (int s : { 2, 6, 10, 14 }) seq.setCell (0, 2, s, true);                          // slice 3: steps 3 7 11 15
    seq.setCell (0, 3, 15, true);                                                        // slice 4: step 16
    seq.play();

    const auto out = r.render(44100 * 2, 512);
    const auto bursts = findBursts (out);

    struct Want { int step; int slice; };
    std::vector<Want> want = { { 0, 0 }, { 2, 2 }, { 4, 1 }, { 6, 2 }, { 8, 0 }, { 10, 2 }, { 14, 2 }, { 15, 3 } };
    // (sorted by step; step 12 and 13 are empty)

    EXPECT_EQ ((int) bursts.size(), (int) want.size());

    for (size_t i = 0; i < std::min (bursts.size(), want.size()); ++i)
    {
        // The first frame of a note is the fade-in's zero: it is audible from the second one.
        const long long expected = (long long) std::floor ((double) want[i].step * 5512.5) + 1;
        EXPECT_EQ (bursts[i].start, expected);
        EXPECT_NEAR (bursts[i].level, 0.1f * (float) (want[i].slice + 1) * kCentre, 0.002);
    }
}

KTEST (EngineSequencer, the_sound_is_the_same_whatever_the_audio_buffer_is)
{
    auto build = [] (Rig& r)
    {
        auto& seq = r.e.sequencer();
        seq.setTempo (127.0f);
        seq.setSwing (0.62f);
        for (int step = 0; step < 16; step += 2) seq.setCell (0, 0, step, true);
        for (int step = 1; step < 16; step += 3) seq.setCell (0, 1, step, true);
        for (int step = 0; step < 16; step += 5) seq.setCell (0, 2, step, true);
        seq.play();
    };

    Rig reference (3, 0.2);
    build (reference);
    const auto want = reference.render (44100 * 4, 512);

    float energy = 0.0f;
    for (float v : want) energy += std::fabs (v);
    EXPECT (energy > 100.0f);   // it really played something

    for (int block : { 64, 100, 333, 1000, 1024, 2048, 4096 })
    {
        Rig other (3, 0.2);
        build (other);
        const auto got = other.render (44100 * 4, block);

        bool same = got.size() == want.size();
        for (size_t i = 0; same && i < got.size(); ++i)
            same = got[i] == want[i];

        EXPECT (same);
    }
}

KTEST (EngineSequencer, a_one_shot_starts_over_on_every_lit_step)
{
    Rig r (1, 2.0);   // one long slice, so it would play for 2 s if left alone
    auto& seq = r.e.sequencer();
    seq.setTempo (120.0f);
    for (int s = 0; s < 4; ++s) seq.setCell (0, 0, s, true);
    seq.play();

    long long done = 0;

    for (int step = 0; step < 4; ++step)
    {
        // Render up to 100 frames after the step began.
        const long long target = (long long) std::floor ((double) step * 5512.5) + 100;
        r.render ((int) (target - done), 100);
        done = target;

        Engine::Playhead heads[Engine::maxPlayheads];
        const int n = r.e.copyPlayheads (heads, Engine::maxPlayheads);
        EXPECT (n >= 1);

        // The newest voice is only ~100 frames (2.3 ms) into the slice.
        bool fresh = false;

        for (int i = 0; i < n; ++i)
            if (heads[i].seconds > 0.001f && heads[i].seconds < 0.004f)
                fresh = true;

        EXPECT (fresh);
    }
}

KTEST (EngineSequencer, a_loop_slice_is_held_for_a_run_of_steps_and_released_after_it)
{
    Rig r (1, 4.0, Slice::Mode::loop);
    auto& seq = r.e.sequencer();
    seq.setTempo (120.0f);
    for (int s = 2; s <= 5; ++s) seq.setCell (0, 0, s, true);   // a run of four lit steps
    seq.play();

    // Silent until step 3 starts (frame 11025)...
    auto before = r.render (11025, 512);
    float peak = 0.0f;
    for (float v : before) peak = std::max (peak, std::fabs (v));
    EXPECT (peak == 0.0f);
    EXPECT (! r.e.isSliceSounding (0));

    // ...then one held voice whose position only ever moves forward (a retrigger would send it back).
    float last = -1.0f;
    bool wentBack = false;
    int voicesSeen = 0;

    for (int block = 0; block < 40; ++block)   // 40 x 512 frames = 20480: well inside the run
    {
        r.render (512, 512);
        Engine::Playhead heads[Engine::maxPlayheads];
        const int n = r.e.copyPlayheads (heads, Engine::maxPlayheads);
        voicesSeen = std::max (voicesSeen, n);

        if (n == 1)
        {
            if (heads[0].seconds < last) wentBack = true;
            last = heads[0].seconds;
        }
    }

    EXPECT_EQ (voicesSeen, 1);
    EXPECT (! wentBack);
    EXPECT (last > 0.4f);   // ~0.46 s of audio has gone by since it started

    // The run ends at the start of step 7 (frame 33075); the slice has no fade-out, so it is gone
    // within a block of that.
    const long long now = 11025 + 40 * 512;
    r.render ((int) (33075 + 2000 - now), 512);
    EXPECT (! r.e.isSliceSounding (0));
}

KTEST (EngineSequencer, stopping_releases_what_it_started_but_leaves_other_sounds_alone)
{
    Rig r (2, 4.0, Slice::Mode::loop);
    auto& seq = r.e.sequencer();
    for (int s = 0; s < 16; ++s) seq.setCell (0, 0, s, true);    // slice 1 held for the whole pattern
    seq.play();
    r.render (4000, 512);
    EXPECT (r.e.isSliceSounding (0));

    r.e.triggerPreview (1);    // the person auditions slice 2 by hand
    r.render (512, 512);
    EXPECT (r.e.isSliceSounding (1));

    seq.stop();
    r.render (44100 / 4, 512);           // longer than any fade-out
    EXPECT (! r.e.isSliceSounding (0));  // the sequencer's note is gone
    EXPECT (r.e.isSliceSounding (1));    // the hand-played one is not
    EXPECT_EQ (seq.currentStep(), -1);
}

KTEST (EngineSequencer, disabled_slices_and_rows_without_slices_stay_silent)
{
    Rig r (2, 0.05);
    r.e.slices().updateAt (1, [] (Slice& s) { s.enabled = false; });
    auto& seq = r.e.sequencer();

    seq.setCell (0, 1, 0, true);                       // a disabled slice (lit on the downbeat of beats 1 and 3)
    seq.setCell (0, 1, 8, true);
    for (int row = 2; row < 40; ++row)                  // rows with no slice behind them
        seq.setCell (0, row, 0, true);
    seq.play();

    const auto out = r.render (22050, 512);
    float peak = 0.0f;
    for (float v : out) peak = std::max (peak, std::fabs (v));
    EXPECT (peak == 0.0f);

    // Enabling the slice makes the same pattern sound.
    r.e.slices().updateAt (1, [] (Slice& s) { s.enabled = true; });
    const auto later = r.render (44100, 512);   // frames 22050-66150: includes step 9 (frame 44100)
    float peak2 = 0.0f;
    for (float v : later) peak2 = std::max (peak2, std::fabs (v));
    EXPECT (peak2 > 0.1f);
}

KTEST (EngineSequencer, the_scene_that_is_edited_can_differ_from_the_one_that_plays)
{
    Rig r (2, 0.05);
    auto& seq = r.e.sequencer();
    seq.setTempo (120.0f);
    seq.setLength (4);                  // 4 steps = one beat = 22050 frames
    seq.setCell (0, 0, 0, true);        // scene 1: slice 1 on the downbeat
    seq.setCell (1, 1, 0, true);        // scene 2: slice 2 on the downbeat
    seq.play();

    auto first = findBursts (r.render (44100, 512));      // two beats of scene 1
    EXPECT_EQ ((int) first.size(), 2);
    for (auto& b : first) EXPECT_NEAR (b.level, 0.1f * kCentre, 0.002);

    seq.setScene (1);                                     // queued: lands at the next downbeat
    auto second = findBursts (r.render (44100, 512));
    EXPECT_EQ ((int) second.size(), 2);
    for (auto& b : second) EXPECT_NEAR (b.level, 0.2f * kCentre, 0.002);
    EXPECT_EQ (seq.activeScene(), 1);
}

KTEST (EngineSequencer, a_new_sample_stops_it_and_clears_the_patterns_but_keeps_the_tempo)
{
    Rig r (2, 0.05);
    auto& seq = r.e.sequencer();
    seq.setTempo (97.0f);
    seq.setCell (0, 0, 0, true);
    seq.setScene (3);
    seq.play();
    r.render (4096, 512);

    r.e.loadSample (makeLevelsSample (3, 0.1));
    r.render (4096, 512);

    EXPECT (! seq.isPlaying());
    EXPECT_EQ (seq.cellCount (0), 0);
    EXPECT_EQ (seq.scene(), 0);
    EXPECT_EQ (seq.tempo(), 97.0f);
    EXPECT_EQ (seq.currentStep(), -1);
    EXPECT_EQ (r.e.copyPlayheads (nullptr, 0), 0);
}

KTEST (EngineSequencer, editing_everything_while_it_runs_does_not_crash)
{
    Rig r (6, 0.1);
    auto& seq = r.e.sequencer();

    std::atomic<bool> stop { false };
    std::atomic<long> blocks { 0 };

    std::thread audio ([&] {
        std::vector<float> out (256 * 2);
        while (! stop.load())
        {
            r.e.render (out.data(), 256);
            ++blocks;
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds (400);
    int i = 0;

    while (std::chrono::steady_clock::now() < deadline)
    {
        seq.setCell (i % 8, i % 9, i % 16, (i & 1) != 0);
        seq.setTempo (60.0f + (float) (i % 200));
        seq.setSwing (0.5f + 0.01f * (float) (i % 25));
        seq.setLength (1 + i % 16);
        seq.setScene (i % 8);

        if (i % 7 == 0) { if (seq.isPlaying()) seq.stop(); else seq.play(); }
        if (i % 11 == 0) seq.insertRow (i % 6);
        if (i % 13 == 0) seq.removeRow (i % 6);
        if (i % 17 == 0) r.e.slices().updateAt (i % 6, [] (Slice& s) { s.mode = s.mode == Slice::Mode::loop ? Slice::Mode::shot : Slice::Mode::loop; });
        if (i % 19 == 0) r.e.slices().add (Slice { 0.0f, 0.2f });
        if (i % 23 == 0) r.e.slices().removeAt (0);
        if (i % 29 == 0) seq.restore (seq.snapshot());
        if (i % 97 == 0) r.e.loadSample (makeLevelsSample (6, 0.1));

        r.e.collectGarbage();
        (void) seq.currentStep();
        (void) seq.activeScene();
        std::this_thread::sleep_for (std::chrono::microseconds (200));
        ++i;
    }

    stop = true;
    audio.join();
    EXPECT (blocks.load() > 10);
    EXPECT (i > 100);
}
