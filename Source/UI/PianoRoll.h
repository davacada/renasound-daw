#pragma once
#include "AppContext.h"

namespace rs
{

// =============================================================================
//  Piano roll editor for the selected MIDI clip
// =============================================================================
class PianoRoll : public juce::Component,
                  public juce::ChangeListener,
                  public juce::ScrollBar::Listener,
                  private juce::Timer
{
public:
    explicit PianoRoll (AppContext&);
    ~PianoRoll() override;

    void showClip (int clipId, bool resetView = true);
    void resized() override;
    void paint (juce::Graphics&) override;

    // geometry shared with the inner grid
    double pixelsPerBeat = 64.0;
    int noteHeight = 13;
    double scrollX = 0;
    int scrollY = 0;
    double snap = 0.25;
    static constexpr int keysWidth = 76, toolbarHeight = 36;

    ClipPtr clip;
    TrackPtr track;
    AppContext& ctx;

private:
    class Keys;
    class Grid;
    std::unique_ptr<Keys> keys;
    std::unique_ptr<Grid> grid;
    juce::ScrollBar hScroll { false }, vScroll { true };
    juce::ComboBox preset, snapBox;
    juce::TextButton quantize { "Quantize" }, clearNotes { "Clear" };
    juce::Label title;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void timerCallback() override;
    void updateScrollBars();
    void refreshHeader();
};

} // namespace rs
