#include "MixerView.h"
#include "DeviceWindow.h"
#include "InsertList.h"

namespace rs
{

// =============================================================================
class MixerView::Strip : public juce::Component
{
public:
    // track == nullptr means the master strip
    Strip (AppContext& c, TrackPtr t) : ctx (c), track (std::move (t)), inserts (c, track != nullptr ? track->id : -1)
    {
        addAndMakeVisible (fader);
        fader.setSliderStyle (juce::Slider::LinearVertical);
        fader.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 18);
        fader.setRange (0.0, 1.5);
        fader.setDoubleClickReturnValue (true, track ? 0.8 : 0.9);
        fader.textFromValueFunction = [] (double v) { return v <= 0.0001 ? juce::String ("-inf") : juce::String (juce::Decibels::gainToDecibels (v), 1) + " dB"; };
        fader.valueFromTextFunction = [] (const juce::String& s) { return juce::Decibels::decibelsToGain (s.getDoubleValue()); };
        fader.onValueChange = [this]
        {
            const juce::ScopedLock sl (ctx.project.lock);
            if (track) track->volume = (float) fader.getValue();
            else       ctx.project.masterVolume = (float) fader.getValue();
        };

        addAndMakeVisible (insertView);
        insertView.setViewedComponent (&inserts, false);
        insertView.setScrollBarsShown (true, false);
        insertView.setScrollBarThickness (6);

        if (track != nullptr)
        {
            addAndMakeVisible (pan);
            pan.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            pan.setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
            pan.setRange (-1.0, 1.0);
            pan.setDoubleClickReturnValue (true, 0.0);
            pan.setTooltip ("Pan (double-click to centre)");
            pan.onValueChange = [this] { const juce::ScopedLock sl (ctx.project.lock); track->pan = (float) pan.getValue(); };

            for (auto* b : { &mute, &solo })
            {
                addAndMakeVisible (b);
                b->setClickingTogglesState (true);
            }
            mute.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffffb020));
            solo.setColour (juce::TextButton::buttonOnColourId, theme::accent2);
            mute.onClick = [this] { const juce::ScopedLock sl (ctx.project.lock); track->mute = mute.getToggleState(); };
            solo.onClick = [this] { const juce::ScopedLock sl (ctx.project.lock); track->solo = solo.getToggleState(); };
        }
        sync();
    }

    void refresh()
    {
        inserts.refresh();
        resized();
        repaint();
    }

    void sync()
    {
        // automation drives the fader and pan knob while it plays
        const float av = track ? track->autoVolume.load() : -1.0f;
        if (! fader.isMouseButtonDown())
            fader.setValue (track ? (av >= 0 ? av : track->volume) : ctx.project.masterVolume, juce::dontSendNotification);
        if (track == nullptr) return;
        const float ap = track->autoPan.load();
        if (! pan.isMouseButtonDown()) pan.setValue (ap >= -1.0f ? ap : track->pan, juce::dontSendNotification);
        mute.setToggleState (track->mute, juce::dontSendNotification);
        solo.setToggleState (track->solo, juce::dontSendNotification);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (6);
        r.removeFromTop (26);                       // colour bar + name
        instrumentRow = r.removeFromTop (24);        // instrument (or input label)
        r.removeFromTop (6);
        insertsLabel = r.removeFromTop (14);

        const int below = (track != nullptr ? 40 : 0) + 72;    // pan row + a usable fader
        const int insertsH = juce::jlimit (InsertList::defaultRowH * 2, juce::jmax (InsertList::defaultRowH * 2, r.getHeight() - below),
                                           inserts.getIdealHeight());
        insertView.setBounds (r.removeFromTop (insertsH));
        inserts.setSize (insertView.getWidth() - (inserts.getIdealHeight() > insertsH ? 7 : 0), inserts.getIdealHeight());
        r.removeFromTop (6);

        if (track != nullptr)
        {
            auto row = r.removeFromTop (34);
            pan.setBounds (row.removeFromLeft (34));
            row.removeFromLeft (6);
            auto ms = row.withSizeKeepingCentre (row.getWidth(), 22);
            mute.setBounds (ms.removeFromLeft (ms.getWidth() / 2).reduced (1, 0));
            solo.setBounds (ms.reduced (1, 0));
            r.removeFromTop (4);
        }
        meterArea = r.removeFromRight (14).withTrimmedBottom (22);
        fader.setBounds (r);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().reduced (2);
        const bool sel = track != nullptr && ctx.selectedTrackId == track->id;
        g.setColour (sel ? theme::panelHi.brighter (0.04f) : theme::panelHi);
        g.fillRoundedRectangle (r.toFloat(), 6.0f);
        g.setColour (track ? track->colour : theme::accent);
        g.fillRoundedRectangle (r.removeFromTop (4).toFloat(), 2.0f);
        if (flash)
        {
            g.setColour (theme::accent);
            g.drawRoundedRectangle (getLocalBounds().reduced (2).toFloat(), 6.0f, 2.0f);
        }

        g.setColour (theme::text);
        g.setFont (theme::font (13.0f, true));
        g.drawText (track ? track->name : juce::String ("MASTER"), getLocalBounds().reduced (8, 0).withTop (8).withHeight (20),
                    juce::Justification::centred, true);

        // instrument slot (instrument tracks) or a quiet label
        auto ir = instrumentRow.toFloat();
        if (track != nullptr && track->type == Track::Type::Instrument)
        {
            const bool hover = instrumentHover;
            g.setColour (hover ? theme::accent.withAlpha (0.28f) : theme::accent.withAlpha (0.16f));
            g.fillRoundedRectangle (ir, 4.0f);
            g.setColour (theme::text);
            g.setFont (theme::font (12.0f, true));
            g.drawText (AppContext::instrumentName (*track), ir.withTrimmedLeft (8).withTrimmedRight (20), juce::Justification::centredLeft, true);
            juce::Path arrow;
            auto a = ir.removeFromRight (18).withSizeKeepingCentre (8, 5);
            arrow.addTriangle (a.getX(), a.getY(), a.getRight(), a.getY(), a.getCentreX(), a.getBottom());
            g.setColour (theme::textDim);
            g.fillPath (arrow);
        }
        else
        {
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f, true));
            g.drawText (track != nullptr ? "AUDIO" : "STEREO OUT", ir, juce::Justification::centred);
        }

        g.setColour (theme::textDim);
        g.setFont (theme::font (10.0f, true));
        g.drawText ("EFFECTS", insertsLabel, juce::Justification::centredLeft);

        // stereo meter
        const float l = track ? track->meterL.load() : ctx.project.masterMeterL.load();
        const float rr = track ? track->meterR.load() : ctx.project.masterMeterR.load();
        auto m = meterArea.toFloat();
        for (int ch = 0; ch < 2; ++ch)
        {
            auto bar = m.removeFromLeft (m.getWidth() / (2 - ch)).reduced (1, 0);
            g.setColour (theme::bg);
            g.fillRect (bar);
            const float lv = juce::jlimit (0.0f, 1.0f, ch == 0 ? l : rr);
            const float db = juce::Decibels::gainToDecibels (lv, -60.0f);
            const float h = bar.getHeight() * juce::jmap (db, -60.0f, 0.0f, 0.0f, 1.0f);
            juce::ColourGradient grad (theme::accent2, 0, bar.getBottom(), theme::record, 0, bar.getY(), false);
            grad.addColour (0.75, juce::Colour (0xffffc247));
            g.setGradientFill (grad);
            g.fillRect (bar.withTop (bar.getBottom() - h));
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const bool h = isInstrumentTrack() && instrumentRow.contains (e.getPosition());
        if (h != instrumentHover) { instrumentHover = h; repaint (instrumentRow); }
        setMouseCursor (h ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override { instrumentHover = false; repaint (instrumentRow); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (track != nullptr) ctx.selectTrack (track->id);
        if (isInstrumentTrack() && instrumentRow.contains (e.getPosition()))
        {
            // the arrow (or right-click) changes the instrument; the name opens it
            if (e.mods.isPopupMenu() || e.x > instrumentRow.getRight() - 22)
                devicemenus::showInstrumentMenu (ctx, track->id, this);
            else if (ctx.openDevice)
                ctx.openDevice (track->id, track->instrument, true);
        }
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (track != nullptr && e.y < 30 && ctx.renameTrack) ctx.renameTrack (track->id);
    }

    bool isInstrumentTrack() const { return track != nullptr && track->type == Track::Type::Instrument; }

    AppContext& ctx;
    TrackPtr track;
    juce::Slider fader, pan;
    juce::TextButton mute { "M" }, solo { "S" };
    juce::Viewport insertView;
    InsertList inserts;
    juce::Rectangle<int> meterArea, instrumentRow, insertsLabel;
    bool instrumentHover = false, flash = false;
};

// =============================================================================
MixerView::MixerView (AppContext& c) : ctx (c)
{
    addAndMakeVisible (viewport);
    viewport.setViewedComponent (&stripHolder, false);
    viewport.setScrollBarsShown (false, true);
    masterStrip = std::make_unique<Strip> (ctx, nullptr);
    addAndMakeVisible (*masterStrip);
    ctx.project.addChangeListener (this);
    rebuild();
    startTimerHz (30);
}

MixerView::~MixerView()
{
    ctx.project.removeChangeListener (this);
}

void MixerView::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // only rebuild the strips when the track list itself changed
    bool same = strips.size() == (int) ctx.project.tracks.size();
    for (int i = 0; same && i < strips.size(); ++i)
        same = strips[i]->track == ctx.project.tracks[(size_t) i];

    if (! same) rebuild();
    else
    {
        for (auto* s : strips) s->refresh();
        masterStrip->refresh();
    }
}

void MixerView::rebuild()
{
    strips.clear();
    for (auto& t : ctx.project.tracks)
        stripHolder.addAndMakeVisible (strips.add (new Strip (ctx, t)));
    masterStrip->refresh();
    resized();
}

void MixerView::scrollToTrack (int trackId)
{
    flashTrackId = trackId;
    flashCountdown = 30;   // about a second at 30 Hz
    for (int i = 0; i < strips.size(); ++i)
        if (strips[i]->track->id == trackId)
        {
            const int x = i * stripWidth;
            const int viewX = viewport.getViewPositionX();
            if (x < viewX || x + stripWidth > viewX + viewport.getViewWidth())
                viewport.setViewPosition (juce::jmax (0, x - stripWidth), 0);
        }
}

void MixerView::resized()
{
    auto r = getLocalBounds().reduced (6);
    masterStrip->setBounds (r.removeFromRight (stripWidth + 6));
    r.removeFromRight (8);
    viewport.setBounds (r);

    const int h = r.getHeight() - (strips.size() * stripWidth > r.getWidth() ? viewport.getScrollBarThickness() : 0);
    stripHolder.setSize (juce::jmax (r.getWidth(), strips.size() * stripWidth), h);
    for (int i = 0; i < strips.size(); ++i)
        strips[i]->setBounds (i * stripWidth, 0, stripWidth - 4, h);
}

void MixerView::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    if (strips.isEmpty())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f));
        g.drawText ("Your tracks' channel strips appear here", viewport.getBounds(), juce::Justification::centred);
    }
}

void MixerView::timerCallback()
{
    if (! isShowing()) return;
    if (flashCountdown > 0) --flashCountdown;
    for (auto* s : strips)
    {
        s->flash = flashCountdown > 0 && (flashCountdown / 5) % 2 == 0 && s->track->id == flashTrackId;
        s->sync();
        s->repaint();
    }
    masterStrip->flash = flashCountdown > 0 && (flashCountdown / 5) % 2 == 0 && flashTrackId < 0;
    masterStrip->sync();
    masterStrip->repaint();
}

} // namespace rs
