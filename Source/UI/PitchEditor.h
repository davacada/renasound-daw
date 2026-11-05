#pragma once
#include "AppContext.h"
#include "../Pitch.h"

namespace rs
{

// =============================================================================
//  Melodyne-style note editor for an audio clip: every sung note is a blob you
//  can drag to a new pitch; the macros correct pitch to a scale and flatten
//  drift. Rendering happens in the background (PitchService).
// =============================================================================
class PitchEditor : public juce::Component,
                    public juce::ChangeListener,
                    public juce::ScrollBar::Listener,
                    private juce::Timer
{
public:
    PitchEditor (AppContext&, PitchService&);
    ~PitchEditor() override;

    void showClip (int clipId);
    int getClipId() const { return clip != nullptr ? clip->id : -1; }

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;
    void zoomAround (double factor, float xInNotes);
    bool keyPressed (const juce::KeyPress&) override;

    // geometry shared with the inner views
    double pxPerSec = 120.0;
    double scrollX = 0;     // px from the clip's first visible sample
    int scrollY = 0;
    int noteH = 16;
    static constexpr int keysWidth = 56, toolbarH = 40, waveH = 44;

    double viewStartSec() const { return clip != nullptr ? clip->offsetSeconds : 0.0; }
    float secToX (double sec) const  { return (float) ((sec - viewStartSec()) * pxPerSec - scrollX); }
    double xToSec (float x) const    { return viewStartSec() + (x + scrollX) / pxPerSec; }
    float midiToY (float m) const    { return (127.5f - m) * (float) noteH - (float) scrollY; }
    float yToMidi (float y) const    { return 127.5f - (y + (float) scrollY) / (float) noteH; }

    AppContext& ctx;
    PitchService& service;
    ClipPtr clip;
    TrackPtr track;
    PitchAnalysisPtr analysis;
    std::set<int> selected;

    void edited (bool finished);   // after any change to the notes
    std::vector<int> targetNotes() const;   // the selection, or every note

private:
    class Keys;
    class NoteView;
    std::unique_ptr<Keys> keys;
    std::unique_ptr<NoteView> notesView;
    juce::ScrollBar hScroll { false }, vScroll { true };
    juce::Label title, status;
    juce::ComboBox keyBox, scaleBox;
    juce::Slider correct, drift;
    juce::Label correctLabel, driftLabel;
    juce::TextButton snapButton { "Snap to scale" }, resetButton { "Reset" }, abButton { "A/B" };
    bool sliderEditOpen = false;

    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void timerCallback() override;
    void updateScrollBars();
    void refreshControls();
    void initialiseNotesIfReady();
    void applyCorrection (float amount);
    void applyDrift (float amount);
};

} // namespace rs
