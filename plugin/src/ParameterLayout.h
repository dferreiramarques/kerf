#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

// Only genuinely host-automatable, global things live here. Per-slice fields (start/end/pitch/
// volume/pan/mode/fades/enabled) are deliberately NOT APVTS parameters - see SliceBank. A DAW
// automating "slice 7 volume" mid-song isn't a real workflow for this plugin (the original web
// app has no automation at all), and tying slice count to a fixed APVTS parameter list is what
// forced an arbitrary slice-count cap in an earlier draft of this plugin's design; removing that
// coupling removed the need for the cap entirely.
namespace ParameterIDs
{
    inline constexpr const char* midiMode = "midiMode";
    inline constexpr const char* noteOffset = "noteOffset";
    inline constexpr const char* masterVolume = "masterVolume";
}

juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();
