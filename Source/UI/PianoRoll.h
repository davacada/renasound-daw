#pragma once
#include "AppContext.h"

namespace rs
{

// =============================================================================
//  Piano roll editor for the selected MIDI clip
// =============================================================================
enum class Tool { Select, Edit, Draw };

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

    Tool tool = Tool::Select;
    void setTool (Tool);

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

    // Select / Edit / Draw, each an icon with its name
    struct ToolButton : juce::Button
    {
        ToolButton (const juce::String& n, Tool t) : juce::Button (n), which (t) { setClickingTogglesState (false); }
        Tool which;
        void paintButton (juce::Graphics&, bool over, bool down) override;
    };
    ToolButton selectTool { "Select", Tool::Select }, editTool { "Edit", Tool::Edit }, drawTool { "Draw", Tool::Draw };
    juce::Label title;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    void zoomAround (double factor, float xInGrid);
    void timerCallback() override;
    void updateScrollBars();
    void refreshHeader();
};

} // namespace rs
