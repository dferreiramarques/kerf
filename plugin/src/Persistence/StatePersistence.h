#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <vector>
#include "../Slice.h"
#include "../SliceBank.h"

// Full plugin state (APVTS params + slice list + the loaded sample itself, FLAC-compressed) as
// one juce::ValueTree, so a host's session save/reload brings everything back - not just the
// three global APVTS parameters. This is what was missing before: without embedding the sample
// and slice list here, closing and reopening a DAW project came back with the plugin blank.
namespace StatePersistence
{
    juce::ValueTree buildStateTree (juce::AudioProcessorValueTreeState& apvts,
                                    SliceBank& sliceBank,
                                    int selectedSlice,
                                    const juce::String& sampleName,
                                    const juce::AudioBuffer<float>* sampleBuffer,
                                    double sampleSourceRate);

    struct RestoredState
    {
        int selectedSlice = 0;
        juce::String sampleName;
        double sampleSourceRate = 44100.0;
        juce::AudioBuffer<float> sampleBuffer; // 0 samples if the tree had no embedded audio
        std::vector<Slice> slices;
    };

    // Also replaces apvts's state in place from the tree's embedded parameter child.
    RestoredState applyStateTree (const juce::ValueTree& tree, juce::AudioProcessorValueTreeState& apvts);
}
