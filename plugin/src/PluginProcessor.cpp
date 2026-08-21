#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "TransientDetector.h"
#include "Persistence/StatePersistence.h"

KerfAudioProcessor::KerfAudioProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout())
{
    formatManager.registerBasicFormats();
}

void KerfAudioProcessor::prepareToPlay (double sampleRate, int)
{
    voiceEngine.prepare (sampleRate);
    voiceEngine.setSampleSource (sampleBuffer.getNumSamples() > 0 ? &sampleBuffer : nullptr, sampleSourceRate);
}

void KerfAudioProcessor::releaseResources()
{
}

bool KerfAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    return layouts.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void KerfAudioProcessor::pushCommand (VoiceCommand cmd)
{
    int start1, size1, start2, size2;
    commandFifo.prepareToWrite (1, start1, size1, start2, size2);

    if (size1 > 0)
        commandBuffer[(size_t) start1] = cmd;
    else if (size2 > 0)
        commandBuffer[(size_t) start2] = cmd;

    commandFifo.finishedWrite (size1 + size2);
}

bool KerfAudioProcessor::popCommand (VoiceCommand& cmd)
{
    if (commandFifo.getNumReady() <= 0)
        return false;

    int start1, size1, start2, size2;
    commandFifo.prepareToRead (1, start1, size1, start2, size2);

    if (size1 > 0)
        cmd = commandBuffer[(size_t) start1];
    else if (size2 > 0)
        cmd = commandBuffer[(size_t) start2];
    else
        return false;

    commandFifo.finishedRead (size1 + size2);
    return true;
}

void KerfAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    buffer.clear();

    const auto slices = sliceBank.getCurrent();

    // Drain UI-originated preview/retrigger requests queued since the last block. These only
    // ever touch voiceEngine from here, on the audio thread - the message thread never calls
    // into it directly, avoiding a data race on the voice pool.
    VoiceCommand cmd;
    while (popCommand (cmd))
    {
        switch (cmd.type)
        {
            case VoiceCommand::Type::triggerPreview:
                if (cmd.sliceIndex >= 0 && cmd.sliceIndex < (int) slices->size())
                    voiceEngine.triggerPreview (cmd.sliceIndex, (*slices)[(size_t) cmd.sliceIndex]);
                break;

            case VoiceCommand::Type::stopPreview:
                voiceEngine.stopPreview (cmd.sliceIndex);
                break;

            case VoiceCommand::Type::retrigger:
                // Matches kerf.html's updatePlayingSliceAudio(), which only re-triggers a slice
                // that's already sounding (`if (!state.playingSources[sliceIdx]) return;`) -
                // dragging a boundary on a slice you haven't started playing stays silent.
                if (cmd.sliceIndex >= 0 && cmd.sliceIndex < (int) slices->size()
                    && voiceEngine.isSliceActive (cmd.sliceIndex))
                    voiceEngine.retrigger (cmd.sliceIndex, (*slices)[(size_t) cmd.sliceIndex]);
                break;
        }
    }

    const auto midiModeParam = (int) apvts.getRawParameterValue (ParameterIDs::midiMode)->load();
    const auto mode = midiModeParam == 0 ? VoiceEngine::MidiMode::trigger : VoiceEngine::MidiMode::sampler;
    const auto noteOffset = (int) apvts.getRawParameterValue (ParameterIDs::noteOffset)->load();
    const auto selectedSlice = selectedSliceForEditing.load();

    // MIDI events are all applied at the top of the block rather than at their exact sample
    // offset - a standard, deliberate simplification for now (up to one block of timing
    // imprecision), not a bug to chase in Phase 2.
    for (const auto metadata : midiMessages)
    {
        const auto msg = metadata.getMessage();

        if (msg.isNoteOn())
            voiceEngine.noteOn (mode, noteOffset, selectedSlice, msg.getNoteNumber(), slices);
        else if (msg.isNoteOff())
            voiceEngine.noteOff (mode, noteOffset, msg.getNoteNumber());
    }

    voiceEngine.renderNextBlock (buffer, 0, buffer.getNumSamples());

    const auto masterVolume = apvts.getRawParameterValue (ParameterIDs::masterVolume)->load();
    buffer.applyGain (masterVolume);
}

juce::AudioProcessorEditor* KerfAudioProcessor::createEditor()
{
    return new KerfAudioProcessorEditor (*this);
}

void KerfAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto tree = StatePersistence::buildStateTree (
        apvts, sliceBank, selectedSliceForEditing.load(), loadedSampleName,
        sampleBuffer.getNumSamples() > 0 ? &sampleBuffer : nullptr, sampleSourceRate);

    juce::MemoryOutputStream stream (destData, false);
    tree.writeToStream (stream);
}

void KerfAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    juce::MemoryInputStream stream (data, (size_t) sizeInBytes, false);
    const auto tree = juce::ValueTree::readFromStream (stream);
    const auto restored = StatePersistence::applyStateTree (tree, apvts);

    voiceEngine.allNotesOff();

    selectedSliceForEditing.store (restored.selectedSlice);
    loadedSampleName = restored.sampleName;
    sampleSourceRate = restored.sampleSourceRate;
    sampleBuffer = std::move (restored.sampleBuffer);
    sliceBank.replaceAll (restored.slices);

    voiceEngine.setSampleSource (sampleBuffer.getNumSamples() > 0 ? &sampleBuffer : nullptr, sampleSourceRate);
}

bool KerfAudioProcessor::loadSampleFile (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formatManager.createReaderFor (file));
    if (reader == nullptr)
        return false;

    juce::AudioBuffer<float> newBuffer ((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read (&newBuffer, 0, (int) reader->lengthInSamples, 0, true, true);

    sampleBuffer = std::move (newBuffer);
    sampleSourceRate = reader->sampleRate;
    loadedSampleName = file.getFileName();

    // A new sample invalidates any previous slice boundaries (they were positions into the old
    // audio). Not clearing them would leave stale start/end points pointing past the new sample.
    sliceBank.clear();
    selectedSliceForEditing.store (0);

    voiceEngine.allNotesOff();
    voiceEngine.setSampleSource (&sampleBuffer, sampleSourceRate);

    return true;
}

double KerfAudioProcessor::getSampleDurationSeconds() const
{
    return sampleSourceRate > 0.0 ? (double) sampleBuffer.getNumSamples() / sampleSourceRate : 0.0;
}

void KerfAudioProcessor::detectTransients (float sensitivity)
{
    if (sampleBuffer.getNumSamples() == 0)
        return;

    auto slices = TransientDetector::detect (sampleBuffer.getReadPointer (0), sampleBuffer.getNumSamples(),
                                             sampleSourceRate, sensitivity);
    sliceBank.replaceAll (std::move (slices));
}

int KerfAudioProcessor::addSlice (float startSec, float endSec)
{
    Slice s;
    s.start = startSec;
    s.end = endSec;
    return sliceBank.add (s);
}

void KerfAudioProcessor::deleteSlice (int index)
{
    sliceBank.removeAt (index);
}

void KerfAudioProcessor::clearSlices()
{
    sliceBank.clear();
}

void KerfAudioProcessor::setSliceParam (int index, const std::function<void (Slice&)>& mutator, bool liveRetrigger)
{
    sliceBank.updateAt (index, mutator);

    if (liveRetrigger)
        pushCommand ({ VoiceCommand::Type::retrigger, index });
}

void KerfAudioProcessor::triggerPreview (int sliceIndex)
{
    pushCommand ({ VoiceCommand::Type::triggerPreview, sliceIndex });
}

void KerfAudioProcessor::stopPreview (int sliceIndex)
{
    pushCommand ({ VoiceCommand::Type::stopPreview, sliceIndex });
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new KerfAudioProcessor();
}
