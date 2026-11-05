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
    void startRecording();
    bool isPlaying() const          { return playing.load(); }
    bool isRecording() const        { return recording.load(); }
    double getPositionBeats() const;
    void setPositionBeats (double);

    std::atomic<bool> metronomeOn { false };
    std::atomic<int> liveInputTrackId { -1 };     // which instrument track the keyboard plays

    // ---- library preview ----
    void preview (AudioDataPtr);
    void stopPreview();

    // ---- mixdown ----
    bool renderToFile (const juce::File&, juce::String& errorOut);

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
    juce::MidiMessageCollector midiCollector;
    juce::MidiBuffer liveMidi;

    // recording
    juce::TimeSliceThread writerThread { "Renasound recorder" };
    std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> threadedWriter;
    std::atomic<juce::AudioFormatWriter::ThreadedWriter*> activeWriter { nullptr };
    juce::File recordingFile;
    double recordStartBeat = 0;
    int recordTrackId = -1;
    int recordInstrumentTrackId = -1;
    std::vector<MidiNote> recordedNotes;            // audio thread fills, message thread reads after stop
    std::array<double, 128> heldNoteStart {};
    std::array<float, 128> heldNoteVelocity {};

    // preview
    AudioDataPtr previewData;
    std::atomic<double> previewPos { -1 };
    juce::SpinLock previewLock;

    double metronomePhase = 0;
    int metronomeSamplesLeft = 0;
    bool metronomeAccent = false;

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
