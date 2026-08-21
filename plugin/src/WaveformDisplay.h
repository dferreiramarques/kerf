#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <vector>
#include "PluginProcessor.h"

// Waveform + slice boundary editor, viewport-aware (mouse-wheel zoom, middle-drag pan).
// Left-click near a slice's start/end line drags that boundary live (with the same
// retrigger-while-playing feedback as kerf.html's updatePlayingSliceAudio()); left-click-drag on
// empty space creates a new slice under the cursor and immediately drags its end; a plain click
// with no drag just selects the slice under the cursor.
class WaveformDisplay final : public juce::Component
{
public:
    explicit WaveformDisplay (KerfAudioProcessor& processorIn) : processor (processorIn) {}

    void paint (juce::Graphics& g) override;
    void resized() override { peaksValidForWidth = -1; }

    void mouseDown (const juce::MouseEvent& e) override;
    void mouseDrag (const juce::MouseEvent& e) override;
    void mouseUp (const juce::MouseEvent& e) override;
    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override;

    // Call after import/auto-detect: rebuilds peaks and resets the view to the whole new sample.
    void sampleChanged();

    std::function<void (int)> onSliceSelected;

private:
    enum class DragMode { none, panning, movingStart, movingEnd, creating };

    void rebuildPeaksIfNeeded();
    float timeToX (float seconds) const;
    float xToTime (float x) const;
    int findSliceAt (float seconds) const;
    int findNearestBoundary (float x, bool& isStartEdge) const;
    void setView (double newStart, double newEnd);

    KerfAudioProcessor& processor;
    std::vector<std::pair<float, float>> peaks; // {min, max} per horizontal pixel, over the current view range
    int peaksValidForWidth = -1;
    double peaksBuiltViewStart = -1.0, peaksBuiltViewEnd = -1.0;
    double sampleDuration = 0.0;

    double viewStart = 0.0, viewEnd = 0.0;

    DragMode dragMode = DragMode::none;
    int dragSliceIndex = -1;
    double dragAnchorViewStart = 0.0, dragAnchorViewEnd = 0.0; // captured at mouseDown, for panning

    static constexpr float boundaryHitToleranceX = 6.0f;
    static constexpr float minSliceLengthSeconds = 0.01f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WaveformDisplay)
};
