#include "PluginEditor.h"

KerfAudioProcessorEditor::KerfAudioProcessorEditor (KerfAudioProcessor& p)
    : AudioProcessorEditor (&p), processor (p), waveformDisplay (p)
{
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
    sensitivitySlider.setSliderStyle (juce::Slider::LinearHorizontal);
    sensitivitySlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);

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
    noteOffsetSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    noteOffsetSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    addAndMakeVisible (noteOffsetLabel);
    noteOffsetAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getAPVTS(), ParameterIDs::noteOffset, noteOffsetSlider);

    addAndMakeVisible (masterVolumeSlider);
    masterVolumeSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    masterVolumeSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
    addAndMakeVisible (masterVolumeLabel);
    masterVolumeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.getAPVTS(), ParameterIDs::masterVolume, masterVolumeSlider);

    addAndMakeVisible (sliceIndexSlider);
    addAndMakeVisible (sliceIndexLabel);
    sliceIndexSlider.setSliderStyle (juce::Slider::LinearHorizontal);
    sliceIndexSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 20);
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

    auto setupSlider = [this] (juce::Slider& slider, juce::Label& label, double minV, double maxV, double defaultV)
    {
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
        slider.setRange (minV, maxV);
        slider.setValue (defaultV, juce::dontSendNotification);
        slider.setSliderStyle (juce::Slider::LinearHorizontal);
        slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 60, 20);
    };

    setupSlider (startSlider, startLabel, 0.0, 10.0, 0.0);
    startSlider.onValueChange = [this]
    {
        const auto v = (float) startSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.start = v; }, true);
    };

    setupSlider (endSlider, endLabel, 0.0, 10.0, 1.0);
    endSlider.onValueChange = [this]
    {
        const auto v = (float) endSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.end = v; }, true);
    };

    setupSlider (pitchSlider, pitchLabel, 0.125, 8.0, 1.0);
    pitchSlider.onValueChange = [this]
    {
        const auto v = (float) pitchSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.pitch = v; }, false);
    };

    setupSlider (volumeSlider, volumeLabel, 0.0, 1.0, 1.0);
    volumeSlider.onValueChange = [this]
    {
        const auto v = (float) volumeSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.volume = v; }, false);
    };

    setupSlider (panSlider, panLabel, -1.0, 1.0, 0.0);
    panSlider.onValueChange = [this]
    {
        const auto v = (float) panSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.pan = v; }, false);
    };

    setupSlider (fadeInSlider, fadeInLabel, 0.0, 200.0, 1.0);
    fadeInSlider.onValueChange = [this]
    {
        const auto v = (float) fadeInSlider.getValue();
        updateCurrentSlice ([v] (Slice& s) { s.fadeInMs = v; }, false);
    };

    setupSlider (fadeOutSlider, fadeOutLabel, 0.0, 500.0, 50.0);
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

    setSize (720, 640);
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
    sampleNameLabel.setText ("Sample: " + (processor.getLoadedSampleName().isEmpty() ? "(none)" : processor.getLoadedSampleName())
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
                          playing ? juce::Colours::limegreen
                                   : juce::LookAndFeel_V4::getDefaultLookAndFeel().findColour (juce::TextButton::buttonColourId));
}

void KerfAudioProcessorEditor::resized()
{
    auto area = getLocalBounds().reduced (10);
    auto row = [&area] (int height) { return area.removeFromTop (height).reduced (0, 2); };

    auto titleRow = row (24);
    importButton.setBounds (titleRow.removeFromLeft (200));
    infoButton.setBounds (titleRow.removeFromRight (24));
    sampleNameLabel.setBounds (row (20));

    auto autoDetectRow = row (24);
    sensitivitySlider.setBounds (autoDetectRow.removeFromLeft (250));
    autoDetectButton.setBounds (autoDetectRow.removeFromLeft (120).translated (10, 0));

    auto sliceButtonsRow = row (24);
    addSliceButton.setBounds (sliceButtonsRow.removeFromLeft (150));
    clearSlicesButton.setBounds (sliceButtonsRow.removeFromLeft (110).translated (10, 0));
    sliceCountLabel.setBounds (row (20));

    waveformDisplay.setBounds (row (140));

    area.removeFromTop (6);

    auto midiModeRow = row (24);
    midiModeLabel.setBounds (midiModeRow.removeFromLeft (100));
    midiModeBox.setBounds (midiModeRow.removeFromLeft (150));

    auto noteOffsetRow = row (24);
    noteOffsetLabel.setBounds (noteOffsetRow.removeFromLeft (100));
    noteOffsetSlider.setBounds (noteOffsetRow.removeFromLeft (250));

    auto masterVolumeRow = row (24);
    masterVolumeLabel.setBounds (masterVolumeRow.removeFromLeft (100));
    masterVolumeSlider.setBounds (masterVolumeRow.removeFromLeft (250));

    area.removeFromTop (6);

    auto sliceIndexRow = row (24);
    sliceIndexLabel.setBounds (sliceIndexRow.removeFromLeft (100));
    sliceIndexSlider.setBounds (sliceIndexRow.removeFromLeft (150));

    auto playRow = row (28);
    playButton.setBounds (playRow.removeFromLeft (100));
    stopButton.setBounds (playRow.removeFromLeft (100).translated (10, 0));
    deleteSliceButton.setBounds (playRow.removeFromLeft (120).translated (20, 0));
    loopToggle.setBounds (playRow.removeFromLeft (70).translated (20, 0));
    enabledToggle.setBounds (playRow.removeFromLeft (90).translated (10, 0));

    auto sliceParamRow = [&] (juce::Label& label, juce::Slider& slider)
    {
        auto r = row (24);
        label.setBounds (r.removeFromLeft (100));
        slider.setBounds (r.removeFromLeft (300));
    };

    sliceParamRow (startLabel, startSlider);
    sliceParamRow (endLabel, endSlider);
    sliceParamRow (pitchLabel, pitchSlider);
    sliceParamRow (volumeLabel, volumeSlider);
    sliceParamRow (panLabel, panSlider);
    sliceParamRow (fadeInLabel, fadeInSlider);
    sliceParamRow (fadeOutLabel, fadeOutSlider);
}
