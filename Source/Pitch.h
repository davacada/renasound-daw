#pragma once
#include "Model.h"

// =============================================================================
//  RENASOUND pitch editing for monophonic audio (vocals, solo instruments):
//    1. analyse:  find the pitch of every short frame (YIN) and group the
//                 voiced frames into notes
//    2. edit:     the user moves notes / flattens drift (PitchEdit in Model.h)
//    3. render:   re-synthesise the audio at the new pitches with TD-PSOLA,
//                 which keeps the voice's character (formants) intact
// =============================================================================

namespace rs
{

struct PitchAnalysis
{
    double sampleRate = 44100.0;
    int hop = 256;                         // source samples between frames
    std::vector<float> pitch;              // MIDI note per frame; NaN = unvoiced
    std::vector<float> levelDb;            // loudness per frame
    std::vector<PitchNote> notes;          // notes found automatically

    int numFrames() const { return (int) pitch.size(); }
    int frameAt (juce::int64 sample) const { return (int) std::clamp<juce::int64> (sample / hop, 0, std::max (0, numFrames() - 1)); }
    bool voiced (int f) const { return f >= 0 && f < numFrames() && ! std::isnan (pitch[(size_t) f]); }
};

using PitchAnalysisPtr = std::shared_ptr<const PitchAnalysis>;

namespace pitchdsp
{
    PitchAnalysisPtr analyse (const AudioData&, const std::atomic<bool>* shouldStop = nullptr);
    // Builds the corrected audio for a whole source file.
    std::shared_ptr<AudioData> render (const AudioData&, const PitchAnalysis&, const std::vector<PitchNote>&);
    // The pitch (MIDI) a frame ends up at after the user's edits.
    float targetPitch (const PitchAnalysis&, const PitchNote&, int frame);
    // Nearest note in the chosen key / scale.
    float snapToScale (float midi, int key, int scale);
    bool inScale (int midiNote, int key, int scale);
}

// Runs analyses and renders on background threads and hands results back to the
// project on the message thread. One per app.
class PitchService : public juce::ChangeBroadcaster,
                     private juce::ChangeListener,
                     private juce::Timer,
                     private juce::AsyncUpdater
{
public:
    explicit PitchService (Project&);
    ~PitchService() override;

    // The analysis for a clip's audio, or nullptr while it's still being worked out.
    PitchAnalysisPtr getAnalysis (const ClipPtr&);
    // Called after editing; renders are batched shortly afterwards.
    void editsChanged() { startTimer (120); }
    bool isBusy() const;
    bool isAnalysing (const ClipPtr&) const;
    bool isRendering (int clipId) const;

private:
    Project& project;
    juce::ThreadPool pool { juce::ThreadPoolOptions{}.withThreadName ("RENASOUND pitch").withNumberOfThreads (2) };
    std::map<juce::String, PitchAnalysisPtr> analyses;   // by file path
    std::set<juce::String> analysing;
    std::map<int, juce::int64> rendering;                // clip id -> hash being rendered
    std::atomic<bool> stopping { false };
    juce::CriticalSection mapLock;

    struct Finished { int clipId; juce::int64 hash; std::shared_ptr<AudioData> audio; };
    std::vector<Finished> finished;   // renders done on a worker, waiting for the message thread
    void handleAsyncUpdate() override;

    void changeListenerCallback (juce::ChangeBroadcaster*) override { startTimer (120); }
    void timerCallback() override;
    void startRender (const ClipPtr&, PitchAnalysisPtr);
};

} // namespace rs
