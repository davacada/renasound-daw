#include "PianoRoll.h"
#include "DeviceWindow.h"
#include "../Drums.h"

namespace rs
{

static bool isBlackKey (int pitch)
{
    const int n = pitch % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

// =============================================================================
class PianoRoll::Keys : public juce::Component
{
public:
    explicit Keys (PianoRoll& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        g.setFont (theme::font (10.0f, true));
        DrumMachine* drums = nullptr;
        if (owner.track != nullptr && owner.track->instrument != nullptr)
            drums = dynamic_cast<DrumMachine*> (owner.track->instrument->processor.get());
        for (int p = 0; p < 128; ++p)
        {
            const int y = (127 - p) * owner.noteHeight - owner.scrollY;
            if (y + owner.noteHeight < 0 || y > getHeight()) continue;
            const bool black = isBlackKey (p);
            g.setColour (p == held ? theme::accent : black ? juce::Colour (0xff22242c) : juce::Colour (0xffd9dbe3));
            g.fillRect (0, y, black ? getWidth() - 18 : getWidth(), owner.noteHeight - 1);
            if (drums != nullptr)
            {
                // drum tracks: name the pads instead of the notes
                if (const int pad = drums->padForNote (p); pad >= 0 && drums->getPadNote (pad) == p)
                {
                    g.setColour (theme::accent.withAlpha (0.25f));
                    g.fillRect (0, y, getWidth(), owner.noteHeight - 1);
                    g.setColour (juce::Colours::black.withAlpha (0.8f));
                    g.setFont (theme::font (9.5f, true));
                    g.drawText (drums->getPadName (pad), 2, y, getWidth() - 4, owner.noteHeight, juce::Justification::centredLeft, true);
                }
            }
            else if (p % 12 == 0)
            {
                g.setColour (juce::Colours::black.withAlpha (0.7f));
                g.drawText ("C" + juce::String (p / 12 - 1), 0, y, getWidth() - 4, owner.noteHeight, juce::Justification::centredRight);
            }
        }
    }

    int pitchAt (int y) const { return juce::jlimit (0, 127, 127 - (y + owner.scrollY) / owner.noteHeight); }

    void mouseDown (const juce::MouseEvent& e) override { press (pitchAt (e.y)); }
    void mouseDrag (const juce::MouseEvent& e) override { if (pitchAt (e.y) != held) press (pitchAt (e.y)); }
    void mouseUp (const juce::MouseEvent&) override     { release(); }

private:
    void press (int p)
    {
        release();
        held = p;
        owner.ctx.engine.keyboardState.noteOn (1, p, 0.8f);
        repaint();
    }
    void release()
    {
        if (held >= 0) owner.ctx.engine.keyboardState.noteOff (1, held, 0.0f);
        held = -1;
        repaint();
    }

    PianoRoll& owner;
    int held = -1;
};

// =============================================================================
class PianoRoll::Grid : public juce::Component
{
public:
    explicit Grid (PianoRoll& o) : owner (o) {}

    float beatToX (double b) const { return (float) (b * owner.pixelsPerBeat - owner.scrollX); }
    double xToBeat (float x) const { return (x + owner.scrollX) / owner.pixelsPerBeat; }
    int pitchToY (int p) const     { return (127 - p) * owner.noteHeight - owner.scrollY; }
    int yToPitch (int y) const     { return juce::jlimit (0, 127, 127 - (y + owner.scrollY) / owner.noteHeight); }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::lane);
        if (owner.clip == nullptr)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (15.0f));
            g.drawText ("Double-click an instrument track's lane to make a MIDI clip, then edit its notes here",
                        getLocalBounds(), juce::Justification::centred);
            return;
        }

        auto& c = *owner.clip;
        for (int p = 0; p < 128; ++p)
        {
            const int y = pitchToY (p);
            if (y + owner.noteHeight < 0 || y > getHeight()) continue;
            g.setColour (isBlackKey (p) ? theme::lane.darker (0.25f) : theme::laneAlt);
            g.fillRect (0, y, getWidth(), owner.noteHeight);
            g.setColour (p % 12 == 0 ? theme::gridBar : theme::bg.withAlpha (0.6f));
            g.drawHorizontalLine (y + owner.noteHeight - 1, 0, (float) getWidth());
        }

        const int bpb = owner.ctx.project.beatsPerBar;
        const double last = xToBeat ((float) getWidth()) + 1;
        for (double b = juce::jmax (0.0, std::floor (xToBeat (0))); b <= last; b += owner.snap)
        {
            const bool bar = std::fmod (b, (double) bpb) < 1.0e-6, beat = std::fmod (b, 1.0) < 1.0e-6;
            g.setColour (bar ? theme::gridBar.brighter (0.2f) : beat ? theme::gridBar : theme::grid);
            g.drawVerticalLine ((int) beatToX (b), 0, (float) getHeight());
        }

        // area outside the clip
        const float endX = beatToX (c.lengthBeats);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (juce::Rectangle<float> (endX, 0, (float) getWidth() - endX, (float) getHeight()));
        g.setColour (theme::accent);
        g.fillRect (endX - 1, 0.0f, 2.0f, (float) getHeight());

        // notes
        const auto col = owner.track != nullptr ? owner.track->colour : theme::accent;
        for (size_t i = 0; i < c.notes.size(); ++i)
        {
            auto r = noteBounds (c.notes[i]);
            g.setColour (col.withAlpha (0.4f + 0.6f * c.notes[i].velocity));
            g.fillRoundedRectangle (r, 2.5f);
            g.setColour ((int) i == selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
            g.drawRoundedRectangle (r, 2.5f, (int) i == selected ? 1.6f : 1.0f);
        }

        // playhead (relative to the clip)
        const double rel = owner.ctx.engine.getPositionBeats() - c.startBeat;
        if (rel >= 0 && rel <= c.lengthBeats)
        {
            g.setColour (theme::playhead.withAlpha (0.9f));
            g.fillRect (beatToX (rel) - 0.5f, 0.0f, 1.5f, (float) getHeight());
        }
    }

    juce::Rectangle<float> noteBounds (const MidiNote& n) const
    {
        const float x0 = beatToX (n.start), x1 = beatToX (n.start + n.length);
        return { x0, (float) pitchToY (n.pitch) + 1, juce::jmax (3.0f, x1 - x0 - 1), (float) owner.noteHeight - 2 };
    }

    int noteAt (juce::Point<float> p) const
    {
        if (owner.clip == nullptr) return -1;
        auto& notes = owner.clip->notes;
        for (int i = (int) notes.size(); --i >= 0;)
            if (noteBounds (notes[(size_t) i]).contains (p)) return i;
        return -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int i = noteAt (e.position);
        if (i >= 0 && e.position.x > noteBounds (owner.clip->notes[(size_t) i]).getRight() - 6)
            setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        else
            setMouseCursor (i >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (owner.clip == nullptr) return;
        auto& p = owner.ctx.project;
        auto& notes = owner.clip->notes;
        const int i = noteAt (e.position);

        // right-click or alt-click deletes
        if (i >= 0 && (e.mods.isPopupMenu() || e.mods.isAltDown()))
        {
            p.beginEdit();
            const juce::ScopedLock sl (p.lock);
            notes.erase (notes.begin() + i);
            selected = -1;
            drag = Drag::None;
            repaint();
            return;
        }
        if (e.mods.isPopupMenu()) return;

        p.beginEdit();
        if (i >= 0)
        {
            selected = i;
            drag = e.position.x > noteBounds (notes[(size_t) i]).getRight() - 6 ? Drag::Resize : Drag::Move;
        }
        else
        {
            // add a note
            MidiNote n;
            n.pitch = yToPitch (e.y);
            n.start = juce::jmax (0.0, std::floor (xToBeat (e.position.x) / owner.snap) * owner.snap);
            n.length = lastLength;
            if (n.start >= owner.clip->lengthBeats) return;
            {
                const juce::ScopedLock sl (p.lock);
                notes.push_back (n);
            }
            selected = (int) notes.size() - 1;
            drag = Drag::Move;
        }
        orig = notes[(size_t) selected];
        downBeat = xToBeat (e.position.x);
        audition (orig.pitch);
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (owner.clip == nullptr || selected < 0 || drag == Drag::None) return;
        auto& p = owner.ctx.project;
        auto& n = owner.clip->notes[(size_t) selected];
        const double delta = xToBeat (e.position.x) - downBeat;
        const double s = e.mods.isShiftDown() ? 0.0 : owner.snap;

        const juce::ScopedLock sl (p.lock);
        if (drag == Drag::Move)
        {
            n.start = juce::jlimit (0.0, owner.clip->lengthBeats - 0.01, owner.ctx.snapBeat (orig.start + delta, s));
            const int newPitch = yToPitch (e.y);
            if (newPitch != n.pitch) { n.pitch = newPitch; audition (newPitch); }
        }
        else
        {
            n.length = juce::jmax (s > 0 ? s : 0.05, owner.ctx.snapBeat (orig.start + orig.length + delta, s) - n.start);
            lastLength = n.length;
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag != Drag::None) owner.ctx.project.changed();
        drag = Drag::None;
        stopAudition();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int i = noteAt (e.position);
        if (i < 0 || owner.clip == nullptr) return;
        owner.ctx.project.beginEdit();
        const juce::ScopedLock sl (owner.ctx.project.lock);
        owner.clip->notes.erase (owner.clip->notes.begin() + i);
        selected = -1;
        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if ((k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) && owner.clip != nullptr
             && selected >= 0 && selected < (int) owner.clip->notes.size())
        {
            owner.ctx.project.beginEdit();
            const juce::ScopedLock sl (owner.ctx.project.lock);
            owner.clip->notes.erase (owner.clip->notes.begin() + selected);
            selected = -1;
            repaint();
            return true;
        }
        return false;
    }

    int selected = -1;

private:
    enum class Drag { None, Move, Resize };

    void audition (int pitch)
    {
        stopAudition();
        if (owner.track == nullptr) return;
        owner.ctx.engine.liveInputTrackId = owner.track->id;
        auditioning = pitch;
        owner.ctx.engine.keyboardState.noteOn (1, pitch, 0.7f);
    }
    void stopAudition()
    {
        if (auditioning >= 0) owner.ctx.engine.keyboardState.noteOff (1, auditioning, 0.0f);
        auditioning = -1;
    }

    PianoRoll& owner;
    Drag drag = Drag::None;
    MidiNote orig;
    double downBeat = 0, lastLength = 1.0;
    int auditioning = -1;
};

// =============================================================================
PianoRoll::PianoRoll (AppContext& c) : ctx (c)
{
    keys = std::make_unique<Keys> (*this);
    grid = std::make_unique<Grid> (*this);
    addAndMakeVisible (*keys);
    addAndMakeVisible (*grid);
    grid->setWantsKeyboardFocus (true);
    addAndMakeVisible (hScroll);
    addAndMakeVisible (vScroll);
    hScroll.addListener (this);
    vScroll.addListener (this);
    hScroll.setAutoHide (false);

    addAndMakeVisible (title);
    title.setFont (theme::font (14.0f, true));

    addAndMakeVisible (preset);
    const auto& all = getSynthPresets();
    for (int i = 0; i < (int) all.size(); ++i)
        preset.addItem (all[(size_t) i].name, i + 1);
    preset.addSeparator();
    preset.addItem ("Plugin instrument...", 1000);
    preset.onChange = [this]
    {
        if (track == nullptr || track->synth == nullptr) return;
        const int id = preset.getSelectedId();
        if (id == 1000) { refreshHeader(); devicemenus::showInstrumentMenu (ctx, track->id, &preset); return; }
        if (id <= 0) return;
        if (track->instrument != nullptr) { ctx.project.beginEdit(); ctx.project.setInstrument (track->id, nullptr); }
        track->synth->presetIndex = id - 1;
        ctx.project.dirty = true;
    };
    preset.setTooltip ("Instrument sound for this track");

    addAndMakeVisible (snapBox);
    snapBox.addItem ("Grid 1/4", 1);
    snapBox.addItem ("Grid 1/8", 2);
    snapBox.addItem ("Grid 1/16", 3);
    snapBox.addItem ("Grid 1/32", 4);
    snapBox.setSelectedId (3, juce::dontSendNotification);
    snapBox.onChange = [this]
    {
        static const double values[] = { 1.0, 0.5, 0.25, 0.125 };
        snap = values[snapBox.getSelectedId() - 1];
        repaint();
    };

    addAndMakeVisible (quantize);
    quantize.setTooltip ("Snap every note's start to the grid");
    quantize.onClick = [this]
    {
        if (clip == nullptr) return;
        ctx.project.beginEdit();
        {
            const juce::ScopedLock sl (ctx.project.lock);
            for (auto& n : clip->notes)
                n.start = std::round (n.start / snap) * snap;
        }
        ctx.project.changed();
    };

    addAndMakeVisible (clearNotes);
    clearNotes.onClick = [this]
    {
        if (clip == nullptr) return;
        ctx.project.beginEdit();
        {
            const juce::ScopedLock sl (ctx.project.lock);
            clip->notes.clear();
        }
        ctx.project.changed();
    };

    scrollY = (127 - 76) * noteHeight;   // start around C4-E5
    ctx.project.addChangeListener (this);
    refreshHeader();
    startTimerHz (30);
}

PianoRoll::~PianoRoll()
{
    ctx.project.removeChangeListener (this);
}

void PianoRoll::showClip (int clipId, bool resetView)
{
    TrackPtr owner;
    clip = ctx.project.findClip (clipId, &owner);
    if (clip != nullptr && clip->type != Clip::Type::Midi) clip = nullptr;
    track = clip != nullptr ? owner : nullptr;
    if (clip == nullptr || grid->selected >= (int) clip->notes.size()) grid->selected = -1;

    if (resetView)
    {
        grid->selected = -1;
        scrollX = 0;
    }
    if (resetView && clip != nullptr && ! clip->notes.empty())
    {
        int hi = 0;
        for (auto& n : clip->notes) hi = juce::jmax (hi, n.pitch);
        scrollY = juce::jmax (0, (127 - hi - 4) * noteHeight);
    }
    refreshHeader();
    updateScrollBars();
    repaint();
}

void PianoRoll::refreshHeader()
{
    title.setText (clip != nullptr ? (track != nullptr ? track->name + "  /  " : juce::String()) + clip->name
                                   : juce::String ("Piano roll"), juce::dontSendNotification);
    preset.setEnabled (track != nullptr && track->synth != nullptr);
    if (track != nullptr && track->synth != nullptr)
    {
        if (track->instrument != nullptr) preset.setText (track->instrument->getName(), juce::dontSendNotification);
        else preset.setSelectedId (track->synth->presetIndex + 1, juce::dontSendNotification);
    }
    quantize.setEnabled (clip != nullptr);
    clearNotes.setEnabled (clip != nullptr);
}

void PianoRoll::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // after undo/redo the clip object may have been replaced: look it up again by id
    if (clip != nullptr)
        showClip (clip->id, false);
}

void PianoRoll::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (toolbarHeight).reduced (8, 5);
    clearNotes.setBounds (bar.removeFromRight (64)); bar.removeFromRight (6);
    quantize.setBounds (bar.removeFromRight (80));   bar.removeFromRight (6);
    snapBox.setBounds (bar.removeFromRight (100));   bar.removeFromRight (6);
    preset.setBounds (bar.removeFromRight (140));    bar.removeFromRight (6);
    title.setBounds (bar);

    hScroll.setBounds (r.removeFromBottom (12).withTrimmedLeft (keysWidth).withTrimmedRight (12));
    vScroll.setBounds (r.removeFromRight (12));
    keys->setBounds (r.removeFromLeft (keysWidth));
    grid->setBounds (r);
    updateScrollBars();
}

void PianoRoll::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
}

void PianoRoll::updateScrollBars()
{
    const double lengthBeats = clip != nullptr ? clip->lengthBeats : 16.0;
    hScroll.setRangeLimits (0, juce::jmax ((lengthBeats + 4) * pixelsPerBeat, (double) grid->getWidth()), juce::dontSendNotification);
    hScroll.setCurrentRange (scrollX, grid->getWidth(), juce::dontSendNotification);
    vScroll.setRangeLimits (0, 128 * noteHeight, juce::dontSendNotification);
    vScroll.setCurrentRange (scrollY, grid->getHeight(), juce::dontSendNotification);
}

void PianoRoll::scrollBarMoved (juce::ScrollBar* b, double start)
{
    if (b == &hScroll) scrollX = start;
    else               scrollY = (int) start;
    repaint();
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        pixelsPerBeat = juce::jlimit (12.0, 400.0, pixelsPerBeat * (w.deltaY > 0 ? 1.15 : 1.0 / 1.15));
    }
    else
    {
        float dx = w.deltaX, dy = w.deltaY;
        if (e.mods.isShiftDown()) std::swap (dx, dy);
        scrollX = juce::jmax (0.0, scrollX - dx * 400.0);
        scrollY = juce::jlimit (0, juce::jmax (0, 128 * noteHeight - grid->getHeight()), scrollY - (int) (dy * 300.0f));
    }
    updateScrollBars();
    repaint();
}

void PianoRoll::timerCallback()
{
    if (ctx.engine.isPlaying() && clip != nullptr) grid->repaint();
}

} // namespace rs
