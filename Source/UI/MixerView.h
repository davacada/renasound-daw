#pragma once
#include "AppContext.h"

namespace rs
{

// =============================================================================
//  Mixer: one channel strip per track plus the master strip. Each strip shows
//  its instrument and its effect inserts directly, like a hardware console.
// =============================================================================
class MixerView : public juce::Component,
                  public juce::ChangeListener,
                  private juce::Timer
{
public:
    explicit MixerView (AppContext&);
    ~MixerView() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void scrollToTrack (int trackId);   // -1 = master (always visible)

    static constexpr int stripWidth = 124;

private:
    class Strip;
    AppContext& ctx;
    juce::Viewport viewport;
    juce::Component stripHolder;
    juce::OwnedArray<Strip> strips;
    std::unique_ptr<Strip> masterStrip;
    int flashTrackId = -1, flashCountdown = 0;

    void rebuild();
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void timerCallback() override;
};

} // namespace rs
