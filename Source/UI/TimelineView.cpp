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

        if (owner.ctx.showTime)
        {
            // minutes:seconds, labels at least ~70 px apart
            const double secPerBeat = 60.0 / p.bpm;
            const double pxPerSec = owner.pixelsPerBeat / secPerBeat;
            double step = 1.0;
            for (double cand : { 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0, 120.0, 300.0 })
                if (cand * pxPerSec >= 70.0) { step = cand; break; } else step = cand;
            const double t0 = juce::jmax (0.0, std::floor (owner.xToBeat (0) * secPerBeat / step) * step);
            const double t1 = owner.xToBeat ((float) getWidth()) * secPerBeat + step;
            for (double t = t0; t <= t1; t += step)
            {
                const float x = owner.beatToX (t / secPerBeat);
                g.setColour (theme::gridBar);
                g.drawVerticalLine ((int) x, 12.0f, (float) getHeight());
                g.setColour (theme::textDim);
                const int whole = (int) t;
                const auto label = juce::String (whole / 60) + ":" + juce::String (whole % 60).paddedLeft ('0', 2)
                                 + (step < 1.0 ? "." + juce::String ((int) std::round ((t - whole) * 10.0)) : juce::String());
                g.drawText (label, (int) x + 4, 11, 60, 16, juce::Justification::centredLeft);
                for (int k = 1; k < 4 && pxPerSec * step > 40; ++k)
                {
                    g.setColour (theme::grid);
                    g.drawVerticalLine ((int) owner.beatToX ((t + step * k / 4.0) / secPerBeat), (float) getHeight() - 7, (float) getHeight());
                }
            }
        }
        else
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
    std::vector<AutomationTarget> out { { "volume", "Volume" } };
    if (! t.isMaster) out.push_back ({ "pan", "Pan" });
    auto addProcessor = [&out] (juce::AudioProcessor* proc, const juce::String& prefix, const juce::String& owner)
    {
        if (proc == nullptr) return;
        const auto& params = proc->getParameters();
        for (int i = 0; i < juce::jmin (128, params.size()); ++i)
            if (params[i]->isAutomatable())
                out.push_back ({ prefix + juce::String (i), owner + ": " + params[i]->getName (40) });
    };
    if (t.instrument != nullptr) addProcessor (t.instrument->processor.get(), "inst:", t.instrument->getName());
    for (auto& f : t.effects()) addProcessor (f->processor.get(), "fx:" + juce::String (f->uid) + ":", f->getName());
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
            if (! track->isMaster) addAndMakeVisible (b);
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
            instrumentPicker.setTooltip ("Click to open the instrument and shape its sound; the arrow chooses another instrument");
            instrumentPicker.onClick = [this]
            {
                // the name opens the instrument's panel, the arrow (or a right-click) picks another one
                const bool onArrow = instrumentPicker.getMouseXYRelative().x > instrumentPicker.getWidth() - 22
                                  || juce::ModifierKeys::currentModifiers.isPopupMenu();
                if (! onArrow && owner.ctx.openDevice) owner.ctx.openDevice (track->id, track->instrument, true);
                else devicemenus::showInstrumentMenu (owner.ctx, track->id, &instrumentPicker);
            };
        }

        addAndMakeVisible (autoButton);
        autoButton.setTooltip ("Automation: add a lane, or show / hide this track's lanes (right-click to add another)");
        autoButton.setName ("Automation");
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
        volume.onValueChange = [this]
        {
            const juce::ScopedLock sl (owner.ctx.project.lock);
            if (track->isMaster) owner.ctx.project.masterVolume = (float) volume.getValue();
            else track->volume = (float) volume.getValue();
        };
        volume.setTooltip ("Volume");

        if (! track->isMaster) addAndMakeVisible (pan);
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
            volume.setValue (av >= 0 ? av : track->isMaster ? owner.ctx.project.masterVolume : track->volume, juce::dontSendNotification);
        if (! pan.isMouseButtonDown())
            pan.setValue (ap >= -1.0f ? ap : track->pan, juce::dontSendNotification);
        if (track->type == Track::Type::Instrument)
        {
            const auto name = AppContext::instrumentName (*track);
            if (name != instrumentPicker.text) { instrumentPicker.text = name; instrumentPicker.repaint(); }
        }
        const bool hasLanes = ! track->automation.empty();
        bool anyOpen = false;
        for (auto& l : track->automation) anyOpen = anyOpen || l.visible;
        if (autoButton.hasLanes != hasLanes || autoButton.open != anyOpen) { autoButton.hasLanes = hasLanes; autoButton.open = anyOpen; autoButton.repaint(); }
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
        r.removeFromTop (22);
        auto row = r.removeFromTop (22);
        if (track->isMaster)
        {
            autoButton.setBounds (row.removeFromRight (24));
            row.removeFromRight (6);
        }
        else
        {
            arm.setBounds (row.removeFromRight (24));        row.removeFromRight (3);
            autoButton.setBounds (row.removeFromRight (24)); row.removeFromRight (3);   // between S and R
        }
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
        auto top = r.removeFromTop (20).withTrimmedRight (4);

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
        if (track->isMaster)
        {
            if (! e.mods.isPopupMenu()) return;
            juce::PopupMenu menu;
            menu.addItem ("Add automation lane...", [this] { showAddLaneMenu (owner.ctx, track, &autoButton); });
            menu.addItem ("Add master effect...", [this] { devicemenus::showAddEffectMenu (owner.ctx, -1, &insertView); });
            menu.addSeparator();
            menu.addItem ("Hide master track", [&o = owner] { o.ctx.project.beginEdit(); o.ctx.project.showMaster = false; o.ctx.project.changed(); });
            menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
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
        if (! track->isMaster && owner.ctx.renameTrack) owner.ctx.renameTrack (track->id);
    }

    TimelineView& owner;
    TrackPtr track;
    juce::TextButton mute { "M" }, solo { "S" }, arm { "R" };

    // "Automation" with a little envelope curve; lit when the track has lanes
    struct AutoButton : juce::Button
    {
        AutoButton() : juce::Button ("Automation") {}
        bool hasLanes = false, open = false;
        void paintButton (juce::Graphics& g, bool over, bool down) override
        {
            auto r = getLocalBounds().toFloat().reduced (0.5f);
            if (open || over || down)
            {
                g.setColour (open ? theme::accent.withAlpha (0.2f) : theme::text.withAlpha (down ? 0.1f : 0.06f));
                g.fillRoundedRectangle (r, 4.0f);
            }
            const auto c = hasLanes ? theme::accent : over ? theme::text : theme::textDim;
            g.setColour (c);
            auto icon = r.withSizeKeepingCentre (14.0f, 11.0f);
            juce::Path curve;
            curve.startNewSubPath (icon.getX(), icon.getBottom());
            curve.lineTo (icon.getX() + icon.getWidth() * 0.35f, icon.getY() + icon.getHeight() * 0.25f);
            curve.lineTo (icon.getX() + icon.getWidth() * 0.65f, icon.getY() + icon.getHeight() * 0.6f);
            curve.lineTo (icon.getRight(), icon.getY());
            g.strokePath (curve, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            for (auto pt : { juce::Point<float> (icon.getX() + icon.getWidth() * 0.35f, icon.getY() + icon.getHeight() * 0.25f),
                             juce::Point<float> (icon.getX() + icon.getWidth() * 0.65f, icon.getY() + icon.getHeight() * 0.6f) })
                g.fillEllipse (pt.x - 2, pt.y - 2, 4, 4);
        }
    } autoButton;
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
        const int numRows = owner.numRows();
        for (int i = 0; i < numRows; ++i)
        {
            const int y = owner.trackTop (i) - owner.scrollY;
            if (y + owner.trackFullHeight (i) < 0 || y > getHeight()) continue;
            auto rt = owner.rowTrack (i);
            const bool sel = owner.ctx.selectedTrackId == rt->id;
            g.setColour (rt->isMaster ? theme::accent.withAlpha (0.06f).overlaidWith (theme::lane) : sel ? theme::laneAlt.brighter (0.04f) : (i % 2 ? theme::laneAlt : theme::lane));
            g.fillRect (0, y, getWidth(), trackHeight);
            g.setColour (theme::bg.brighter (0.02f));   // automation lanes are darker
            g.fillRect (0, y + trackHeight, getWidth(), owner.trackFullHeight (i) - trackHeight);
        }

        // loop region
        if (p.loopEnabled)
        {
            const float lx0 = owner.beatToX (p.loopStart), lx1 = owner.beatToX (p.loopEnd);
            g.setColour (theme::accent2.withAlpha (0.08f));
            g.fillRect (juce::Rectangle<float> (lx0, 0, lx1 - lx0, (float) getHeight()));
            g.setColour (theme::accent2.withAlpha (0.45f));
            g.drawVerticalLine ((int) lx0, 0, (float) getHeight());
            g.drawVerticalLine ((int) lx1, 0, (float) getHeight());
        }

        // grid
        const double first = std::floor (owner.xToBeat (0)), last = owner.xToBeat ((float) getWidth()) + 1;
        const double snap = owner.getGridStep();
        for (double b = juce::jmax (0.0, first); b <= last; b += juce::jmax (snap, 0.25))
        {
            const float x = owner.beatToX (b);
            const bool bar = std::fmod (b, (double) p.beatsPerBar) < 1.0e-6;
            const bool beat = std::fmod (b, 1.0) < 1.0e-6;
            if (! bar && ! beat && owner.pixelsPerBeat < 60) continue;
            g.setColour (bar ? theme::gridBar : beat ? theme::grid : theme::grid.withAlpha (0.45f));
            g.drawVerticalLine ((int) x, 0, (float) getHeight());
        }

        for (int i = 0; i < numRows; ++i)
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
        if (p.showMaster)
        {
            const int y = owner.trackTop (numTracks) - owner.scrollY;
            g.setColour (theme::accent.withAlpha (0.35f));
            g.setFont (theme::font (11.0f, true));
            g.drawText ("MASTER  -  volume and master effect automation", 10, y, 400, trackHeight, juce::Justification::centredLeft);
            drawAutomation (g, numTracks);
        }

        // the highlighted area (drag on an empty part of the lanes) and the selection box
        if (owner.ctx.hasRange())
        {
            const float rx0 = owner.beatToX (owner.ctx.rangeStart), rx1 = owner.beatToX (owner.ctx.rangeEnd);
            g.setColour (theme::accent.withAlpha (0.10f));
            g.fillRect (juce::Rectangle<float> (rx0, 0, rx1 - rx0, (float) getHeight()));
            g.setColour (theme::accent.withAlpha (0.5f));
            g.drawVerticalLine ((int) rx0, 0, (float) getHeight());
            g.drawVerticalLine ((int) rx1, 0, (float) getHeight());
        }
        if (! marquee.isEmpty())
        {
            g.setColour (theme::text.withAlpha (0.08f));
            g.fillRect (marquee);
            g.setColour (theme::text.withAlpha (0.6f));
            g.drawRect (marquee, 1.0f);
        }

        if (owner.ctx.engine.isRecording()) drawRecording (g);

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
        auto tp = owner.rowTrack (trackIndex);
        if (tp == nullptr) return;
        auto& t = *tp;
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
        if (hit.track < 0 || hit.lane < 0 || owner.rowTrack (hit.track) == nullptr) return false;
        auto& p = owner.ctx.project;
        auto track = owner.rowTrack (hit.track);
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
        auto dragTrack = owner.rowTrack (dragLaneTrack);
        if (dragTrack == nullptr || dragLane >= (int) dragTrack->automation.size()) return;
        auto& lane = dragTrack->automation[(size_t) dragLane];
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
        const bool selected = owner.ctx.isClipSelected (c.id);
        auto body = t.colour.withMultipliedSaturation (c.locked ? 0.35f : 0.75f).withMultipliedBrightness (0.55f);

        g.setColour (body.withAlpha (0.9f));
        g.fillRoundedRectangle (r, 5.0f);
        auto title = r.removeFromTop (16.0f);
        g.setColour ((c.locked ? t.colour.withMultipliedSaturation (0.45f) : t.colour).withAlpha (0.9f));
        g.fillRoundedRectangle (title.withHeight (title.getHeight() + 5), 5.0f);
        g.setColour (body.withAlpha (0.9f));
        g.fillRect (title.withTop (title.getBottom()).withHeight (5.0f));

        auto titleText = title.reduced (6, 0);
        if (c.locked)
        {
            // padlock
            auto lk = titleText.removeFromRight (12).withSizeKeepingCentre (9, 10);
            g.setColour (juce::Colours::black.withAlpha (0.8f));
            g.fillRoundedRectangle (lk.withTrimmedTop (4), 1.5f);
            juce::Path shackle;
            shackle.addCentredArc (lk.getCentreX(), lk.getY() + 4, 3.0f, 3.2f, 0, -juce::MathConstants<float>::halfPi, juce::MathConstants<float>::halfPi, true);
            g.strokePath (shackle, juce::PathStrokeType (1.4f));
        }
        g.setColour (juce::Colours::black.withAlpha (0.85f));
        g.setFont (theme::font (11.5f, true));
        g.drawText (c.name + (c.pitch.active && c.pitch.hasChanges() ? (c.pitch.bypass ? "  -  pitch (bypassed)" : "  -  pitch edited") : ""),
                    titleText.toNearestInt(), juce::Justification::centredLeft, true);

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
        else if (c.type == Clip::Type::Midi)
            drawNotes (g, c.notes, c.lengthBeats, content, t.colour.brighter (0.5f));
        g.restoreState();

        auto full = r.withTop (title.getY());
        g.setColour (selected ? juce::Colours::white : juce::Colours::black.withAlpha (0.4f));
        g.drawRoundedRectangle (full.reduced (0.5f), 5.0f, selected ? 2.0f : 1.0f);
        if (selected && ! c.locked)
        {
            // trim handles on both sides
            g.setColour (juce::Colours::white.withAlpha (0.75f));
            g.fillRoundedRectangle (full.getX() + 2, full.getCentreY() - 9, 3, 18, 1.5f);
            g.fillRoundedRectangle (full.getRight() - 5, full.getCentreY() - 9, 3, 18, 1.5f);
        }
    }

    void drawNotes (juce::Graphics& g, const std::vector<MidiNote>& notes, double lengthBeats, juce::Rectangle<float> content, juce::Colour col)
    {
        if (notes.empty()) return;
        int lo = 127, hi = 0;
        for (auto& n : notes) { lo = juce::jmin (lo, n.pitch); hi = juce::jmax (hi, n.pitch); }
        lo -= 2; hi += 2;
        const float nh = juce::jmin (6.0f, content.getHeight() / (float) (hi - lo + 1));
        g.setColour (col);
        for (auto& n : notes)
        {
            if (n.start >= lengthBeats || n.start < 0) continue;
            const float x = content.getX() + (float) (n.start * owner.pixelsPerBeat);
            const float w = (float) (juce::jmin (n.length, lengthBeats - n.start) * owner.pixelsPerBeat);
            const float y = content.getBottom() - (float) (n.pitch - lo + 1) / (float) (hi - lo + 1) * content.getHeight();
            g.fillRect (x, y, juce::jmax (1.5f, w - 1), juce::jmax (1.5f, nh - 1));
        }
    }

    // what's being recorded, drawn as it forms
    void drawRecording (juce::Graphics& g)
    {
        AudioEngine::RecordingPreview rec;
        if (! owner.ctx.engine.getRecordingPreview (rec)) return;
        auto& p = owner.ctx.project;
        auto rowOf = [&] (int trackId) { for (int i = 0; i < (int) p.tracks.size(); ++i) if (p.tracks[(size_t) i]->id == trackId) return i; return -1; };
        auto box = [&] (int row, double b0, double b1)
        {
            return juce::Rectangle<float> (owner.beatToX (b0), (float) (owner.trackTop (row) - owner.scrollY + 3),
                                           juce::jmax (4.0f, owner.beatToX (b1) - owner.beatToX (b0)), (float) trackHeight - 7);
        };
        auto frame = [&] (juce::Rectangle<float> r, const juce::String& label)
        {
            g.setColour (theme::record.withAlpha (0.22f));
            g.fillRoundedRectangle (r, 5.0f);
            g.setColour (theme::record.withAlpha (0.9f));
            g.drawRoundedRectangle (r.reduced (0.5f), 5.0f, 1.5f);
            g.setFont (theme::font (11.0f, true));
            g.drawText (label, r.withHeight (16).reduced (6, 0).toNearestInt(), juce::Justification::centredLeft, true);
        };

        if (const int row = rowOf (rec.instrumentTrackId); row >= 0)
        {
            const double bar = std::floor (rec.startBeat / p.beatsPerBar) * p.beatsPerBar;
            auto r = box (row, bar, juce::jmax (rec.nowBeat, bar + 0.25));
            frame (r, "Recording...");
            std::vector<MidiNote> rel = rec.notes;
            for (auto& n : rel) n.start -= bar;
            drawNotes (g, rel, rec.nowBeat - bar + 1.0, r.withTrimmedTop (16).reduced (2, 3), theme::text);
        }
        for (auto& a : rec.audio)
        {
            const int row = rowOf (a.trackId);
            if (row < 0) continue;
            auto r = box (row, rec.startBeat, juce::jmax (rec.nowBeat, rec.startBeat + 0.05));
            frame (r, "Recording...");
            auto content = r.withTrimmedTop (16).reduced (2, 3);
            const float mid = content.getCentreY(), half = content.getHeight() * 0.5f;
            g.setColour (theme::text.withAlpha (0.85f));
            const int n = (int) a.peaks.size();
            for (int px = 0; px < (int) content.getWidth() && n > 0; ++px)
            {
                const int k0 = px * n / juce::jmax (1, (int) content.getWidth());
                const int k1 = juce::jmax (k0 + 1, (px + 1) * n / juce::jmax (1, (int) content.getWidth()));
                float pk = 0;
                for (int k = k0; k < k1 && k < n; ++k) pk = juce::jmax (pk, a.peaks[(size_t) k]);
                pk = juce::jmin (1.0f, pk);
                g.drawVerticalLine ((int) content.getX() + px, mid - pk * half, mid + pk * half + 1);
            }
        }
    }

    enum class Edge { None, Left, Right };
    struct Hit { TrackPtr track; ClipPtr clip; int trackIndex = -1; Edge edge = Edge::None; };

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
                const float grip = juce::jmin (8.0f, b.getWidth() / 4.0f);
                h.edge = pos.x > b.getRight() - grip ? Edge::Right : pos.x < b.getX() + grip ? Edge::Left : Edge::None;
                return h;
            }
        }
        return h;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto h = hitTestClip (e.getPosition());
        if (h.clip == nullptr)          setMouseCursor (juce::MouseCursor::NormalCursor);
        else if (h.clip->locked)        setMouseCursor (juce::MouseCursor::NormalCursor);
        else if (h.edge != Edge::None)  setMouseCursor (juce::MouseCursor::LeftRightResizeCursor);
        else if (e.mods.isAltDown())    setMouseCursor (juce::MouseCursor::CopyingCursor);   // Option-drag makes a copy
        else                            setMouseCursor (juce::MouseCursor::DraggingHandCursor);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragLaneTrack = dragLane = dragPoint = -1;
        mode = Mode::None;
        dragging = false;
        selectOnlyOnUp = false;
        duplicateOnDrag = false;
        origs.clear();
        if (automationMouseDown (e)) return;
        grabKeyboardFocus();

        auto& ctx = owner.ctx;
        auto& p = ctx.project;
        auto h = hitTestClip (e.getPosition());
        downBeat = owner.xToBeat ((float) e.x);
        downRow = h.trackIndex;

        if (h.clip != nullptr)
        {
            if (e.mods.isPopupMenu())
            {
                if (! ctx.isClipSelected (h.clip->id)) ctx.selectClip (h.clip->id);
                owner.repaint();
                showClipMenu (h.clip, downBeat);
                return;
            }
            if (e.mods.isShiftDown() || e.mods.isCommandDown())
            {
                ctx.toggleClipSelection (h.clip->id);
                owner.repaint();
                return;
            }
            if (! ctx.isClipSelected (h.clip->id)) ctx.selectClip (h.clip->id);
            else { selectOnlyOnUp = true; ctx.selectedClipId = h.clip->id; if (ctx.onSelectionChanged) ctx.onSelectionChanged(); }

            anchor = h.clip;
            if (h.clip->locked) { owner.repaint(); return; }
            mode = h.edge == Edge::Left ? Mode::TrimLeft : h.edge == Edge::Right ? Mode::TrimRight : Mode::Move;
            duplicateOnDrag = mode == Mode::Move && e.mods.isAltDown();   // Option (Alt) + drag: drag copies, the originals stay

            auto remember = [&] (ClipPtr c)
            {
                TrackPtr t;
                p.findClip (c->id, &t);
                int index = -1;
                for (int i = 0; i < (int) p.tracks.size(); ++i) if (p.tracks[(size_t) i] == t) index = i;
                origs.push_back ({ c, c->startBeat, c->offsetSeconds, c->lengthSeconds, c->lengthBeats, c->notes, index });
            };
            if (mode == Mode::Move)
            {
                for (int id : ctx.getSelectedClips())
                    if (auto c = p.findClip (id); c != nullptr && ! c->locked) remember (c);
            }
            else remember (h.clip);
        }
        else
        {
            if (e.mods.isPopupMenu())
            {
                if (h.track != nullptr) { ctx.selectTrack (h.track->id); showLaneMenu (h.track, owner.ctx.snapBeat (downBeat, owner.getSnap())); }
                return;
            }
            if (h.track != nullptr) ctx.selectTrack (h.track->id);
            marqueeStartSelection = e.mods.isShiftDown() ? ctx.getSelectedClips() : std::vector<int>();
            if (! e.mods.isShiftDown()) ctx.clearClipSelection();
            ctx.rangeStart = ctx.rangeEnd = -1;
            ctx.engine.setPositionBeats (juce::jmax (0.0, owner.ctx.snapBeat (downBeat, owner.getSnap())));
            mode = Mode::Marquee;
            marquee = {};
        }
        owner.repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragPoint >= 0) { automationMouseDrag (e); return; }
        if (mode == Mode::None || e.mods.isPopupMenu()) return;
        if (! dragging)
        {
            if (e.getDistanceFromDragStart() < 3) return;
            dragging = true;
            selectOnlyOnUp = false;
            if (mode != Mode::Marquee) owner.ctx.project.beginEdit();
            if (duplicateOnDrag) makeDragCopies();
        }

        auto& ctx = owner.ctx;
        auto& p = ctx.project;
        // Command (Ctrl) while dragging: move freely, off the grid
        const double snap = e.mods.isCommandDown() || e.mods.isCtrlDown() ? 0.0 : owner.getSnap();
        const double raw = owner.xToBeat ((float) e.x) - downBeat;

        if (mode == Mode::Marquee)
        {
            marquee = juce::Rectangle<int> (e.getMouseDownPosition(), e.getPosition()).toFloat();
            double b0 = owner.xToBeat (marquee.getX()), b1 = owner.xToBeat (marquee.getRight());
            ctx.rangeStart = juce::jmax (0.0, ctx.snapBeat (b0, snap));
            ctx.rangeEnd = juce::jmax (ctx.rangeStart, ctx.snapBeat (b1, snap));
            std::vector<int> ids = marqueeStartSelection;
            for (int i = 0; i < (int) p.tracks.size(); ++i)
                for (auto& c : p.tracks[(size_t) i]->clips)
                    if (getClipBounds (i, *c).intersects (marquee)) ids.push_back (c->id);
            ctx.setClipSelection (ids);
            owner.repaint();
            return;
        }

        const juce::ScopedLock sl (p.lock);
        const double spb = 60.0 / p.bpm;
        if (mode == Mode::Move)
        {
            const Orig* a = nullptr;
            for (auto& o : origs) if (o.clip == anchor) a = &o;
            if (a == nullptr) return;
            double delta = ctx.snapBeat (a->start + raw, snap) - a->start;
            for (auto& o : origs) delta = juce::jmax (delta, -o.start);   // nothing before the start of the song

            // move between tracks (every clip shifts by the same number of rows, onto tracks of its kind)
            int rowDelta = 0;
            if (const int row = owner.trackIndexAtY (e.y); row >= 0 && row < (int) p.tracks.size() && downRow >= 0)
            {
                rowDelta = row - downRow;
                for (auto& o : origs)
                {
                    const int ti = o.trackIndex + rowDelta;
                    if (ti < 0 || ti >= (int) p.tracks.size() || o.trackIndex < 0
                        || p.tracks[(size_t) ti]->type != p.tracks[(size_t) o.trackIndex]->type) { rowDelta = 0; break; }
                }
            }
            for (auto& o : origs)
            {
                o.clip->startBeat = o.start + delta;
                if (o.trackIndex < 0) continue;
                auto target = p.tracks[(size_t) (o.trackIndex + rowDelta)];
                TrackPtr current;
                p.findClip (o.clip->id, &current);
                if (current != nullptr && target != current)
                {
                    auto& v = current->clips;
                    v.erase (std::remove (v.begin(), v.end(), o.clip), v.end());
                    target->clips.push_back (o.clip);
                    if (o.clip == anchor) ctx.selectedTrackId = target->id;
                }
            }
        }
        else if (! origs.empty())
        {
            auto& o = origs.front();
            auto& c = *o.clip;
            const double origLen = c.type == Clip::Type::Midi ? o.lenBeats : o.lenSec / spb;
            const double minLen = juce::jmax (0.0625, snap > 0 ? juce::jmin (snap, 0.25) : 0.0625);
            if (mode == Mode::TrimRight)
            {
                const double newEnd = ctx.snapBeat (o.start + origLen + raw, snap);
                const double lenBeats = juce::jmax (minLen, newEnd - o.start);
                if (c.type == Clip::Type::Midi) c.lengthBeats = lenBeats;
                else if (c.audio != nullptr)    c.lengthSeconds = juce::jmin (lenBeats * spb, c.audio->getLengthSeconds() - o.offset);
            }
            else   // TrimLeft: the end stays put
            {
                const double end = o.start + origLen;
                double newStart = ctx.snapBeat (o.start + raw, snap);
                newStart = juce::jmin (newStart, end - minLen);
                newStart = juce::jmax (newStart, 0.0);
                if (c.type == Clip::Type::Audio) newStart = juce::jmax (newStart, o.start - o.offset / spb);   // can't go before the file starts
                const double d = newStart - o.start;
                c.startBeat = newStart;
                if (c.type == Clip::Type::Midi)
                {
                    c.lengthBeats = o.lenBeats - d;
                    c.notes = o.notes;
                    for (auto& n : c.notes) n.start -= d;   // the notes stay where they are in the song
                }
                else
                {
                    c.offsetSeconds = o.offset + d * spb;
                    c.lengthSeconds = o.lenSec - d * spb;
                }
            }
        }
        owner.repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragPoint >= 0)
        {
            dragLaneTrack = dragLane = dragPoint = -1;
            owner.ctx.project.changed();
            return;
        }
        if (dragging && mode != Mode::Marquee) owner.ctx.project.changed();
        // a plain click on a clip also puts the playhead there (a click on empty space already does)
        if (! dragging && mode != Mode::Marquee && mode != Mode::None && ! e.mods.isAnyModifierKeyDown())
            owner.ctx.engine.setPositionBeats (juce::jmax (0.0, owner.ctx.snapBeat (downBeat, owner.getSnap())));
        if (! dragging && selectOnlyOnUp && anchor != nullptr) owner.ctx.selectClip (anchor->id);
        if (mode == Mode::Marquee && ! dragging) owner.ctx.rangeStart = owner.ctx.rangeEnd = -1;
        marquee = {};
        dragging = false;
        mode = Mode::None;
        origs.clear();
        anchor = nullptr;
        owner.repaint();
    }

    // Option-drag: copies take the originals' place in the drag (and in the selection); the originals stay put
    void makeDragCopies()
    {
        auto& p = owner.ctx.project;
        std::vector<int> ids;
        for (auto& o : origs)
        {
            if (o.trackIndex < 0 || o.trackIndex >= (int) p.tracks.size()) continue;
            Clip src;
            {
                const juce::ScopedLock sl (p.lock);
                src = *o.clip;
            }
            if (auto copy = p.addClipCopy (p.tracks[(size_t) o.trackIndex]->id, src, o.start))
            {
                if (o.clip == anchor) anchor = copy;
                o.clip = copy;
                ids.push_back (copy->id);
            }
        }
        if (! ids.empty())
        {
            owner.ctx.setClipSelection (ids);
            if (anchor != nullptr) owner.ctx.selectedClipId = anchor->id;
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        // double-click an automation point to delete it
        if (auto hit = owner.rowAt (e.y); hit.track >= 0 && hit.lane >= 0 && owner.rowTrack (hit.track) != nullptr)
        {
            auto& p = owner.ctx.project;
            auto rt = owner.rowTrack (hit.track);
            auto& lane = rt->automation[(size_t) hit.lane];
            if (const int pi = pointAt (*rt, lane, hit.laneTop - owner.scrollY, e.getPosition()); pi >= 0)
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

    void showClipMenu (ClipPtr clip, double atBeat)
    {
        auto& ctx = owner.ctx;
        const auto sel = ctx.getSelectedClips();
        const bool many = sel.size() > 1;
        bool anyUnlocked = false, allLocked = true, joinable = false;
        std::map<int, int> perTrack;
        for (int id : sel)
        {
            TrackPtr t;
            if (auto c = ctx.project.findClip (id, &t))
            {
                anyUnlocked = anyUnlocked || ! c->locked;
                allLocked = allLocked && c->locked;
                if (t != nullptr && ! c->locked) joinable = joinable || ++perTrack[t->id] > 1;
            }
        }
        const auto key = [] (const char* k) { return juce::String (juce::SystemStats::getOperatingSystemType() & juce::SystemStats::MacOSX ? juce::String::fromUTF8 ("\xe2\x8c\x98") : juce::String ("Ctrl+")) + k; };
        juce::Component::SafePointer<TimelineView> tv (&owner);
        const int id = clip->id;
        const double snapped = ctx.snapBeat (atBeat, owner.getSnap());

        juce::PopupMenu m;
        m.addSectionHeader (many ? juce::String ((int) sel.size()) + " clips" : clip->name);
        m.addItem (clip->type == Clip::Type::Midi ? "Edit notes" : "Edit pitch", [tv, id] { if (tv && tv->ctx.openEditor) tv->ctx.openEditor (id); });
        m.addItem (juce::String::fromUTF8 ("\xe2\x96\xb6  Play this clip"), [tv, id] { if (tv) tv->ctx.engine.playClip (id); });
        m.addSeparator();
        juce::PopupMenu::Item cut ("Cut");        cut.shortcutKeyDescription = key ("X"); cut.isEnabled = anyUnlocked; cut.action = [tv] { if (tv) tv->cutSelection(); };
        juce::PopupMenu::Item copy ("Copy");      copy.shortcutKeyDescription = key ("C"); copy.action = [tv] { if (tv) tv->copySelection(); };
        juce::PopupMenu::Item paste ("Paste here"); paste.shortcutKeyDescription = key ("V"); paste.isEnabled = ! ctx.clipboard.empty();
        paste.action = [tv, snapped] { if (tv) tv->pasteAt (snapped); };
        juce::PopupMenu::Item dup ("Duplicate");  dup.shortcutKeyDescription = key ("D"); dup.action = [tv] { if (tv) tv->duplicateSelection(); };
        m.addItem (cut); m.addItem (copy); m.addItem (paste); m.addItem (dup);
        m.addSeparator();
        m.addItem ("Split here", ! clip->locked, false, [tv, id, snapped] { if (tv) { tv->ctx.project.beginEdit(); tv->ctx.project.splitClip (id, snapped); } });
        juce::PopupMenu::Item split ("Split at playhead"); split.shortcutKeyDescription = "S"; split.isEnabled = anyUnlocked;
        split.action = [tv] { if (tv) tv->splitSelection (tv->ctx.engine.getPositionBeats()); };
        m.addItem (split);
        juce::PopupMenu::Item join ("Join into one clip"); join.shortcutKeyDescription = key ("J"); join.isEnabled = joinable;
        join.action = [tv] { if (tv) tv->joinSelection(); };
        m.addItem (join);
        juce::PopupMenu::Item rev ("Reverse"); rev.shortcutKeyDescription = key ("R"); rev.isEnabled = anyUnlocked;
        rev.action = [tv] { if (tv) tv->reverseSelection(); };
        m.addItem (rev);
        juce::PopupMenu::Item lock (allLocked ? "Unlock" : "Lock"); lock.shortcutKeyDescription = key ("L");
        lock.action = [tv] { if (tv) tv->toggleLockSelection(); };
        m.addItem (lock);
        m.addItem ("Rename...", ! many, false, [tv, id] { if (tv) tv->renameClip (id); });
        m.addItem ("Loop this", [tv] { if (tv) tv->loopSelection(); });
        m.addSeparator();
        juce::PopupMenu::Item del ("Delete"); del.shortcutKeyDescription = "Delete"; del.isEnabled = anyUnlocked;
        del.action = [tv] { if (tv) tv->deleteSelection(); };
        m.addItem (del);
        m.showMenuAsync ({});
    }

    void showLaneMenu (TrackPtr track, double beat)
    {
        auto& p = owner.ctx.project;
        juce::Component::SafePointer<TimelineView> tv (&owner);
        juce::PopupMenu m;
        m.addItem ("Paste here", ! owner.ctx.clipboard.empty(), false, [tv, beat] { if (tv) tv->pasteAt (beat); });
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
        m.addItem ("Select all clips on this track", [tv, track]
        {
            if (! tv) return;
            std::vector<int> ids;
            for (auto& c : track->clips) ids.push_back (c->id);
            tv->ctx.setClipSelection (ids);
            tv->repaint();
        });
        m.addItem ("Loop the highlighted area", owner.ctx.hasRange(), false, [tv] { if (tv) tv->loopSelection(); });
        m.showMenuAsync ({});
    }

    juce::Rectangle<float> marquee;

private:
    enum class Mode { None, Move, TrimLeft, TrimRight, Marquee };
    struct Orig { ClipPtr clip; double start, offset, lenSec, lenBeats; std::vector<MidiNote> notes; int trackIndex; };
    TimelineView& owner;
    Mode mode = Mode::None;
    std::vector<Orig> origs;
    ClipPtr anchor;
    std::vector<int> marqueeStartSelection;
    double downBeat = 0;
    int downRow = -1;
    bool dragging = false, selectOnlyOnUp = false, duplicateOnDrag = false;
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
    for (auto* b : { &addAudioButton, &addInstrumentButton, &addVocoderButton, &addSamplerButton, &addMasterButton })
    {
        addAndMakeVisible (*b);
        b->getProperties().set ("compact", true);
    }
    addVocoderButton.setTooltip ("Add a track with the Vocoder, already listening to your vocal track");
    addSamplerButton.setTooltip ("Add a track with the Sampler, ready to play (drop any sound on it)");
    addMasterButton.setTooltip ("Show or hide the master track, for master volume and effect automation");
    addMasterButton.onClick = [this]
    {
        ctx.project.beginEdit();
        ctx.project.showMaster = ! ctx.project.showMaster;
        ctx.project.changed();
    };
    addAudioButton.setTooltip ("Add an audio track for recording or loops");
    addInstrumentButton.setTooltip ("Add a virtual instrument track you play with MIDI");
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

int TimelineView::numRows() const { return (int) ctx.project.tracks.size() + (ctx.project.showMaster ? 1 : 0); }

TrackPtr TimelineView::rowTrack (int row) const
{
    const int n = (int) ctx.project.tracks.size();
    if (row >= 0 && row < n) return ctx.project.tracks[(size_t) row];
    if (row == n && ctx.project.showMaster) return ctx.project.master;
    return nullptr;
}

int TimelineView::trackFullHeight (int i) const
{
    auto t = rowTrack (i);
    if (t == nullptr) return trackHeight;
    int h = trackHeight;
    for (auto& l : t->automation) if (l.visible) h += laneHeight;
    return h;
}

int TimelineView::trackTop (int i) const
{
    int y = 0;
    for (int k = 0; k < i && k < numRows(); ++k) y += trackFullHeight (k);
    return y;
}

int TimelineView::contentHeight() const { return trackTop (numRows()); }

TimelineView::RowHit TimelineView::rowAt (int yInLanes) const
{
    RowHit hit;
    const int y = yInLanes + scrollY;
    if (y < 0) return hit;
    int top = 0;
    const int n = numRows();
    for (int i = 0; i < n; ++i)
    {
        const int h = trackFullHeight (i);
        if (y < top + h)
        {
            hit.track = i;
            if (y >= top + trackHeight)
            {
                int laneTop = top + trackHeight;
                const auto& trackLanes = rowTrack (i)->automation;
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
    hit.track = (int) ctx.project.tracks.size() + (ctx.project.showMaster ? 1 : 0);   // below the last row
    return hit;
}

double TimelineView::getSnap() const
{
    return ctx.snapOn ? getGridStep() : 0.0;
}

double TimelineView::getGridStep() const
{
    if (ctx.gridBeats > 0) return ctx.gridBeats;
    if (pixelsPerBeat >= 120) return 0.25;
    if (pixelsPerBeat >= 60)  return 0.5;
    if (pixelsPerBeat >= 14)  return 1.0;
    return (double) ctx.project.beatsPerBar;
}

void TimelineView::rebuild()
{
    headers.clear();
    for (int i = 0; i < numRows(); ++i)
        headerHolder.addAndMakeVisible (headers.add (new TrackHeader (*this, rowTrack (i))));
    addMasterButton.setButtonText (ctx.project.showMaster ? juce::String::fromUTF8 ("\xe2\x80\x93 Master") : juce::String ("+ Master"));
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
    auto corner = top.removeFromLeft (headerWidth).reduced (4, 3);
    const int widths[] { 58, 86, 70, 68, 62 };
    juce::TextButton* order[] { &addAudioButton, &addInstrumentButton, &addVocoderButton, &addSamplerButton, &addMasterButton };
    const float scale = (float) corner.getWidth() / (float) (58 + 86 + 70 + 68 + 62);
    for (int i = 0; i < 5; ++i)
        order[i]->setBounds (i == 4 ? corner : corner.removeFromLeft (juce::roundToInt ((float) widths[i] * scale)));
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
    juce::ignoreUnused (g);
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

// ---- clip editing ----
namespace
{
    std::vector<int> unlockedOnly (Project& p, const std::vector<int>& ids)
    {
        std::vector<int> v;
        for (int id : ids) if (auto c = p.findClip (id); c != nullptr && ! c->locked) v.push_back (id);
        return v;
    }
}

void TimelineView::deleteSelection()
{
    const auto ids = unlockedOnly (ctx.project, ctx.getSelectedClips());
    if (ids.empty()) return;
    ctx.project.beginEdit();
    for (int id : ids) ctx.project.removeClip (id);
    ctx.clearClipSelection();
}

void TimelineView::duplicateSelection()
{
    const auto ids = ctx.getSelectedClips();
    if (ids.empty()) return;
    ctx.project.beginEdit();
    // the copies go right after the whole selection, keeping their spacing
    const auto cb = clipops::copy (ctx.project, ids);
    double first = 1.0e9;
    for (int id : ids) if (auto c = ctx.project.findClip (id)) first = juce::jmin (first, c->startBeat);
    ctx.setClipSelection (clipops::paste (ctx.project, cb, first + cb.span, -1));
}

void TimelineView::splitSelection (double beat)
{
    auto ids = unlockedOnly (ctx.project, ctx.getSelectedClips());
    if (ids.empty())   // nothing selected: split whatever is under the playhead on the selected track
        if (auto t = ctx.project.findTrack (ctx.selectedTrackId))
            for (auto& c : t->clips)
                if (! c->locked && beat > c->startBeat && beat < c->getEndBeat (ctx.project.bpm)) ids.push_back (c->id);
    if (ids.empty()) return;
    ctx.project.beginEdit();
    std::vector<int> keep = ids;
    for (int id : ids)
        if (auto right = ctx.project.splitClip (id, beat)) keep.push_back (right->id);
    ctx.setClipSelection (keep);
}

void TimelineView::copySelection()
{
    const auto ids = ctx.getSelectedClips();
    if (! ids.empty()) ctx.clipboard = clipops::copy (ctx.project, ids);
}

void TimelineView::cutSelection()
{
    const auto ids = unlockedOnly (ctx.project, ctx.getSelectedClips());
    if (ids.empty()) return;
    ctx.clipboard = clipops::copy (ctx.project, ids);
    ctx.project.beginEdit();
    for (int id : ids) ctx.project.removeClip (id);
    ctx.clearClipSelection();
}

void TimelineView::pasteAt (double beat)
{
    if (ctx.clipboard.empty()) return;
    ctx.project.beginEdit();
    ctx.setClipSelection (clipops::paste (ctx.project, ctx.clipboard, juce::jmax (0.0, beat), ctx.selectedTrackId));
}

void TimelineView::joinSelection()
{
    const auto ids = ctx.getSelectedClips();
    if (ids.size() < 2) return;
    ctx.project.beginEdit();
    const auto made = clipops::join (ctx.project, ids);
    if (! made.empty()) ctx.setClipSelection (made);
}

void TimelineView::reverseSelection()
{
    const auto ids = ctx.getSelectedClips();
    if (ids.empty()) return;
    ctx.project.beginEdit();
    clipops::reverse (ctx.project, ids);
}

void TimelineView::toggleLockSelection()
{
    const auto ids = ctx.getSelectedClips();
    if (ids.empty()) return;
    bool allLocked = true;
    for (int id : ids) if (auto c = ctx.project.findClip (id)) allLocked = allLocked && c->locked;
    ctx.project.beginEdit();
    {
        const juce::ScopedLock sl (ctx.project.lock);
        for (int id : ids) if (auto c = ctx.project.findClip (id)) c->locked = ! allLocked;
    }
    ctx.project.changed();
}

void TimelineView::selectAll()
{
    std::vector<int> ids;
    for (auto& t : ctx.project.tracks) for (auto& c : t->clips) ids.push_back (c->id);
    ctx.setClipSelection (ids);
    repaint();
}

void TimelineView::nudgeSelection (int direction)
{
    const auto ids = unlockedOnly (ctx.project, ctx.getSelectedClips());
    if (ids.empty()) return;
    const double step = getGridStep() * direction;
    for (int id : ids) if (auto c = ctx.project.findClip (id); c != nullptr && c->startBeat + step < -1.0e-9) return;
    ctx.project.beginEdit();
    {
        const juce::ScopedLock sl (ctx.project.lock);
        for (int id : ids) if (auto c = ctx.project.findClip (id)) c->startBeat = juce::jmax (0.0, c->startBeat + step);
    }
    ctx.project.changed();
}

void TimelineView::loopSelection()
{
    double a = -1, b = -1;
    if (ctx.hasRange()) { a = ctx.rangeStart; b = ctx.rangeEnd; }
    else
        for (int id : ctx.getSelectedClips())
            if (auto c = ctx.project.findClip (id))
            {
                a = a < 0 ? c->startBeat : juce::jmin (a, c->startBeat);
                b = juce::jmax (b, c->getEndBeat (ctx.project.bpm));
            }
    if (a < 0 || b <= a) return;
    ctx.project.beginEdit();
    {
        const juce::ScopedLock sl (ctx.project.lock);
        ctx.project.loopStart = a;
        ctx.project.loopEnd = b;
        ctx.project.loopEnabled = true;
    }
    ctx.engine.setPositionBeats (a);
    ctx.project.changed();
}

void TimelineView::renameClip (int clipId)
{
    auto c = ctx.project.findClip (clipId);
    if (c == nullptr) return;
    renameWindow = std::make_unique<juce::AlertWindow> ("Rename clip", "", juce::MessageBoxIconType::NoIcon, this);
    renameWindow->addTextEditor ("name", c->name);
    renameWindow->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    renameWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    renameWindow->enterModalState (true, juce::ModalCallbackFunction::create ([this, clipId] (int result)
    {
        const auto name = renameWindow->getTextEditorContents ("name").trim();
        renameWindow.reset();
        if (result != 1 || name.isEmpty()) return;
        if (auto clip = ctx.project.findClip (clipId))
        {
            ctx.project.beginEdit();
            { const juce::ScopedLock sl (ctx.project.lock); clip->name = name; }
            ctx.project.changed();
        }
    }), false);
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

void TimelineView::mouseMagnify (const juce::MouseEvent& e, float scaleFactor)
{
    // pinch on the trackpad: zoom in / out around the fingers
    zoom (scaleFactor, juce::jmax (0, e.getEventRelativeTo (lanes.get()).x));
}

void TimelineView::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
{
    // Option or Cmd + scroll zooms (as in Adobe's apps); smooth for trackpads, stepped for wheels
    if (e.mods.isAltDown() || e.mods.isCommandDown() || e.mods.isCtrlDown())
    {
        const float d = std::abs (w.deltaY) > std::abs (w.deltaX) ? w.deltaY : w.deltaX;
        zoom (std::exp (juce::jlimit (-0.5f, 0.5f, d * (w.isSmooth ? 2.0f : 1.2f))), juce::jmax (0, e.getEventRelativeTo (lanes.get()).x));
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
