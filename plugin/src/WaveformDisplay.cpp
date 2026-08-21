#include "WaveformDisplay.h"
#include <cmath>

void WaveformDisplay::sampleChanged()
{
    auto* buffer = processor.getSampleBufferForDisplay();
    sampleDuration = buffer != nullptr ? (double) buffer->getNumSamples() / processor.getSampleSourceRate() : 0.0;
    viewStart = 0.0;
    viewEnd = sampleDuration;
    peaksValidForWidth = -1;
}

void WaveformDisplay::setView (double newStart, double newEnd)
{
    if (sampleDuration <= 0.0)
        return;

    const auto span = juce::jlimit (0.005, sampleDuration, newEnd - newStart);
    const auto start = juce::jlimit (0.0, sampleDuration - span, newStart);

    viewStart = start;
    viewEnd = start + span;
    repaint();
}

void WaveformDisplay::rebuildPeaksIfNeeded()
{
    const auto width = juce::jmax (1, getWidth());
    const auto viewChanged = std::abs (viewStart - peaksBuiltViewStart) > 1.0e-9 || std::abs (viewEnd - peaksBuiltViewEnd) > 1.0e-9;

    if (peaksValidForWidth == width && ! viewChanged)
        return;

    peaks.assign ((size_t) width, { 0.0f, 0.0f });

    auto* buffer = processor.getSampleBufferForDisplay();

    if (buffer == nullptr || buffer->getNumSamples() == 0 || viewEnd <= viewStart)
    {
        peaksValidForWidth = width;
        peaksBuiltViewStart = viewStart;
        peaksBuiltViewEnd = viewEnd;
        return;
    }

    const auto sourceRate = processor.getSampleSourceRate();
    const auto numSamples = buffer->getNumSamples();
    const auto numChannels = buffer->getNumChannels();

    for (int x = 0; x < width; ++x)
    {
        const auto t0 = viewStart + (viewEnd - viewStart) * ((double) x / width);
        const auto t1 = viewStart + (viewEnd - viewStart) * ((double) (x + 1) / width);

        auto s0 = juce::jlimit (0, numSamples, (int) (t0 * sourceRate));
        auto s1 = juce::jlimit (0, numSamples, (int) (t1 * sourceRate) + 1);
        if (s1 <= s0)
            s1 = juce::jmin (numSamples, s0 + 1);

        float lo = 0.0f, hi = 0.0f;

        for (int ch = 0; ch < numChannels; ++ch)
            for (int i = s0; i < s1; ++i)
            {
                const auto s = buffer->getSample (ch, i);
                lo = juce::jmin (lo, s);
                hi = juce::jmax (hi, s);
            }

        peaks[(size_t) x] = { lo, hi };
    }

    peaksValidForWidth = width;
    peaksBuiltViewStart = viewStart;
    peaksBuiltViewEnd = viewEnd;
}

float WaveformDisplay::timeToX (float seconds) const
{
    const auto span = viewEnd - viewStart;
    if (span <= 0.0)
        return 0.0f;

    return (float) (((double) seconds - viewStart) / span * getWidth());
}

float WaveformDisplay::xToTime (float x) const
{
    if (getWidth() <= 0)
        return 0.0f;

    const auto span = viewEnd - viewStart;
    return (float) (viewStart + ((double) x / getWidth()) * span);
}

int WaveformDisplay::findSliceAt (float seconds) const
{
    const auto snapshot = processor.getSliceBank().getCurrent();

    for (size_t i = 0; i < snapshot->size(); ++i)
        if (seconds >= (*snapshot)[i].start && seconds <= (*snapshot)[i].end)
            return (int) i;

    return -1;
}

int WaveformDisplay::findNearestBoundary (float x, bool& isStartEdge) const
{
    const auto snapshot = processor.getSliceBank().getCurrent();

    for (size_t i = 0; i < snapshot->size(); ++i)
    {
        const auto& slice = (*snapshot)[i];

        if (std::abs (x - timeToX (slice.start)) <= boundaryHitToleranceX)
        {
            isStartEdge = true;
            return (int) i;
        }

        if (std::abs (x - timeToX (slice.end)) <= boundaryHitToleranceX)
        {
            isStartEdge = false;
            return (int) i;
        }
    }

    return -1;
}

void WaveformDisplay::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff14161a));

    rebuildPeaksIfNeeded();

    if (peaks.empty() || sampleDuration <= 0.0)
    {
        const auto sliceCount = processor.getSliceBank().size();

        if (sliceCount > 0)
        {
            // Slices exist but there's no audio behind them - the embedded sample failed to
            // restore (or was never saved, e.g. an older project file). The slice positions
            // themselves are still intact and will keep working once a sample is loaded again.
            g.setColour (juce::Colours::orange);
            g.drawText (juce::String::formatted ("Sample missing - please reimport the audio file (%d slice%s saved)",
                                                 sliceCount, sliceCount == 1 ? "" : "s"),
                        getLocalBounds().reduced (10), juce::Justification::centred, true);
        }
        else
        {
            g.setColour (juce::Colours::grey);
            g.drawText ("No sample loaded", getLocalBounds(), juce::Justification::centred);
        }

        return;
    }

    const auto snapshot = processor.getSliceBank().getCurrent();
    const auto selectedIndex = processor.getSelectedSliceForEditing();

    for (size_t i = 0; i < snapshot->size(); ++i)
    {
        const auto& slice = (*snapshot)[i];
        const auto x0 = timeToX (slice.start);
        const auto x1 = timeToX (slice.end);

        if (x1 < 0.0f || x0 > (float) getWidth())
            continue; // fully outside the current view, skip

        const auto isSelected = (int) i == selectedIndex;

        g.setColour (isSelected ? juce::Colour (0x552f8fff) : juce::Colour (0x30ffffff));
        g.fillRect (juce::Rectangle<float> (x0, 0.0f, juce::jmax (1.0f, x1 - x0), (float) getHeight()));

        g.setColour (isSelected ? juce::Colours::deepskyblue : juce::Colours::grey);
        g.drawVerticalLine ((int) x0, 0.0f, (float) getHeight());
        g.drawVerticalLine ((int) x1, 0.0f, (float) getHeight());
    }

    g.setColour (juce::Colour (0xff7fd0ff));
    const auto midY = getHeight() * 0.5f;

    for (int x = 0; x < (int) peaks.size(); ++x)
    {
        const auto [lo, hi] = peaks[(size_t) x];
        const auto y0 = midY - hi * midY;
        const auto y1 = midY - lo * midY;
        g.drawVerticalLine (x, y0, juce::jmax (y0 + 1.0f, y1));
    }
}

void WaveformDisplay::mouseDown (const juce::MouseEvent& e)
{
    if (sampleDuration <= 0.0)
        return;

    if (e.mods.isMiddleButtonDown())
    {
        dragMode = DragMode::panning;
        dragAnchorViewStart = viewStart;
        dragAnchorViewEnd = viewEnd;
        return;
    }

    bool isStartEdge = false;
    const auto boundaryIndex = findNearestBoundary ((float) e.position.x, isStartEdge);

    if (boundaryIndex >= 0)
    {
        dragMode = isStartEdge ? DragMode::movingStart : DragMode::movingEnd;
        dragSliceIndex = boundaryIndex;

        if (onSliceSelected != nullptr)
            onSliceSelected (boundaryIndex);

        return;
    }

    const auto time = xToTime ((float) e.position.x);
    const auto existingIndex = findSliceAt (time);

    if (existingIndex >= 0)
    {
        dragMode = DragMode::none;

        if (onSliceSelected != nullptr)
            onSliceSelected (existingIndex);

        return;
    }

    // Empty space: start a new slice here: zero-length until the drag (if any) defines its end.
    // mouseUp discards it if the user just clicked without dragging.
    dragSliceIndex = processor.addSlice (time, time);
    dragMode = DragMode::creating;

    if (onSliceSelected != nullptr)
        onSliceSelected (dragSliceIndex);
}

void WaveformDisplay::mouseDrag (const juce::MouseEvent& e)
{
    if (dragMode == DragMode::none)
        return;

    if (dragMode == DragMode::panning)
    {
        const auto span = dragAnchorViewEnd - dragAnchorViewStart;
        const auto secondsPerPixel = span / juce::jmax (1, getWidth());
        const auto deltaSeconds = -(double) e.getDistanceFromDragStartX() * secondsPerPixel;
        setView (dragAnchorViewStart + deltaSeconds, dragAnchorViewEnd + deltaSeconds);
        return;
    }

    const auto snapshot = processor.getSliceBank().getCurrent();

    if (dragSliceIndex < 0 || dragSliceIndex >= (int) snapshot->size())
        return;

    const auto time = juce::jlimit (0.0f, (float) sampleDuration, xToTime ((float) e.position.x));
    const auto& slice = (*snapshot)[(size_t) dragSliceIndex];

    if (dragMode == DragMode::movingStart)
    {
        const auto newStart = juce::jmin (time, slice.end - minSliceLengthSeconds);
        processor.setSliceParam (dragSliceIndex, [newStart] (Slice& s) { s.start = newStart; }, true);
    }
    else // movingEnd or creating
    {
        const auto newEnd = juce::jmax (time, slice.start + minSliceLengthSeconds);
        processor.setSliceParam (dragSliceIndex, [newEnd] (Slice& s) { s.end = newEnd; }, true);
    }

    repaint();
}

void WaveformDisplay::mouseUp (const juce::MouseEvent&)
{
    if (dragMode == DragMode::creating && dragSliceIndex >= 0)
    {
        const auto snapshot = processor.getSliceBank().getCurrent();

        if (dragSliceIndex < (int) snapshot->size())
        {
            const auto& slice = (*snapshot)[(size_t) dragSliceIndex];

            if (slice.end - slice.start < minSliceLengthSeconds)
                processor.deleteSlice (dragSliceIndex); // just a click, no real drag - discard
        }
    }

    dragMode = DragMode::none;
    dragSliceIndex = -1;
}

void WaveformDisplay::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    if (sampleDuration <= 0.0)
        return;

    const auto timeAtCursor = xToTime (e.position.x);
    const auto zoomFactor = wheel.deltaY > 0.0f ? 0.8 : 1.25; // wheel up = zoom in

    const auto span = viewEnd - viewStart;
    const auto newSpan = juce::jlimit (0.005, sampleDuration, span * zoomFactor);
    const auto ratio = ((double) timeAtCursor - viewStart) / juce::jmax (1.0e-9, span);
    const auto newStart = (double) timeAtCursor - ratio * newSpan;

    setView (newStart, newStart + newSpan);
}
