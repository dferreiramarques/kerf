#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Light, flat, Ableton-Simpler-style look: pale panels, small flat buttons, thin-arc knobs and
// value "bars" with an orange fill.
namespace KerfColours
{
    const juce::Colour window   { 0xffdcdcde };
    const juce::Colour panel    { 0xffcbcbce };
    const juce::Colour control  { 0xffb9b9be };
    const juce::Colour outline  { 0xff8c8c92 };
    const juce::Colour text     { 0xff1e1e22 };
    const juce::Colour textDim  { 0xff55555c };
    const juce::Colour accent   { 0xffffa726 };
    const juce::Colour marker   { 0xff29a8d6 };
    const juce::Colour wave     { 0xfff29a1f };
    const juce::Colour waveBg   { 0xfff7f6f2 };
}

class KerfLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    KerfLookAndFeel()
    {
        using namespace KerfColours;
        setColour (juce::ResizableWindow::backgroundColourId, window);
        setColour (juce::Label::textColourId, text);
        setColour (juce::TextButton::buttonColourId, control);
        setColour (juce::TextButton::buttonOnColourId, accent);
        setColour (juce::TextButton::textColourOffId, text);
        setColour (juce::TextButton::textColourOnId, text);
        setColour (juce::ToggleButton::textColourId, text);
        setColour (juce::Slider::backgroundColourId, control);
        setColour (juce::Slider::trackColourId, accent);
        setColour (juce::Slider::thumbColourId, text);
        setColour (juce::Slider::rotarySliderFillColourId, accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, outline);
        setColour (juce::Slider::textBoxTextColourId, text);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::backgroundColourId, control);
        setColour (juce::ComboBox::textColourId, text);
        setColour (juce::ComboBox::outlineColourId, outline);
        setColour (juce::ComboBox::arrowColourId, text);
        setColour (juce::PopupMenu::backgroundColourId, window);
        setColour (juce::PopupMenu::textColourId, text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
        setColour (juce::PopupMenu::highlightedTextColourId, text);
        setColour (juce::AlertWindow::backgroundColourId, window);
        setColour (juce::AlertWindow::textColourId, text);
        setColour (juce::AlertWindow::outlineColourId, outline);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int) override { return juce::Font (juce::FontOptions (12.5f)); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg,
                               bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = bg;
        if (down) c = c.darker (0.15f);
        else if (over) c = c.brighter (0.12f);
        g.setColour (c);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (KerfColours::outline);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
    }

    void drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool over, bool) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        const auto on = b.getToggleState();
        g.setColour (on ? KerfColours::accent : (over ? KerfColours::control.brighter (0.12f) : KerfColours::control));
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (KerfColours::outline);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
        g.setColour (KerfColours::text);
        g.setFont (juce::Font (juce::FontOptions (12.5f)));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        auto area = juce::Rectangle<int> (x, y, w, h).toFloat().reduced (4.0f);
        const auto d = juce::jmin (area.getWidth(), area.getHeight());
        auto knob = juce::Rectangle<float> (d, d).withCentre (area.getCentre());
        const auto angle = startAngle + pos * (endAngle - startAngle);
        const auto arcR = d * 0.5f - 1.5f;

        juce::Path track;
        track.addCentredArc (knob.getCentreX(), knob.getCentreY(), arcR, arcR, 0.0f, startAngle, endAngle, true);
        g.setColour (KerfColours::outline.withAlpha (0.55f));
        g.strokePath (track, juce::PathStrokeType (2.5f));

        // bipolar knobs (range spans zero symmetrically, e.g. pan) fill from centre
        const auto bipolar = s.getMinimum() < 0.0 && s.getMaximum() > 0.0;
        const auto fromAngle = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;
        juce::Path val;
        val.addCentredArc (knob.getCentreX(), knob.getCentreY(), arcR, arcR, 0.0f,
                           juce::jmin (fromAngle, angle), juce::jmax (fromAngle, angle), true);
        g.setColour (s.isEnabled() ? KerfColours::accent : KerfColours::outline);
        g.strokePath (val, juce::PathStrokeType (2.5f));

        const auto inner = knob.reduced (6.0f);
        g.setColour (KerfColours::window.brighter (0.2f));
        g.fillEllipse (inner);
        g.setColour (KerfColours::outline);
        g.drawEllipse (inner, 1.0f);

        const auto c = inner.getCentre();
        const auto rad = inner.getWidth() * 0.5f;
        g.setColour (KerfColours::text);
        g.drawLine (c.x + std::sin (angle) * rad * 0.25f, c.y - std::cos (angle) * rad * 0.25f,
                    c.x + std::sin (angle) * rad * 0.9f,  c.y - std::cos (angle) * rad * 0.9f, 2.0f);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        if (style != juce::Slider::LinearBar)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0, 0, style, s);
            return;
        }

        auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (0.5f);
        g.setColour (KerfColours::control);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (KerfColours::accent.withAlpha (0.85f));
        g.fillRoundedRectangle (r.withRight (juce::jmax (r.getX() + 2.0f, pos)), 2.0f);
        g.setColour (KerfColours::outline);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);
    }

    juce::Slider::SliderLayout getSliderLayout (juce::Slider& s) override
    {
        auto l = LookAndFeel_V4::getSliderLayout (s);
        if (s.getSliderStyle() == juce::Slider::LinearBar)
            l.sliderBounds = s.getLocalBounds();
        return l;
    }

    juce::Label* createSliderTextBox (juce::Slider& s) override
    {
        auto* l = LookAndFeel_V4::createSliderTextBox (s);
        l->setFont (juce::Font (juce::FontOptions (12.0f)));
        l->setJustificationType (juce::Justification::centred);
        return l;
    }

    void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox&) override
    {
        auto r = juce::Rectangle<float> (0.0f, 0.0f, (float) w, (float) h).reduced (0.5f);
        g.setColour (KerfColours::control);
        g.fillRoundedRectangle (r, 2.0f);
        g.setColour (KerfColours::outline);
        g.drawRoundedRectangle (r, 2.0f, 1.0f);

        juce::Path arrow;
        const auto cx = (float) w - 11.0f, cy = (float) h * 0.5f;
        arrow.addTriangle (cx - 3.5f, cy - 2.0f, cx + 3.5f, cy - 2.0f, cx, cy + 3.0f);
        g.setColour (KerfColours::text);
        g.fillPath (arrow);
    }
};
