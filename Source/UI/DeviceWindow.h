#pragma once
#include "AppContext.h"

namespace rs
{

// =============================================================================
//  A floating window showing one device's editor: a built-in effect, a VST3 /
//  AU plugin, or a track's instrument. Effect chains themselves live right on
//  the mixer's channel strips.
// =============================================================================
class DeviceWindow : public juce::DocumentWindow,
                     public juce::ChangeListener,
                     private juce::Timer
{
public:
    // slot == nullptr with instrument == true shows the built-in synth's sounds
    DeviceWindow (AppContext&, int trackId, FxPtr slot, bool instrument);
    ~DeviceWindow() override;

    void closeButtonPressed() override { if (onClose) onClose(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;   // floats above RENASOUND while RENASOUND is the app in front

    bool shows (int t, const FxPtr& s, bool instrument) const { return t == trackId && s == slot && instrument == isInstrument; }

    std::function<void()> onClose;
    const int trackId;
    const FxPtr slot;
    const bool isInstrument;

private:
    class Content;
    AppContext& ctx;
    bool stillExists() const;
    void updateTitle();
};

// Lets the user scan for and manage installed plugins.
class PluginManagerWindow : public juce::DocumentWindow
{
public:
    explicit PluginManagerWindow (AppContext&);
    void closeButtonPressed() override;
    std::function<void()> onClose;

private:
    AppContext& ctx;
};

// Builds the "add effect" / "choose instrument" menus and loads the choice.
namespace devicemenus
{
    void showAddEffectMenu (AppContext&, int trackId, juce::Component* target, std::function<void (FxPtr)> added = {});
    void showInstrumentMenu (AppContext&, int trackId, juce::Component* target);
    // gives the track RENASYNTH (if it hasn't got it) with one of its factory sounds
    void chooseSynthSound (AppContext&, int trackId, int patchIndex);
}

} // namespace rs
