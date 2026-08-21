#include <juce_core/juce_core.h>
#include <juce_audio_basics/juce_audio_basics.h>
#include "../src/Slice.h"
#include "../src/SliceBank.h"
#include "../src/Voice.h"
#include "../src/VoiceEngine.h"
#include "../src/TransientDetector.h"

namespace
{
    // A ramp buffer (sample[i] == i) makes linear-interpolated reads trivially predictable:
    // reading at position p gives back p exactly, so pitch/position math can be checked directly
    // against the output values instead of needing to inspect Voice's private read position.
    juce::AudioBuffer<float> makeRampBuffer (int numSamples, int numChannels = 1)
    {
        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, (float) i);
        return buffer;
    }
}

class SliceBankTests final : public juce::UnitTest
{
public:
    SliceBankTests() : UnitTest ("SliceBank", "Kerf") {}

    void runTest() override
    {
        beginTest ("starts empty");
        {
            SliceBank bank;
            expectEquals (bank.size(), 0);
        }

        beginTest ("add/removeAt/clear");
        {
            SliceBank bank;
            Slice a; a.start = 0.0f; a.end = 1.0f;
            Slice b; b.start = 1.0f; b.end = 2.0f;

            bank.add (a);
            const auto secondIndex = bank.add (b);
            expectEquals (bank.size(), 2);
            expectEquals (secondIndex, 1);

            bank.removeAt (0);
            expectEquals (bank.size(), 1);
            expectWithinAbsoluteError (bank.getCurrent()->at (0).start, 1.0f, 1.0e-6f);

            bank.clear();
            expectEquals (bank.size(), 0);
        }

        beginTest ("updateAt mutates only the targeted slice");
        {
            SliceBank bank;
            Slice a; a.pitch = 1.0f;
            Slice b; b.pitch = 2.0f;
            bank.add (a);
            bank.add (b);

            bank.updateAt (0, [] (Slice& s) { s.pitch = 5.0f; });

            const auto snap = bank.getCurrent();
            expectWithinAbsoluteError (snap->at (0).pitch, 5.0f, 1.0e-6f);
            expectWithinAbsoluteError (snap->at (1).pitch, 2.0f, 1.0e-6f);
        }

        beginTest ("published snapshots are immutable - an old snapshot survives a later mutation");
        {
            SliceBank bank;
            Slice a; a.pitch = 1.0f;
            bank.add (a);

            auto oldSnapshot = bank.getCurrent();
            bank.updateAt (0, [] (Slice& s) { s.pitch = 99.0f; });

            expectWithinAbsoluteError (oldSnapshot->at (0).pitch, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (bank.getCurrent()->at (0).pitch, 99.0f, 1.0e-6f);
        }
    }
};

class TransientDetectorTests final : public juce::UnitTest
{
public:
    TransientDetectorTests() : UnitTest ("TransientDetector", "Kerf") {}

    void runTest() override
    {
        beginTest ("silence produces no slices");
        {
            std::vector<float> data (2000, 0.0f);
            const auto slices = TransientDetector::detect (data.data(), (int) data.size(), 1000.0, 1.0f);
            expectEquals ((int) slices.size(), 0);
        }

        beginTest ("a single isolated jump followed by silence produces one slice");
        {
            // 1000Hz sample rate: minDuration=200 samples, minSliceLength=150 samples. A jump at
            // sample 100 held above threshold for 300 samples (long enough to close and keep),
            // then back to flat, should yield exactly one slice from ~100 to ~400.
            std::vector<float> data (2000, 0.0f);
            for (int i = 100; i < 400; ++i)
                data[(size_t) i] = (i % 2 == 0) ? 1.0f : -1.0f; // alternating -> large sample-to-sample delta

            const auto slices = TransientDetector::detect (data.data(), (int) data.size(), 1000.0, 1.0f);
            expectEquals ((int) slices.size(), 1);

            if (! slices.empty())
            {
                expectWithinAbsoluteError (slices[0].start, 0.1f, 0.01f);
                expectWithinAbsoluteError (slices[0].end, 0.4f, 0.01f);
                expect (slices[0].mode == Slice::Mode::shot);
                expectWithinAbsoluteError (slices[0].pitch, 1.0f, 1.0e-6f);
            }
        }

        beginTest ("a brief blip doesn't close its slice until minDuration has elapsed since sliceStart");
        {
            // minDuration (200 samples @ 1000Hz) gates *closing* a slice, not how long the
            // transient itself needs to last - even a 20-sample blip keeps the slice open until
            // enough time has passed since it started, so the resulting slice ends up ~200
            // samples long, not 20. Since minDuration (200) is hardcoded larger than
            // minSliceLength (150) for any sample rate, the `> minSliceLength` rejection in the
            // original (kerf.html:3183) can never actually reject anything once a slice reaches
            // its close condition - a quirk of the original worth knowing, not something to
            // "fix" in a faithful port.
            std::vector<float> data (2000, 0.0f);
            for (int i = 100; i < 120; ++i) // only 20 samples of actual signal
                data[(size_t) i] = (i % 2 == 0) ? 1.0f : -1.0f;

            const auto slices = TransientDetector::detect (data.data(), (int) data.size(), 1000.0, 1.0f);
            expectEquals ((int) slices.size(), 1);

            if (! slices.empty())
                expectWithinAbsoluteError (slices[0].end - slices[0].start, 0.201f, 0.01f);
        }
    }
};

class VoiceTests final : public juce::UnitTest
{
public:
    VoiceTests() : UnitTest ("Voice", "Kerf") {}

    void runTest() override
    {
        beginTest ("shot mode: pitch=1, hard-left pan reproduces the ramp source on the left channel");
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
            voice.start (slice, /*sliceIndex*/ 0, /*midiNote*/ 60, &sample, /*sourceRate*/ 1000.0, /*outputRate*/ 1000.0, /*pitchRatio*/ 1.0f);

            juce::AudioBuffer<float> out (2, 10);
            out.clear();
            voice.renderNextBlock (out, 0, 10);

            // Sample 0 is degenerate (1-sample-minimum fade-in still ramping from 0), check 1..9.
            for (int i = 1; i < 10; ++i)
                expectWithinAbsoluteError (out.getSample (0, i), (float) i, 0.01f);

            for (int i = 0; i < 10; ++i)
                expectWithinAbsoluteError (out.getSample (1, i), 0.0f, 1.0e-6f);
        }

        beginTest ("shot mode stops exactly at the slice end");
        {
            auto sample = makeRampBuffer (200);
            Slice slice;
            slice.start = 0.0f;
            slice.end = 0.01f; // 10 samples
            slice.pitch = 1.0f;
            slice.fadeInMs = 0.0f;
            slice.fadeOutMs = 0.0f;
            slice.mode = Slice::Mode::shot;

            Voice voice;
            voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);
            expect (voice.isActive());

            juce::AudioBuffer<float> out (2, 20);
            out.clear();
            voice.renderNextBlock (out, 0, 20);

            expect (! voice.isActive());
        }

        beginTest ("loop mode keeps playing past the slice end and wraps");
        {
            auto sample = makeRampBuffer (200);
            Slice slice;
            slice.start = 0.0f;
            slice.end = 0.01f; // 10-sample loop
            slice.pitch = 1.0f;
            slice.pan = -1.0f;
            slice.fadeInMs = 0.0f;
            slice.fadeOutMs = 0.0f;
            slice.mode = Slice::Mode::loop;

            Voice voice;
            voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);

            juce::AudioBuffer<float> out (2, 25);
            out.clear();
            voice.renderNextBlock (out, 0, 25);

            expect (voice.isActive()); // still going, unlike shot mode
            // Sample 10 should have wrapped back to source position ~0, not kept climbing to 10.
            expect (out.getSample (0, 10) < 5.0f);
        }

        beginTest ("release() fades to silence over fadeOutMs then goes idle");
        {
            auto sample = makeRampBuffer (200);
            Slice slice;
            slice.start = 0.0f;
            slice.end = 1.0f; // long enough not to hit natural end during this test
            slice.pitch = 1.0f;
            slice.volume = 1.0f;
            slice.fadeInMs = 0.0f;
            slice.fadeOutMs = 10.0f; // 10 samples at 1000Hz
            slice.mode = Slice::Mode::shot;

            Voice voice;
            voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, 1.0f);

            juce::AudioBuffer<float> warmup (2, 5);
            warmup.clear();
            voice.renderNextBlock (warmup, 0, 5); // get past the fade-in

            voice.release();
            expect (voice.isActive());

            juce::AudioBuffer<float> tail (2, 15);
            tail.clear();
            voice.renderNextBlock (tail, 0, 15);

            expect (! voice.isActive());
        }

        beginTest ("pitchRatio doubles the source read rate");
        {
            auto sample = makeRampBuffer (200);
            Slice slice;
            slice.start = 0.0f;
            slice.end = 0.1f;
            slice.pitch = 1.0f;
            slice.pan = -1.0f;
            slice.fadeInMs = 0.0f;
            slice.fadeOutMs = 0.0f;
            slice.mode = Slice::Mode::shot;

            Voice voice;
            voice.start (slice, 0, 60, &sample, 1000.0, 1000.0, /*pitchRatio*/ 2.0f);

            juce::AudioBuffer<float> out (2, 5);
            out.clear();
            voice.renderNextBlock (out, 0, 5);

            // At 2x pitch, output sample i should read source position ~2*i (skipping sample 0,
            // still fade-degenerate as above).
            for (int i = 1; i < 5; ++i)
                expectWithinAbsoluteError (out.getSample (0, i), (float) (2 * i), 0.05f);
        }

        beginTest ("source/output sample rate mismatch is compensated automatically, without changing musical pitch");
        {
            // A 48kHz sample played back in a 44.1kHz host, with the slice's own pitch left at
            // 1.0 (no transposition requested) - the engine must still read the source ~1.088x
            // faster per output sample to keep the musical pitch correct, purely to account for
            // the rate difference. This is the same mechanism that keeps a WAV's own bit depth
            // irrelevant too (JUCE's AudioFormatReader always normalises to float on read).
            auto sample = makeRampBuffer (500);
            Slice slice;
            slice.start = 0.0f;
            slice.end = 1.0f; // long enough not to hit the natural end during this test
            slice.pitch = 1.0f;
            slice.pan = -1.0f;
            slice.fadeInMs = 0.0f;
            slice.fadeOutMs = 0.0f;
            slice.mode = Slice::Mode::shot;

            Voice voice;
            voice.start (slice, 0, 60, &sample, /*sourceSampleRate*/ 48000.0, /*outputSampleRate*/ 44100.0, /*pitchRatio*/ 1.0f);

            juce::AudioBuffer<float> out (2, 5);
            out.clear();
            voice.renderNextBlock (out, 0, 5);

            const auto expectedRatio = 48000.0f / 44100.0f;

            for (int i = 1; i < 5; ++i)
                expectWithinAbsoluteError (out.getSample (0, i), (float) i * expectedRatio, 0.05f);
        }
    }
};

class VoiceEngineTests final : public juce::UnitTest
{
public:
    VoiceEngineTests() : UnitTest ("VoiceEngine", "Kerf") {}

    void runTest() override
    {
        beginTest ("trigger mode maps note-offset to slice index, ignores note-based pitch");
        {
            auto sample = makeRampBuffer (2000);
            SliceBank bank;
            Slice s0; s0.start = 0.0f; s0.end = 0.5f; s0.pitch = 1.0f;
            Slice s1; s1.start = 0.0f; s1.end = 0.5f; s1.pitch = 3.0f; // distinctive pitch to identify which slice fired
            bank.add (s0);
            bank.add (s1);

            VoiceEngine engine;
            engine.prepare (1000.0);
            engine.setSampleSource (&sample, 1000.0);

            const auto snapshot = bank.getCurrent();

            // noteOffset=60, note=61 -> relativeNote=1 -> should trigger slice 1, at its own
            // pitch (3.0), NOT transposed by the note number (confirmed against kerf.html: trigger
            // mode calls playSlice(relativeNote) with no midiNote argument - "SEM pitch").
            engine.noteOn (VoiceEngine::MidiMode::trigger, 60, -1, 61, snapshot);

            expect (engine.isSliceActive (1));
            expect (! engine.isSliceActive (0));

            juce::AudioBuffer<float> out (2, 5);
            out.clear();
            engine.renderNextBlock (out, 0, 5);
            // Sample 1 onward should read at pitch ratio 3.0 (slice 1's own pitch), not e.g. the
            // 2^((61-60)/12) semitone ratio a note-based transpose would have produced.
            expectWithinAbsoluteError (out.getSample (0, 1), 3.0f * 0.7071f, 0.05f); // pan=0 default -> ~0.707 per channel

            engine.noteOn (VoiceEngine::MidiMode::trigger, 60, -1, 200, snapshot); // out of range, ignored
            expectEquals (engine.getNumActiveVoices(), 1);
        }

        beginTest ("trigger mode note-off releases the matching slice's voice");
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
            expect (engine.isSliceActive (0));

            engine.noteOff (VoiceEngine::MidiMode::trigger, 60, 60);

            juce::AudioBuffer<float> out (2, 10);
            out.clear();
            engine.renderNextBlock (out, 0, 10); // long enough for the 1-sample release to finish

            expect (! engine.isSliceActive (0));
        }

        beginTest ("sampler mode transposes the selected slice per-note and is polyphonic across notes");
        {
            auto sample = makeRampBuffer (2000);
            SliceBank bank;
            Slice s0; s0.start = 0.0f; s0.end = 1.0f; s0.pitch = 1.0f; s0.fadeOutMs = 1.0f; // fast release for this test
            bank.add (s0);

            VoiceEngine engine;
            engine.prepare (1000.0);
            engine.setSampleSource (&sample, 1000.0);
            const auto snapshot = bank.getCurrent();

            // C4=60 is the reference: same pitch as the slice's own stored pitch.
            engine.noteOn (VoiceEngine::MidiMode::sampler, 60, /*selectedSlice*/ 0, 60, snapshot);
            // One octave up: pitch ratio should be 2.0x.
            engine.noteOn (VoiceEngine::MidiMode::sampler, 60, 0, 72, snapshot);

            expectEquals (engine.getNumActiveVoices(), 2); // polyphonic - both notes sound at once
            expect (engine.isNoteActive (60));
            expect (engine.isNoteActive (72));

            engine.noteOff (VoiceEngine::MidiMode::sampler, 60, 60);
            juce::AudioBuffer<float> out (2, 10);
            out.clear();
            engine.renderNextBlock (out, 0, 10); // >= fadeOutMs (1 sample) so the release actually completes
            expect (! engine.isNoteActive (60));
            expect (engine.isNoteActive (72));
        }

        beginTest ("retriggering the same slice crossfades rather than cutting instantly");
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
            expectEquals (engine.getNumActiveVoices(), 1);

            engine.retrigger (0, snapshot->at (0));
            // Immediately after retrigger, the old (releasing) voice and the new one briefly
            // coexist - matches kerf.html's stopSlice()+playSlice(), which crossfades.
            expectEquals (engine.getNumActiveVoices(), 2);

            juce::AudioBuffer<float> out (2, 30);
            out.clear();
            engine.renderNextBlock (out, 0, 30); // long enough for the 20-sample release to finish

            expectEquals (engine.getNumActiveVoices(), 1); // only the new voice remains
        }
    }
};

static SliceBankTests sliceBankTests;
static TransientDetectorTests transientDetectorTests;
static VoiceTests voiceTests;
static VoiceEngineTests voiceEngineTests;
