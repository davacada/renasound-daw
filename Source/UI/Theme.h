#pragma once
#include <JuceHeader.h>

namespace rs::theme
{
    const juce::Colour bg        { 0xff15161b };
    const juce::Colour panel     { 0xff1d1f26 };
    const juce::Colour panelHi   { 0xff262934 };
    const juce::Colour lane      { 0xff1a1c22 };
    const juce::Colour laneAlt   { 0xff1e2028 };
    const juce::Colour grid      { 0xff2a2d38 };
    const juce::Colour gridBar   { 0xff3a3e4c };
    const juce::Colour text      { 0xffe8e9ee };
    const juce::Colour textDim   { 0xff8b8fa0 };
    const juce::Colour accent    { 0xffff5a36 };   // Renasound orange
    const juce::Colour accent2   { 0xff3ec7a5 };
    const juce::Colour record    { 0xffff3b4e };
    const juce::Colour playhead  { 0xffffffff };

    inline juce::Font font (float h, bool bold = false)
    {
        return juce::Font (juce::FontOptions (h, bold ? juce::Font::bold : juce::Font::plain));
    }

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel()
        {
            setColourScheme ({ panel, bg, panelHi, grid, text, accent, juce::Colours::white, accent, text });

            setColour (juce::ResizableWindow::backgroundColourId, bg);
            setColour (juce::TextButton::buttonColourId, panelHi);
            setColour (juce::TextButton::buttonOnColourId, accent);
            setColour (juce::TextButton::textColourOffId, text);
            setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            setColour (juce::Slider::thumbColourId, accent);
            setColour (juce::Slider::trackColourId, accent.withAlpha (0.7f));
            setColour (juce::Slider::backgroundColourId, grid);
            setColour (juce::Slider::rotarySliderFillColourId, accent);
            setColour (juce::Slider::rotarySliderOutlineColourId, grid);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxBackgroundColourId, panelHi);
            setColour (juce::Label::textColourId, text);
            setColour (juce::ComboBox::backgroundColourId, panelHi);
            setColour (juce::ComboBox::outlineColourId, grid);
            setColour (juce::PopupMenu::backgroundColourId, panel);
            setColour (juce::PopupMenu::highlightedBackgroundColourId, accent);
            setColour (juce::ListBox::backgroundColourId, panel);
            setColour (juce::TabbedButtonBar::tabOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::TabbedButtonBar::frontOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::TabbedComponent::outlineColourId, juce::Colours::transparentBlack);
            setColour (juce::ScrollBar::thumbColourId, gridBar);
            setColour (juce::TableHeaderComponent::backgroundColourId, panelHi);
            setColour (juce::TableHeaderComponent::textColourId, text);
            setColour (juce::TableHeaderComponent::outlineColourId, bg);
            setColour (juce::TableHeaderComponent::highlightColourId, accent.withAlpha (0.3f));
            setColour (juce::MidiKeyboardComponent::keyDownOverlayColourId, accent);
            setColour (juce::MidiKeyboardComponent::mouseOverKeyOverlayColourId, accent.withAlpha (0.3f));
        }

        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bgc, bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            auto c = bgc;
            if (down) c = c.brighter (0.15f); else if (over) c = c.brighter (0.07f);
            g.setColour (c);
            g.fillRoundedRectangle (r, 5.0f);
        }

        // Knobs: a body with a pointer, a track ring and a value arc. Bipolar
        // controls (pan, -x..+x) fill from the centre, so "centre" reads clearly.
        void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos,
                               float startAngle, float endAngle, juce::Slider& s) override
        {
            auto r = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (1.5f);
            const float size = juce::jmin (r.getWidth(), r.getHeight());
            auto box = r.withSizeKeepingCentre (size, size);
            const float ring = juce::jlimit (2.0f, 4.0f, size * 0.11f);
            const auto c = box.getCentre();
            const float radius = size * 0.5f - ring * 0.5f;

            const bool bipolar = juce::approximatelyEqual (s.getMinimum(), -s.getMaximum());
            const float angle = startAngle + pos * (endAngle - startAngle);
            const float from = bipolar ? (startAngle + endAngle) * 0.5f : startAngle;

            juce::Path track;
            track.addCentredArc (c.x, c.y, radius, radius, 0.0f, startAngle, endAngle, true);
            g.setColour (grid.brighter (0.25f));
            g.strokePath (track, juce::PathStrokeType (ring, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

            if (std::abs (angle - from) > 0.01f)
            {
                juce::Path value;
                value.addCentredArc (c.x, c.y, radius, radius, 0.0f, juce::jmin (from, angle), juce::jmax (from, angle), true);
                g.setColour (s.isEnabled() ? s.findColour (juce::Slider::rotarySliderFillColourId) : grid);
                g.strokePath (value, juce::PathStrokeType (ring, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            const float bodyR = radius - ring * 0.9f;
            g.setColour (panelHi.brighter (0.12f));
            g.fillEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2.0f, bodyR * 2.0f);
            g.setColour (bg.withAlpha (0.6f));
            g.drawEllipse (c.x - bodyR, c.y - bodyR, bodyR * 2.0f, bodyR * 2.0f, 1.0f);

            const auto tip = c.getPointOnCircumference (bodyR * 0.85f, angle);
            const auto base = c.getPointOnCircumference (bodyR * 0.2f, angle);
            g.setColour (juce::Colours::white.withAlpha (0.9f));
            g.drawLine ({ base, tip }, juce::jmax (1.5f, size * 0.07f));
        }

        juce::Font getTextButtonFont (juce::TextButton&, int h) override { return font (juce::jmin (14.0f, h * 0.55f), true); }

        void drawTabButton (juce::TabBarButton& b, juce::Graphics& g, bool over, bool) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (2.0f, 4.0f);
            const bool front = b.isFrontTab();
            if (front || over)
            {
                g.setColour (front ? panelHi : panelHi.withAlpha (0.5f));
                g.fillRoundedRectangle (r, 5.0f);
            }
            g.setColour (front ? text : textDim);
            g.setFont (font (13.0f, true));
            g.drawText (b.getButtonText(), r, juce::Justification::centred);
            if (front)
            {
                g.setColour (accent);
                g.fillRect (r.getX() + 10, r.getBottom() - 2, r.getWidth() - 20, 2.0f);
            }
        }
    };
}
