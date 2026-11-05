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
//  The note grid. Three tools:
//    Select - click / drag a box to select, drag to move, drag an edge to resize
//    Edit   - drag a note up / down for velocity, drag either edge for length
//    Draw   - click to write a note (drag to set its length), click a note to move it
//  Right-click anywhere for the menu. Keys: Delete, Cmd+A/C/X/V/D, arrows.
// =============================================================================
class PianoRoll::Grid : public juce::Component
{
public:
    explicit Grid (PianoRoll& o) : owner (o) { setWantsKeyboardFocus (true); }

    float beatToX (double b) const { return (float) (b * owner.pixelsPerBeat - owner.scrollX); }
    double xToBeat (float x) const { return (x + owner.scrollX) / owner.pixelsPerBeat; }
    int pitchToY (int p) const     { return (127 - p) * owner.noteHeight - owner.scrollY; }
    int yToPitch (int y) const     { return juce::jlimit (0, 127, 127 - (y + owner.scrollY) / owner.noteHeight); }

    std::vector<int> selection;   // indices into the clip's notes

    bool isSelected (int i) const { return std::find (selection.begin(), selection.end(), i) != selection.end(); }
    void clearSelection()         { selection.clear(); repaint(); }

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
        const double step = owner.snap * std::pow (2.0, std::ceil (std::log2 (juce::jmax (1.0, 6.0 / (owner.snap * owner.pixelsPerBeat)))));
        for (double b = juce::jmax (0.0, std::floor (xToBeat (0))); b <= last; b += step)
        {
            const bool bar = std::fmod (b, (double) bpb) < 1.0e-6, beat = std::fmod (b, 1.0) < 1.0e-6;
            g.setColour (bar ? theme::gridBar.brighter (0.2f) : beat ? theme::gridBar : theme::grid);
            g.drawVerticalLine ((int) beatToX (b), 0, (float) getHeight());
        }

        const float endX = beatToX (c.lengthBeats);
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRect (juce::Rectangle<float> (endX, 0, (float) getWidth() - endX, (float) getHeight()));
        g.setColour (theme::accent);
        g.fillRect (endX - 1, 0.0f, 2.0f, (float) getHeight());

        // notes: brightness follows velocity; the Edit tool also shows a velocity bar
        const auto col = owner.track != nullptr ? owner.track->colour : theme::accent;
        for (size_t i = 0; i < c.notes.size(); ++i)
        {
            const auto& n = c.notes[i];
            auto r = noteBounds (n);
            const bool sel = isSelected ((int) i);
            g.setColour (col.withAlpha (0.35f + 0.65f * n.velocity));
            g.fillRoundedRectangle (r, 2.5f);
            if (owner.tool == Tool::Edit && r.getWidth() > 6)
            {
                g.setColour (juce::Colours::white.withAlpha (0.55f));
                g.fillRect (r.getX() + 2, r.getBottom() - 3, (r.getWidth() - 4) * n.velocity, 2.0f);
            }
            g.setColour (sel ? juce::Colours::white : juce::Colours::black.withAlpha (0.5f));
            g.drawRoundedRectangle (r, 2.5f, sel ? 1.6f : 1.0f);
        }

        if (! marquee.isEmpty())
        {
            g.setColour (theme::accent.withAlpha (0.12f));
            g.fillRect (marquee);
            g.setColour (theme::accent.withAlpha (0.8f));
            g.drawRect (marquee, 1.0f);
        }

        if (velocityReadout.isNotEmpty())
        {
            g.setColour (theme::bg.withAlpha (0.85f));
            auto tip = juce::Rectangle<float> (readoutPos.x + 12, readoutPos.y - 26, 74, 20);
            g.fillRoundedRectangle (tip, 4.0f);
            g.setColour (theme::text);
            g.setFont (theme::font (11.5f, true));
            g.drawText (velocityReadout, tip, juce::Justification::centred);
        }

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

    enum class Edge { None, Left, Right };
    Edge edgeAt (int i, juce::Point<float> p) const
    {
        if (i < 0) return Edge::None;
        auto r = noteBounds (owner.clip->notes[(size_t) i]);
        const float grab = juce::jmin (6.0f, r.getWidth() * 0.3f);
        if (p.x > r.getRight() - grab) return Edge::Right;
        if (owner.tool != Tool::Draw && p.x < r.getX() + grab) return Edge::Left;
        return Edge::None;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int i = noteAt (e.position);
        const auto edge = edgeAt (i, e.position);
        if (edge != Edge::None)                         setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        else if (i >= 0 && owner.tool == Tool::Edit)    setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        else if (i >= 0)                                setMouseCursor (juce::MouseCursor::DraggingHandCursor);
        else if (owner.tool == Tool::Draw)              setMouseCursor (juce::MouseCursor::CrosshairCursor);
        else                                            setMouseCursor (juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (owner.clip == nullptr) return;
        grabKeyboardFocus();
        auto& notes = owner.clip->notes;
        const int i = noteAt (e.position);

        if (e.mods.isPopupMenu())
        {
            if (i >= 0 && ! isSelected (i)) selection = { i };
            repaint();
            showMenu (e.position);
            return;
        }

        downBeat = xToBeat (e.position.x);
        downPos = e.position;
        drag = Drag::None;

        if (i >= 0)
        {
            if (e.mods.isShiftDown() || e.mods.isCommandDown())
            {
                // add to / take away from the selection
                if (isSelected (i)) selection.erase (std::find (selection.begin(), selection.end(), i));
                else selection.push_back (i);
                repaint();
                return;
            }
            if (! isSelected (i)) selection = { i };
            const auto edge = edgeAt (i, e.position);
            owner.ctx.project.beginEdit();
            drag = edge == Edge::Right ? Drag::ResizeRight : edge == Edge::Left ? Drag::ResizeLeft
                 : owner.tool == Tool::Edit ? Drag::Velocity : Drag::Move;
            originals.clear();
            for (int s : selection) originals.push_back (notes[(size_t) s]);
            anchor = i;
            audition (notes[(size_t) i].pitch);
        }
        else if (owner.tool == Tool::Draw)
        {
            // write a new note; dragging sets its length
            MidiNote n;
            n.pitch = yToPitch (e.y);
            n.start = juce::jmax (0.0, std::floor (downBeat / owner.snap) * owner.snap);
            n.length = lastLength;
            n.velocity = lastVelocity;
            if (n.start >= owner.clip->lengthBeats) return;
            owner.ctx.project.beginEdit();
            {
                const juce::ScopedLock sl (owner.ctx.project.lock);
                notes.push_back (n);
            }
            selection = { (int) notes.size() - 1 };
            originals = { n };
            anchor = selection[0];
            drag = Drag::DrawLength;
            audition (n.pitch);
        }
        else
        {
            // empty space: start a selection box (Shift keeps the current selection)
            if (! e.mods.isShiftDown()) selection.clear();
            selectionBefore = selection;
            drag = Drag::Marquee;
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (owner.clip == nullptr || drag == Drag::None) return;
        auto& p = owner.ctx.project;
        auto& notes = owner.clip->notes;

        if (drag == Drag::Marquee)
        {
            marquee = juce::Rectangle<float> (downPos, e.position);
            selection = selectionBefore;
            for (int i = 0; i < (int) notes.size(); ++i)
                if (marquee.intersects (noteBounds (notes[(size_t) i])) && ! isSelected (i)) selection.push_back (i);
            repaint();
            return;
        }

        const double s = e.mods.isShiftDown() ? 0.0 : owner.snap;
        const double delta = xToBeat (e.position.x) - downBeat;
        const juce::ScopedLock sl (p.lock);

        if (drag == Drag::Velocity)
        {
            const float dv = (downPos.y - e.position.y) / 150.0f;
            for (size_t k = 0; k < selection.size(); ++k)
                notes[(size_t) selection[k]].velocity = juce::jlimit (0.02f, 1.0f, originals[k].velocity + dv);
            lastVelocity = notes[(size_t) anchor].velocity;
            velocityReadout = "Vel " + juce::String (juce::roundToInt (lastVelocity * 127));
            readoutPos = e.position;
        }
        else if (drag == Drag::Move)
        {
            const auto& a = originals[(size_t) (std::find (selection.begin(), selection.end(), anchor) - selection.begin())];
            const double moved = owner.ctx.snapBeat (a.start + delta, s) - a.start;
            const int dp = yToPitch (e.y) - a.pitch;
            for (size_t k = 0; k < selection.size(); ++k)
            {
                auto& n = notes[(size_t) selection[k]];
                n.start = juce::jlimit (0.0, owner.clip->lengthBeats - 0.01, originals[k].start + moved);
                const int np = juce::jlimit (0, 127, originals[k].pitch + dp);
                if (selection[k] == anchor && np != n.pitch) audition (np);
                n.pitch = np;
            }
        }
        else if (drag == Drag::ResizeRight || drag == Drag::DrawLength)
        {
            const auto& a = originals[(size_t) (std::find (selection.begin(), selection.end(), anchor) - selection.begin())];
            const double minLen = s > 0 ? s : 0.03;
            const double newEnd = owner.ctx.snapBeat (a.start + a.length + delta, s);
            const double dl = juce::jmax (minLen, newEnd - a.start) - a.length;
            for (size_t k = 0; k < selection.size(); ++k)
                notes[(size_t) selection[k]].length = juce::jmax (minLen, originals[k].length + dl);
            lastLength = notes[(size_t) anchor].length;
        }
        else if (drag == Drag::ResizeLeft)
        {
            const auto& a = originals[(size_t) (std::find (selection.begin(), selection.end(), anchor) - selection.begin())];
            const double moved = owner.ctx.snapBeat (a.start + delta, s) - a.start;
            for (size_t k = 0; k < selection.size(); ++k)
            {
                const auto& o = originals[k];
                const double minLen = s > 0 ? s : 0.03;
                const double ns = juce::jlimit (0.0, o.start + o.length - minLen, o.start + moved);
                notes[(size_t) selection[k]].start = ns;
                notes[(size_t) selection[k]].length = o.start + o.length - ns;
            }
        }
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (drag != Drag::None && drag != Drag::Marquee) owner.ctx.project.changed();
        drag = Drag::None;
        marquee = {};
        velocityReadout.clear();
        stopAudition();
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // Draw tool: double-click a note to remove it
        const int i = noteAt (e.position);
        if (i < 0 || owner.clip == nullptr || owner.tool != Tool::Draw) return;
        selection = { i };
        deleteSelection();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        if (owner.clip == nullptr) return false;
        const auto m = k.getModifiers();
        const int code = k.getKeyCode();
        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey) { deleteSelection(); return true; }
        if (m.isCommandDown() && code == 'A') { selectAll(); return true; }
        if (m.isCommandDown() && code == 'C') { copy(); return true; }
        if (m.isCommandDown() && code == 'X') { copy(); deleteSelection(); return true; }
        if (m.isCommandDown() && code == 'V') { paste (pasteBeat()); return true; }
        if (m.isCommandDown() && code == 'D') { duplicate(); return true; }
        if (code == juce::KeyPress::upKey)    { transpose (m.isShiftDown() ? 12 : 1); return true; }
        if (code == juce::KeyPress::downKey)  { transpose (m.isShiftDown() ? -12 : -1); return true; }
        if (code == juce::KeyPress::leftKey)  { nudge (-owner.snap); return true; }
        if (code == juce::KeyPress::rightKey) { nudge (owner.snap); return true; }
        if (! m.isAnyModifierKeyDown())
        {
            if (code == 'V' || code == '1') { owner.setTool (Tool::Select); return true; }
            if (code == 'E' || code == '2') { owner.setTool (Tool::Edit); return true; }
            if (code == 'D' || code == 'B' || code == '3') { owner.setTool (Tool::Draw); return true; }
        }
        if (k == juce::KeyPress::escapeKey) { clearSelection(); return true; }
        return false;
    }

    // ---- editing operations (each one is a single undo step) ----
    void selectAll()
    {
        selection.clear();
        for (int i = 0; i < (int) owner.clip->notes.size(); ++i) selection.push_back (i);
        repaint();
    }

    void deleteSelection()
    {
        if (selection.empty()) return;
        auto sorted = selection;
        std::sort (sorted.rbegin(), sorted.rend());
        owner.ctx.project.beginEdit();
        {
            const juce::ScopedLock sl (owner.ctx.project.lock);
            for (int i : sorted) if (i < (int) owner.clip->notes.size()) owner.clip->notes.erase (owner.clip->notes.begin() + i);
        }
        selection.clear();
        owner.ctx.project.changed();
    }

    void copy()
    {
        clipboard().clear();
        double first = 1.0e9;
        for (int i : selection) first = juce::jmin (first, owner.clip->notes[(size_t) i].start);
        for (int i : selection) { auto n = owner.clip->notes[(size_t) i]; n.start -= first; clipboard().push_back (n); }
    }

    void paste (double atBeat)
    {
        if (clipboard().empty()) return;
        owner.ctx.project.beginEdit();
        selection.clear();
        {
            const juce::ScopedLock sl (owner.ctx.project.lock);
            for (auto n : clipboard())
            {
                n.start += atBeat;
                if (n.start >= owner.clip->lengthBeats) continue;
                owner.clip->notes.push_back (n);
                selection.push_back ((int) owner.clip->notes.size() - 1);
            }
        }
        owner.ctx.project.changed();
    }

    void duplicate()
    {
        if (selection.empty()) return;
        double first = 1.0e9, last = 0;
        for (int i : selection) { auto& n = owner.clip->notes[(size_t) i]; first = juce::jmin (first, n.start); last = juce::jmax (last, n.start + n.length); }
        copy();
        const double span = std::ceil ((last - first) / owner.snap - 1.0e-6) * owner.snap;
        paste (first + juce::jmax (owner.snap, span));
    }

    template <typename Fn> void changeSelected (Fn&& fn)
    {
        auto targets = selection;
        if (targets.empty()) for (int i = 0; i < (int) owner.clip->notes.size(); ++i) targets.push_back (i);
        if (targets.empty()) return;
        owner.ctx.project.beginEdit();
        {
            const juce::ScopedLock sl (owner.ctx.project.lock);
            for (int i : targets) fn (owner.clip->notes[(size_t) i]);
        }
        owner.ctx.project.changed();
    }

    void transpose (int semis) { changeSelected ([semis] (MidiNote& n) { n.pitch = juce::jlimit (0, 127, n.pitch + semis); }); }
    void nudge (double beats)
    {
        const double len = owner.clip->lengthBeats;
        changeSelected ([beats, len] (MidiNote& n) { n.start = juce::jlimit (0.0, len - 0.01, n.start + beats); });
    }
    void quantize (double grid, bool lengthsToo)
    {
        changeSelected ([grid, lengthsToo] (MidiNote& n)
        {
            n.start = std::round (n.start / grid) * grid;
            if (lengthsToo) n.length = juce::jmax (grid, std::round (n.length / grid) * grid);
        });
    }

    double pasteBeat() const
    {
        // at the playhead if it's inside this clip, otherwise right after the selection
        const double rel = owner.ctx.engine.getPositionBeats() - owner.clip->startBeat;
        if (rel >= 0 && rel < owner.clip->lengthBeats) return std::floor (rel / owner.snap) * owner.snap;
        return 0.0;
    }

    void showMenu (juce::Point<float> at)
    {
        juce::PopupMenu m;
        const bool hasSel = ! selection.empty();
        const bool hasNotes = ! owner.clip->notes.empty();
        const double clickBeat = juce::jmax (0.0, std::floor (xToBeat (at.x) / owner.snap) * owner.snap);
        juce::Component::SafePointer<Grid> sp (this);
        auto act = [sp] (std::function<void (Grid&)> f) { return [sp, f] { if (sp != nullptr) f (*sp); }; };

        m.addItem ("Cut", hasSel, false, act ([] (Grid& g) { g.copy(); g.deleteSelection(); }));
        m.addItem ("Copy", hasSel, false, act ([] (Grid& g) { g.copy(); }));
        m.addItem ("Paste here", ! clipboard().empty(), false, act ([clickBeat] (Grid& g) { g.paste (clickBeat); }));
        m.addItem ("Duplicate", hasSel, false, act ([] (Grid& g) { g.duplicate(); }));
        m.addItem ("Delete", hasSel, false, act ([] (Grid& g) { g.deleteSelection(); }));
        m.addSeparator();
        m.addItem ("Select all", hasNotes, false, act ([] (Grid& g) { g.selectAll(); }));
        m.addItem ("Select none", hasSel, false, act ([] (Grid& g) { g.clearSelection(); }));
        m.addSeparator();

        const juce::String scope = hasSel ? " selected" : " all";
        juce::PopupMenu q;
        for (auto [name, grid] : { std::pair { "1/4", 1.0 }, { "1/8", 0.5 }, { "1/16", 0.25 }, { "1/32", 0.125 } })
            q.addItem (juce::String ("Starts to ") + name, hasNotes, false, act ([grid] (Grid& g) { g.quantize (grid, false); }));
        q.addSeparator();
        q.addItem ("Starts and lengths to the grid", hasNotes, false, act ([] (Grid& g) { g.quantize (g.owner.snap, true); }));
        m.addSubMenu ("Quantize" + scope, q, hasNotes);

        juce::PopupMenu t;
        t.addItem ("Up a semitone", hasNotes, false, act ([] (Grid& g) { g.transpose (1); }));
        t.addItem ("Down a semitone", hasNotes, false, act ([] (Grid& g) { g.transpose (-1); }));
        t.addItem ("Up an octave", hasNotes, false, act ([] (Grid& g) { g.transpose (12); }));
        t.addItem ("Down an octave", hasNotes, false, act ([] (Grid& g) { g.transpose (-12); }));
        m.addSubMenu ("Transpose" + scope, t, hasNotes);

        juce::PopupMenu v;
        for (int pct : { 25, 50, 75, 100 })
            v.addItem ("Set to " + juce::String (pct) + "%", hasNotes, false, act ([pct] (Grid& g) { g.changeSelected ([pct] (MidiNote& n) { n.velocity = (float) pct / 100.0f; }); }));
        v.addSeparator();
        v.addItem ("Softer (-10%)", hasNotes, false, act ([] (Grid& g) { g.changeSelected ([] (MidiNote& n) { n.velocity = juce::jlimit (0.02f, 1.0f, n.velocity - 0.1f); }); }));
        v.addItem ("Louder (+10%)", hasNotes, false, act ([] (Grid& g) { g.changeSelected ([] (MidiNote& n) { n.velocity = juce::jlimit (0.02f, 1.0f, n.velocity + 0.1f); }); }));
        v.addItem ("Humanize", hasNotes, false, act ([] (Grid& g)
        {
            juce::Random rnd;
            g.changeSelected ([&rnd] (MidiNote& n) { n.velocity = juce::jlimit (0.05f, 1.0f, n.velocity + (rnd.nextFloat() - 0.5f) * 0.2f); });
        }));
        m.addSubMenu ("Velocity" + scope, v, hasNotes);

        juce::PopupMenu l;
        l.addItem ("Double", hasNotes, false, act ([] (Grid& g) { g.changeSelected ([] (MidiNote& n) { n.length *= 2.0; }); }));
        l.addItem ("Halve", hasNotes, false, act ([] (Grid& g) { g.changeSelected ([] (MidiNote& n) { n.length = juce::jmax (0.03, n.length * 0.5); }); }));
        l.addItem ("Set to one grid step", hasNotes, false, act ([] (Grid& g) { const double s = g.owner.snap; g.changeSelected ([s] (MidiNote& n) { n.length = s; }); }));
        l.addItem ("Legato (each note up to the next)", hasNotes, false, act ([] (Grid& g) { g.legato(); }));
        m.addSubMenu ("Length" + scope, l, hasNotes);

        m.addSeparator();
        juce::PopupMenu tools;
        tools.addItem ("Select   (V)", true, owner.tool == Tool::Select, act ([] (Grid& g) { g.owner.setTool (Tool::Select); }));
        tools.addItem ("Edit   (E)", true, owner.tool == Tool::Edit, act ([] (Grid& g) { g.owner.setTool (Tool::Edit); }));
        tools.addItem ("Draw   (D)", true, owner.tool == Tool::Draw, act ([] (Grid& g) { g.owner.setTool (Tool::Draw); }));
        m.addSubMenu ("Tool", tools);
        juce::PopupMenu grid;
        int id = 1;
        for (auto [name, val] : { std::pair { "1/4", 1.0 }, { "1/8", 0.5 }, { "1/16", 0.25 }, { "1/32", 0.125 } })
        {
            const int boxId = id++;
            grid.addItem (name, true, juce::approximatelyEqual (owner.snap, val), act ([boxId] (Grid& g) { g.owner.snapBox.setSelectedId (boxId); }));
        }
        m.addSubMenu ("Grid", grid);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (juce::Rectangle<int> (1, 1).withPosition (localPointToGlobal (at.toInt()))));
    }

    void legato()
    {
        auto& notes = owner.clip->notes;
        std::vector<int> order = selection;
        if (order.empty()) for (int i = 0; i < (int) notes.size(); ++i) order.push_back (i);
        std::sort (order.begin(), order.end(), [&notes] (int a, int b) { return notes[(size_t) a].start < notes[(size_t) b].start; });
        owner.ctx.project.beginEdit();
        {
            const juce::ScopedLock sl (owner.ctx.project.lock);
            for (size_t k = 0; k < order.size(); ++k)
            {
                auto& n = notes[(size_t) order[k]];
                double next = owner.clip->lengthBeats;
                for (size_t j = k + 1; j < order.size(); ++j)
                    if (notes[(size_t) order[j]].start > n.start + 1.0e-6) { next = notes[(size_t) order[j]].start; break; }
                n.length = juce::jmax (0.03, next - n.start);
            }
        }
        owner.ctx.project.changed();
    }

private:
    enum class Drag { None, Move, ResizeRight, ResizeLeft, Velocity, DrawLength, Marquee };

    static std::vector<MidiNote>& clipboard() { static std::vector<MidiNote> c; return c; }

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
    std::vector<MidiNote> originals;
    std::vector<int> selectionBefore;
    int anchor = -1;
    juce::Point<float> downPos, readoutPos;
    juce::Rectangle<float> marquee;
    juce::String velocityReadout;
    double downBeat = 0, lastLength = 1.0;
    float lastVelocity = 0.8f;
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

    // a ruler that moves the playhead, and a button that plays just this clip
    addAndMakeVisible (ruler);
    ruler.xToBeat = [this] (float x) { return (clip != nullptr ? clip->startBeat : 0.0) + (x + scrollX) / pixelsPerBeat; };
    ruler.beatToX = [this] (double b) { return (float) ((b - (clip != nullptr ? clip->startBeat : 0.0)) * pixelsPerBeat - scrollX); };
    ruler.clipSpan = [this] { return clip != nullptr ? std::pair<double, double> { clip->startBeat, clip->startBeat + clip->lengthBeats } : std::pair<double, double> { 0, 0 }; };
    addAndMakeVisible (playClip);
    setUpPlayClipButton (playClip);
    playClip.onClick = [this] { if (clip != nullptr) ctx.engine.playClip (clip->id); };

    addAndMakeVisible (preset);
    const auto& all = getSynthPatches();
    for (int i = 0; i < (int) all.size(); ++i)
        preset.addItem (all[(size_t) i].name, i + 1);
    preset.addSeparator();
    preset.addItem ("Other instruments...", 1000);
    preset.onChange = [this]
    {
        if (track == nullptr) return;
        const int id = preset.getSelectedId();
        if (id == 1000) { refreshHeader(); devicemenus::showInstrumentMenu (ctx, track->id, &preset); return; }
        if (id <= 0) return;
        devicemenus::chooseSynthSound (ctx, track->id, id - 1);
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
    quantize.setTooltip ("Snap the selected notes' starts to the grid (all notes if none are selected)");
    quantize.onClick = [this] { if (clip != nullptr) grid->quantize (snap, false); };

    for (auto* b : { &selectTool, &editTool, &drawTool })
    {
        addAndMakeVisible (*b);
        b->onClick = [this, b] { setTool (b->which); };
    }
    selectTool.setTooltip ("Select (V): click or drag a box to select notes, then move or resize them");
    editTool.setTooltip ("Edit (E): drag notes up / down to change velocity, drag an edge to change length");
    drawTool.setTooltip ("Draw (D): click to write notes, drag to set their length");
    setTool (Tool::Select);

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
    if (clip == nullptr) grid->selection.clear();
    else
        grid->selection.erase (std::remove_if (grid->selection.begin(), grid->selection.end(),
                                               [this] (int i) { return i >= (int) clip->notes.size(); }), grid->selection.end());

    if (resetView)
    {
        grid->selection.clear();
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
    preset.setEnabled (track != nullptr && track->type == Track::Type::Instrument);
    if (track != nullptr && track->type == Track::Type::Instrument)
    {
        const auto name = AppContext::instrumentName (*track);
        int found = 0;
        for (int i = 0; i < (int) getSynthPatches().size(); ++i)
            if (track->instrument != nullptr && track->instrument->type == "builtin:synth" && getSynthPatches()[(size_t) i].name == name) found = i + 1;
        if (found > 0) preset.setSelectedId (found, juce::dontSendNotification);
        else preset.setText (name, juce::dontSendNotification);
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
    playClip.setBounds (bar.removeFromLeft (36));
    bar.removeFromLeft (8);
    selectTool.setBounds (bar.removeFromLeft (78));
    editTool.setBounds (bar.removeFromLeft (64));
    drawTool.setBounds (bar.removeFromLeft (70));
    bar.removeFromLeft (14);
    clearNotes.setBounds (bar.removeFromRight (64)); bar.removeFromRight (6);
    quantize.setBounds (bar.removeFromRight (80));   bar.removeFromRight (6);
    snapBox.setBounds (bar.removeFromRight (100));   bar.removeFromRight (6);
    preset.setBounds (bar.removeFromRight (140));    bar.removeFromRight (6);
    title.setBounds (bar);

    hScroll.setBounds (r.removeFromBottom (12).withTrimmedLeft (keysWidth).withTrimmedRight (12));
    ruler.setBounds (r.removeFromTop (rulerHeight).withTrimmedLeft (keysWidth).withTrimmedRight (12));
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
    ruler.repaint();
    repaint();
}

void PianoRoll::setTool (Tool t)
{
    tool = t;
    for (auto* b : { &selectTool, &editTool, &drawTool }) b->setToggleState (b->which == t, juce::dontSendNotification);
    if (grid != nullptr) grid->repaint();
}

void PianoRoll::ToolButton::paintButton (juce::Graphics& g, bool over, bool down)
{
    auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = getToggleState();
    if (on || over || down)
    {
        g.setColour (on ? theme::accent.withAlpha (0.2f) : theme::text.withAlpha (down ? 0.1f : 0.055f));
        g.fillRoundedRectangle (r, 5.0f);
    }
    const auto c = on ? theme::accent.brighter (0.2f) : over ? theme::text : theme::textDim;
    g.setColour (c);
    auto icon = r.removeFromLeft (r.getHeight()).reduced (6.0f);
    juce::Path p;
    switch (which)
    {
        case Tool::Select:   // arrow pointer
            p.startNewSubPath (icon.getX() + 2, icon.getY());
            p.lineTo (icon.getX() + 2, icon.getBottom() - 2);
            p.lineTo (icon.getX() + 5.5f, icon.getBottom() - 5.5f);
            p.lineTo (icon.getX() + 8, icon.getBottom());
            p.lineTo (icon.getX() + 10, icon.getBottom() - 1);
            p.lineTo (icon.getX() + 7.5f, icon.getBottom() - 6.5f);
            p.lineTo (icon.getRight() - 1, icon.getBottom() - 6.5f);
            p.closeSubPath();
            g.fillPath (p);
            break;
        case Tool::Edit:     // velocity bars
            for (int k = 0; k < 4; ++k)
            {
                const float h = icon.getHeight() * (0.35f + 0.2f * (float) ((k * 5) % 4));
                g.fillRoundedRectangle (icon.getX() + k * icon.getWidth() / 4.0f + 0.5f, icon.getBottom() - h, icon.getWidth() / 4.0f - 1.5f, h, 1.0f);
            }
            break;
        case Tool::Draw:     // pencil
        {
            juce::Path pen;
            pen.addRectangle (-1.6f, -6.0f, 3.2f, 9.0f);
            pen.addTriangle (-1.6f, 3.0f, 1.6f, 3.0f, 0.0f, 6.5f);
            pen.applyTransform (juce::AffineTransform::rotation (juce::MathConstants<float>::pi * 0.25f).translated (icon.getCentre()));
            g.fillPath (pen);
            break;
        }
    }
    g.setFont (theme::font (12.5f, true));
    g.drawText (getName(), r, juce::Justification::centredLeft);
}

void PianoRoll::zoomAround (double factor, float xInGrid)
{
    const double beatAt = (xInGrid + scrollX) / pixelsPerBeat;
    pixelsPerBeat = juce::jlimit (8.0, 600.0, pixelsPerBeat * factor);
    scrollX = juce::jmax (0.0, beatAt * pixelsPerBeat - xInGrid);
    updateScrollBars();
    repaint();
}

void PianoRoll::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    zoomAround (scaleFactor, (float) e.getEventRelativeTo (grid.get()).x);
}

void PianoRoll::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : w.deltaX;
    if ((e.mods.isAltDown() || e.mods.isCommandDown() || e.mods.isCtrlDown()) && e.mods.isShiftDown())
    {
        // vertical zoom: taller / shorter note rows, keeping the pitch under the mouse in place
        const int my = e.getEventRelativeTo (grid.get()).y;
        const double pitchAt = (my + scrollY) / (double) noteHeight;
        noteHeight = juce::jlimit (7, 30, noteHeight + (d > 0 ? 1 : -1));
        scrollY = juce::jlimit (0, juce::jmax (0, 128 * noteHeight - grid->getHeight()), (int) (pitchAt * noteHeight) - my);
    }
    else if (e.mods.isAltDown() || e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoomAround (std::exp (juce::jlimit (-0.5f, 0.5f, d * (w.isSmooth ? 2.0f : 1.2f))), (float) e.getEventRelativeTo (grid.get()).x);
        return;
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
    playClip.setEnabled (clip != nullptr);
    const bool on = clip != nullptr && ctx.engine.getPlayingClipId() == clip->id;
    playClip.setToggleState (on, juce::dontSendNotification);
    playClip.setIcon (on ? IconButton::Icon::Stop : IconButton::Icon::Play);
}

} // namespace rs
