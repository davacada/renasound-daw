#include "PitchEditor.h"

namespace rs
{

static const char* noteNames[] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

// =============================================================================
//  Piano keys on the left, with the chosen scale's notes highlighted
// =============================================================================
class PitchEditor::Keys : public juce::Component
{
public:
    explicit Keys (PitchEditor& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        const int key = owner.clip != nullptr ? owner.clip->pitch.key : 0;
        const int scale = owner.clip != nullptr ? owner.clip->pitch.scale : 2;
        g.setFont (theme::font (10.0f, true));
        for (int m = 0; m < 128; ++m)
        {
            const float y = owner.midiToY ((float) m) - owner.noteH * 0.5f;
            if (y + owner.noteH < 0 || y > getHeight()) continue;
            const int pc = m % 12;
            const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
            g.setColour (black ? juce::Colour (0xff22242c) : juce::Colour (0xffd9dbe3));
            g.fillRect (0.0f, y, (float) (black ? getWidth() - 16 : getWidth()), (float) owner.noteH - 1);
            if (scale != 2 && pitchdsp::inScale (m, key, scale))
            {
                g.setColour (theme::accent);
                g.fillRect ((float) getWidth() - 4, y, 4.0f, (float) owner.noteH - 1);
            }
            if (pc == 0 || owner.noteH >= 14)
            {
                g.setColour (black ? juce::Colours::white.withAlpha (0.6f) : juce::Colours::black.withAlpha (0.65f));
                g.drawText (juce::String (noteNames[pc]) + (pc == 0 ? juce::String (m / 12 - 1) : juce::String()),
                            0, (int) y, getWidth() - 8, owner.noteH, juce::Justification::centredRight);
            }
        }
    }

private:
    PitchEditor& owner;
};

// =============================================================================
//  The blobs
// =============================================================================
class PitchEditor::NoteView : public juce::Component
{
public:
    explicit NoteView (PitchEditor& o) : owner (o) { setWantsKeyboardFocus (true); }

    juce::Rectangle<float> blobBounds (const PitchNote& n) const
    {
        const double sr = owner.analysis != nullptr ? owner.analysis->sampleRate : owner.clip->audio->sampleRate;
        const float x0 = owner.secToX ((double) n.start / sr), x1 = owner.secToX ((double) n.end / sr);
        const float h = (float) owner.noteH * (0.55f + 0.9f * n.level);
        const float cy = owner.midiToY (n.detected + n.shift);
        return { x0, cy - h * 0.5f, juce::jmax (3.0f, x1 - x0), h };
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::lane);
        auto* clip = owner.clip.get();
        if (clip == nullptr)
        {
            message (g, "Double-click an audio clip (a vocal or a solo instrument) to edit its notes here.");
            return;
        }

        // pitch rows, scale rows a little lighter
        for (int m = 0; m < 128; ++m)
        {
            const float y = owner.midiToY ((float) m) - owner.noteH * 0.5f;
            if (y + owner.noteH < 0 || y > getHeight()) continue;
            const bool inKey = clip->pitch.scale != 2 && pitchdsp::inScale (m, clip->pitch.key, clip->pitch.scale);
            g.setColour (inKey ? theme::laneAlt.brighter (0.03f) : (m % 2 ? theme::lane : theme::lane.darker (0.08f)));
            g.fillRect (0.0f, y, (float) getWidth(), (float) owner.noteH);
            if (m % 12 == 0) { g.setColour (theme::gridBar); g.drawHorizontalLine ((int) (y + owner.noteH), 0, (float) getWidth()); }
        }

        // bar / beat lines, following the song's tempo
        const double bpm = owner.ctx.project.bpm;
        const double secPerBeat = 60.0 / bpm;
        const double clipStartSec = clip->startBeat * secPerBeat;   // song time where the clip begins
        for (double songSec = clipStartSec - std::fmod (clipStartSec, secPerBeat); ; songSec += secPerBeat)
        {
            const double srcSec = clip->offsetSeconds + (songSec - clipStartSec);
            const float x = owner.secToX (srcSec);
            if (x > getWidth()) break;
            if (x < 0) continue;
            const bool bar = std::abs (std::fmod (songSec / secPerBeat, (double) owner.ctx.project.beatsPerBar)) < 1.0e-6;
            g.setColour (bar ? theme::gridBar : theme::grid.withAlpha (0.6f));
            g.drawVerticalLine ((int) x, 0, (float) getHeight());
        }

        if (owner.analysis == nullptr || ! clip->pitch.active)
        {
            message (g, owner.service.isAnalysing (clip != nullptr ? owner.clip : nullptr) || owner.analysis == nullptr
                            ? "Finding the notes..." : "No notes found. Pitch editing works on a single voice or instrument.");
            return;
        }
        if (clip->pitch.notes.empty())
        {
            message (g, "No notes found. Pitch editing works on a single voice or instrument.");
            return;
        }

        const auto& an = *owner.analysis;
        const auto col = owner.track != nullptr ? owner.track->colour : theme::accent;

        for (int i = 0; i < (int) clip->pitch.notes.size(); ++i)
        {
            const auto& n = clip->pitch.notes[(size_t) i];
            auto r = blobBounds (n);
            if (r.getRight() < 0 || r.getX() > getWidth()) continue;
            const bool sel = owner.selected.count (i) > 0;
            const bool moved = std::abs (n.shift) > 0.01f;

            // where the note was sung, as a ghost
            if (moved)
            {
                auto ghost = r.withY (owner.midiToY (n.detected) - r.getHeight() * 0.5f);
                g.setColour (col.withAlpha (0.18f));
                g.drawRoundedRectangle (ghost, r.getHeight() * 0.45f, 1.0f);
            }

            g.setColour (sel ? col.brighter (0.35f) : col.withMultipliedSaturation (0.85f));
            g.fillRoundedRectangle (r, r.getHeight() * 0.45f);
            if (sel) { g.setColour (juce::Colours::white); g.drawRoundedRectangle (r, r.getHeight() * 0.45f, 1.5f); }

            // the pitch line inside the blob: what you'll hear after the edits
            juce::Path curve;
            bool started = false;
            const int f0 = an.frameAt (n.start), f1 = an.frameAt (n.end);
            for (int f = f0; f <= f1; ++f)
            {
                if (! an.voiced (f)) { started = false; continue; }
                const float x = owner.secToX ((double) f * an.hop / an.sampleRate);
                const float y = owner.midiToY (pitchdsp::targetPitch (an, n, f));
                if (! started) { curve.startNewSubPath (x, y); started = true; }
                else curve.lineTo (x, y);
            }
            g.setColour (juce::Colours::white.withAlpha (0.85f));
            g.strokePath (curve, juce::PathStrokeType (1.3f));

            // cents off the nearest note, for selected notes
            if (sel && r.getWidth() > 34)
            {
                const float p = n.detected + n.shift;
                const int cents = juce::roundToInt ((p - std::round (p)) * 100.0f);
                const auto name = juce::String (noteNames[((int) std::round (p) % 12 + 12) % 12]) + juce::String ((int) std::round (p) / 12 - 1);
                g.setColour (theme::text);
                g.setFont (theme::font (11.0f, true));
                g.drawText (name + (cents != 0 ? (cents > 0 ? " +" : " ") + juce::String (cents) : juce::String()),
                            r.translated (0, -15).withHeight (14).toNearestInt(), juce::Justification::centredLeft, false);
            }
        }

        // rubber-band selection
        if (! lasso.isEmpty())
        {
            g.setColour (theme::accent.withAlpha (0.12f));
            g.fillRect (lasso);
            g.setColour (theme::accent);
            g.drawRect (lasso, 1);
        }

        // playhead
        const double songSec = owner.ctx.engine.getPositionBeats() * secPerBeat;
        const float px = owner.secToX (clip->offsetSeconds + songSec - clipStartSec);
        if (px >= 0 && px <= getWidth())
        {
            g.setColour (theme::playhead.withAlpha (0.9f));
            g.fillRect (px - 0.5f, 0.0f, 1.5f, (float) getHeight());
        }
    }

    int noteAt (juce::Point<float> p) const
    {
        if (owner.clip == nullptr) return -1;
        const auto& notes = owner.clip->pitch.notes;
        for (int i = (int) notes.size(); --i >= 0;)
            if (blobBounds (notes[(size_t) i]).expanded (0, 3).contains (p)) return i;
        return -1;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (noteAt (e.position) >= 0 ? (e.mods.isCommandDown() ? juce::MouseCursor::IBeamCursor : juce::MouseCursor::UpDownResizeCursor)
                                                 : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        dragging = false;
        lasso = {};
        if (owner.clip == nullptr || ! owner.clip->pitch.active) return;
        auto& notes = owner.clip->pitch.notes;
        const int i = noteAt (e.position);

        if (i >= 0 && e.mods.isCommandDown())
        {
            splitNote (i, owner.xToSec (e.position.x));
            return;
        }
        if (i >= 0)
        {
            if (e.mods.isShiftDown())
            {
                if (owner.selected.count (i)) owner.selected.erase (i); else owner.selected.insert (i);
            }
            else if (! owner.selected.count (i))
            {
                owner.selected = { i };
            }
            pressedNote = i;
            startShifts.clear();
            for (int s : owner.selected) startShifts[s] = notes[(size_t) s].shift;
            downMidi = owner.yToMidi (e.position.y);
        }
        else
        {
            pressedNote = -1;
            if (! e.mods.isShiftDown()) owner.selected.clear();
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (owner.clip == nullptr || ! owner.clip->pitch.active) return;
        auto& notes = owner.clip->pitch.notes;

        if (pressedNote < 0)
        {
            lasso = juce::Rectangle<int> (e.getMouseDownPosition(), e.getPosition());
            for (int i = 0; i < (int) notes.size(); ++i)
                if (blobBounds (notes[(size_t) i]).intersects (lasso.toFloat())) owner.selected.insert (i);
            repaint();
            return;
        }
        if (! dragging)
        {
            if (std::abs (e.getDistanceFromDragStartY()) < 3) return;
            dragging = true;
            owner.ctx.project.beginEdit();
        }

        // move by whole semitones (the pressed note lands exactly on a note); Alt moves freely
        const float delta = owner.yToMidi (e.position.y) - downMidi;
        auto& pressed = notes[(size_t) pressedNote];
        float applied = delta;
        if (! e.mods.isAltDown())
        {
            const float startPitch = pressed.detected + startShifts[pressedNote];
            applied = std::round (startPitch + delta) - startPitch;
        }
        for (auto& [idx, s] : startShifts)
            notes[(size_t) idx].shift = s + applied;
        owner.edited (false);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging) owner.edited (true);
        dragging = false;
        pressedNote = -1;
        lasso = {};
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // double-click a note: put it right on the nearest note of the scale
        if (owner.clip == nullptr) return;
        const int i = noteAt (e.position);
        if (i < 0) return;
        auto& n = owner.clip->pitch.notes[(size_t) i];
        owner.ctx.project.beginEdit();
        n.shift = pitchdsp::snapToScale (n.detected + n.shift, owner.clip->pitch.key, owner.clip->pitch.scale) - n.detected;
        owner.edited (true);
    }

private:
    void message (juce::Graphics& g, const juce::String& text)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (15.0f));
        g.drawFittedText (text, getLocalBounds().reduced (40), juce::Justification::centred, 2);
    }

    void splitNote (int index, double atSec)
    {
        auto& notes = owner.clip->pitch.notes;
        auto n = notes[(size_t) index];
        const auto at = (juce::int64) (atSec * owner.analysis->sampleRate);
        if (at <= n.start + owner.analysis->hop * 2 || at >= n.end - owner.analysis->hop * 2) return;

        owner.ctx.project.beginEdit();
        auto right = n;
        n.end = at;
        right.start = at;
        // each half gets its own sung pitch
        for (auto* part : { &n, &right })
        {
            std::vector<float> v;
            for (int f = owner.analysis->frameAt (part->start); f <= owner.analysis->frameAt (part->end); ++f)
                if (owner.analysis->voiced (f)) v.push_back (owner.analysis->pitch[(size_t) f]);
            if (! v.empty())
            {
                std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
                const float oldTarget = part->detected + part->shift;
                part->detected = v[v.size() / 2];
                part->shift = oldTarget - part->detected;
            }
        }
        notes[(size_t) index] = n;
        notes.insert (notes.begin() + index + 1, right);
        owner.selected = { index + 1 };
        owner.edited (true);
    }

    PitchEditor& owner;
    bool dragging = false;
    int pressedNote = -1;
    float downMidi = 0;
    std::map<int, float> startShifts;
    juce::Rectangle<int> lasso;
};

// =============================================================================
PitchEditor::PitchEditor (AppContext& c, PitchService& s) : ctx (c), service (s)
{
    keys = std::make_unique<Keys> (*this);
    notesView = std::make_unique<NoteView> (*this);

    // a ruler that moves the playhead, and a button that plays just this clip
    addAndMakeVisible (ruler);
    ruler.xToBeat = [this] (float x)
    {
        if (clip == nullptr) return 0.0;
        const double spb = 60.0 / ctx.project.bpm;
        return (clip->startBeat * spb + (xToSec (x) - clip->offsetSeconds)) / spb;
    };
    ruler.beatToX = [this] (double b)
    {
        if (clip == nullptr) return 0.0f;
        const double spb = 60.0 / ctx.project.bpm;
        return secToX (b * spb - clip->startBeat * spb + clip->offsetSeconds);
    };
    ruler.clipSpan = [this] { return clip != nullptr ? std::pair<double, double> { clip->startBeat, clip->getEndBeat (ctx.project.bpm) } : std::pair<double, double> { 0, 0 }; };
    addAndMakeVisible (playClip);
    setUpPlayClipButton (playClip);
    playClip.onClick = [this] { if (clip != nullptr) ctx.engine.playClip (clip->id); };
    addAndMakeVisible (*keys);
    addAndMakeVisible (*notesView);
    addAndMakeVisible (hScroll);
    addAndMakeVisible (vScroll);
    hScroll.addListener (this);
    vScroll.addListener (this);
    hScroll.setAutoHide (false);

    addAndMakeVisible (title);
    title.setFont (theme::font (14.0f, true));
    addAndMakeVisible (status);
    status.setFont (theme::font (12.0f, true));
    status.setColour (juce::Label::textColourId, theme::accent);

    addAndMakeVisible (keyBox);
    for (int k = 0; k < 12; ++k) keyBox.addItem (noteNames[k], k + 1);
    keyBox.setTooltip ("Key of the song");
    addAndMakeVisible (scaleBox);
    scaleBox.addItem ("Major", 1);
    scaleBox.addItem ("Minor", 2);
    scaleBox.addItem ("Chromatic", 3);
    scaleBox.setTooltip ("Scale used by Snap to scale and Correct pitch");
    auto scaleChanged = [this]
    {
        if (clip == nullptr) return;
        clip->pitch.key = keyBox.getSelectedId() - 1;
        clip->pitch.scale = scaleBox.getSelectedId() - 1;
        ctx.project.dirty = true;
        repaint();
    };
    keyBox.onChange = scaleChanged;
    scaleBox.onChange = scaleChanged;

    for (auto* sl : { &correct, &drift })
    {
        addAndMakeVisible (sl);
        sl->setSliderStyle (juce::Slider::LinearHorizontal);
        sl->setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
        sl->setRange (0.0, 100.0, 1.0);
        sl->setTextValueSuffix ("%");
        sl->onDragStart = [this] { ctx.project.beginEdit(); sliderEditOpen = true; };
        sl->onDragEnd   = [this] { sliderEditOpen = false; edited (true); };
    }
    correct.onValueChange = [this] { if (! sliderEditOpen) ctx.project.beginEdit(); applyCorrection ((float) correct.getValue() / 100.0f); if (! sliderEditOpen) edited (true); };
    drift.onValueChange   = [this] { if (! sliderEditOpen) ctx.project.beginEdit(); applyDrift ((float) drift.getValue() / 100.0f); if (! sliderEditOpen) edited (true); };
    correct.setTooltip ("Pull notes toward the nearest note of the scale (selected notes, or all)");
    drift.setTooltip ("Flatten the wobble within each note (selected notes, or all)");

    for (auto* l : { &correctLabel, &driftLabel })
    {
        addAndMakeVisible (l);
        l->setFont (theme::font (11.0f, true));
        l->setColour (juce::Label::textColourId, theme::textDim);
        l->setJustificationType (juce::Justification::centredRight);
    }
    correctLabel.setText ("PITCH", juce::dontSendNotification);
    driftLabel.setText ("DRIFT", juce::dontSendNotification);

    addAndMakeVisible (snapButton);
    snapButton.setColour (juce::TextButton::buttonColourId, theme::accent);
    snapButton.setTooltip ("Put the notes exactly on the scale (selected notes, or all)");
    snapButton.onClick = [this]
    {
        if (clip == nullptr || ! clip->pitch.active) return;
        ctx.project.beginEdit();
        applyCorrection (1.0f);
        correct.setValue (100.0, juce::dontSendNotification);
        edited (true);
    };

    addAndMakeVisible (resetButton);
    resetButton.setTooltip ("Undo all pitch edits on the selected notes (or all)");
    resetButton.onClick = [this]
    {
        if (clip == nullptr) return;
        ctx.project.beginEdit();
        for (int i : targetNotes()) { clip->pitch.notes[(size_t) i].shift = 0; clip->pitch.notes[(size_t) i].drift = 0; }
        correct.setValue (0, juce::dontSendNotification);
        drift.setValue (0, juce::dontSendNotification);
        edited (true);
    };

    addAndMakeVisible (abButton);
    abButton.setClickingTogglesState (true);
    abButton.setTooltip ("Hear the original (on) or your edits (off)");
    abButton.onClick = [this]
    {
        if (clip == nullptr) return;
        const juce::ScopedLock sl (ctx.project.lock);
        clip->pitch.bypass = abButton.getToggleState();
        ctx.project.dirty = true;
    };

    scrollY = (int) ((127.5f - 66.0f) * (float) noteH);
    service.addChangeListener (this);
    ctx.project.addChangeListener (this);
    refreshControls();
    startTimerHz (20);
}

PitchEditor::~PitchEditor()
{
    service.removeChangeListener (this);
    ctx.project.removeChangeListener (this);
}

void PitchEditor::showClip (int clipId)
{
    TrackPtr owner;
    auto c = ctx.project.findClip (clipId, &owner);
    if (c != nullptr && c->type != Clip::Type::Audio) c = nullptr;
    const bool same = clip != nullptr && c != nullptr && c->id == clip->id;
    clip = c;
    track = c != nullptr ? owner : nullptr;
    if (! same) selected.clear();
    analysis = clip != nullptr ? service.getAnalysis (clip) : nullptr;
    initialiseNotesIfReady();

    if (! same && clip != nullptr)
    {
        // fit the clip's length and centre its notes
        scrollX = 0;
        pxPerSec = juce::jlimit (20.0, 800.0, (notesView->getWidth() > 50 ? notesView->getWidth() - 20 : 900) / juce::jmax (0.5, clip->lengthSeconds));
        if (! clip->pitch.notes.empty())
        {
            float lo = 127, hi = 0;
            for (auto& n : clip->pitch.notes) { lo = juce::jmin (lo, n.detected + n.shift); hi = juce::jmax (hi, n.detected + n.shift); }
            scrollY = juce::jmax (0, (int) (midiToY ((lo + hi) * 0.5f) + (float) scrollY) - notesView->getHeight() / 2);
        }
    }
    refreshControls();
    updateScrollBars();
    repaint();
}

void PitchEditor::initialiseNotesIfReady()
{
    if (clip == nullptr || analysis == nullptr || clip->pitch.active) return;
    // first time: take the notes the analysis found (not an undoable edit, nothing has changed yet)
    const juce::ScopedLock sl (ctx.project.lock);
    clip->pitch.notes = analysis->notes;
    clip->pitch.active = true;
    ctx.project.dirty = true;

    float lo = 127, hi = 0;
    for (auto& n : clip->pitch.notes) { lo = juce::jmin (lo, n.detected); hi = juce::jmax (hi, n.detected); }
    if (hi >= lo) scrollY = juce::jmax (0, (int) ((127.5f - (lo + hi) * 0.5f) * (float) noteH) - juce::jmax (100, notesView->getHeight()) / 2);
}

void PitchEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (clip == nullptr) return;
    if (source == &ctx.project)
    {
        // after undo / redo the clip object is new: look it up again by id
        showClip (clip->id);
        return;
    }
    if (analysis == nullptr)
    {
        analysis = service.getAnalysis (clip);
        if (analysis != nullptr)
        {
            initialiseNotesIfReady();
            refreshControls();
        }
    }
    repaint();
}

void PitchEditor::refreshControls()
{
    const bool ready = clip != nullptr && clip->pitch.active;
    title.setText (clip != nullptr ? (track != nullptr ? track->name + "  /  " : juce::String()) + clip->name + "  -  pitch"
                                   : juce::String ("Pitch editor"), juce::dontSendNotification);
    for (juce::Component* comp : { (juce::Component*) &keyBox, (juce::Component*) &scaleBox, (juce::Component*) &correct, (juce::Component*) &drift,
                                   (juce::Component*) &snapButton, (juce::Component*) &resetButton, (juce::Component*) &abButton })
        comp->setEnabled (ready);
    if (clip != nullptr)
    {
        keyBox.setSelectedId (clip->pitch.key + 1, juce::dontSendNotification);
        scaleBox.setSelectedId (clip->pitch.scale + 1, juce::dontSendNotification);
        abButton.setToggleState (clip->pitch.bypass, juce::dontSendNotification);
    }
}

std::vector<int> PitchEditor::targetNotes() const
{
    std::vector<int> out;
    if (clip == nullptr) return out;
    if (! selected.empty()) out.assign (selected.begin(), selected.end());
    else for (int i = 0; i < (int) clip->pitch.notes.size(); ++i) out.push_back (i);
    return out;
}

void PitchEditor::applyCorrection (float amount)
{
    if (clip == nullptr || ! clip->pitch.active) return;
    for (int i : targetNotes())
    {
        auto& n = clip->pitch.notes[(size_t) i];
        const float target = pitchdsp::snapToScale (n.detected, clip->pitch.key, clip->pitch.scale);
        n.shift = (target - n.detected) * amount;
    }
    edited (false);
}

void PitchEditor::applyDrift (float amount)
{
    if (clip == nullptr || ! clip->pitch.active) return;
    for (int i : targetNotes()) clip->pitch.notes[(size_t) i].drift = amount;
    edited (false);
}

void PitchEditor::edited (bool finished)
{
    ctx.project.dirty = true;
    if (finished)
    {
        service.editsChanged();
        ctx.project.changed();   // timeline badges, undo menu, etc.
    }
    repaint();
}

void PitchEditor::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (toolbarH).reduced (8, 7);
    playClip.setBounds (bar.removeFromLeft (34));
    bar.removeFromLeft (8);
    abButton.setBounds (bar.removeFromRight (52));     bar.removeFromRight (6);
    resetButton.setBounds (bar.removeFromRight (60));  bar.removeFromRight (6);
    snapButton.setBounds (bar.removeFromRight (110));  bar.removeFromRight (10);
    drift.setBounds (bar.removeFromRight (150));
    driftLabel.setBounds (bar.removeFromRight (48));   bar.removeFromRight (6);
    correct.setBounds (bar.removeFromRight (150));
    correctLabel.setBounds (bar.removeFromRight (48)); bar.removeFromRight (8);
    scaleBox.setBounds (bar.removeFromRight (100));    bar.removeFromRight (4);
    keyBox.setBounds (bar.removeFromRight (58));       bar.removeFromRight (8);
    status.setBounds (bar.removeFromRight (110));
    title.setBounds (bar);

    hScroll.setBounds (r.removeFromBottom (12).withTrimmedLeft (keysWidth).withTrimmedRight (12));
    r.removeFromBottom (waveH);
    ruler.setBounds (r.removeFromTop (rulerH).withTrimmedLeft (keysWidth).withTrimmedRight (12));
    vScroll.setBounds (r.removeFromRight (12));
    keys->setBounds (r.removeFromLeft (keysWidth));
    notesView->setBounds (r);
    updateScrollBars();
}

void PitchEditor::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);

    // waveform overview along the bottom
    auto wave = getLocalBounds().withTrimmedBottom (12).removeFromBottom (waveH).withTrimmedLeft (keysWidth).withTrimmedRight (12);
    g.setColour (theme::bg);
    g.fillRect (wave);
    if (clip == nullptr || clip->audio == nullptr) return;
    const auto& peaks = clip->audio->peaks;
    const int numPeaks = (int) peaks.size() / 2;
    const double peaksPerSec = clip->audio->sampleRate / AudioData::peakBlock;
    const float mid = (float) wave.getCentreY(), half = wave.getHeight() * 0.45f;
    g.setColour ((track != nullptr ? track->colour : theme::accent).withAlpha (0.6f));
    for (int x = 0; x < wave.getWidth(); ++x)
    {
        const double s0 = xToSec ((float) x), s1 = xToSec ((float) x + 1);
        if (s0 < clip->offsetSeconds || s0 > clip->offsetSeconds + clip->lengthSeconds) continue;
        int p0 = juce::jlimit (0, numPeaks, (int) (s0 * peaksPerSec)), p1 = juce::jlimit (0, numPeaks, juce::jmax (p0 + 1, (int) (s1 * peaksPerSec)));
        float lo = 0, hi = 0;
        for (int k = p0; k < p1; ++k) { lo = juce::jmin (lo, peaks[(size_t) k * 2]); hi = juce::jmax (hi, peaks[(size_t) k * 2 + 1]); }
        g.drawVerticalLine (wave.getX() + x, mid - hi * half, mid - lo * half + 1);
    }
}

void PitchEditor::updateScrollBars()
{
    const double contentW = (clip != nullptr ? clip->lengthSeconds : 10.0) * pxPerSec + 40;
    hScroll.setRangeLimits (0, juce::jmax (contentW, (double) notesView->getWidth()), juce::dontSendNotification);
    hScroll.setCurrentRange (scrollX, notesView->getWidth(), juce::dontSendNotification);
    vScroll.setRangeLimits (0, 128 * noteH, juce::dontSendNotification);
    vScroll.setCurrentRange (scrollY, notesView->getHeight(), juce::dontSendNotification);
}

void PitchEditor::scrollBarMoved (juce::ScrollBar* b, double start)
{
    if (b == &hScroll) scrollX = start; else scrollY = (int) start;
    repaint();
}

void PitchEditor::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    zoomAround (scaleFactor, (float) e.getEventRelativeTo (notesView.get()).x);
}

void PitchEditor::zoomAround (double factor, float mx)
{
    const double secAt = xToSec (mx);
    pxPerSec = juce::jlimit (10.0, 2000.0, pxPerSec * factor);
    scrollX = juce::jmax (0.0, (secAt - viewStartSec()) * pxPerSec - mx);
    updateScrollBars();
    repaint();
}

void PitchEditor::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : w.deltaX;
    if ((e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isAltDown()) && e.mods.isShiftDown())
    {
        noteH = juce::jlimit (8, 32, noteH + (d > 0 ? 2 : -2));
    }
    else if (e.mods.isCommandDown() || e.mods.isCtrlDown() || e.mods.isAltDown())
    {
        zoomAround (std::exp (juce::jlimit (-0.5f, 0.5f, d * (w.isSmooth ? 2.0f : 1.2f))), (float) e.getEventRelativeTo (notesView.get()).x);
        return;
    }
    else
    {
        float dx = w.deltaX, dy = w.deltaY;
        if (e.mods.isShiftDown()) std::swap (dx, dy);
        scrollX = juce::jmax (0.0, scrollX - dx * 400.0);
        scrollY = juce::jlimit (0, juce::jmax (0, 128 * noteH - notesView->getHeight()), scrollY - (int) (dy * 300.0f));
    }
    updateScrollBars();
    repaint();
}

bool PitchEditor::keyPressed (const juce::KeyPress& k)
{
    if (clip == nullptr || ! clip->pitch.active) return false;
    if (k.getModifiers().isCommandDown() && k.getKeyCode() == 'A')
    {
        selected.clear();
        for (int i = 0; i < (int) clip->pitch.notes.size(); ++i) selected.insert (i);
        repaint();
        return true;
    }
    if ((k.getKeyCode() == juce::KeyPress::upKey || k.getKeyCode() == juce::KeyPress::downKey) && ! selected.empty())
    {
        const float step = (k.getModifiers().isShiftDown() ? 12.0f : 1.0f) * (k.getKeyCode() == juce::KeyPress::upKey ? 1.0f : -1.0f);
        ctx.project.beginEdit();
        for (int i : selected) clip->pitch.notes[(size_t) i].shift += step;
        edited (true);
        return true;
    }
    return false;
}

void PitchEditor::timerCallback()
{
    playClip.setEnabled (clip != nullptr);
    {
        const bool on = clip != nullptr && ctx.engine.getPlayingClipId() == clip->id;
        playClip.setToggleState (on, juce::dontSendNotification);
        playClip.setIcon (on ? IconButton::Icon::Stop : IconButton::Icon::Play);
    }
    // status line + playhead
    juce::String s;
    if (clip != nullptr)
    {
        if (analysis == nullptr) s = "Finding notes...";
        else if (service.isRendering (clip->id)) s = "Processing...";
        else if (clip->pitch.hasChanges() && clip->pitchRenderedHash != clip->pitch.hash()) s = "Processing...";
    }
    if (status.getText() != s) status.setText (s, juce::dontSendNotification);
    if (clip != nullptr && (ctx.engine.isPlaying() || s.isNotEmpty())) notesView->repaint();
}

} // namespace rs
