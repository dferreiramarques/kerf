#include "ParameterLayout.h"

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout()
{
    std::vector<std::unique_ptr<juce::RangedAudioParameter>> params;

    params.push_back (std::make_unique<juce::AudioParameterChoice> (
        juce::ParameterID { ParameterIDs::midiMode, 1 },
        "MIDI Mode",
        juce::StringArray { "Trigger", "Sampler" },
        0));

    params.push_back (std::make_unique<juce::AudioParameterInt> (
        juce::ParameterID { ParameterIDs::noteOffset, 1 },
        "Note Offset",
        0, 127, 60));

    params.push_back (std::make_unique<juce::AudioParameterFloat> (
        juce::ParameterID { ParameterIDs::masterVolume, 1 },
        "Master Volume",
        juce::NormalisableRange<float> (0.0f, 1.0f),
        1.0f));

    return { params.begin(), params.end() };
}
