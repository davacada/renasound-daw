#pragma once
#include "AppContext.h"

namespace rs
{

// =============================================================================
//  The arrangement: ruler, track headers and clip lanes (Mixcraft's main view)
// =============================================================================
class TimelineView : public juce::Component,
                     public juce::ChangeListener,
                     public juce::ScrollBar::Listener,
                     public juce::FileDragAndDropTarget,
                     public juce::DragAndDropTarget,
                     private juce::Timer
{
public:
    explicit TimelineView (AppContext&);
    ~TimelineView() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void mouseMagnify (const juce::MouseEvent&, float scaleFactor) override;   // trackpad pinch

    // the "+ Audio" / "+ Instrument" buttons above the track headers
    juce::TextButton addAudioButton { "+ Audio" }, addInstrumentButton { "+ Instrument" },
                     addVocoderButton { "+ Vocoder" }, addSamplerButton { "+ Sampler" }, addMasterButton { "+ Master" };

    void rebuild();
    void zoom (float factor, int aroundX = -1);
    void zoomToFit();   // show the whole song
    double getSnap() const;       // 0 when snap is off
    double getGridStep() const;   // the grid, whether or not snapping is on

    // clip editing, shared by the right-click menus and the keyboard commands
    bool hasClipSelection() const { return ! ctx.getSelectedClips().empty(); }
    void deleteSelection();
    void duplicateSelection();
    void splitSelection (double beat);
    void cutSelection();
    void copySelection();
    void pasteAt (double beat);
    void joinSelection();
    void reverseSelection();
    void toggleLockSelection();
    void selectAll();
    void nudgeSelection (int direction);   // by one grid step
    void loopSelection();                  // loop the highlighted area, or the selected clips
    void renameClip (int clipId);

    // geometry
    static constexpr int headerWidth = 340, rulerHeight = 30, trackHeight = 78, laneHeight = 56;
    double pixelsPerBeat = 28.0;
    double scrollX = 0;   // pixels
    int scrollY = 0;      // pixels

    float beatToX (double beat) const      { return (float) (beat * pixelsPerBeat - scrollX); }   // inside lanes area
    double xToBeat (float x) const         { return (x + scrollX) / pixelsPerBeat; }
    // rows: every track, then the master track when it's shown (Project::showMaster)
    int numRows() const;
    TrackPtr rowTrack (int row) const;
    // each row: the track's clip row, followed by its visible automation lanes
    int trackTop (int trackIndex) const;            // in content pixels (before scrolling)
    int trackFullHeight (int trackIndex) const;
    int contentHeight() const;
    struct RowHit { int track = -1; int lane = -1; int laneTop = 0; };   // lane = index into Track::automation, -1 = clip row
    RowHit rowAt (int yInLanes) const;
    int trackIndexAtY (int yInLanes) const { return rowAt (yInLanes).track; }

    // drag & drop
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray&, int x, int y) override;
    bool isInterestedInDragSource (const SourceDetails&) override { return true; }
    void itemDropped (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override { dropIndicatorBeat = -1; repaint(); }

    AppContext& ctx;

private:
    class Ruler;
    class Lanes;
    class TrackHeader;
    class LaneHeader;

    std::unique_ptr<Ruler> ruler;
    std::unique_ptr<Lanes> lanes;
    juce::OwnedArray<TrackHeader> headers;
    juce::Component headerHolder;
    juce::ScrollBar hScroll { false }, vScroll { true };
    std::unique_ptr<juce::AlertWindow> renameWindow;
    double dropIndicatorBeat = -1;
    int dropIndicatorTrack = -1;

    void importFilesAt (const juce::StringArray& files, int xInThis, int yInThis);
    void changeListenerCallback (juce::ChangeBroadcaster*) override { rebuild(); }
    void scrollBarMoved (juce::ScrollBar*, double) override;
    void timerCallback() override;
    void updateScrollBars();
    void layoutHeaders();
};

} // namespace rs
