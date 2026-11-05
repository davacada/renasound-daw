#pragma once
#include "../Effects.h"
#include "../DspHelpers.h"
#include "Theme.h"

// =============================================================================
//  Faceplate kit shared by the built-in devices: a look-and-feel per device
//  (knob styles and colours), labelled knobs, segmented switches, and a base
//  editor class.
// =============================================================================

namespace rs
{

using APVTS = juce::AudioProcessorValueTreeState;

enum class KnobStyle { Vintage, Neon, ChickenHead, Glossy, Minimal };

struct Skin
{
    KnobStyle style;
    juce::Colour body, pointer, arc, arcBg, text, textDim, box, boxText;
};

class SkinLook : public juce::LookAndFeel_V4
{
public:
    explicit SkinLook (Skin sk) : s (sk)
    {
        setColour (juce::Slider::textBoxTextColourId, s.boxText);
        setColour (juce::Slider::textBoxBackgroundColourId, s.box);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, s.arc.withAlpha (0.4f));
        setColour (juce::Label::textColourId, s.text);
        setColour (juce::ComboBox::backgroundColourId, s.box);
        setColour (juce::ComboBox::textColourId, s.boxText);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::arrowColourId, s.boxText);
        setColour (juce::PopupMenu::backgroundColourId, theme::panel);
        setColour (juce::PopupMenu::textColourId, theme::text);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, s.arc);
        setColour (juce::TextButton::buttonColourId, s.box);
        setColour (juce::TextButton::buttonOnColourId, s.arc);
        setColour (juce::TextButton::textColourOffId, s.boxText);
        setColour (juce::TextButton::textColourOnId, s.arc.getPerceivedBrightness() > 0.6f ? juce::Colours::black : juce::Colours::white);
        setColour (juce::TooltipWindow::backgroundColourId, theme::panel);
        setColour (juce::TooltipWindow::textColourId, theme::text);
    }

    Skin s;

    juce::Font getComboBoxFont (juce::ComboBox&) override              { return theme::font (13.0f, true); }
    juce::Font getTextButtonFont (juce::TextButton&, int h) override   { return theme::font (juce::jmin (13.0f, h * 0.5f), true); }
    juce::Font getLabelFont (juce::Label& l) override                  { return l.getFont(); }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& bg, bool over, bool down) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (down ? bg.brighter (0.15f) : over ? bg.brighter (0.07f) : bg);
        g.fillRoundedRectangle (r, 4.0f);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& sl) override
    {
        auto box = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).reduced (2.0f);
        const float size = juce::jmin (box.getWidth(), box.getHeight());
        box = box.withSizeKeepingCentre (size, size);
        const auto c = box.getCentre();
        const float R = size * 0.5f;
        const float angle = a0 + pos * (a1 - a0);
        const bool bipolar = juce::approximatelyEqual (sl.getMinimum(), -sl.getMaximum());
        const float from = bipolar ? (a0 + a1) * 0.5f : a0;
        const float alpha = sl.isEnabled() ? 1.0f : 0.4f;

        auto arcPath = [&] (float radius, float s0, float s1)
        {
            juce::Path p;
            p.addCentredArc (c.x, c.y, radius, radius, 0.0f, juce::jmin (s0, s1), juce::jmax (s0, s1), true);
            return p;
        };
        auto pointer = [&] (float r0, float r1, float thick, juce::Colour col)
        {
            g.setColour (col.withMultipliedAlpha (alpha));
            g.drawLine ({ c.getPointOnCircumference (r0, angle), c.getPointOnCircumference (r1, angle) }, thick);
        };

        switch (s.style)
        {
            case KnobStyle::Vintage:
            {
                // tick ring, metal skirt, black cap, amber line
                g.setColour (s.textDim.withMultipliedAlpha (alpha));
                for (int i = 0; i <= 10; ++i)
                {
                    const float a = a0 + (a1 - a0) * (float) i / 10.0f;
                    g.drawLine ({ c.getPointOnCircumference (R * 0.86f, a), c.getPointOnCircumference (R * (i % 5 == 0 ? 1.0f : 0.95f), a) }, i % 5 == 0 ? 1.6f : 1.0f);
                }
                const float skirt = R * 0.78f;
                juce::ColourGradient metal (juce::Colour (0xff9a9ea6), c.x - skirt, c.y - skirt, juce::Colour (0xff3a3d42), c.x + skirt, c.y + skirt, false);
                g.setGradientFill (metal);
                g.fillEllipse (c.x - skirt, c.y - skirt, skirt * 2, skirt * 2);
                const float cap = R * 0.6f;
                juce::ColourGradient capG (juce::Colour (0xff2c2e33), c.x, c.y - cap, juce::Colour (0xff0d0e10), c.x, c.y + cap, false);
                g.setGradientFill (capG);
                g.fillEllipse (c.x - cap, c.y - cap, cap * 2, cap * 2);
                pointer (R * 0.15f, R * 0.74f, 2.6f, s.pointer);
                break;
            }
            case KnobStyle::Neon:
            {
                const float ring = R * 0.86f;
                g.setColour (s.arcBg.withMultipliedAlpha (alpha));
                g.strokePath (arcPath (ring, a0, a1), juce::PathStrokeType (R * 0.13f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                juce::ColourGradient glow (s.arc, c.x - R, c.y, s.pointer, c.x + R, c.y, false);
                auto value = arcPath (ring, from, angle);
                g.setGradientFill (glow);
                g.setOpacity (0.25f * alpha);
                g.strokePath (value, juce::PathStrokeType (R * 0.32f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setOpacity (alpha);
                g.strokePath (value, juce::PathStrokeType (R * 0.13f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                const float body = R * 0.64f;
                g.setColour (s.body);
                g.fillEllipse (c.x - body, c.y - body, body * 2, body * 2);
                g.setColour (s.arcBg.brighter (0.3f));
                g.drawEllipse (c.x - body, c.y - body, body * 2, body * 2, 1.0f);
                const auto dot = c.getPointOnCircumference (body * 0.72f, angle);
                g.setColour (juce::Colours::white.withAlpha (alpha));
                g.fillEllipse (dot.x - 3, dot.y - 3, 6, 6);
                break;
            }
            case KnobStyle::ChickenHead:
            {
                // numbers 0..10 printed on the panel (plain ticks for other ranges), black pointer knob
                g.setFont (theme::font (juce::jmax (8.0f, R * 0.2f), true));
                g.setColour (s.textDim.withMultipliedAlpha (alpha));
                const bool zeroToTen = juce::approximatelyEqual (sl.getMinimum(), 0.0) && juce::approximatelyEqual (sl.getMaximum(), 10.0);
                for (int i = 0; i <= 10; ++i)
                {
                    const float a = a0 + (a1 - a0) * (float) i / 10.0f;
                    const auto p = c.getPointOnCircumference (R * 0.9f, a);
                    if (zeroToTen) g.drawText (juce::String (i), juce::Rectangle<float> (p.x - 8, p.y - 6, 16, 12), juce::Justification::centred);
                    else g.drawLine ({ c.getPointOnCircumference (R * 0.8f, a), c.getPointOnCircumference (R * 0.95f, a) }, 1.2f);
                }
                const float base = R * 0.5f;
                g.setColour (juce::Colours::black.withAlpha (0.25f));
                g.fillEllipse (c.x - base + 2, c.y - base + 3, base * 2, base * 2);
                juce::ColourGradient bg (juce::Colour (0xff3b3a38), c.x, c.y - base, juce::Colour (0xff0e0e0d), c.x, c.y + base, false);
                g.setGradientFill (bg);
                g.fillEllipse (c.x - base, c.y - base, base * 2, base * 2);
                juce::Path head;
                head.startNewSubPath (-base * 0.42f, 0); head.lineTo (0, -R * 0.74f); head.lineTo (base * 0.42f, 0);
                head.addCentredArc (0, 0, base * 0.42f, base * 0.42f, 0, juce::MathConstants<float>::halfPi, juce::MathConstants<float>::pi * 1.5f);
                head.closeSubPath();
                head.applyTransform (juce::AffineTransform::rotation (angle).translated (c));
                g.setColour (juce::Colour (0xff141413));
                g.fillPath (head);
                g.setColour (juce::Colour (0xfff3ead2).withMultipliedAlpha (alpha));
                g.drawLine ({ c.getPointOnCircumference (base * 0.15f, angle), c.getPointOnCircumference (R * 0.66f, angle) }, 2.2f);
                break;
            }
            case KnobStyle::Glossy:
            {
                const float ring = R * 0.9f;
                g.setColour (s.arcBg.withMultipliedAlpha (alpha));
                g.strokePath (arcPath (ring, a0, a1), juce::PathStrokeType (R * 0.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                juce::ColourGradient grad (s.arc, c.x - R, c.y + R, s.pointer, c.x + R, c.y - R, false);
                g.setGradientFill (grad);
                g.setOpacity (alpha);
                g.strokePath (arcPath (ring, from, angle), juce::PathStrokeType (R * 0.1f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                const float body = R * 0.7f;
                juce::ColourGradient bodyG (s.body.brighter (0.5f), c.x - body * 0.5f, c.y - body * 0.7f, s.body.darker (0.6f), c.x + body * 0.4f, c.y + body, true);
                g.setGradientFill (bodyG);
                g.fillEllipse (c.x - body, c.y - body, body * 2, body * 2);
                g.setColour (juce::Colours::white.withAlpha (0.12f));
                g.fillEllipse (c.x - body * 0.7f, c.y - body * 0.85f, body * 1.4f, body * 0.8f);
                const auto dot = c.getPointOnCircumference (body * 0.7f, angle);
                g.setColour (s.pointer.withMultipliedAlpha (alpha));
                g.fillEllipse (dot.x - 3.5f, dot.y - 3.5f, 7, 7);
                break;
            }
            case KnobStyle::Minimal:
            {
                const float ring = R * 0.88f;
                g.setColour (s.arcBg.withMultipliedAlpha (alpha));
                g.strokePath (arcPath (ring, a0, a1), juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                g.setColour (s.arc.withMultipliedAlpha (alpha));
                g.strokePath (arcPath (ring, from, angle), juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
                const float body = R * 0.68f;
                g.setColour (juce::Colours::black.withAlpha (0.10f));
                g.fillEllipse (c.x - body, c.y - body + 3, body * 2, body * 2);
                g.setColour (s.body);
                g.fillEllipse (c.x - body, c.y - body, body * 2, body * 2);
                g.setColour (s.arcBg);
                g.drawEllipse (c.x - body, c.y - body, body * 2, body * 2, 1.0f);
                pointer (body * 0.25f, body * 0.9f, 2.5f, s.pointer);
                break;
            }
        }
    }
};

// A knob with its name, bound to a parameter
class Knob : public juce::Component
{
public:
    Knob (BuiltinFx& fx, const juce::String& id, const juce::String& name, int boxW = 64)
    {
        slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, boxW, 17);
        label.setText (name, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setFont (theme::font (11.0f, true));
        addAndMakeVisible (slider);
        addAndMakeVisible (label);
        attachment = std::make_unique<APVTS::SliderAttachment> (fx.state, id, slider);
        if (auto* p = fx.state.getParameter (id))
            slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    }
    void resized() override
    {
        auto r = getLocalBounds();
        label.setBounds (r.removeFromTop (16));
        slider.setBounds (r);
    }
    juce::Slider slider;
    juce::Label label;
private:
    std::unique_ptr<APVTS::SliderAttachment> attachment;
};

// Segmented buttons for a choice parameter
class Switch : public juce::Component
{
public:
    Switch (BuiltinFx& fx, const juce::String& id)
    {
        auto* p = dynamic_cast<juce::AudioParameterChoice*> (fx.state.getParameter (id));
        jassert (p != nullptr);
        for (int i = 0; i < p->choices.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (p->choices[i]));
            b->setClickingTogglesState (false);
            b->onClick = [this, i] { attachment->setValueAsCompleteGesture ((float) i); };
            addAndMakeVisible (b);
        }
        attachment = std::make_unique<juce::ParameterAttachment> (*p, [this] (float v)
        {
            for (int i = 0; i < buttons.size(); ++i) buttons[i]->setToggleState (i == juce::roundToInt (v), juce::dontSendNotification);
        });
        attachment->sendInitialUpdate();
    }
    void resized() override
    {
        auto r = getLocalBounds();
        const int w = r.getWidth() / juce::jmax (1, buttons.size());
        for (auto* b : buttons) b->setBounds (r.removeFromLeft (w).reduced (1, 0));
    }
private:
    juce::OwnedArray<juce::TextButton> buttons;
    std::unique_ptr<juce::ParameterAttachment> attachment;
};

class FaceEditor : public juce::AudioProcessorEditor, protected juce::Timer
{
public:
    FaceEditor (BuiltinFx& p, Skin sk) : AudioProcessorEditor (p), look (sk) { setLookAndFeel (&look); }
    ~FaceEditor() override { setLookAndFeel (nullptr); }
protected:
    SkinLook look;
    void timerCallback() override { repaint(); }
};


} // namespace rs
