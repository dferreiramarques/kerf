#include <juce_core/juce_core.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include "../src/Persistence/StatePersistence.h"
#include "../src/ParameterLayout.h"

namespace
{
    // Minimal AudioProcessor stand-in just so APVTS has something to bind to - this test is
    // about StatePersistence's tree building/round-trip, not the real plugin's audio behaviour.
    class DummyProcessor final : public juce::AudioProcessor
    {
    public:
        const juce::String getName() const override { return "Dummy"; }
        void prepareToPlay (double, int) override {}
        void releaseResources() override {}
        void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override {}
        double getTailLengthSeconds() const override { return 0.0; }
        bool acceptsMidi() const override { return true; }
        bool producesMidi() const override { return false; }
        juce::AudioProcessorEditor* createEditor() override { return nullptr; }
        bool hasEditor() const override { return false; }
        int getNumPrograms() override { return 1; }
        int getCurrentProgram() override { return 0; }
        void setCurrentProgram (int) override {}
        const juce::String getProgramName (int) override { return {}; }
        void changeProgramName (int, const juce::String&) override {}
        void getStateInformation (juce::MemoryBlock&) override {}
        void setStateInformation (const void*, int) override {}
    };

    juce::AudioBuffer<float> makeTestTone (int numSamples, int numChannels = 2)
    {
        juce::AudioBuffer<float> buffer (numChannels, numSamples);
        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = 0; i < numSamples; ++i)
                buffer.setSample (ch, i, std::sin ((float) i * 0.1f) * 0.5f);
        return buffer;
    }
}

class PersistenceTests final : public juce::UnitTest
{
public:
    PersistenceTests() : UnitTest ("StatePersistence", "Kerf") {}

    void runTest() override
    {
        beginTest ("round-trips slices, selected index, sample name/rate, and sample audio through a byte stream");
        {
            DummyProcessor dummy;
            juce::AudioProcessorValueTreeState apvts (dummy, nullptr, "PARAMETERS", createParameterLayout());
            apvts.getParameter (ParameterIDs::noteOffset)->setValueNotifyingHost (
                apvts.getParameter (ParameterIDs::noteOffset)->convertTo0to1 (72.0f));

            SliceBank bank;
            Slice a; a.start = 0.1f; a.end = 0.5f; a.pitch = 1.5f; a.volume = 0.8f; a.pan = -0.3f;
            a.enabled = false; a.mode = Slice::Mode::loop; a.fadeInMs = 5.0f; a.fadeOutMs = 25.0f;
            Slice b; b.start = 0.6f; b.end = 1.0f;
            bank.add (a);
            bank.add (b);

            const auto sample = makeTestTone (2000);

            const auto tree = StatePersistence::buildStateTree (apvts, bank, /*selectedSlice*/ 1,
                                                                 "test.wav", &sample, 48000.0);

            // Round-trip through a byte stream, exactly like getStateInformation/setStateInformation do.
            juce::MemoryBlock bytes;
            {
                juce::MemoryOutputStream out (bytes, false);
                tree.writeToStream (out);
            }

            juce::MemoryInputStream in (bytes, false);
            const auto reloadedTree = juce::ValueTree::readFromStream (in);

            DummyProcessor dummy2; // separate processor - two APVTS on the same AudioProcessor would double-register parameters
            juce::AudioProcessorValueTreeState freshApvts (dummy2, nullptr, "PARAMETERS", createParameterLayout());
            const auto restored = StatePersistence::applyStateTree (reloadedTree, freshApvts);

            expectEquals (restored.selectedSlice, 1);
            expectEquals (restored.sampleName, juce::String ("test.wav"));
            expectWithinAbsoluteError (restored.sampleSourceRate, 48000.0, 1.0e-6);

            expectEquals ((int) restored.slices.size(), 2);

            if (restored.slices.size() == 2)
            {
                expectWithinAbsoluteError (restored.slices[0].start, 0.1f, 1.0e-4f);
                expectWithinAbsoluteError (restored.slices[0].end, 0.5f, 1.0e-4f);
                expectWithinAbsoluteError (restored.slices[0].pitch, 1.5f, 1.0e-4f);
                expectWithinAbsoluteError (restored.slices[0].volume, 0.8f, 1.0e-4f);
                expectWithinAbsoluteError (restored.slices[0].pan, -0.3f, 1.0e-4f);
                expect (! restored.slices[0].enabled);
                expect (restored.slices[0].mode == Slice::Mode::loop);
                expectWithinAbsoluteError (restored.slices[0].fadeInMs, 5.0f, 1.0e-4f);
                expectWithinAbsoluteError (restored.slices[0].fadeOutMs, 25.0f, 1.0e-4f);

                expect (restored.slices[1].mode == Slice::Mode::shot);
                expect (restored.slices[1].enabled);
            }

            const auto* noteOffsetParam = freshApvts.getRawParameterValue (ParameterIDs::noteOffset);
            expectWithinAbsoluteError (noteOffsetParam->load(), 72.0f, 0.5f);

            expectEquals (restored.sampleBuffer.getNumChannels(), 2);
            expectEquals (restored.sampleBuffer.getNumSamples(), 2000);

            // FLAC at 16 bits is lossy relative to the original float buffer - check it's close,
            // not bit-exact.
            float maxError = 0.0f;
            for (int i = 0; i < 2000; ++i)
                maxError = juce::jmax (maxError, std::abs (restored.sampleBuffer.getSample (0, i) - sample.getSample (0, i)));

            expect (maxError < 0.01f);
        }

        beginTest ("a tree with no sample loaded round-trips with an empty buffer and no slices");
        {
            DummyProcessor dummy;
            juce::AudioProcessorValueTreeState apvts (dummy, nullptr, "PARAMETERS", createParameterLayout());
            SliceBank bank;

            const auto tree = StatePersistence::buildStateTree (apvts, bank, 0, {}, nullptr, 44100.0);

            DummyProcessor dummy2;
            juce::AudioProcessorValueTreeState freshApvts (dummy2, nullptr, "PARAMETERS", createParameterLayout());
            const auto restored = StatePersistence::applyStateTree (tree, freshApvts);

            expectEquals (restored.sampleBuffer.getNumSamples(), 0);
            expectEquals ((int) restored.slices.size(), 0);
        }
    }
};

static PersistenceTests persistenceTests;
