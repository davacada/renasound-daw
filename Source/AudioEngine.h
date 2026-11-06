#pragma once
#include "Model.h"

namespace rs
{

// Finds, remembers and loads third-party plugins (VST3 everywhere, Audio Units on Mac).
class PluginHost
{
public:
    PluginHost();
    ~PluginHost();

    juce::AudioPluginFormatManager formats;
    juce::KnownPluginList knownPlugins;
    std::unique_ptr<juce::PropertiesFile> settings;

    juce::Array<juce::PluginDescription> getEffects() const;
    juce::Array<juce::PluginDescription> getInstruments() const;
    juce::File getDeadMansPedalFile() const;
    void save();
};

// =============================================================================
//  The audio engine: owns the audio device, plays the project, records,
//  plays the metronome, previews library sounds and renders mixdowns.
// =============================================================================
class AudioEngine : public juce::AudioIODeviceCallback,
                    public juce::MidiInputCallback,
                    private juce::ChangeListener,
                    public juce::AudioPlayHead
{
public:
    explicit AudioEngine (Project&);
    ~AudioEngine() override;

    juce::AudioDeviceManager deviceManager;
    juce::MidiKeyboardState keyboardState;   // on-screen + computer keyboard + MIDI hardware
    PluginHost pluginHost;

    // Builds an effect / instrument slot, ready to process. type is "builtin:<id>" or "plugin".
    FxPtr createFx (const juce::String& type, const juce::PluginDescription*, juce::String& error);

    // AudioPlayHead: lets synced effects and plugins follow RENASOUND's tempo and transport
    juce::Optional<PositionInfo> getPosition() const override;

    // ---- transport (message thread) ----
    void play();
    void stop();
    void togglePlay()               { isPlaying() ? stop() : play(); }

    // plays one clip on its own (the editor's play button); stops at the clip's end
    void playClip (int clipId);
    int getPlayingClipId() const    { return soloClipId.load(); }
    void updateClipPlayback();      // message thread, from a timer: stops at the end of the clip

    // what's being recorded right now, for drawing it as it forms
    struct RecordingPreview
    {
        double startBeat = 0, nowBeat = 0;
        int instrumentTrackId = -1;
        std::vector<MidiNote> notes;                       // absolute beats
        struct Audio { int trackId = -1; std::vector<float> peaks; };
        std::vector<Audio> audio;                          // peaks spread evenly from startBeat to nowBeat
    };
    bool getRecordingPreview (RecordingPreview&);
    void startRecording();
    bool isPlaying() const          { return playing.load(); }
    bool isRecording() const        { return recording.load(); }
    bool isCountingIn() const       { return countInLeft.load() > 0; }
    int countInBeatsLeft() const;   // for the clock while counting in
    double getPositionBeats() const;
    void setPositionBeats (double);

    std::atomic<bool> metronomeOn { false };
    std::atomic<int> liveInputTrackId { -1 };     // which instrument track the keyboard plays

    // ---- inputs (message thread) ----
    juce::StringArray getAudioInputNames() const;                 // hardware inputs of the current device
    juce::StringArray getMidiInputNames() const { return midiInputNames; }
    void resolveMidiInputs();                                     // after a track's MIDI input changed
    static juce::String describeInput (const Track&);             // "1+2", "In 3", "All MIDI", ...

    // ---- library preview ----
    void preview (AudioDataPtr);
    void stopPreview();

    // ---- mixdown ----
    bool renderToFile (const juce::File&, juce::String& errorOut);
    // renders the song (or one track, for a stem) into `out` at the engine's sample rate.
    // progress gets 0..1 and can return false to cancel. Safe to call from a background thread.
    bool renderMix (juce::AudioBuffer<float>& out, int soloTrackId, bool withMasterProcessing,
                    const std::function<bool (double)>& progress, juce::String& errorOut);
    double getSampleRate() const { return sampleRate; }

    static juce::File getRecordingsFolder();

    // Fired on the message thread when a recording has become a clip.
    std::function<void()> onRecordingFinished;

    // ---- AudioIODeviceCallback ----
    void audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                           int numSamples, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage&) override;

private:
    Project& project;

    std::atomic<bool> playing { false }, recording { false };
    std::atomic<double> positionSeconds { 0 };
    double sampleRate = 44100.0;
    int blockSize = 512;
    bool wasPlaying = false;

    juce::AudioBuffer<float> master;
    juce::MidiMessageCollector midiCollector;     // on-screen / computer keyboard and unknown devices
    juce::MidiBuffer liveMidi;                    // what the selected track hears

    // MIDI devices, each with its own queue so a track can listen to just one
    static constexpr int maxMidiDevices = 16;
    juce::StringArray midiInputNames, midiInputIds;
    std::array<juce::MidiMessageCollector, maxMidiDevices> deviceCollectors;
    std::array<juce::MidiBuffer, maxMidiDevices> deviceMidi;
    std::atomic<juce::uint32> claimedDevices { 0 };   // devices given to a particular track
    const juce::MidiBuffer* midiFor (const Track&, int midiSource) const;

    // hardware input channel -> position in the callback's input array (-1 = inactive)
    std::array<int, 64> inputPos {};
    juce::HeapBlock<float> silence;
    void enableAllInputs();
    void changeListenerCallback (juce::ChangeBroadcaster*) override { enableAllInputs(); }

    // recording
    juce::TimeSliceThread writerThread { "Renasound recorder" };
    struct Take { int trackId; int left, right; juce::File file; std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer; std::vector<float> peaks; };
    std::vector<Take> takes;                         // one per armed audio track (message thread)
    struct RecSlot { juce::AudioFormatWriter::ThreadedWriter* writer = nullptr; int left = 0, right = 0; float* peaks = nullptr; int peakCap = 0; };
    std::array<RecSlot, 64> recSlots;                // what the audio thread writes
    std::array<std::atomic<int>, 64> recPeakCount {};
    std::atomic<int> soloClipId { 0 };
    std::atomic<int> renderSoloTrack { 0 };   // mixdown of one stem
    // count-in: clicks before recording starts
    std::atomic<int> countInLeft { 0 };
    int countInTotal = 0, countInBeatLen = 1, ciSamplesLeft = 0;
    double ciPhase = 0;
    bool ciAccent = false;
    juce::AudioBuffer<float> countInBuf;
    double soloClipReturnBeat = 0;
    std::atomic<int> numRecSlots { 0 };
    double recordStartBeat = 0;
    int recordInstrumentTrackId = -1;
    int recordMidiSource = -1;
    std::vector<MidiNote> recordedNotes;            // audio thread fills, message thread reads after stop
    std::array<double, 128> heldNoteStart {};
    std::array<float, 128> heldNoteVelocity {};

    // preview
    AudioDataPtr previewData;
    std::atomic<double> previewPos { -1 };
    juce::SpinLock previewLock;
public:
    std::atomic<int> missedBlocks { 0 };   // blocks skipped because the project was being edited (for diagnostics)
private:
    float lastOut[2] {};                    // last output sample, to fade out cleanly if a block is skipped
    bool needsFadeIn = false;

    double metronomePhase = 0;
    int metronomeSamplesLeft = 0;
    bool metronomeAccent = false;

    void processMaster (juce::AudioBuffer<float>& mix, double startSeconds, int numSamples);   // master automation, effects, volume
    void renderBlock (juce::AudioBuffer<float>& out, double startSeconds, int numSamples, bool transportRunning,
                      const juce::MidiBuffer& live, int liveOffset, bool includeLive);
    void renderTrack (Track&, double startSec, int numSamples, bool transportRunning,
                      const juce::MidiBuffer& live, int liveOffset, bool includeLive);
    void renderMetronome (juce::AudioBuffer<float>&, double startSec, int numSamples);
    void captureLiveNotes (const juce::MidiBuffer&, double blockStartSec, int numSamples);
    void finishRecording();
    void allNotesOff();
    void prepareProcessor (juce::AudioProcessor&);
    void processFx (FxSlot&, juce::AudioBuffer<float>&, juce::MidiBuffer&, int numSamples);
    void processChain (std::vector<FxPtr>&, juce::AudioBuffer<float>&, int numSamples);

    juce::AudioBuffer<float> fxTemp;
    juce::MidiBuffer emptyMidi;
    double playheadSeconds = 0;
    bool playheadRunning = false;
    int maxBlock = 1024;
};

} // namespace rs
