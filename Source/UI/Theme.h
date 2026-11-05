#pragma once
#include <JuceHeader.h>

namespace rs::theme
{
    // "Black sandstone": warm near-black stone, sand-coloured accents
    const juce::Colour bg        { 0xff0c0b0a };
    const juce::Colour panel     { 0xff141210 };
    const juce::Colour panelHi   { 0xff1d1a17 };
    const juce::Colour lane      { 0xff100f0d };
    const juce::Colour laneAlt   { 0xff141210 };
    const juce::Colour grid      { 0xff26221e };
    const juce::Colour gridBar   { 0xff3a342d };
    const juce::Colour text      { 0xffeee5d6 };
    const juce::Colour textDim   { 0xff978b7b };
    const juce::Colour accent    { 0xffd8a96e };   // sand
    const juce::Colour accent2   { 0xff8fbfa8 };   // sage
    const juce::Colour record    { 0xffe5484d };

    // fine stone grain, tiled over the whole window at low strength
    inline const juce::Image& grain()
    {
        static juce::Image img = []
        {
            juce::Image im (juce::Image::ARGB, 192, 192, true);
            juce::Random rnd (42);
            for (int y = 0; y < im.getHeight(); ++y)
                for (int x = 0; x < im.getWidth(); ++x)
                {
                    const float n = rnd.nextFloat();
                    const auto c = n > 0.5f ? juce::Colour (0xfff2dcc0) : juce::Colour (0xff000000);
                    im.setPixelAt (x, y, c.withAlpha (std::abs (n - 0.5f) * 0.09f));
                }
            return im;
        }();
        return img;
    }
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
            setColour (juce::TextButton::textColourOnId, bg);
            setColour (juce::Slider::thumbColourId, accent);
            setColour (juce::Slider::trackColourId, accent.withAlpha (0.7f));
            setColour (juce::Slider::backgroundColourId, grid);
            setColour (juce::Slider::rotarySliderFillColourId, accent);
            setColour (juce::Slider::rotarySliderOutlineColourId, grid);
            setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
            setColour (juce::Slider::textBoxBackgroundColourId, panelHi);
            setColour (juce::Label::textColourId, text);
            setColour (juce::ComboBox::backgroundColourId, panelHi);
            setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
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

        // Borderless buttons: just text, a soft wash on hover, and a tint of the
        // button's "on" colour when it's switched on.
        void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
        {
            auto r = b.getLocalBounds().toFloat().reduced (0.5f);
            if (b.getToggleState())
            {
                g.setColour (b.findColour (juce::TextButton::buttonOnColourId).withAlpha (down ? 0.32f : over ? 0.26f : 0.2f));
                g.fillRoundedRectangle (r, 5.0f);
            }
            else if (down || over)
            {
                g.setColour (text.withAlpha (down ? 0.10f : 0.055f));
                g.fillRoundedRectangle (r, 5.0f);
            }
        }

        void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool over, bool) override
        {
            auto c = b.getToggleState() ? b.findColour (juce::TextButton::buttonOnColourId).brighter (0.25f)
                                        : b.findColour (juce::TextButton::textColourOffId);
            if (! b.isEnabled()) c = c.withMultipliedAlpha (0.4f);
            else if (over && ! b.getToggleState()) c = c.brighter (0.15f);
            g.setColour (c);
            g.setFont (getTextButtonFont (b, b.getHeight()));
            g.drawFittedText (b.getButtonText(), b.getLocalBounds().reduced (4, 2), juce::Justification::centred, 1);
        }

        void drawComboBox (juce::Graphics& g, int w, int h, bool, int, int, int, int, juce::ComboBox& box) override
        {
            auto r = juce::Rectangle<float> (0, 0, (float) w, (float) h).reduced (0.5f);
            g.setColour (box.isMouseOver (true) ? text.withAlpha (0.07f) : text.withAlpha (0.035f));
            g.fillRoundedRectangle (r, 5.0f);
            juce::Path arrow;
            const float cx = (float) w - 14.0f, cy = (float) h * 0.5f;
            arrow.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
            g.setColour (box.findColour (juce::ComboBox::arrowColourId).withAlpha (box.isEnabled() ? 0.8f : 0.3f));
            g.fillPath (arrow);
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

        juce::Font getTextButtonFont (juce::TextButton& b, int h) override
        {
            return font (b.getProperties()["compact"] ? 12.5f : juce::jmin (14.0f, h * 0.55f), true);
        }

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
