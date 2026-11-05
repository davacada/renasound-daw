#include "TimelineView.h"
#include "DeviceWindow.h"
#include "InsertList.h"

namespace rs
{

// =============================================================================
//  Ruler: bar numbers, click to move the playhead, shift-drag to set the loop
// =============================================================================
class TimelineView::Ruler : public juce::Component
{
public:
    explicit Ruler (TimelineView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        auto& p = owner.ctx.project;
        g.fillAll (theme::panel);

        // loop region
        const float lx0 = owner.beatToX (p.loopStart), lx1 = owner.beatToX (p.loopEnd);
        g.setColour (p.loopEnabled ? theme::accent2.withAlpha (0.35f) : theme::textDim.withAlpha (0.15f));
        g.fillRect (juce::Rectangle<float> (lx0, 0, lx1 - lx0, 9.0f));

        const int bpb = p.beatsPerBar;
        const double firstBeat = std::floor (owner.xToBeat (0));
        const double lastBeat = owner.xToBeat ((float) getWidth()) + 1;
        const int barStep = owner.pixelsPerBeat * bpb < 40 ? (owner.pixelsPerBeat * bpb < 18 ? 8 : 4)
                                                          : (owner.pixelsPerBeat * bpb < 70 ? 2 : 1);
        g.setFont (theme::font (11.5f, true));

        for (double b = juce::jmax (0.0, firstBeat); b <= lastBeat; b += 1.0)
        {
            const float x = owner.beatToX (b);
            const int beatIndex = (int) b;
            if (beatIndex % bpb == 0)
            {
                const int bar = beatIndex / bpb;
                if (bar % barStep != 0) continue;
                g.setColour (theme::gridBar);
                g.drawVerticalLine ((int) x, 12.0f, (float) getHeight());
                g.setColour (theme::textDim);
                g.drawText (juce::String (bar + 1), (int) x + 4, 11, 40, 16, juce::Justification::centredLeft);
            }
            else if (owner.pixelsPerBeat > 14)
            {
                g.setColour (theme::grid);
                g.drawVerticalLine ((int) x, (float) getHeight() - 7, (float) getHeight());
            }
        }

        // playhead marker
        const float px = owner.beatToX (owner.ctx.engine.getPositionBeats());
        juce::Path tri;
        tri.addTriangle (px - 6, (float) getHeight() - 10, px + 6, (float) getHeight() - 10, px, (float) getHeight());
        g.setColour (theme::accent);
        g.fillPath (tri);

        g.setColour (theme::bg);
        g.drawHorizontalLine (getHeight() - 1, 0, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragStartBeat = owner.ctx.snapBeat (owner.xToBeat ((float) e.x), owner.getSnap());
        if (e.mods.isShiftDown() || e.y < 10)
        {
            settingLoop = true;
            owner.ctx.project.beginEdit();
        }
        else
        {
            settingLoop = false;
            owner.ctx.engine.setPositionBeats (juce::jmax (0.0, owner.xToBeat ((float) e.x)));
        }
        owner.repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto& p = owner.ctx.project;
        const double b = juce::jmax (0.0, owner.ctx.snapBeat (owner.xToBeat ((float) e.x), owner.getSnap()));
        if (settingLoop)
        {
            const juce::ScopedLock sl (p.lock);
            p.loopStart = juce::jmin (dragStartBeat, b);
            p.loopEnd = juce::jmax (dragStartBeat, b);
            if (p.loopEnd - p.loopStart >= owner.getSnap()) p.loopEnabled = true;
        }
        else
        {
            owner.ctx.engine.setPositionBeats (juce::jmax (0.0, owner.xToBeat ((float) e.x)));
        }
        owner.repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        auto& p = owner.ctx.project;
        const juce::ScopedLock sl (p.lock);
        p.loopEnabled = ! p.loopEnabled;
        owner.repaint();
    }

private:
    TimelineView& owner;
    double dragStartBeat = 0;
    bool settingLoop = false;
};

// =============================================================================
//  Automation helpers
// =============================================================================
struct AutomationTarget { juce::String id, label; };

static std::vector<AutomationTarget> automationTargets (const Track& t)
{
    std::vector<AutomationTarget> out { { "volume", "Volume" }, { "pan", "Pan" } };
    auto addProcessor = [&out] (juce::AudioProcessor* proc, const juce::String& prefix, const juce::String& owner)
    {
        if (proc == nullptr) return;
        const auto& params = proc->getParameters();
        for (int i = 0; i < juce::jmin (128, params.size()); ++i)
            if (params[i]->isAutomatable())
                out.push_back ({ prefix + juce::String (i), owner + ": " + params[i]->getName (40) });
    };
    if (t.instrument != nullptr) addProcessor (t.instrument->processor.get(), "inst:", t.instrument->getName());
    for (auto& f : t.fx) addProcessor (f->processor.get(), "fx:" + juce::String (f->uid) + ":", f->getName());
    return out;
}

// value 0..1 shown in the parameter's own units
static juce::String automationValueText (const Track& t, const AutomationLane& lane, float v)
{
    switch (lane.kind)
    {
        case AutomationLane::Kind::Volume:
        {
            const float g = AutomationLane::valueToVolume (v);
            return g < 0.0001f ? juce::String ("-inf dB") : juce::String (juce::Decibels::gainToDecibels (g), 1) + " dB";
        }
        case AutomationLane::Kind::Pan:
        {
            const int p = juce::roundToInt (std::abs (AutomationLane::valueToPan (v)) * 100.0f);
            return p == 0 ? juce::String ("Centre") : juce::String (p) + (v < 0.5f ? " L" : " R");
        }
        case AutomationLane::Kind::Effect:
        case AutomationLane::Kind::Instrument:
        default:
            if (auto* param = t.getAutomationParameter (lane)) return param->getText (v, 12) + " " + param->getLabel();
            return juce::String (juce::roundToInt (v * 100.0f)) + "%";
    }
}

static void showAddLaneMenu (AppContext& ctx, TrackPtr track, juce::Component* target)
{
    juce::PopupMenu menu, more;
    for (auto& t : automationTargets (*track))
    {
        bool exists = false;
        for (auto& l : track->automation) exists = exists || l.getId() == t.id;
        auto add = [&ctx, track, id = t.id]
        {
            ctx.project.beginEdit();
            {
                const juce::ScopedLock sl (ctx.project.lock);
                auto lane = AutomationLane::fromId (id);
                track->automation.push_back (lane);
            }
            ctx.project.changed();
        };
        if (t.id == "volume" || t.id == "pan") menu.addItem (t.label, ! exists, false, add);
        else more.addItem (t.label, ! exists, false, add);
    }
    if (more.getNumItems() > 0) menu.addSubMenu ("Effects & instrument", more);
    else menu.addItem ("Add effects or a plugin instrument to automate their settings", false, false, [] {});
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

// The header of one automation lane: which parameter, and a remove button.
class TimelineView::LaneHeader : public juce::Component
{
public:
    LaneHeader (TimelineView& o, TrackPtr t, int index) : owner (o), track (std::move (t)), laneIndex (index)
    {
        addAndMakeVisible (target);
        const auto targets = automationTargets (*track);
        int id = 1, selected = 0;
        for (auto& tg : targets)
        {
            target.addItem (tg.label, id);
            if (tg.id == track->automation[(size_t) laneIndex].getId()) selected = id;
            ++id;
        }
        if (selected == 0) target.setText ("(missing parameter)", juce::dontSendNotification);
        else target.setSelectedId (selected, juce::dontSendNotification);
        target.onChange = [this, targets]
        {
            const int i = target.getSelectedId() - 1;
            if (i < 0 || i >= (int) targets.size()) return;
            owner.ctx.project.beginEdit();
            {
                const juce::ScopedLock sl (owner.ctx.project.lock);
                auto& lane = track->automation[(size_t) laneIndex];
                auto fresh = AutomationLane::fromId (targets[(size_t) i].id);
                lane.kind = fresh.kind;
                lane.fxUid = fresh.fxUid;
                lane.paramIndex = fresh.paramIndex;
            }
            owner.ctx.project.changed();
        };

        addAndMakeVisible (remove);
        remove.setTooltip ("Remove this automation lane");
        remove.onClick = [this]
        {
            auto& p = owner.ctx.project;
            auto owningTrack = track;
            const int idx = laneIndex;
            juce::MessageManager::callAsync ([&p, owningTrack, idx]
            {
                p.beginEdit();
                {
                    const juce::ScopedLock sl (p.lock);
                    if (idx < (int) owningTrack->automation.size()) owningTrack->automation.erase (owningTrack->automation.begin() + idx);
                }
                p.changed();
            });
        };
        addAndMakeVisible (clear);
        clear.setTooltip ("Delete every point in this lane");
        clear.onClick = [this]
        {
            owner.ctx.project.beginEdit();
            {
                const juce::ScopedLock sl (owner.ctx.project.lock);
                track->automation[(size_t) laneIndex].points.clear();
            }
            owner.ctx.project.changed();
        };
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg.brighter (0.04f));
        g.setColour (track->colour.withAlpha (0.6f));
        g.fillRect (0, 0, 5, getHeight());
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f, true));
        g.drawText ("AUTOMATION", 14, 4, 120, 12, juce::Justification::centredLeft);
        const auto& lane = track->automation[(size_t) laneIndex];
        const float v = lane.points.empty() ? -1.0f : lane.valueAt (owner.ctx.engine.getPositionBeats());
        if (v >= 0)
        {
            g.setColour (theme::accent);
            g.drawText (automationValueText (*track, lane, v), getWidth() - 140, 4, 128, 12, juce::Justification::centredRight);
        }
        g.setColour (theme::bg);
        g.drawHorizontalLine (getHeight() - 1, 0, (float) getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 6).withTrimmedLeft (2);
        r.removeFromTop (14);
        remove.setBounds (r.removeFromRight (24));
        r.removeFromRight (4);
        clear.setBounds (r.removeFromRight (48));
        r.removeFromRight (6);
        target.setBounds (r);
    }

private:
    TimelineView& owner;
    TrackPtr track;
    const int laneIndex;
    juce::ComboBox target;
    juce::TextButton remove { "x" }, clear { "Clear" };
};

// =============================================================================
//  Track header: name, mute / solo / arm, volume, meter
// =============================================================================
class TimelineView::TrackHeader : public juce::Component
{
public:
    static constexpr int fxColumnWidth = 128;

    TrackHeader (TimelineView& o, TrackPtr t) : owner (o), track (std::move (t)), inserts (o.ctx, track->id, 17)
    {
        addAndMakeVisible (insertView);
        insertView.setViewedComponent (&inserts, false);
        insertView.setScrollBarsShown (true, false);
        insertView.setScrollBarThickness (5);

        for (auto* b : { &mute, &solo, &arm })
        {
            addAndMakeVisible (b);
            b->setClickingTogglesState (true);
        }
        mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb020));
        solo.setColour (juce::TextButton::buttonOnColourId, theme::accent2);
        arm.setColour  (juce::TextButton::buttonOnColourId, theme::record);
        mute.setTooltip ("Mute");
        solo.setTooltip ("Solo");
        arm.setTooltip ("Arm for recording");

        if (track->type == Track::Type::Instrument)
        {
            addAndMakeVisible (instrumentPicker);
            instrumentPicker.setTooltip ("Choose this track's instrument");
            instrumentPicker.onClick = [this] { devicemenus::showInstrumentMenu (owner.ctx, track->id, &instrumentPicker); };
        }

        addAndMakeVisible (autoButton);
        autoButton.setTooltip ("Automation: show or hide this track's lanes (right-click to add a lane)");
        autoButton.onClick = [this]
        {
            if (juce::ModifierKeys::currentModifiers.isPopupMenu()) return;
            if (track->automation.empty()) { showAddLaneMenu (owner.ctx, track, &autoButton); return; }
            bool anyVisible = false;
            for (auto& l : track->automation) anyVisible = anyVisible || l.visible;
            {
                const juce::ScopedLock sl (owner.ctx.project.lock);
                for (auto& l : track->automation) l.visible = ! anyVisible;
            }
            owner.ctx.project.changed();
        };

        for (int i = 0; i < (int) track->automation.size(); ++i)
            if (track->automation[(size_t) i].visible)
                addAndMakeVisible (laneHeaders.add (new LaneHeader (owner, track, i)));

        mute.onClick = [this] { const juce::ScopedLock sl (owner.ctx.project.lock); track->mute = mute.getToggleState(); };
        solo.onClick = [this] { const juce::ScopedLock sl (owner.ctx.project.lock); track->solo = solo.getToggleState(); };
        arm.onClick  = [this] { const juce::ScopedLock sl (owner.ctx.project.lock); track->armed = arm.getToggleState(); };

        addAndMakeVisible (volume);
        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange (0.0, 1.5);
        volume.setDoubleClickReturnValue (true, 0.8);
        volume.onValueChange = [this] { const juce::ScopedLock sl (owner.ctx.project.lock); track->volume = (float) volume.getValue(); };
        volume.setTooltip ("Volume");

        addAndMakeVisible (pan);
        pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
        pan.setRange (-1.0, 1.0);
        pan.setDoubleClickReturnValue (true, 0.0);
        pan.setPopupDisplayEnabled (true, true, nullptr);
        pan.textFromValueFunction = [] (double v)
        {
            const int p = juce::roundToInt (std::abs (v) * 100.0);
            return p == 0 ? juce::String ("Centre") : juce::String (p) + (v < 0 ? " L" : " R");
        };
        pan.onValueChange = [this] { const juce::ScopedLock sl (owner.ctx.project.lock); track->pan = (float) pan.getValue(); };
        pan.setTooltip ("Pan (double-click to centre)");

        sync();
    }

    void sync()
    {
        mute.setToggleState (track->mute, juce::dontSendNotification);
        solo.setToggleState (track->solo, juce::dontSendNotification);
        arm.setToggleState (track->armed, juce::dontSendNotification);
        // when automation is driving them, the controls follow it
        const float av = track->autoVolume.load(), ap = track->autoPan.load();
        if (! volume.isMouseButtonDown())
            volume.setValue (av >= 0 ? av : track->volume, juce::dontSendNotification);
        if (! pan.isMouseButtonDown())
            pan.setValue (ap >= -1.0f ? ap : track->pan, juce::dontSendNotification);
        if (track->type == Track::Type::Instrument)
        {
            const auto name = AppContext::instrumentName (*track);
            if (name != instrumentPicker.text) { instrumentPicker.text = name; instrumentPicker.repaint(); }
        }
        const bool hasLanes = ! track->automation.empty();
        autoButton.setColour (juce::TextButton::buttonColourId, hasLanes ? theme::accent.withAlpha (0.35f) : theme::panelHi);
        for (auto* lh : laneHeaders) lh->repaint();
    }

    void resized() override
    {
        auto all = getLocalBounds();
        auto mainArea = all.removeFromTop (trackHeight);
        for (auto* lh : laneHeaders) lh->setBounds (all.removeFromTop (laneHeight));

        // effects column on the right
        auto fxArea = mainArea.removeFromRight (fxColumnWidth).reduced (6, 5).withTrimmedRight (4);
        insertView.setBounds (fxArea);
        inserts.setSize (fxArea.getWidth() - (inserts.getIdealHeight() > fxArea.getHeight() ? 6 : 0), inserts.getIdealHeight());

        auto r = mainArea.reduced (10, 8).withTrimmedLeft (6);
        autoButton.setBounds (r.getRight() - 24, r.getY(), 24, 18);
        r.removeFromTop (22);
        auto row = r.removeFromTop (22);
        arm.setBounds (row.removeFromRight (24));  row.removeFromRight (3);
        solo.setBounds (row.removeFromRight (24)); row.removeFromRight (3);
        mute.setBounds (row.removeFromRight (24)); row.removeFromRight (4);
        pan.setBounds (row.removeFromRight (26).expanded (0, 2)); row.removeFromRight (2);
        volume.setBounds (row.withTrimmedRight (2));
        if (track->type == Track::Type::Instrument)
            instrumentPicker.setBounds (r.removeFromBottom (18).withWidth (juce::jmin (r.getWidth(), 170)));
    }

    void paint (juce::Graphics& g) override
    {
        const bool selected = owner.ctx.selectedTrackId == track->id;
        const auto mainArea = getLocalBounds().removeFromTop (trackHeight);
        g.setColour (selected ? theme::panelHi : theme::panel);
        g.fillRect (mainArea);
        g.setColour (track->colour);
        g.fillRect (0, 0, 5, trackHeight);

        // divider before the effects column
        g.setColour (theme::bg.withAlpha (0.6f));
        g.drawVerticalLine (getWidth() - fxColumnWidth, 6.0f, (float) trackHeight - 6.0f);

        auto r = mainArea.withTrimmedRight (fxColumnWidth).reduced (10, 8).withTrimmedLeft (6);
        auto top = r.removeFromTop (20).withTrimmedRight (28);

        // type icon
        auto icon = top.removeFromLeft (16).toFloat().withSizeKeepingCentre (14, 12);
        g.setColour (theme::textDim);
        if (track->type == Track::Type::Instrument)
        {
            g.drawRoundedRectangle (icon, 2, 1.2f);
            for (int k = 1; k < 4; ++k)
                g.fillRect (icon.getX() + k * icon.getWidth() / 4 - 0.6f, icon.getY(), 1.2f, icon.getHeight() * 0.6f);
        }
        else
        {
            for (int k = 0; k < 5; ++k)
            {
                const float h = icon.getHeight() * (k % 2 ? 0.9f : 0.5f);
                g.fillRect (icon.getX() + k * 3.0f, icon.getCentreY() - h / 2, 1.6f, h);
            }
        }
        top.removeFromLeft (6);
        g.setColour (theme::text);
        g.setFont (theme::font (14.0f, true));
        g.drawText (track->name, top, juce::Justification::centredLeft, true);


        // meter
        auto m = mainArea.withLeft (mainArea.getRight() - 6).reduced (1, 8);
        g.setColour (theme::bg);
        g.fillRect (m);
        const float level = juce::jlimit (0.0f, 1.0f, juce::jmax (track->meterL.load(), track->meterR.load()));
        const int h = (int) (m.getHeight() * std::sqrt (level));
        g.setColour (level > 0.95f ? theme::record : theme::accent2);
        g.fillRect (m.withTop (m.getBottom() - h));

        g.setColour (theme::bg);
        g.drawHorizontalLine (trackHeight - 1, 0, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == &autoButton || e.originalComponent == &autoButton)
        {
            if (e.mods.isPopupMenu()) showAddLaneMenu (owner.ctx, track, &autoButton);
            return;
        }
        owner.ctx.selectTrack (track->id);
        if (e.mods.isPopupMenu())
        {
            juce::PopupMenu menu;
            menu.addItem ("Rename...", [this] { if (owner.ctx.renameTrack) owner.ctx.renameTrack (track->id); });
            if (track->type == Track::Type::Instrument)
                menu.addItem ("Choose instrument...", [this] { devicemenus::showInstrumentMenu (owner.ctx, track->id, this); });
            menu.addItem ("Show in mixer", [this] { if (owner.ctx.showInMixer) owner.ctx.showInMixer (track->id); });
            menu.addItem ("Add automation lane...", [this] { showAddLaneMenu (owner.ctx, track, &autoButton); });
            menu.addItem ("Add effect...", [this] { devicemenus::showAddEffectMenu (owner.ctx, track->id, &insertView,
                                                       [this, id = track->id] (FxPtr s) { if (owner.ctx.openDevice) owner.ctx.openDevice (id, s, false); }); });
            menu.addSeparator();
            menu.addItem ("Delete track", [&o = owner, id = track->id]
            {
                o.ctx.project.beginEdit();
                o.ctx.project.removeTrack (id);
            });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
        }
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (owner.ctx.renameTrack) owner.ctx.renameTrack (track->id);
    }

    TimelineView& owner;
    TrackPtr track;
    juce::TextButton mute { "M" }, solo { "S" }, arm { "R" }, autoButton { "A" };
    juce::OwnedArray<LaneHeader> laneHeaders;
    juce::Viewport insertView;
    InsertList inserts;
    juce::Slider volume, pan;

    // the instrument's name as a pill with an arrow; click to choose another
    struct InstrumentPicker : juce::Button
    {
        InstrumentPicker() : juce::Button ("instrument") {}
        juce::String text;
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            g.setColour (down ? theme::accent.withAlpha (0.35f) : over ? theme::grid.brighter (0.25f) : theme::grid);
            g.fillRoundedRectangle (r, 4.0f);
            g.setColour (over ? theme::text : theme::textDim);
            g.setFont (theme::font (11.5f, true));
            g.drawText (text, r.withTrimmedLeft (8).withTrimmedRight (18), juce::Justification::centredLeft, true);
            juce::Path arrow;
            const float cx = r.getRight() - 10, cy = r.getCentreY();
            arrow.addTriangle (cx - 4, cy - 2, cx + 4, cy - 2, cx, cy + 3);
            g.fillPath (arrow);
        }
    } instrumentPicker;
};

// =============================================================================
//  Lanes: grid, clips, playhead. Handles moving / resizing / creating clips.
// =============================================================================
class TimelineView::Lanes : public juce::Component
{
public:
    explicit Lanes (TimelineView& o) : owner (o) {}

    void paint (juce::Graphics& g) override
    {
        auto& p = owner.ctx.project;
        g.fillAll (theme::lane);

        const int numTracks = (int) p.tracks.size();
        for (int i = 0; i < numTracks; ++i)
        {
            const int y = owner.trackTop (i) - owner.scrollY;
            if (y + owner.trackFullHeight (i) < 0 || y > getHeight()) continue;
            const bool sel = owner.ctx.selectedTrackId == p.tracks[(size_t) i]->id;
            g.setColour (sel ? theme::laneAlt.brighter (0.04f) : (i % 2 ? theme::laneAlt : theme::lane));
            g.fillRect (0, y, getWidth(), trackHeight);
            g.setColour (theme::bg.brighter (0.02f));   // automation lanes are darker
            g.fillRect (0, y + trackHeight, getWidth(), owner.trackFullHeight (i) - trackHeight);
        }

        // loop region
        if (p.loopEnabled)
        {
            const float lx0 = owner.beatToX (p.loopStart), lx1 = owner.beatToX (p.loopEnd);
            g.setColour (theme::accent2.withAlpha (0.06f));
            g.fillRect (juce::Rectangle<float> (lx0, 0, lx1 - lx0, (float) getHeight()));
        }

        // grid
        const double first = std::floor (owner.xToBeat (0)), last = owner.xToBeat ((float) getWidth()) + 1;
        const double snap = owner.getSnap();
        for (double b = juce::jmax (0.0, first); b <= last; b += juce::jmax (snap, 0.25))
        {
            const float x = owner.beatToX (b);
            const bool bar = std::fmod (b, (double) p.beatsPerBar) < 1.0e-6;
            const bool beat = std::fmod (b, 1.0) < 1.0e-6;
            if (! bar && ! beat && owner.pixelsPerBeat < 60) continue;
            g.setColour (bar ? theme::gridBar : beat ? theme::grid : theme::grid.withAlpha (0.45f));
            g.drawVerticalLine ((int) x, 0, (float) getHeight());
        }

        for (int i = 0; i < numTracks; ++i)
        {
            g.setColour (theme::bg);
            g.drawHorizontalLine (owner.trackTop (i) + owner.trackFullHeight (i) - owner.scrollY - 1, 0, (float) getWidth());
        }

        // clips
        for (int i = 0; i < numTracks; ++i)
        {
            auto& t = *p.tracks[(size_t) i];
            const int y = owner.trackTop (i) - owner.scrollY;
            if (y + owner.trackFullHeight (i) < 0 || y > getHeight()) continue;
            for (auto& c : t.clips)
                drawClip (g, t, *c, getClipBounds (i, *c));
            drawAutomation (g, i);
        }

        // drop indicator
        if (owner.dropIndicatorBeat >= 0)
        {
            const float x = owner.beatToX (owner.dropIndicatorBeat);
            g.setColour (theme::accent.withAlpha (0.8f));
            const float y = (float) ((owner.dropIndicatorTrack < numTracks ? owner.trackTop (owner.dropIndicatorTrack) : owner.contentHeight()) - owner.scrollY);
            g.fillRect (x - 1, y, 2.0f, (float) trackHeight);
        }

        // empty state
        if (numTracks == 0)
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (16.0f));
            g.drawText ("Add a track with the + buttons above, or drop audio files here",
                        getLocalBounds(), juce::Justification::centred);
        }

        // playhead
        const float px = owner.beatToX (owner.ctx.engine.getPositionBeats());
        g.setColour (owner.ctx.engine.isRecording() ? theme::record : theme::playhead.withAlpha (0.9f));
        g.fillRect (px - 0.5f, 0.0f, 1.5f, (float) getHeight());
    }

    juce::Rectangle<float> getClipBounds (int trackIndex, const Clip& c) const
    {
        const double bpm = owner.ctx.project.bpm;
        const float x0 = owner.beatToX (c.startBeat);
        const float x1 = owner.beatToX (c.getEndBeat (bpm));
        return { x0, (float) (owner.trackTop (trackIndex) - owner.scrollY + 3), juce::jmax (4.0f, x1 - x0), (float) trackHeight - 7 };
    }

    // ---- automation lanes ----
    float laneValueToY (int laneTop, float v) const { return (float) laneTop + 6.0f + (1.0f - v) * (float) (laneHeight - 12); }
    float laneYToValue (int laneTop, float y) const { return juce::jlimit (0.0f, 1.0f, 1.0f - (y - (float) laneTop - 6.0f) / (float) (laneHeight - 12)); }

    void drawAutomation (juce::Graphics& g, int trackIndex)
    {
        auto& t = *owner.ctx.project.tracks[(size_t) trackIndex];
        int y = owner.trackTop (trackIndex) + trackHeight - owner.scrollY;
        for (int li = 0; li < (int) t.automation.size(); ++li)
        {
            auto& lane = t.automation[(size_t) li];
            if (! lane.visible) continue;
            const int top = y;
            y += laneHeight;
            if (top > getHeight() || top + laneHeight < 0) continue;

            g.setColour (theme::bg);
            g.drawHorizontalLine (top + laneHeight - 1, 0, (float) getWidth());

            if (lane.points.empty())
            {
                // no points yet: a dashed line at the current setting
                float v = 0.5f;
                if (lane.kind == AutomationLane::Kind::Volume) v = AutomationLane::volumeToValue (t.volume);
                else if (lane.kind == AutomationLane::Kind::Pan) v = AutomationLane::panToValue (t.pan);
                else if (auto* param = t.getAutomationParameter (lane)) v = param->getValue();
                const float ly = laneValueToY (top, v);
                juce::Path line;
                line.startNewSubPath (0, ly);
                line.lineTo ((float) getWidth(), ly);
                juce::Path dashed;
                const float dashes[] = { 4.0f, 4.0f };
                juce::PathStrokeType (1.0f).createDashedStroke (dashed, line, dashes, 2);
                g.setColour (t.colour.withAlpha (0.5f));
                g.fillPath (dashed);
                g.setColour (theme::textDim);
                g.setFont (theme::font (11.0f));
                g.drawText ("Click to add automation points", 10, top, 300, laneHeight, juce::Justification::centredLeft);
                continue;
            }

            // filled curve
            juce::Path curve, fill;
            const float firstY = laneValueToY (top, lane.points.front().value);
            curve.startNewSubPath (0, firstY);
            for (auto& pt : lane.points)
                curve.lineTo (owner.beatToX (pt.beat), laneValueToY (top, pt.value));
            curve.lineTo ((float) getWidth(), laneValueToY (top, lane.points.back().value));
            fill = curve;
            fill.lineTo ((float) getWidth(), (float) (top + laneHeight - 2));
            fill.lineTo (0, (float) (top + laneHeight - 2));
            fill.closeSubPath();
            g.setColour (t.colour.withAlpha (0.14f));
            g.fillPath (fill);
            g.setColour (t.colour.brighter (0.3f));
            g.strokePath (curve, juce::PathStrokeType (1.8f));

            for (int pi = 0; pi < (int) lane.points.size(); ++pi)
            {
                const auto& pt = lane.points[(size_t) pi];
                const float px = owner.beatToX (pt.beat), py = laneValueToY (top, pt.value);
                if (px < -6 || px > getWidth() + 6) continue;
                const bool active = dragLaneTrack == trackIndex && dragLane == li && dragPoint == pi;
                g.setColour (active ? juce::Colours::white : t.colour.brighter (0.5f));
                g.fillEllipse (px - 4.5f, py - 4.5f, 9.0f, 9.0f);
                g.setColour (theme::bg);
                g.drawEllipse (px - 4.5f, py - 4.5f, 9.0f, 9.0f, 1.2f);
                if (active)
                {
                    g.setColour (theme::text);
                    g.setFont (theme::font (11.0f, true));
                    g.drawText (automationValueText (t, lane, pt.value), (int) px + 8, (int) py - 16, 120, 14, juce::Justification::centredLeft);
                }
            }
        }
    }

    int pointAt (const Track& t, const AutomationLane& lane, int laneTop, juce::Point<int> pos) const
    {
        juce::ignoreUnused (t);
        for (int i = (int) lane.points.size(); --i >= 0;)
        {
            const auto& pt = lane.points[(size_t) i];
            if (juce::Point<float> (owner.beatToX (pt.beat), laneValueToY (laneTop, pt.value)).getDistanceFrom (pos.toFloat()) < 7.0f)
                return i;
        }
        return -1;
    }

    // returns true if the click was in an automation lane
    bool automationMouseDown (const juce::MouseEvent& e)
    {
        auto hit = owner.rowAt (e.y);
        if (hit.track < 0 || hit.lane < 0 || hit.track >= (int) owner.ctx.project.tracks.size()) return false;
        auto& p = owner.ctx.project;
        auto track = p.tracks[(size_t) hit.track];
        auto& lane = track->automation[(size_t) hit.lane];
        const int top = hit.laneTop - owner.scrollY;
        owner.ctx.selectTrack (track->id);

        int pi = pointAt (*track, lane, top, e.getPosition());
        if (pi >= 0 && (e.mods.isPopupMenu() || e.mods.isAltDown()))
        {
            p.beginEdit();
            { const juce::ScopedLock sl (p.lock); lane.points.erase (lane.points.begin() + pi); }
            p.changed();
            return true;
        }
        if (e.mods.isPopupMenu()) return true;

        p.beginEdit();
        if (pi < 0)
        {
            AutomationPoint pt;
            pt.beat = juce::jmax (0.0, owner.ctx.snapBeat (owner.xToBeat ((float) e.x), e.mods.isShiftDown() ? 0.0 : owner.getSnap() * 0.25));
            pt.value = laneYToValue (top, (float) e.y);
            const juce::ScopedLock sl (p.lock);
            auto it = std::lower_bound (lane.points.begin(), lane.points.end(), pt.beat, [] (const AutomationPoint& a, double b) { return a.beat < b; });
            pi = (int) (lane.points.insert (it, pt) - lane.points.begin());
        }
        dragLaneTrack = hit.track;
        dragLane = hit.lane;
        dragPoint = pi;
        dragLaneTop = hit.laneTop;
        repaint();
        return true;
    }

    void automationMouseDrag (const juce::MouseEvent& e)
    {
        auto& p = owner.ctx.project;
        if (dragLaneTrack >= (int) p.tracks.size()) return;
        auto& lane = p.tracks[(size_t) dragLaneTrack]->automation[(size_t) dragLane];
        if (dragPoint < 0 || dragPoint >= (int) lane.points.size()) return;
        const juce::ScopedLock sl (p.lock);
        auto& pt = lane.points[(size_t) dragPoint];
        double beat = juce::jmax (0.0, owner.ctx.snapBeat (owner.xToBeat ((float) e.x), e.mods.isShiftDown() ? 0.0 : owner.getSnap() * 0.25));
        // stay between the neighbours so the lane stays in order
        if (dragPoint > 0) beat = juce::jmax (beat, lane.points[(size_t) dragPoint - 1].beat + 1.0e-4);
        if (dragPoint + 1 < (int) lane.points.size()) beat = juce::jmin (beat, lane.points[(size_t) dragPoint + 1].beat - 1.0e-4);
        pt.beat = beat;
        pt.value = laneYToValue (dragLaneTop - owner.scrollY, (float) e.y);
        repaint();
    }

    void drawClip (juce::Graphics& g, const Track& t, const Clip& c, juce::Rectangle<float> r)
    {
        if (r.getRight() < 0 || r.getX() > getWidth()) return;
        const bool selected = owner.ctx.selectedClipId == c.id;
        auto body = t.colour.withMultipliedSaturation (0.75f).withMultipliedBrightness (0.55f);

        g.setColour (body.withAlpha (0.9f));
        g.fillRoundedRectangle (r, 5.0f);
        auto title = r.removeFromTop (16.0f);
        g.setColour (t.colour.withAlpha (0.9f));
        g.fillRoundedRectangle (title.withHeight (title.getHeight() + 5), 5.0f);
        g.setColour (body.withAlpha (0.9f));
        g.fillRect (title.withTop (title.getBottom()).withHeight (5.0f));

        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.setFont (theme::font (11.5f, true));
        g.drawText (c.name + (c.pitch.active && c.pitch.hasChanges() ? (c.pitch.bypass ? "  -  pitch (bypassed)" : "  -  pitch edited") : ""),
                    title.reduced (6, 0).toNearestInt(), juce::Justification::centredLeft, true);

        auto content = r.reduced (2, 3);
        g.saveState();
        g.reduceClipRegion (content.toNearestInt());

        if (c.type == Clip::Type::Audio && c.audio != nullptr)
        {
            // waveform from the cached peak overview
            const auto& peaks = c.audio->peaks;
            const int numPeaks = (int) peaks.size() / 2;
            const double secondsPerPixel = (60.0 / owner.ctx.project.bpm) / owner.pixelsPerBeat;
            const double peaksPerSecond = c.audio->sampleRate / AudioData::peakBlock;
            const float mid = content.getCentreY(), half = content.getHeight() * 0.5f;
            g.setColour (t.colour.brighter (0.4f).withAlpha (0.85f));

            const int px0 = (int) juce::jmax (content.getX(), 0.0f);
            const int px1 = (int) juce::jmin (content.getRight(), (float) getWidth());
            for (int px = px0; px < px1; ++px)
            {
                const double s0 = c.offsetSeconds + (px - r.getX()) * secondsPerPixel;
                const double s1 = s0 + secondsPerPixel;
                int p0 = (int) (s0 * peaksPerSecond), p1 = juce::jmax (p0 + 1, (int) (s1 * peaksPerSecond));
                p0 = juce::jlimit (0, numPeaks, p0); p1 = juce::jlimit (0, numPeaks, p1);
                float lo = 0, hi = 0;
                for (int k = p0; k < p1; ++k) { lo = juce::jmin (lo, peaks[(size_t) k * 2]); hi = juce::jmax (hi, peaks[(size_t) k * 2 + 1]); }
                g.drawVerticalLine (px, mid - hi * half, mid - lo * half + 1);
            }
        }
        else if (c.type == Clip::Type::Midi && ! c.notes.empty())
        {
            int lo = 127, hi = 0;
            for (auto& n : c.notes) { lo = juce::jmin (lo, n.pitch); hi = juce::jmax (hi, n.pitch); }
            lo -= 2; hi += 2;
            const float nh = juce::jmin (6.0f, content.getHeight() / (float) (hi - lo + 1));
            g.setColour (t.colour.brighter (0.5f));
            for (auto& n : c.notes)
            {
                if (n.start >= c.lengthBeats) continue;
                const float x = content.getX() + (float) (n.start * owner.pixelsPerBeat);
                const float w = (float) (juce::jmin (n.length, c.lengthBeats - n.start) * owner.pixelsPerBeat);
                const float y = content.getBottom() - (float) (n.pitch - lo + 1) / (float) (hi - lo + 1) * content.getHeight();
                g.fillRect (x, y, juce::jmax (1.5f, w - 1), juce::jmax (1.5f, nh - 1));
            }
        }
        g.restoreState();

        auto full = r.withTop (title.getY());
        g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
        g.drawRoundedRectangle (full.reduced (0.5f), 5.0f, selected ? 2.0f : 1.0f);
    }

    struct Hit { TrackPtr track; ClipPtr clip; int trackIndex = -1; bool onRightEdge = false; };

    Hit hitTestClip (juce::Point<int> pos) const
    {
        Hit h;
        auto& p = owner.ctx.project;
        const auto row = owner.rowAt (pos.y);
        h.trackIndex = row.track;
        if (h.trackIndex < 0 || h.trackIndex >= (int) p.tracks.size() || row.lane >= 0) { h.trackIndex = -1; return h; }
        h.track = p.tracks[(size_t) h.trackIndex];
        for (int k = (int) h.track->clips.size(); --k >= 0;)
        {
            auto& c = h.track->clips[(size_t) k];
            auto b = getClipBounds (h.trackIndex, *c);
            if (b.contains (pos.toFloat()))
            {
                h.clip = c;
                h.onRightEdge = pos.x > b.getRight() - 7;
                return h;
            }
        }
        return h;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto h = hitTestClip (e.getPosition());
        setMouseCursor (h.clip != nullptr ? (h.onRightEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::DraggingHandCursor)
                                          : juce::MouseCursor::NormalCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragLaneTrack = dragLane = dragPoint = -1;
        if (automationMouseDown (e)) return;
        auto& ctx = owner.ctx;
        auto h = hitTestClip (e.getPosition());
        dragging = false;
        dragClip = nullptr;

        if (h.clip != nullptr)
        {
            ctx.selectClip (h.clip->id);
            dragClip = h.clip;
            dragMode = h.onRightEdge ? Mode::Resize : Mode::Move;
            origStart = h.clip->startBeat;
            origLengthBeats = h.clip->lengthBeats;
            origLengthSeconds = h.clip->lengthSeconds;
            origTrack = h.track;
            mouseDownBeat = owner.xToBeat ((float) e.x);

            if (e.mods.isPopupMenu()) showClipMenu (h.clip);
        }
        else
        {
            ctx.selectedClipId = -1;
            if (h.track != nullptr) ctx.selectTrack (h.track->id);
            else if (ctx.onSelectionChanged) ctx.onSelectionChanged();

            if (e.mods.isPopupMenu() && h.track != nullptr)
                showLaneMenu (h.track, owner.ctx.snapBeat (owner.xToBeat ((float) e.x), owner.getSnap()));
            else if (! e.mods.isPopupMenu())
                ctx.engine.setPositionBeats (juce::jmax (0.0, owner.ctx.snapBeat (owner.xToBeat ((float) e.x), owner.getSnap())));
        }
        owner.repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragPoint >= 0) { automationMouseDrag (e); return; }
        if (dragClip == nullptr || e.mods.isPopupMenu()) return;
        if (! dragging)
        {
            if (e.getDistanceFromDragStart() < 3) return;
            dragging = true;
            owner.ctx.project.beginEdit();
        }

        auto& p = owner.ctx.project;
        const double snap = e.mods.isAltDown() ? 0.0 : owner.getSnap();
        const double delta = owner.xToBeat ((float) e.x) - mouseDownBeat;

        const juce::ScopedLock sl (p.lock);
        if (dragMode == Mode::Move)
        {
            dragClip->startBeat = juce::jmax (0.0, owner.ctx.snapBeat (origStart + delta, snap));

            // move between tracks of the same kind
            const int ti = owner.trackIndexAtY (e.y);
            if (ti >= 0 && ti < (int) p.tracks.size())
            {
                auto target = p.tracks[(size_t) ti];
                TrackPtr current;
                p.findClip (dragClip->id, &current);
                if (target != current && current != nullptr && target->type == current->type)
                {
                    auto& v = current->clips;
                    v.erase (std::remove (v.begin(), v.end(), dragClip), v.end());
                    target->clips.push_back (dragClip);
                    owner.ctx.selectedTrackId = target->id;
                }
            }
        }
        else
        {
            const double newEnd = owner.ctx.snapBeat (origStart + (dragClip->type == Clip::Type::Midi ? origLengthBeats : origLengthSeconds * p.bpm / 60.0) + delta, snap);
            const double minLen = juce::jmax (0.25, snap);
            const double lenBeats = juce::jmax (minLen, newEnd - dragClip->startBeat);
            if (dragClip->type == Clip::Type::Midi)
                dragClip->lengthBeats = lenBeats;
            else if (dragClip->audio != nullptr)
                dragClip->lengthSeconds = juce::jmin (lenBeats * 60.0 / p.bpm, dragClip->audio->getLengthSeconds() - dragClip->offsetSeconds);
        }
        owner.repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragPoint >= 0)
        {
            dragLaneTrack = dragLane = dragPoint = -1;
            owner.ctx.project.changed();
            return;
        }
        if (dragging) owner.ctx.project.changed();
        dragging = false;
        dragClip = nullptr;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // double-click an automation point to delete it
        if (auto hit = owner.rowAt (e.y); hit.track >= 0 && hit.lane >= 0 && hit.track < (int) owner.ctx.project.tracks.size())
        {
            auto& p = owner.ctx.project;
            auto& lane = p.tracks[(size_t) hit.track]->automation[(size_t) hit.lane];
            if (const int pi = pointAt (*p.tracks[(size_t) hit.track], lane, hit.laneTop - owner.scrollY, e.getPosition()); pi >= 0)
            {
                p.beginEdit();
                { const juce::ScopedLock sl (p.lock); lane.points.erase (lane.points.begin() + pi); }
                p.changed();
            }
            return;
        }
        auto h = hitTestClip (e.getPosition());
        if (h.clip != nullptr)
        {
            if (owner.ctx.openEditor) owner.ctx.openEditor (h.clip->id);
            return;
        }
        if (h.track != nullptr && h.track->type == Track::Type::Instrument)
        {
            auto& p = owner.ctx.project;
            const double bar = std::floor (owner.xToBeat ((float) e.x) / p.beatsPerBar) * p.beatsPerBar;
            p.beginEdit();
            if (auto c = p.addMidiClip (h.track->id, bar, p.beatsPerBar))
            {
                owner.ctx.selectClip (c->id);
                if (owner.ctx.openEditor) owner.ctx.openEditor (c->id);
            }
        }
    }

    void showClipMenu (ClipPtr clip)
    {
        auto& p = owner.ctx.project;
        juce::PopupMenu m;
        if (clip->type == Clip::Type::Midi)
            m.addItem ("Edit notes", [this, id = clip->id] { if (owner.ctx.openEditor) owner.ctx.openEditor (id); });
        m.addItem ("Split at playhead", [&p, id = clip->id, this] { p.beginEdit(); p.splitClip (id, owner.ctx.engine.getPositionBeats()); });
        m.addItem ("Duplicate", [&p, id = clip->id] { p.beginEdit(); p.duplicateClip (id); });
        m.addSeparator();
        m.addItem ("Delete", [&p, id = clip->id] { p.beginEdit(); p.removeClip (id); });
        m.showMenuAsync ({});
    }

    void showLaneMenu (TrackPtr track, double beat)
    {
        auto& p = owner.ctx.project;
        juce::PopupMenu m;
        if (track->type == Track::Type::Instrument)
            m.addItem ("New MIDI clip here", [this, &p, id = track->id, beat]
            {
                p.beginEdit();
                if (auto c = p.addMidiClip (id, beat, p.beatsPerBar)) owner.ctx.selectClip (c->id);
            });
        else
            m.addItem ("Import audio here...", [this, id = track->id, beat]
            {
                chooser = std::make_unique<juce::FileChooser> ("Import audio", juce::File(), owner.ctx.project.audioCache.formats.getWildcardForAllFormats());
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [this, id, beat] (const juce::FileChooser& fc)
                {
                    if (fc.getResult().existsAsFile())
                    {
                        owner.ctx.project.beginEdit();
                        owner.ctx.project.addAudioClip (id, fc.getResult(), beat);
                    }
                });
            });
        m.showMenuAsync ({});
    }

private:
    enum class Mode { Move, Resize };
    TimelineView& owner;
    ClipPtr dragClip;
    TrackPtr origTrack;
    Mode dragMode = Mode::Move;
    double origStart = 0, origLengthBeats = 0, origLengthSeconds = 0, mouseDownBeat = 0;
    bool dragging = false;
    int dragLaneTrack = -1, dragLane = -1, dragPoint = -1, dragLaneTop = 0;
    std::unique_ptr<juce::FileChooser> chooser;
};

// =============================================================================
TimelineView::TimelineView (AppContext& c) : ctx (c)
{
    ruler = std::make_unique<Ruler> (*this);
    lanes = std::make_unique<Lanes> (*this);
    addAndMakeVisible (*ruler);
    addAndMakeVisible (*lanes);
    addAndMakeVisible (headerHolder);
    addAndMakeVisible (hScroll);
    addAndMakeVisible (vScroll);
    hScroll.addListener (this);
    vScroll.addListener (this);
    hScroll.setAutoHide (false);
    lanes->setInterceptsMouseClicks (true, false);

    ctx.project.addChangeListener (this);
    rebuild();
    startTimerHz (30);
}

TimelineView::~TimelineView()
{
    ctx.project.removeChangeListener (this);
}

int TimelineView::trackFullHeight (int i) const
{
    if (i < 0 || i >= (int) ctx.project.tracks.size()) return trackHeight;
    int h = trackHeight;
    for (auto& l : ctx.project.tracks[(size_t) i]->automation) if (l.visible) h += laneHeight;
    return h;
}

int TimelineView::trackTop (int i) const
{
    int y = 0;
    for (int k = 0; k < i && k < (int) ctx.project.tracks.size(); ++k) y += trackFullHeight (k);
    return y;
}

int TimelineView::contentHeight() const { return trackTop ((int) ctx.project.tracks.size()); }

TimelineView::RowHit TimelineView::rowAt (int yInLanes) const
{
    RowHit hit;
    const int y = yInLanes + scrollY;
    if (y < 0) return hit;
    int top = 0;
    const int n = (int) ctx.project.tracks.size();
    for (int i = 0; i < n; ++i)
    {
        const int h = trackFullHeight (i);
        if (y < top + h)
        {
            hit.track = i;
            if (y >= top + trackHeight)
            {
                int laneTop = top + trackHeight;
                const auto& trackLanes = ctx.project.tracks[(size_t) i]->automation;
                for (int l = 0; l < (int) trackLanes.size(); ++l)
                {
                    if (! trackLanes[(size_t) l].visible) continue;
                    if (y < laneTop + laneHeight) { hit.lane = l; hit.laneTop = laneTop; break; }
                    laneTop += laneHeight;
                }
            }
            return hit;
        }
        top += h;
    }
    hit.track = n;   // below the last track
    return hit;
}

double TimelineView::getSnap() const
{
    if (pixelsPerBeat >= 120) return 0.25;
    if (pixelsPerBeat >= 60)  return 0.5;
    if (pixelsPerBeat >= 14)  return 1.0;
    return (double) ctx.project.beatsPerBar;
}

void TimelineView::rebuild()
{
    headers.clear();
    for (auto& t : ctx.project.tracks)
        headerHolder.addAndMakeVisible (headers.add (new TrackHeader (*this, t)));
    layoutHeaders();
    updateScrollBars();
    repaint();
}

void TimelineView::layoutHeaders()
{
    for (int i = 0; i < headers.size(); ++i)
        headers[i]->setBounds (0, trackTop (i) - scrollY, headerWidth, trackFullHeight (i));
}

void TimelineView::resized()
{
    auto r = getLocalBounds();
    auto bottom = r.removeFromBottom (12);
    vScroll.setBounds (r.removeFromRight (12).withTrimmedTop (rulerHeight));
    auto top = r.removeFromTop (rulerHeight);
    top.removeFromLeft (headerWidth);
    ruler->setBounds (top);
    headerHolder.setBounds (r.removeFromLeft (headerWidth));
    lanes->setBounds (r);
    hScroll.setBounds (bottom.withTrimmedLeft (headerWidth).withTrimmedRight (12));
    layoutHeaders();
    updateScrollBars();
}

void TimelineView::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.0f, true));
    g.drawText ("TRACKS", 16, 0, headerWidth - 16, rulerHeight, juce::Justification::centredLeft);
}

void TimelineView::updateScrollBars()
{
    const double contentBeats = juce::jmax (ctx.project.getEndBeat() + 32.0, xToBeat ((float) lanes->getWidth()) + 8.0, 128.0);
    hScroll.setRangeLimits (0, contentBeats * pixelsPerBeat, juce::dontSendNotification);
    hScroll.setCurrentRange (scrollX, lanes->getWidth(), juce::dontSendNotification);

    const int contentH = contentHeight() + 40;
    vScroll.setRangeLimits (0, juce::jmax (contentH, lanes->getHeight()), juce::dontSendNotification);
    vScroll.setCurrentRange (scrollY, lanes->getHeight(), juce::dontSendNotification);
}

void TimelineView::scrollBarMoved (juce::ScrollBar* bar, double start)
{
    if (bar == &hScroll) scrollX = start;
    else                 scrollY = (int) start;
    layoutHeaders();
    repaint();
}

void TimelineView::zoom (float factor, int aroundX)
{
    if (aroundX < 0) aroundX = lanes->getWidth() / 2;
    const double beatAt = xToBeat ((float) aroundX);
    pixelsPerBeat = juce::jlimit (3.0, 400.0, pixelsPerBeat * factor);
    scrollX = juce::jmax (0.0, beatAt * pixelsPerBeat - aroundX);
    updateScrollBars();
    repaint();
}

void TimelineView::zoomToFit()
{
    const double end = juce::jmax (8.0, ctx.project.getEndBeat() + 4.0);
    pixelsPerBeat = juce::jlimit (3.0, 400.0, (lanes->getWidth() > 50 ? lanes->getWidth() - 20 : 900) / end);
    scrollX = 0;
    scrollY = 0;
    updateScrollBars();
    layoutHeaders();
    repaint();
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    if (e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        zoom (w.deltaY > 0 ? 1.15f : 1.0f / 1.15f, e.getEventRelativeTo (lanes.get()).x);
        return;
    }
    float dx = w.deltaX, dy = w.deltaY;
    if (e.mods.isShiftDown()) std::swap (dx, dy);
    scrollX = juce::jmax (0.0, scrollX - dx * 400.0);
    scrollY = juce::jlimit (0, juce::jmax (0, (int) vScroll.getMaximumRangeLimit() - lanes->getHeight()), scrollY - (int) (dy * 300.0f));
    updateScrollBars();
    layoutHeaders();
    repaint();
}

void TimelineView::timerCallback()
{
    // follow the playhead while playing
    if (ctx.engine.isPlaying())
    {
        const float px = beatToX (ctx.engine.getPositionBeats());
        if (px > lanes->getWidth() - 40 || px < 0)
        {
            scrollX = juce::jmax (0.0, ctx.engine.getPositionBeats() * pixelsPerBeat - 60.0);
            updateScrollBars();
        }
    }
    for (auto* h : headers) { h->sync(); h->repaint(); }
    ruler->repaint();
    lanes->repaint();
}

// ---- drag & drop ------------------------------------------------------------
void TimelineView::importFilesAt (const juce::StringArray& files, int x, int y)
{
    auto& p = ctx.project;
    const auto lp = lanes->getLocalPoint (this, juce::Point<int> (x, y));
    double beat = juce::jmax (0.0, ctx.snapBeat (xToBeat ((float) lp.x), getSnap()));
    int ti = trackIndexAtY (lp.y);

    p.beginEdit();
    for (auto& path : files)
    {
        juce::File f (path);
        if (! f.existsAsFile()) continue;

        TrackPtr target = (ti >= 0 && ti < (int) p.tracks.size()) ? p.tracks[(size_t) ti] : nullptr;
        if (target == nullptr || target->type != Track::Type::Audio)
        {
            target = p.addTrack (Track::Type::Audio);
            ti = (int) p.tracks.size() - 1;
        }
        if (auto c = p.addAudioClip (target->id, f, beat))
        {
            ctx.selectClip (c->id);
            ++ti;   // several files go onto consecutive tracks
        }
    }
    dropIndicatorBeat = -1;
    repaint();
}

void TimelineView::filesDropped (const juce::StringArray& files, int x, int y)
{
    // a project file (RENASOUND or Mixcraft) opens as a project instead of becoming a clip
    for (auto& path : files)
    {
        const juce::File f (path);
        if ((f.hasFileExtension ("renasound") || f.hasFileExtension ("als") || f.getFileExtension().toLowerCase().startsWith (".mx")) && ctx.openProjectFile)
        {
            ctx.openProjectFile (f);
            return;
        }
    }
    importFilesAt (files, x, y);
}

void TimelineView::itemDropped (const SourceDetails& d)
{
    juce::StringArray files;
    files.addTokens (d.description.toString(), "\n", {});
    importFilesAt (files, d.localPosition.x, d.localPosition.y);
}

void TimelineView::itemDragMove (const SourceDetails& d)
{
    const auto lp = lanes->getLocalPoint (this, d.localPosition);
    dropIndicatorBeat = juce::jmax (0.0, ctx.snapBeat (xToBeat ((float) lp.x), getSnap()));
    dropIndicatorTrack = juce::jlimit (0, juce::jmax (0, (int) ctx.project.tracks.size()), trackIndexAtY (lp.y));
    repaint();
}

} // namespace rs
