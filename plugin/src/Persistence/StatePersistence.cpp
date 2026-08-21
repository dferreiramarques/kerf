#include "StatePersistence.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace StatePersistence
{
    juce::ValueTree buildStateTree (juce::AudioProcessorValueTreeState& apvts,
                                    SliceBank& sliceBank,
                                    int selectedSlice,
                                    const juce::String& sampleName,
                                    const juce::AudioBuffer<float>* sampleBuffer,
                                    double sampleSourceRate)
    {
        juce::ValueTree root ("KerfPluginState");
        root.appendChild (apvts.copyState(), nullptr);
        root.setProperty ("selectedSlice", selectedSlice, nullptr);
        root.setProperty ("sampleName", sampleName, nullptr);
        root.setProperty ("sampleSourceRate", sampleSourceRate, nullptr);

        if (sampleBuffer != nullptr && sampleBuffer->getNumSamples() > 0)
        {
            juce::MemoryBlock encoded;
            juce::FlacAudioFormat flac;
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (encoded, false);

            const auto options = juce::AudioFormatWriterOptions{}
                .withSampleRate (sampleSourceRate)
                .withNumChannels (sampleBuffer->getNumChannels())
                .withBitsPerSample (16);

            if (auto writer = flac.createWriterFor (stream, options))
            {
                writer->writeFromAudioSampleBuffer (*sampleBuffer, 0, sampleBuffer->getNumSamples());
                writer.reset(); // flush/finalize before `encoded` is read below
                root.setProperty ("sampleAudio", juce::var (encoded), nullptr);
            }
        }

        juce::ValueTree slicesTree ("Slices");
        const auto snapshot = sliceBank.getCurrent();

        for (const auto& slice : *snapshot)
        {
            juce::ValueTree s ("Slice");
            s.setProperty ("start", slice.start, nullptr);
            s.setProperty ("end", slice.end, nullptr);
            s.setProperty ("pitch", slice.pitch, nullptr);
            s.setProperty ("volume", slice.volume, nullptr);
            s.setProperty ("pan", slice.pan, nullptr);
            s.setProperty ("enabled", slice.enabled, nullptr);
            s.setProperty ("mode", slice.mode == Slice::Mode::loop ? "loop" : "shot", nullptr);
            s.setProperty ("fadeInMs", slice.fadeInMs, nullptr);
            s.setProperty ("fadeOutMs", slice.fadeOutMs, nullptr);
            slicesTree.appendChild (s, nullptr);
        }

        root.appendChild (slicesTree, nullptr);
        return root;
    }

    RestoredState applyStateTree (const juce::ValueTree& tree, juce::AudioProcessorValueTreeState& apvts)
    {
        RestoredState result;

        if (! tree.isValid())
            return result;

        const auto paramsTree = tree.getChildWithName (apvts.state.getType());
        if (paramsTree.isValid())
            apvts.replaceState (paramsTree);

        result.selectedSlice = tree.getProperty ("selectedSlice", 0);
        result.sampleName = tree.getProperty ("sampleName", juce::String()).toString();
        result.sampleSourceRate = (double) tree.getProperty ("sampleSourceRate", 44100.0);

        if (tree.hasProperty ("sampleAudio"))
        {
            const auto audioVar = tree.getProperty ("sampleAudio");

            if (auto* mb = audioVar.getBinaryData())
            {
                juce::FlacAudioFormat flac;
                std::unique_ptr<juce::InputStream> stream = std::make_unique<juce::MemoryInputStream> (mb->getData(), mb->getSize(), false);

                if (auto* reader = flac.createReaderFor (stream.release(), true))
                {
                    std::unique_ptr<juce::AudioFormatReader> readerOwner (reader);
                    result.sampleBuffer.setSize ((int) reader->numChannels, (int) reader->lengthInSamples);
                    reader->read (&result.sampleBuffer, 0, (int) reader->lengthInSamples, 0, true, true);
                }
            }
        }

        const auto slicesTree = tree.getChildWithName ("Slices");

        for (int i = 0; i < slicesTree.getNumChildren(); ++i)
        {
            const auto s = slicesTree.getChild (i);

            Slice slice;
            slice.start = s.getProperty ("start", 0.0f);
            slice.end = s.getProperty ("end", 0.0f);
            slice.pitch = s.getProperty ("pitch", 1.0f);
            slice.volume = s.getProperty ("volume", 1.0f);
            slice.pan = s.getProperty ("pan", 0.0f);
            slice.enabled = s.getProperty ("enabled", true);
            slice.mode = s.getProperty ("mode", "shot").toString() == "loop" ? Slice::Mode::loop : Slice::Mode::shot;
            slice.fadeInMs = s.getProperty ("fadeInMs", 1.0f);
            slice.fadeOutMs = s.getProperty ("fadeOutMs", 50.0f);

            result.slices.push_back (slice);
        }

        return result;
    }
}
