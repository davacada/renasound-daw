#pragma once
#include <JuceHeader.h>
#include "Theme.h"

// =============================================================================
//  Musical typing: the computer keyboard plays the selected instrument.
//    A S D F G H J K L ;   white keys        W E   T Y U   O P   black keys
//    Z / X                 octave down / up  C / V           velocity down / up
//  The on-screen piano shows which letter plays which key while it's on.
// =============================================================================

namespace rs
{

class MusicalTyping : public juce::KeyListener
{
public:
    explicit MusicalTyping (juce::MidiKeyboardState& s) : state (s) {}
    ~MusicalTyping() override { releaseAll(); }

    static constexpr const char* keys = "awsedftgyhujkolp;";   // C, C#, D ... E (17 semitones)

    bool isEnabled() const { return enabled; }
    void setEnabled (bool on) { if (! on) releaseAll(); enabled = on; if (onChange) onChange(); }

    int baseNote = 60;      // the note under A (C4)
    int velocity = 100;     // 1..127
    std::function<void()> onChange;

    int noteForKey (int index) const { return baseNote + index; }
    int keyIndexForNote (int note) const { const int i = note - baseNote; return i >= 0 && i < (int) std::strlen (keys) ? i : -1; }

    bool keyPressed (const juce::KeyPress& key, juce::Component* originator) override
    {
        if (! wantsKeys (key.getModifiers(), originator)) return false;
        const auto c = juce::CharacterFunctions::toLowerCase (key.getTextCharacter());
        if (c == 'z' || c == 'x')
        {
            const int nb = juce::jlimit (12, 108, baseNote + (c == 'z' ? -12 : 12));
            if (nb != baseNote) { releaseAll(); baseNote = nb; if (onChange) onChange(); }
            return true;
        }
        if (c == 'c' || c == 'v')
        {
            velocity = juce::jlimit (1, 127, velocity + (c == 'c' ? -20 : 20));
            if (onChange) onChange();
            return true;
        }
        return c != 0 && std::strchr (keys, (int) c) != nullptr;   // played in keyStateChanged
    }

    bool keyStateChanged (bool, juce::Component* originator) override
    {
        if (! wantsKeys (juce::ModifierKeys::getCurrentModifiers(), originator)) return false;
        bool used = false;
        for (int i = 0; keys[i] != 0; ++i)
        {
            const bool down = juce::KeyPress (keys[i], 0, 0).isCurrentlyDown();
            auto& h = held[(size_t) i];
            if (down && h < 0)
            {
                h = juce::jlimit (0, 127, noteForKey (i));
                state.noteOn (1, h, (float) velocity / 127.0f);
                used = true;
            }
            else if (! down && h >= 0)
            {
                state.noteOff (1, h, 0.0f);
                h = -1;
                used = true;
            }
        }
        return used;
    }

    void releaseAll()
    {
        for (auto& h : held)
            if (h >= 0) { state.noteOff (1, h, 0.0f); h = -1; }
    }

private:
    juce::MidiKeyboardState& state;
    bool enabled = false;
    std::array<int, 17> held { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

    bool wantsKeys (juce::ModifierKeys m, juce::Component* originator) const
    {
        if (! enabled || m.isCommandDown() || m.isCtrlDown() || m.isAltDown()) return false;
        // typing a name or a value still types
        for (auto* c = originator; c != nullptr; c = c->getParentComponent())
            if (dynamic_cast<juce::TextEditor*> (c) != nullptr) return false;
        if (auto* f = juce::Component::getCurrentlyFocusedComponent(); dynamic_cast<juce::TextEditor*> (f) != nullptr) return false;
        return true;
    }
};

// The on-screen piano, with the typing letters drawn on the keys while musical typing is on
class TypingKeyboard : public juce::MidiKeyboardComponent
{
public:
    TypingKeyboard (juce::MidiKeyboardState& s, const MusicalTyping& t)
        : juce::MidiKeyboardComponent (s, horizontalKeyboard), typing (t) {}

    void drawWhiteNote (int note, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver,
                        juce::Colour line, juce::Colour text) override
    {
        juce::MidiKeyboardComponent::drawWhiteNote (note, g, area, isDown, isOver, line, text);
        drawLetter (note, g, area, juce::Colour (0xff2a2520), area.getBottom() - 16);
    }

    void drawBlackNote (int note, juce::Graphics& g, juce::Rectangle<float> area, bool isDown, bool isOver, juce::Colour fill) override
    {
        juce::MidiKeyboardComponent::drawBlackNote (note, g, area, isDown, isOver, fill);
        drawLetter (note, g, area, juce::Colour (0xfff2e7d6), area.getBottom() - 16);
    }

    juce::String getWhiteNoteText (int note) override
    {
        // the letter takes the place of the "C4" label while typing is on
        return typing.isEnabled() && typing.keyIndexForNote (note) >= 0 ? juce::String() : juce::MidiKeyboardComponent::getWhiteNoteText (note);
    }

private:
    const MusicalTyping& typing;

    void drawLetter (int note, juce::Graphics& g, juce::Rectangle<float> area, juce::Colour c, float y)
    {
        if (! typing.isEnabled()) return;
        const int i = typing.keyIndexForNote (note);
        if (i < 0) return;
        g.setColour (theme::accent.withAlpha (0.9f));
        const float w = juce::jmin (14.0f, area.getWidth() - 2.0f);
        auto r = juce::Rectangle<float> (area.getCentreX() - w * 0.5f, y, w, 14);
        g.fillRoundedRectangle (r, 3.0f);
        g.setColour (juce::Colour (0xff15120f));
        juce::ignoreUnused (c);
        g.setFont (theme::font (10.0f, true));
        g.drawText (juce::String::charToString ((juce::juce_wchar) juce::CharacterFunctions::toUpperCase ((juce::juce_wchar) MusicalTyping::keys[i])),
                    r, juce::Justification::centred);
    }
};

} // namespace rs
