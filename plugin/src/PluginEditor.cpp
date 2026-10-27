#include "PluginEditor.h"

KerfAudioProcessorEditor::KerfAudioProcessorEditor (KerfAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), waveformDisplay (p)
{
    setLookAndFeel (&lookAndFeel);

    auto styleLabel = [] (juce::Label& l, juce::Justification j)
    {
        l.setFont (juce::Font (juce::FontOptions (11.5f)));
        l.setColour (juce::Label::textColourId, KerfColours::textDim);
        l.setJustificationType (j);
    };
    auto knob = [] (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::RotaryVerticalDrag);
        s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 16);
        s.setRotaryParameters (juce::degreesToRadians (225.0f), juce::degreesToRadians (495.0f), true);
    };
    auto bar = [] (juce::Slider& s)
    {
        s.setSliderStyle (juce::Slider::LinearBar);
        s.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 60, 20);
    };

    addAndMakeVisible (importButton);
    importButton.onClick = [this]
    {
        auto chooser = std::make_shared<juce::FileChooser> ("Import audio file", juce::File(), "*.wav;*.aiff;*.flac");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this, chooser] (const juce::FileChooser& fc)
        {
            const auto file = fc.getResult();
            if (file != juce::File() && processor.loadSampleFile (file))
            {
                waveformDisplay.sampleChanged();
                refreshSliceDisplay();
                loadSliderValuesFromCurrentSlice();
            }
        });
    };

    addAndMakeVisible (sampleNameLabel);

    addAndMakeVisible (infoButton);
    infoButton.setTooltip ("About sample rate/bit depth handling and project recall");
    infoButton.onClick = [this]
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::InfoIcon, "Kerf Lite (Beta) - notes",
            "Sample rate & bit depth: handled automatically. Any WAV/AIFF/FLAC plays back at the "
            "correct pitch regardless of its own sample rate or bit depth versus your project's - "
            "no need to convert files before importing.\n\n"
            "Project recall: your imported sample and every slice edit are saved inside your DAW "
            "project itself (not just a reference to the original file) - closing and reopening the "
            "project brings everything back, even if you move or delete the original file afterwards.\n\n"
            "If a reopened project ever shows \"Sample missing\": the slice positions are still intact, "
            "only the embedded audio failed to restore - just reimport the original file and your "
            "slices will line up again.\n\n"
            "This is a beta: please report anything that sounds wrong or behaves unexpectedly.");
    };

    addAndMakeVisible (sensitivitySlider);
    sensitivitySlider.setRange (0.0, 1.0);
    sensitivitySlider.setValue (0.5);
    bar (sensitivitySlider);
    sensitivitySlider.textFromValueFunction = [] (double v) { return juce::String (juce::roundToInt (v * 100.0)) + " %"; };
    sensitivitySlider.valueFromTextFunction = [] (const juce::String& t) { return t.getDoubleValue() / 100.0; };
    sensitivitySlider.updateText();
    addAndMakeVisible (sensitivityLabel);
    styleLabel (sensitivityLabel, juce::Justification::centredRight);

    addAndMakeVisible (autoDetectButton);
    autoDetectButton.onClick = [this]
    {
        processor.detectTransients ((float) sensitivitySlider.getValue());
        refreshSliceDisplay();
        selectSlice (0);
        waveformDisplay.repaint();
    };

    addAndMakeVisible (clearSlicesButton);
    clearSlicesButton.onClick = [this]
    {
        processor.clearSlices();
        refreshSliceDisplay();
        waveformDisplay.repaint();
    };

    addAndMakeVisible (addSliceButton);
    addSliceButton.onClick = [this]
    {
        const auto newIndex = processor.addSlice (0.0f, 1.0f);
        refreshSliceDisplay();
        selectSlice (newIndex);
        waveformDisplay.repaint();
    };

    addAndMakeVisible (sliceCountLabel);

    addAndMakeVisible (waveformDisplay);
    waveformDisplay.onSliceSelected = [this] (int index) { selectSlice (index); };

    addAndMakeVisible (midiModeBox);
    midiModeBox.addItem ("Trigger", 1);
    midiModeBox.addItem ("Sampler", 2);
    addAndMakeVisible (midiModeLabel);
    midiModeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
        processor.getAPVTS(), ParameterIDs::midiMode, midiModeBox);

    addAndMakeVisible (noteOffsetSlider);
    knob (noteOffsetSlider);
    styleLabel (noteOffsetLabel, juce::Justification::centred);
    addAndMakeVisible (noteOffsetLabel);
    noteOffsetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getAPVTS(), ParameterIDs::noteOffset, noteOffsetSlider);

    addAndMakeVisible (masterVolumeSlider);
    knob (masterVolumeSlider);
    styleLabel (masterVolumeLabel, juce::Justification::centred);
    addAndMakeVisible (masterVolumeLabel);
    masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getAPVTS(), ParameterIDs::masterVolume, masterVolumeSlider);

    addAndMakeVisible (sliceIndexSlider);
    addAndMakeVisible (sliceIndexLabel);
    bar (sliceIndexSlider);
    styleLabel (sliceIndexLabel, juce::Justification::centredRight);
    styleLabel (midiModeLabel, juce::Justification::centredLeft);
    styleLabel (sliceCountLabel, juce::Justification::centredRight);
    styleLabel (sampleNameLabel, juce::Justification::centredLeft);
    sampleNameLabel.setColour (juce::Label::textColourId, KerfColours::text);
    sliceIndexSlider.setRange (0.0, 0.0, 1.0);
    sliceIndexSlider.onValueChange = [this] { selectSlice ((int) sliceIndexSlider.getValue()); };

    addAndMakeVisible (playButton);
    playButton.onClick = [this] { processor.triggerPreview (processor.getSelectedSliceForEditing()); };
    addAndMakeVisible (stopButton);
    stopButton.onClick = [this] { processor.stopPreview (processor.getSelectedSliceForEditing()); };

    addAndMakeVisible (deleteSliceButton);
    deleteSliceButton.onClick = [this]
    {
        processor.deleteSlice (processor.getSelectedSliceForEditing());
        refreshSliceDisplay();
        selectSlice (processor.getSelectedSliceForEditing());
        waveformDisplay.repaint();
    };

    auto setupSlider = [&] (juce::Slider& slider, juce::Label& label, double minV, double maxV, double defaultV, bool isBar = false)
    {
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
        slider.setRange (minV, maxV);
        slider.setValue (defaultV, juce::dontSendNotification);
        if (isBar)
        {
            bar (slider);
            styleLabel (label, juce::Justification::centredRight);
        }
        else
        {
            knob (slider);
            styleLabel (label, juce::Justification::centred);
        }
    };

    setupSlider (startSlider, startLabel, 0.0, 10.0, 0.0, true);
    startSlider.setTextValueSuffix (" s");
    startSlider.setNumDecimalPlacesToDisplay (3);
    startSlider.onValueChange = [this]
    {
        const auto v = (float) startSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.start = v; }, true);
    };

    setupSlider (endSlider, endLabel, 0.0, 10.0, 1.0, true);
    endSlider.setTextValueSuffix (" s");
    endSlider.setNumDecimalPlacesToDisplay (3);
    endSlider.onValueChange = [this]
    {
        const auto v = (float) endSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.end = v; }, true);
    };

    setupSlider (pitchSlider, pitchLabel, 0.125, 8.0, 1.0);
    pitchSlider.setSkewFactorFromMidPoint (1.0);
    pitchSlider.setNumDecimalPlacesToDisplay (2);
    pitchSlider.setTextValueSuffix (" x");
    pitchSlider.onValueChange = [this]
    {
        const auto v = (float) pitchSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.pitch = v; }, false);
    };

    setupSlider (volumeSlider, volumeLabel, 0.0, 1.0, 1.0);
    volumeSlider.setNumDecimalPlacesToDisplay (2);
    volumeSlider.onValueChange = [this]
    {
        const auto v = (float) volumeSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.volume = v; }, false);
    };

    setupSlider (panSlider, panLabel, -1.0, 1.0, 0.0);
    panSlider.setNumDecimalPlacesToDisplay (2);
    panSlider.onValueChange = [this]
    {
        const auto v = (float) panSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.pan = v; }, false);
    };

    setupSlider (fadeInSlider, fadeInLabel, 0.0, 200.0, 1.0);
    fadeInSlider.setNumDecimalPlacesToDisplay (1);
    fadeInSlider.setTextValueSuffix (" ms");
    fadeInSlider.onValueChange = [this]
    {
        const auto v = (float) fadeInSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.fadeInMs = v; }, false);
    };

    setupSlider (fadeOutSlider, fadeOutLabel, 0.0, 500.0, 50.0);
    fadeOutSlider.setNumDecimalPlacesToDisplay (0);
    fadeOutSlider.setTextValueSuffix (" ms");
    fadeOutSlider.onValueChange = [this]
    {
        const auto v = (float) fadeOutSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.fadeOutMs = v; }, false);
    };

    addAndMakeVisible (loopToggle);
    loopToggle.onClick = [this]
    {
        const auto loop = loopToggle.getToggleState();
        updateCurrentSlice ([loop] (Slice& s) { s.mode = loop ? Slice::Mode::loop : Slice::Mode::shot; }, false);
    };

    addAndMakeVisible (enabledToggle);
    enabledToggle.setToggleState (true, juce::dontSendNotification);
    enabledToggle.onClick = [this]
    {
        const auto enabled = enabledToggle.getToggleState();
        updateCurrentSlice ([enabled] (Slice& s) { s.enabled = enabled; }, false);
    };

    setSize (780, 430);
    startTimerHz (5);
    // Reflects whatever the processor already has (e.g. restored via setStateInformation before
    // this editor was ever created) rather than assuming a blank slate.
    waveformDisplay.sampleChanged();
    refreshSliceDisplay();
    loadSliderValuesFromCurrentSlice();
}

KerfAudioProcessorEditor::~KerfAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

void KerfAudioProcessorEditor::selectSlice (int index)
{
    const auto count = processor.getSliceBank().size();
    const auto clamped = count == 0 ? 0 : juce::jlimit (0, count - 1, index);

    processor.setSelectedSliceForEditing (clamped);
    sliceIndexSlider.setValue (clamped, juce::dontSendNotification);
    loadSliderValuesFromCurrentSlice();
    waveformDisplay.repaint();
}

void KerfAudioProcessorEditor::loadSliderValuesFromCurrentSlice()
{
    const auto snapshot = processor.getSliceBank().getCurrent();
    const auto index = processor.getSelectedSliceForEditing();

    if (index < 0 || index >= (int) snapshot->size())
        return;

    const auto& slice = (*snapshot)[(size_t) index];

    startSlider.setValue (slice.start, juce::dontSendNotification);
    endSlider.setValue (slice.end, juce::dontSendNotification);
    pitchSlider.setValue (slice.pitch, juce::dontSendNotification);
    volumeSlider.setValue (slice.volume, juce::dontSendNotification);
    panSlider.setValue (slice.pan, juce::dontSendNotification);
    fadeInSlider.setValue (slice.fadeInMs, juce::dontSendNotification);
    fadeOutSlider.setValue (slice.fadeOutMs, juce::dontSendNotification);
    loopToggle.setToggleState (slice.mode == Slice::Mode::loop, juce::dontSendNotification);
    enabledToggle.setToggleState (slice.enabled, juce::dontSendNotification);
}

void KerfAudioProcessorEditor::updateCurrentSlice (const std::function<void (Slice&)>& mutator, bool liveRetrigger)
{
    const auto index = processor.getSelectedSliceForEditing();
    if (index >= 0 && index < processor.getSliceBank().size())
        processor.setSliceParam (index, mutator, liveRetrigger);
}

void KerfAudioProcessorEditor::refreshSliceDisplay()
{
    sampleNameLabel.setText ((processor.getLoadedSampleName().isEmpty() ? "(no sample)" : processor.getLoadedSampleName())
                                  + juce::String::formatted ("  (%.2fs)", processor.getSampleDurationSeconds()),
                             juce::dontSendNotification);

    const auto count = processor.getSliceBank().size();
    sliceCountLabel.setText ("Slices: " + juce::String (count), juce::dontSendNotification);
    sliceIndexSlider.setRange (0.0, (double) juce::jmax (0, count - 1), 1.0);

    const auto duration = juce::jmax (0.1, processor.getSampleDurationSeconds());
    startSlider.setRange (0.0, duration);
    endSlider.setRange (0.0, duration);
}

void KerfAudioProcessorEditor::timerCallback()
{
    refreshSliceDisplay();
    loadSliderValuesFromCurrentSlice(); // keeps the sliders tracking a live waveform-drag
    waveformDisplay.repaint();

    const auto playing = processor.getVoiceEngine().isSliceActive (processor.getSelectedSliceForEditing());
    playButton.setColour (juce::TextButton::buttonColourId,
                          playing ? KerfColours::accent : KerfColours::control);
}

void KerfAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (KerfColours::window);

    g.setColour (KerfColours::panel);
    g.fillRect (headerArea);

    g.setColour (KerfColours::panel);
    g.fillRoundedRectangle (sliceToolsArea.toFloat(), 3.0f);

    auto drawPanel = [&g] (juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (KerfColours::panel);
        g.fillRoundedRectangle (r.toFloat(), 3.0f);
        g.setColour (KerfColours::textDim);
        g.setFont (juce::Font (juce::FontOptions (11.0f, juce::Font::bold)));
        g.drawText (title.toUpperCase(), r.withHeight (18).withTrimmedLeft (8), juce::Justification::centredLeft);
    };

    drawPanel (slicePanel, "Slice");
    drawPanel (soundPanel, "Sound");
    drawPanel (globalPanel, "Global");
}

void KerfAudioProcessorEditor::resized()
{
    auto area = getLocalBounds();

    headerArea = area.removeFromTop (30);
    {
        auto r = headerArea.reduced (6, 4);
        importButton.setBounds (r.removeFromLeft (130));
        infoButton.setBounds (r.removeFromRight (22));
        r.removeFromLeft (10);
        sampleNameLabel.setBounds (r);
    }

    area = area.reduced (8, 0);
    area.removeFromTop (8);
    waveformDisplay.setBounds (area.removeFromTop (160));
    area.removeFromTop (6);

    sliceToolsArea = area.removeFromTop (64);
    {
        auto r = sliceToolsArea.reduced (6, 5);

        auto top = r.removeFromTop (24);
        sensitivityLabel.setBounds (top.removeFromLeft (70));
        top.removeFromLeft (4);
        sensitivitySlider.setBounds (top.removeFromLeft (150));
        top.removeFromLeft (6);
        autoDetectButton.setBounds (top.removeFromLeft (90));
        top.removeFromLeft (14);
        addSliceButton.setBounds (top.removeFromLeft (80));
        top.removeFromLeft (4);
        clearSlicesButton.setBounds (top.removeFromLeft (80));
        sliceCountLabel.setBounds (top);

        r.removeFromTop (6);
        auto bottom = r.removeFromTop (24);
        sliceIndexLabel.setBounds (bottom.removeFromLeft (70));
        bottom.removeFromLeft (4);
        sliceIndexSlider.setBounds (bottom.removeFromLeft (150));
        bottom.removeFromLeft (6);
        playButton.setBounds (bottom.removeFromLeft (60));
        bottom.removeFromLeft (4);
        stopButton.setBounds (bottom.removeFromLeft (60));
        bottom.removeFromLeft (4);
        deleteSliceButton.setBounds (bottom.removeFromLeft (80));
        bottom.removeFromLeft (14);
        loopToggle.setBounds (bottom.removeFromLeft (60));
        bottom.removeFromLeft (4);
        enabledToggle.setBounds (bottom.removeFromLeft (70));
    }

    area.removeFromTop (6);
    auto panels = area.removeFromTop (juce::jmin (area.getHeight(), 138));

    slicePanel = panels.removeFromLeft (190);
    panels.removeFromLeft (6);
    globalPanel = panels.removeFromRight (230);
    panels.removeFromRight (6);
    soundPanel = panels;

    {
        auto r = slicePanel.reduced (8, 0).withTrimmedTop (28);
        auto barRow = [&r] (juce::Label& label, juce::Slider& slider)
        {
            auto line = r.removeFromTop (24);
            label.setBounds (line.removeFromLeft (40));
            line.removeFromLeft (4);
            slider.setBounds (line);
            r.removeFromTop (8);
        };
        barRow (startLabel, startSlider);
        barRow (endLabel, endSlider);
    }

    auto knobCell = [] (juce::Rectangle<int> cell, juce::Label& label, juce::Slider& slider)
    {
        label.setBounds (cell.removeFromTop (16));
        slider.setBounds (cell);
    };

    {
        auto r = soundPanel.withTrimmedTop (20).reduced (4, 2);
        const auto w = r.getWidth() / 5;
        knobCell (r.removeFromLeft (w), pitchLabel, pitchSlider);
        knobCell (r.removeFromLeft (w), volumeLabel, volumeSlider);
        knobCell (r.removeFromLeft (w), panLabel, panSlider);
        knobCell (r.removeFromLeft (w), fadeInLabel, fadeInSlider);
        knobCell (r, fadeOutLabel, fadeOutSlider);
    }

    {
        auto r = globalPanel.withTrimmedTop (20).reduced (4, 2);
        auto left = r.removeFromLeft (r.getWidth() / 3 + 14);
        midiModeLabel.setBounds (left.removeFromTop (16).reduced (4, 0));
        midiModeBox.setBounds (left.removeFromTop (24).reduced (4, 0));
        const auto w = r.getWidth() / 2;
        knobCell (r.removeFromLeft (w), noteOffsetLabel, noteOffsetSlider);
        knobCell (r, masterVolumeLabel, masterVolumeSlider);
    }
}
