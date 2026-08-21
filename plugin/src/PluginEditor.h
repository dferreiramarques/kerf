#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "PluginProcessor.h"
#include "WaveformDisplay.h"

// Native JUCE GUI (kept, not replaced by a WebView - see the plan's 2026-08-21 update). Waveform
// click/double-click and the slice index stepper both drive the same "selected slice" state on
// the processor, which doubles as Sampler mode's target slice, matching kerf.html's own
// `state.selectedSliceForEditing` doing double duty for exactly the same two things.
class KerfAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                        private juce::Timer
{
public:
    explicit KerfAudioProcessorEditor (KerfAudioProcessor&);
    ~KerfAudioProcessorEditor() override;

    void resized() override;

private:
    void timerCallback() override;
    void refreshSliceDisplay();
    void selectSlice (int index);
    void loadSliderValuesFromCurrentSlice();
    void updateCurrentSlice (const std::function<void (Slice&)>& mutator, bool liveRetrigger);

    KerfAudioProcessor& processor;

    juce::TextButton importButton { "Import Audio File..." };
    juce::Label sampleNameLabel;
    juce::TextButton infoButton { "i" };

    juce::Slider sensitivitySlider;
    juce::TextButton autoDetectButton { "Auto Detect" };
    juce::TextButton clearSlicesButton { "Clear Slices" };
    juce::TextButton addSliceButton { "Add Slice (0-1s)" };
    juce::Label sliceCountLabel;

    WaveformDisplay waveformDisplay;

    juce::ComboBox midiModeBox;
    juce::Label midiModeLabel { {}, "MIDI Mode" };
    juce::Slider noteOffsetSlider;
    juce::Label noteOffsetLabel { {}, "Note Offset" };
    juce::Slider masterVolumeSlider;
    juce::Label masterVolumeLabel { {}, "Master Volume" };

    juce::Slider sliceIndexSlider;
    juce::Label sliceIndexLabel { {}, "Editing Slice" };
    juce::TextButton playButton { "Play" };
    juce::TextButton stopButton { "Stop" };
    juce::TextButton deleteSliceButton { "Delete Slice" };

    juce::Slider startSlider, endSlider, pitchSlider, volumeSlider, panSlider, fadeInSlider, fadeOutSlider;
    juce::Label startLabel { {}, "Start (s)" }, endLabel { {}, "End (s)" }, pitchLabel { {}, "Pitch" },
                volumeLabel { {}, "Volume" }, panLabel { {}, "Pan" }, fadeInLabel { {}, "Fade In (ms)" },
                fadeOutLabel { {}, "Fade Out (ms)" };
    juce::ToggleButton loopToggle { "Loop" };
    juce::ToggleButton enabledToggle { "Enabled" };

    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> midiModeAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> noteOffsetAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterVolumeAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (KerfAudioProcessorEditor)
};
