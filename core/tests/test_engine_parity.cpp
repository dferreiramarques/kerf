// Ports of the VST3 plugin's juce::UnitTest suites (plugin/tests/EngineTests.cpp) to the
// JUCE-free core. The assertions are kept as close as possible to the originals on purpose: they
// are the spec the engine was verified against by ear in a DAW.

#include <vector>

#include "minitest.h"
#include "slice.h"
#include "slice_bank.h"
#include "transient_detector.h"
#include "voice.h"
#include "voice_engine.h"

using namespace kerf;

namespace
{
    // A ramp buffer (sample[i] == i) makes linear-interpolated reads trivially predictable:
    // reading at position p gives back p exactly.
    AudioBuffer makeRampBuffer (int numSamples, int numChannels = 1)
    {
        AudioBuffer buffer (numChannels, numSamples);

        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.set (ch, i, (float) i);

        return buffer;
    }
}

// ---------------------------------------------------------------------------------------------
// SliceBank

KTEST (SliceBank, starts_empty)
{
    SliceBank bank;
    EXPECT_EQ (bank.size(), 0);
}

KTEST (SliceBank, add_removeAt_clear)
{
    SliceBank bank;
    Slice a; a.start = 0.0f; a.end = 1.0f;
    Slice b; b.start = 1.0f; b.end = 2.0f;

    bank.add (a);
    const auto secondIndex = bank.add (b);
    EXPECT_EQ (bank.size(), 2);
    EXPECT_EQ (secondIndex, 1);

    bank.removeAt (0);
    EXPECT_EQ (bank.size(), 1);
    EXPECT_NEAR (bank.getCurrent()->at (0).start, 1.0f, 1.0e-6f);

    bank.clear();
    EXPECT_EQ (bank.size(), 0);
}

KTEST (SliceBank, updateAt_mutates_only_the_targeted_slice)
{
    SliceBank bank;
    Slice a; a.pitch = 1.0f;
    Slice b; b.pitch = 2.0f;
    bank.add (a);
    bank.add (b);

    bank.updateAt (0, [] (Slice& s) { s.pitch = 5.0f; });

    const auto snap = bank.getCurrent();
    EXPECT_NEAR (snap->at (0).pitch, 5.0f, 1.0e-6f);
    EXPECT_NEAR (snap->at (1).pitch, 2.0f, 1.0e-6f);
}

KTEST (SliceBank, published_snapshots_are_immutable)
{
    SliceBank bank;
    Slice a; a.pitch = 1.0f;
    bank.add (a);

    auto oldSnapshot = bank.getCurrent();
    bank.updateAt (0, [] (Slice& s) { s.pitch = 99.0f; });

    EXPECT_NEAR (oldSnapshot->at (0).pitch, 1.0f, 1.0e-6f);
    EXPECT_NEAR (bank.getCurrent()->at (0).pitch, 99.0f, 1.0e-6f);
}

KTEST (SliceBank, out_of_range_edits_are_ignored)
{
    SliceBank bank;
    bank.add (Slice {});
    bank.removeAt (-1);
    bank.removeAt (5);
    bank.updateAt (7, [] (Slice& s) { s.pitch = 3.0f; });
    EXPECT_EQ (bank.size(), 1);
    EXPECT_NEAR (bank.getCurrent()->at (0).pitch, 1.0f, 1.0e-6f);
}

// ---------------------------------------------------------------------------------------------
// TransientDetector

KTEST (TransientDetector, silence_produces_no_slices)
{
    std::vector<float> data (2000, 0.0f);
    const auto slices = TransientDetector::detect (data.data(), (int) data.size(), 1000.0, 1.0f);
    EXPECT_EQ ((int) slices.size(), 0);
}

KTEST (TransientDetector, a_single_isolated_jump_followed_by_silence_produces_one_slice)
{
    // 1000Hz: minDuration=200 samples, minSliceLength=150 samples. A burst from 100 to 400 should
    // yield exactly one slice from ~0.1s to ~0.4s.
    std::vector<float> data (2000, 0.0f);
    for (int i = 100; i < 400; ++i)
        data[(size_t) i] = (i % 2 == 0) ? 1.0f : -1.0f;

    const auto slices = TransientDetector::detect (data.data(), (int) data.size(), 1000.0, 1.0f);
    EXPECT_EQ ((int) slices.size(), 1);

    if (! slices.empty())
    {
        EXPECT_NEAR (slices[0].start, 0.1f, 0.01f);
        EXPECT_NEAR (slices[0].end, 0.4f, 0.01f);
        EXPECT (slices[0].mode == Slice::Mode::shot);
        EXPECT_NEAR (slices[0].pitch, 1.0f, 1.0e-6f);
    }
}

KTEST (TransientDetector, a_brief_blip_keeps_its_slice_open_until_minDuration)
{
    // Quirk inherited from kerf.html on purpose: minDuration (200ms) gates *closing* a slice and
    // is always larger than minSliceLength (150ms), so even a 20-sample blip yields a ~0.2s slice.
    std::vector<float> data (2000, 0.0f);
    for (int i = 100; i < 120; ++i)
        data[(size_t) i] = (i % 2 == 0) ? 1.0f : -1.0f;

    const auto slices = TransientDetector::detect (data.data(), (int) data.size(), 1000.0, 1.0f);
    EXPECT_EQ ((int) slices.size(), 1);

    if (! slices.empty())
        EXPECT_NEAR (slices[0].end - slices[0].start, 0.201f, 0.01f);
}

// ---------------------------------------------------------------------------------------------
// Voice

KTEST (Voice, shot_mode_pitch1_hard_left_reproduces_the_ramp)
{
    auto sample = makeRampBuffer (200);

    Slice slice;
    slice.start = 0.0f;
    slice.end = 0.1f; // 100 samples at 1000Hz
    slice.pitch = 1.0f;
    slice.volume = 1.0f;
    slice.pan = -1.0f; // hard left: leftGain=1, rightGain=0
    slice.fadeInMs = 0.0f;
    slice.fadeOutMs = 0.0f;
    slice.mode = Slice::Mode::shot;

    Voice voice;
    voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);

    AudioBuffer out (2, 10);
    voice.renderNextBlock (out, 0, 10);

    // Sample 0 is degenerate (the 1-sample minimum fade-in is still ramping from 0).
    for (int i = 1; i < 10; ++i)
        EXPECT_NEAR (out.get (0, i), (float) i, 0.01f);

    for (int i = 0; i < 10; ++i)
        EXPECT_NEAR (out.get (1, i), 0.0f, 1.0e-6f);
}

KTEST (Voice, shot_mode_stops_exactly_at_the_slice_end)
{
    auto sample = makeRampBuffer (200);
    Slice slice;
    slice.start = 0.0f;
    slice.end = 0.01f; // 10 samples
    slice.fadeInMs = 0.0f;
    slice.fadeOutMs = 0.0f;

    Voice voice;
    voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);
    EXPECT (voice.isActive());

    AudioBuffer out (2, 20);
    voice.renderNextBlock (out, 0, 20);

    EXPECT (! voice.isActive());
}

KTEST (Voice, loop_mode_keeps_playing_past_the_end_and_wraps)
{
    auto sample = makeRampBuffer (200);
    Slice slice;
    slice.start = 0.0f;
    slice.end = 0.01f; // 10-sample loop
    slice.pan = -1.0f;
    slice.fadeInMs = 0.0f;
    slice.fadeOutMs = 0.0f;
    slice.mode = Slice::Mode::loop;

    Voice voice;
    voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);

    AudioBuffer out (2, 25);
    voice.renderNextBlock (out, 0, 25);

    EXPECT (voice.isActive());
    EXPECT (out.get (0, 10) < 5.0f); // wrapped back near the start instead of climbing to 10
}

KTEST (Voice, release_fades_to_silence_then_goes_idle)
{
    auto sample = makeRampBuffer (200);
    Slice slice;
    slice.start = 0.0f;
    slice.end = 1.0f;
    slice.fadeInMs = 0.0f;
    slice.fadeOutMs = 10.0f; // 10 samples at 1000Hz

    Voice voice;
    voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);

    AudioBuffer warmup (2, 5);
    voice.renderNextBlock (warmup, 0, 5);

    voice.release();
    EXPECT (voice.isActive());

    AudioBuffer tail (2, 15);
    voice.renderNextBlock (tail, 0, 15);

    EXPECT (! voice.isActive());
}

KTEST (Voice, pitchRatio_doubles_the_read_rate)
{
    auto sample = makeRampBuffer (200);
    Slice slice;
    slice.start = 0.0f;
    slice.end = 0.1f;
    slice.pan = -1.0f;
    slice.fadeInMs = 0.0f;
    slice.fadeOutMs = 0.0f;

    Voice voice;
    voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 2.0f);

    AudioBuffer out (2, 5);
    voice.renderNextBlock (out, 0, 5);

    for (int i = 1; i < 5; ++i)
        EXPECT_NEAR (out.get (0, i), (float) (2 * i), 0.05f);
}

KTEST (Voice, sample_rate_mismatch_is_compensated_without_changing_musical_pitch)
{
    auto sample = makeRampBuffer (500);
    Slice slice;
    slice.start = 0.0f;
    slice.end = 1.0f;
    slice.pan = -1.0f;
    slice.fadeInMs = 0.0f;
    slice.fadeOutMs = 0.0f;

    Voice voice;
    voice.start (slice, 0, 60, &sample, 48000.0, 44100.0, 1.0f);

    AudioBuffer out (2, 5);
    voice.renderNextBlock (out, 0, 5);

    const auto expectedRatio = 48000.0f / 44100.0f;
    for (int i = 1; i < 5; ++i)
        EXPECT_NEAR (out.get (0, i), (float) i * expectedRatio, 0.05f);
}

KTEST (Voice, reports_playhead_position_in_seconds)
{
    auto sample = makeRampBuffer (2000);
    Slice slice;
    slice.start = 0.5f;
    slice.end = 1.5f;
    slice.fadeInMs = 0.0f;

    Voice voice;
    voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);
    EXPECT_NEAR (voice.getPositionSeconds(), 0.5, 1.0e-9);

    AudioBuffer out (2, 100);
    voice.renderNextBlock (out, 0, 100);
    EXPECT_NEAR (voice.getPositionSeconds(), 0.6, 1.0e-6);
}

KTEST (Voice, a_mono_sample_plays_on_both_channels)
{
    auto sample = makeRampBuffer (200, 1);
    Slice slice;
    slice.start = 0.0f;
    slice.end = 0.1f;
    slice.pan = 0.0f; // centre
    slice.fadeInMs = 0.0f;
    slice.fadeOutMs = 0.0f;

    Voice voice;
    voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);

    AudioBuffer out (2, 10);
    voice.renderNextBlock (out, 0, 10);

    for (int i = 1; i < 10; ++i)
    {
        EXPECT_NEAR (out.get (0, i), out.get (1, i), 1.0e-5f);
        EXPECT (out.get (0, i) > 0.0f);
    }
}

// ---------------------------------------------------------------------------------------------
// VoiceEngine

KTEST (VoiceEngine, trigger_mode_maps_note_offset_to_slice_and_ignores_note_pitch)
{
    auto sample = makeRampBuffer (2000);
    SliceBank bank;
    Slice s0; s0.start = 0.0f; s0.end = 0.5f; s0.pitch = 1.0f;
    Slice s1; s1.start = 0.0f; s1.end = 0.5f; s1.pitch = 3.0f; // distinctive pitch identifies which slice fired
    bank.add (s0);
    bank.add (s1);

    VoiceEngine engine;
    engine.prepare (1000.0);
    engine.setSampleSource (&sample, 1000.0);

    const auto snapshot = bank.getCurrent();

    // noteOffset=60, note=61 -> slice 1 at its own pitch (3.0), NOT transposed by the note number.
    engine.noteOn (VoiceEngine::MidiMode::trigger, 60, -1, 61, snapshot);

    EXPECT (engine.isSliceActive (1));
    EXPECT (! engine.isSliceActive (0));

    AudioBuffer out (2, 5);
    engine.renderNextBlock (out, 0, 5);
    EXPECT_NEAR (out.get (0, 1), 3.0f * 0.7071f, 0.05f); // pan=0 -> ~0.707 per channel

    engine.noteOn (VoiceEngine::MidiMode::trigger, 60, -1, 200, snapshot); // out of range, ignored
    EXPECT_EQ (engine.getNumActiveVoices(), 1);
}

KTEST (VoiceEngine, trigger_mode_note_off_releases_the_matching_voice)
{
    auto sample = makeRampBuffer (2000);
    SliceBank bank;
    Slice s0; s0.start = 0.0f; s0.end = 1.0f; s0.fadeOutMs = 1.0f;
    bank.add (s0);

    VoiceEngine engine;
    engine.prepare (1000.0);
    engine.setSampleSource (&sample, 1000.0);
    const auto snapshot = bank.getCurrent();

    engine.noteOn (VoiceEngine::MidiMode::trigger, 60, -1, 60, snapshot);
    EXPECT (engine.isSliceActive (0));

    engine.noteOff (VoiceEngine::MidiMode::trigger, 60, 60);

    AudioBuffer out (2, 10);
    engine.renderNextBlock (out, 0, 10);

    EXPECT (! engine.isSliceActive (0));
}

KTEST (VoiceEngine, sampler_mode_transposes_the_selected_slice_and_is_polyphonic)
{
    auto sample = makeRampBuffer (2000);
    SliceBank bank;
    Slice s0; s0.start = 0.0f; s0.end = 1.0f; s0.pitch = 1.0f; s0.fadeOutMs = 1.0f;
    bank.add (s0);

    VoiceEngine engine;
    engine.prepare (1000.0);
    engine.setSampleSource (&sample, 1000.0);
    const auto snapshot = bank.getCurrent();

    engine.noteOn (VoiceEngine::MidiMode::sampler, 60, 0, 60, snapshot);
    engine.noteOn (VoiceEngine::MidiMode::sampler, 60, 0, 72, snapshot);

    EXPECT_EQ (engine.getNumActiveVoices(), 2);
    EXPECT (engine.isNoteActive (60));
    EXPECT (engine.isNoteActive (72));

    engine.noteOff (VoiceEngine::MidiMode::sampler, 60, 60);
    AudioBuffer out (2, 10);
    engine.renderNextBlock (out, 0, 10);
    EXPECT (! engine.isNoteActive (60));
    EXPECT (engine.isNoteActive (72));
}

KTEST (VoiceEngine, sampler_mode_octave_up_reads_twice_as_fast)
{
    auto sample = makeRampBuffer (4000);
    SliceBank bank;
    Slice s0; s0.start = 0.0f; s0.end = 3.0f; s0.pan = -1.0f; s0.fadeInMs = 0.0f;
    bank.add (s0);

    VoiceEngine engine;
    engine.prepare (1000.0);
    engine.setSampleSource (&sample, 1000.0);

    engine.noteOn (VoiceEngine::MidiMode::sampler, 60, 0, 72, bank.getCurrent());

    AudioBuffer out (2, 6);
    engine.renderNextBlock (out, 0, 6);

    for (int i = 1; i < 6; ++i)
        EXPECT_NEAR (out.get (0, i), (float) (2 * i), 0.05f);
}

KTEST (VoiceEngine, retriggering_the_same_slice_crossfades_instead_of_cutting)
{
    auto sample = makeRampBuffer (2000);
    SliceBank bank;
    Slice s0; s0.start = 0.0f; s0.end = 1.0f; s0.fadeOutMs = 20.0f;
    bank.add (s0);

    VoiceEngine engine;
    engine.prepare (1000.0);
    engine.setSampleSource (&sample, 1000.0);
    const auto snapshot = bank.getCurrent();

    engine.triggerPreview (0, snapshot->at (0));
    EXPECT_EQ (engine.getNumActiveVoices(), 1);

    engine.retrigger (0, snapshot->at (0));
    EXPECT_EQ (engine.getNumActiveVoices(), 2); // old (releasing) + new coexist briefly

    AudioBuffer out (2, 30);
    engine.renderNextBlock (out, 0, 30);

    EXPECT_EQ (engine.getNumActiveVoices(), 1); // only the new voice remains
}

KTEST (VoiceEngine, disabled_slices_do_not_sound)
{
    auto sample = makeRampBuffer (2000);
    SliceBank bank;
    Slice s0; s0.start = 0.0f; s0.end = 1.0f; s0.enabled = false;
    bank.add (s0);

    VoiceEngine engine;
    engine.prepare (1000.0);
    engine.setSampleSource (&sample, 1000.0);

    engine.noteOn (VoiceEngine::MidiMode::trigger, 60, -1, 60, bank.getCurrent());
    EXPECT_EQ (engine.getNumActiveVoices(), 0);
}

KTEST (VoiceEngine, voice_pool_steals_instead_of_dropping_when_full)
{
    auto sample = makeRampBuffer (4000);
    SliceBank bank;
    for (int i = 0; i < 40; ++i)
    {
        Slice s; s.start = 0.0f; s.end = 3.0f;
        bank.add (s);
    }

    VoiceEngine engine;
    engine.prepare (1000.0);
    engine.setSampleSource (&sample, 1000.0);
    const auto snapshot = bank.getCurrent();

    for (int i = 0; i < 40; ++i)
        engine.noteOn (VoiceEngine::MidiMode::trigger, 0, -1, i, snapshot);

    EXPECT_EQ (engine.getNumActiveVoices(), VoiceEngine::maxVoices);
    EXPECT (engine.isSliceActive (39)); // the newest note is not the one that got dropped
}
