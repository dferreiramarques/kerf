#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <array>
#include <atomic>
#include <functional>
#include "SliceBank.h"
#include "VoiceEngine.h"
#include "ParameterLayout.h"

class KerfAudioProcessor final : public juce::AudioProcessor
{
public:
    KerfAudioProcessor();
    ~KerfAudioProcessor() override = default;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    // Message-thread API, called from the (for now, native/temporary) editor.

    // Returns false (and leaves the current sample untouched) if the file can't be read.
    bool loadSampleFile (const juce::File& file);
    juce::String getLoadedSampleName() const { return loadedSampleName; }
    double getSampleDurationSeconds() const;

    // Synchronous for now (Phase 2's native GUI); Phase 4's WebBridge should move this to a
    // background thread so a long sample doesn't stall the message thread on click.
    void detectTransients (float sensitivity);

    int addSlice (float startSec, float endSec);
    void deleteSlice (int index);
    void clearSlices();
    void setSliceParam (int index, const std::function<void (Slice&)>& mutator, bool liveRetrigger);

    void triggerPreview (int sliceIndex);
    void stopPreview (int sliceIndex);

    void setSelectedSliceForEditing (int index) { selectedSliceForEditing.store (index); }
    int getSelectedSliceForEditing() const { return selectedSliceForEditing.load(); }

    SliceBank& getSliceBank() { return sliceBank; }
    juce::AudioProcessorValueTreeState& getAPVTS() { return apvts; }
    const VoiceEngine& getVoiceEngine() const { return voiceEngine; }

    // For the waveform display only - message-thread read access to the loaded sample.
    const juce::AudioBuffer<float>* getSampleBufferForDisplay() const
    {
        return sampleBuffer.getNumSamples() > 0 ? &sampleBuffer : nullptr;
    }
    double getSampleSourceRate() const { return sampleSourceRate; }

private:
    struct VoiceCommand
    {
        enum class Type { triggerPreview, stopPreview, retrigger };
        Type type;
        int sliceIndex = -1;
    };

    void pushCommand (VoiceCommand cmd);
    bool popCommand (VoiceCommand& cmd);

    juce::AudioProcessorValueTreeState apvts;
    SliceBank sliceBank;
    VoiceEngine voiceEngine;

    juce::AudioFormatManager formatManager;
    juce::AudioBuffer<float> sampleBuffer;
    double sampleSourceRate = 44100.0;
    juce::String loadedSampleName;

    std::atomic<int> selectedSliceForEditing { 0 };

    static constexpr int commandQueueCapacity = 64;
    juce::AbstractFifo commandFifo { commandQueueCapacity };
    std::array<VoiceCommand, commandQueueCapacity> commandBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KerfAudioProcessor)
};
