#pragma once
#include "../AudioEngine.h"
#include "Theme.h"

namespace rs
{

// Shared state the views need: the project, the engine and the current selection.
struct AppContext
{
    AppContext (Project& p, AudioEngine& e) : project (p), engine (e) {}

    Project& project;
    AudioEngine& engine;

    int selectedTrackId = -1;
    bool showTime = false;   // clock and ruler: minutes:seconds instead of bars and beats
    int selectedClipId = -1;

    std::function<void()> onSelectionChanged;      // views listen to this
    std::function<void (int clipId)> openEditor;   // double-click a MIDI clip
    std::function<void (int trackId)> renameTrack;
    std::function<void (int trackId, FxPtr slot, bool instrument)> openDevice;   // a device's editor window
    std::function<void (int trackId)> showInMixer;     // jump to a track's channel strip
    std::function<void()> openPluginManager;
    std::function<void (const juce::File&)> openProjectFile;   // .renasound or another DAW's project

    // "Soft Pad", or the plugin instrument's name
    static juce::String instrumentName (const Track& t)
    {
        if (t.instrument != nullptr) return t.instrument->getName();
        if (t.synth == nullptr) return {};
        const auto& presets = getSynthPresets();
        return presets[(size_t) juce::jlimit (0, (int) presets.size() - 1, t.synth->presetIndex.load())].name;
    }

    void selectTrack (int id)
    {
        selectedTrackId = id;
        if (auto t = project.findTrack (id); t != nullptr && t->type == Track::Type::Instrument)
            engine.liveInputTrackId = id;
        if (onSelectionChanged) onSelectionChanged();
    }

    void selectClip (int clipId)
    {
        selectedClipId = clipId;
        TrackPtr owner;
        if (project.findClip (clipId, &owner) != nullptr && owner != nullptr)
            selectTrack (owner->id);
        else if (onSelectionChanged)
            onSelectionChanged();
    }

    double snapBeat (double beat, double grid) const { return grid <= 0 ? beat : std::round (beat / grid) * grid; }
};

// Small vector-icon button used by the transport bar.
class IconButton : public juce::Button
{
public:
    enum class Icon { Play, Pause, Stop, Record, ToStart, Loop, Metronome, Plus, Piano };

    IconButton (const juce::String& name, Icon i) : juce::Button (name), icon (i) {}

    void setIcon (Icon i) { icon = i; repaint(); }
    juce::Colour onColour = theme::accent;

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const bool on = getToggleState();
        // borderless: only a wash on hover, a tint when on
        if (on || down || over)
        {
            g.setColour (on ? onColour.withAlpha (0.2f) : theme::text.withAlpha (down ? 0.1f : 0.055f));
            g.fillRoundedRectangle (r, 6.0f);
        }

        auto c = on ? onColour : theme::text;
        if (icon == Icon::Record) c = on ? juce::Colours::white : theme::record;
        if (on && icon == Icon::Record) { g.setColour (theme::record); g.fillRoundedRectangle (r, 6.0f); }
        g.setColour (c);

        auto ir = r.withSizeKeepingCentre (r.getHeight() * 0.42f, r.getHeight() * 0.42f);
        juce::Path p;
        switch (icon)
        {
            case Icon::Play:   p.addTriangle (ir.getX(), ir.getY(), ir.getX(), ir.getBottom(), ir.getRight(), ir.getCentreY()); break;
            case Icon::Pause:  p.addRectangle (ir.getX(), ir.getY(), ir.getWidth() * 0.35f, ir.getHeight());
                               p.addRectangle (ir.getRight() - ir.getWidth() * 0.35f, ir.getY(), ir.getWidth() * 0.35f, ir.getHeight()); break;
            case Icon::Stop:   p.addRectangle (ir.reduced (1)); break;
            case Icon::Record: p.addEllipse (ir); break;
            case Icon::ToStart:
                p.addRectangle (ir.getX(), ir.getY(), 2.5f, ir.getHeight());
                p.addTriangle (ir.getRight(), ir.getY(), ir.getRight(), ir.getBottom(), ir.getX() + 3, ir.getCentreY()); break;
            case Icon::Plus:
                p.addRectangle (ir.getCentreX() - 1.25f, ir.getY(), 2.5f, ir.getHeight());
                p.addRectangle (ir.getX(), ir.getCentreY() - 1.25f, ir.getWidth(), 2.5f); break;
            case Icon::Loop:
            {
                juce::Path a;
                a.addRoundedRectangle (ir.expanded (1, -1), 4.0f);
                g.strokePath (a, juce::PathStrokeType (2.0f));
                p.addTriangle (ir.getCentreX() - 1, ir.getY() - 4, ir.getCentreX() - 1, ir.getY() + 2, ir.getCentreX() + 4, ir.getY() - 1);
                break;
            }
            case Icon::Piano:
            {
                auto k = r.withSizeKeepingCentre (r.getHeight() * 0.62f, r.getHeight() * 0.46f);
                g.drawRoundedRectangle (k, 2.0f, 1.6f);
                const float w = k.getWidth() / 5.0f;   // C D E F G
                for (int i = 1; i < 5; ++i) g.drawVerticalLine ((int) (k.getX() + w * i), k.getCentreY(), k.getBottom());
                for (int i : { 1, 2, 4 }) g.fillRect (k.getX() + w * i - w * 0.3f, k.getY(), w * 0.6f, k.getHeight() * 0.58f);
                return;
            }
            case Icon::Metronome:
                p.addTriangle (ir.getCentreX(), ir.getY() - 1, ir.getX(), ir.getBottom(), ir.getRight(), ir.getBottom());
                g.fillPath (p);
                g.setColour (theme::panel);
                g.drawLine (ir.getCentreX(), ir.getBottom() - 2, ir.getRight() + 1, ir.getY(), 2.0f);
                g.setColour (c);
                g.drawLine (ir.getCentreX(), ir.getBottom() - 2, ir.getRight() + 1, ir.getY(), 1.5f);
                return;
        }
        g.fillPath (p);
    }

private:
    Icon icon;
};

} // namespace rs
