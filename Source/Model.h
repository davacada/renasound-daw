#pragma once
#include <JuceHeader.h>
#include "Synth.h"
#include "Effects.h"

// =============================================================================
//  Project data model.
//  All edits from the UI happen under Project::lock; the audio thread only
//  try-locks it, so a long edit can never stall audio for long.
//  Positions on the timeline are in beats; audio material is in seconds.
// =============================================================================

namespace rs
{

struct AudioData
{
    juce::AudioBuffer<float> buffer;    // always 2 channels
    double sampleRate = 44100.0;
    juce::File file;
    std::vector<float> peaks;           // min/max pairs for drawing, 1 pair per 256 samples
    static constexpr int peakBlock = 256;

    double getLengthSeconds() const { return buffer.getNumSamples() / sampleRate; }
};

using AudioDataPtr = std::shared_ptr<const AudioData>;

// Loads files once and shares the buffers between clips, undo states and previews.
class AudioCache
{
public:
    AudioCache();
    AudioDataPtr load (const juce::File&);
    juce::AudioFormatManager formats;

private:
    std::map<juce::String, AudioDataPtr> cache;
    juce::CriticalSection cacheLock;
};

struct MidiNote
{
    int pitch = 60;
    double start = 0;       // beats, relative to clip start
    double length = 1;      // beats
    float velocity = 0.8f;
};

// A sung / played note found in an audio clip, and how the user changed it.
// Positions are in samples of the clip's source file.
struct PitchNote
{
    juce::int64 start = 0, end = 0;
    float detected = 60.0f;    // the note's centre pitch as sung (MIDI note number, fractional)
    float shift = 0.0f;        // semitones the user moved it
    float drift = 0.0f;        // 0..1: how much wobble around the centre is flattened
    float level = 0.5f;        // loudness 0..1, for drawing
};

struct PitchEdit
{
    bool active = false;       // notes have been detected for this clip
    bool bypass = false;       // A/B: play the original
    int key = 0;               // 0 = C ... 11 = B
    int scale = 0;             // 0 major, 1 minor, 2 chromatic
    std::vector<PitchNote> notes;

    bool hasChanges() const
    {
        for (auto& n : notes) if (std::abs (n.shift) > 0.001f || n.drift > 0.001f) return true;
        return false;
    }
    juce::int64 hash() const
    {
        juce::int64 h = 1469598103934665603LL;
        auto mix = [&h] (juce::int64 v) { h = (h ^ v) * 1099511628211LL; };
        for (auto& n : notes)
        {
            mix (n.start); mix (n.end);
            mix ((juce::int64) std::lround (n.shift * 1000.0f));
            mix ((juce::int64) std::lround (n.drift * 1000.0f));
        }
        return h;
    }
};

struct Clip
{
    enum class Type { Audio, Midi };

    Type type = Type::Audio;
    int id = 0;
    juce::String name;
    double startBeat = 0;
    bool locked = false;        // can't be moved, trimmed or deleted until unlocked

    // Audio clips
    AudioDataPtr audio;
    double offsetSeconds = 0;   // trimmed from the start of the file
    double lengthSeconds = 0;
    double fadeInSeconds = 0, fadeOutSeconds = 0;   // e.g. crossfades between overlapping clips

    // Pitch editing (audio clips)
    PitchEdit pitch;
    AudioDataPtr pitchRendered;            // the corrected audio, made in the background
    juce::int64 pitchRenderedHash = 0;     // which edits pitchRendered was made from

    // What playback should use: the corrected audio when there are edits, else the original
    const AudioData* getPlaybackAudio() const
    {
        if (pitch.active && ! pitch.bypass && pitch.hasChanges() && pitchRendered != nullptr)
            return pitchRendered.get();
        return audio.get();
    }

    // MIDI clips (notes outside 0..lengthBeats are kept but not played, so trimming isn't destructive)
    double lengthBeats = 4;
    std::vector<MidiNote> notes;

    double getLengthBeats (double bpm) const { return type == Type::Audio ? lengthSeconds * bpm / 60.0 : lengthBeats; }
    double getEndBeat (double bpm) const     { return startBeat + getLengthBeats (bpm); }
};

using ClipPtr = std::shared_ptr<Clip>;

// One slot in an effect chain (or a track's plugin instrument): a built-in
// effect or a third-party VST3 / AU plugin.
struct FxSlot
{
    FxSlot();
    int uid = 0;                                     // stable id, so automation can find this slot
    juce::String type;                               // "builtin:<id>" or "plugin"
    juce::PluginDescription plugin;                  // for plugins
    std::unique_ptr<juce::AudioProcessor> processor; // nullptr if the plugin is missing on this computer
    std::atomic<bool> bypassed { false };
    juce::MemoryBlock missingState;                  // kept so a missing plugin's settings aren't lost

    juce::String getName() const
    {
        if (type == "plugin") return (processor == nullptr ? "Missing: " : "") + plugin.name;
        return processor != nullptr ? processor->getName() : type;
    }
    juce::String identity() const { return type == "plugin" ? "plugin:" + plugin.createIdentifierString() : type; }
};

using FxPtr = std::shared_ptr<FxSlot>;

// ---------------------------------------------------------------------------
//  Automation: a lane of points (beat, value 0..1) that drives one parameter.
// ---------------------------------------------------------------------------
struct AutomationPoint
{
    double beat = 0;
    float value = 0.5f;
};

struct AutomationLane
{
    enum class Kind { Volume, Pan, Effect, Instrument };

    Kind kind = Kind::Volume;
    int fxUid = 0;            // Effect: which slot
    int paramIndex = 0;       // Effect / Instrument: which parameter
    bool visible = true;
    std::vector<AutomationPoint> points;   // kept sorted by beat

    juce::String getId() const;            // "volume", "pan", "fx:<uid>:<param>", "inst:<param>"
    static AutomationLane fromId (const juce::String&);
    bool sameTarget (const AutomationLane& o) const { return kind == o.kind && fxUid == o.fxUid && paramIndex == o.paramIndex; }

    float valueAt (double beat) const
    {
        if (points.empty()) return -1.0f;
        if (beat <= points.front().beat) return points.front().value;
        if (beat >= points.back().beat) return points.back().value;
        auto it = std::upper_bound (points.begin(), points.end(), beat, [] (double b, const AutomationPoint& p) { return b < p.beat; });
        const auto& b = *it;
        const auto& a = *(it - 1);
        const double t = (beat - a.beat) / juce::jmax (1.0e-9, b.beat - a.beat);
        return (float) (a.value + (b.value - a.value) * t);
    }

    // value <-> the parameter's own units
    static float volumeToValue (float gain) { return std::sqrt (juce::jlimit (0.0f, 1.5f, gain) / 1.5f); }
    static float valueToVolume (float v)    { return 1.5f * v * v; }
    static float panToValue (float pan)     { return (pan + 1.0f) * 0.5f; }
    static float valueToPan (float v)       { return v * 2.0f - 1.0f; }
};

struct Track
{
    enum class Type { Audio, Instrument };

    Track();

    Type type = Type::Audio;
    int id = 0;
    juce::String name;
    juce::Colour colour;
    float volume = 0.8f;    // linear gain, 0..1.5
    float pan = 0.0f;       // -1..1
    bool mute = false, solo = false, armed = false;
    bool autoName = false;   // the name follows the chosen instrument until the user renames the track
    std::vector<ClipPtr> clips;

    // Where the track records from.
    //   Audio tracks: a hardware input (first channel, 0-based; -1 = none), mono or a stereo pair.
    //   Instrument tracks: "" = follows the selection (on-screen / computer keyboard and every MIDI
    //   device that isn't given to another track), "none", or a MIDI device's name.
    int audioInput = 0;
    bool audioInputStereo = true;
    juce::String midiInput;
    std::atomic<int> midiInputIndex { -1 };   // resolved by the engine: -1 follows selection, -2 none, >= 0 device

    // Instrument tracks: the built-in synth, or a plugin instrument if `instrument` is set
    std::unique_ptr<RenaSynth> synth;
    FxPtr instrument;
    std::atomic<bool> pendingNotesOff { false };

    // Insert effects, processed in order before the fader
    std::vector<FxPtr> fx;

    // The master track (Project::master) isn't in the track list; its effects are the master bus's
    bool isMaster = false;
    std::vector<FxPtr>* externalFx = nullptr;
    const std::vector<FxPtr>& effects() const { return externalFx != nullptr ? *externalFx : fx; }

    // Automation lanes shown under the track
    std::vector<AutomationLane> automation;
    std::atomic<float> autoVolume { -1.0f }, autoPan { -2.0f };   // what automation is doing right now (for the UI)
    FxSlot* findFx (int uid) const { for (auto& f : effects()) if (f->uid == uid) return f.get(); return nullptr; }
    juce::AudioProcessorParameter* getAutomationParameter (const AutomationLane&) const;

    // Written by the audio thread, read by meters
    std::atomic<float> meterL { 0 }, meterR { 0 };

    // Per-track scratch, owned by the audio thread
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midiScratch;
    juce::AudioBuffer<float> sidechainOut;   // this track's audio, kept when another track listens to it
};

using TrackPtr = std::shared_ptr<Track>;

// -----------------------------------------------------------------------------
class Project : public juce::ChangeBroadcaster
{
public:
    explicit Project (AudioCache&);

    juce::CriticalSection lock;

    double bpm = 120.0;
    // the song's details and how it was last mixed down (the Mix down window)
    struct SongDetails
    {
        juce::String title, artist, album, writers, producers, coverPath;
        int format = 0, bitDepth = 24, oggKbps = 256;   // format: 0 WAV, 1 AIFF, 2 FLAC, 3 Ogg
        double sampleRate = 0;                           // 0 = the project's rate
        bool stems = false, stemsWithMix = true;
    } song;
    int countIn = 0;   // clicks before recording starts: 0 none, 1-4 clicks, 101 = 1 bar, 102 = 2 bars
    int countInBeats() const { return countIn > 100 ? (countIn - 100) * beatsPerBar : countIn; }
    int beatsPerBar = 4;
    bool loopEnabled = false;
    double loopStart = 0, loopEnd = 16;   // beats
    std::vector<TrackPtr> tracks;
    float masterVolume = 0.9f;
    std::vector<FxPtr> masterFx;
    std::atomic<float> masterMeterL { 0 }, masterMeterR { 0 };

    // The master track: shown in the timeline with "+ Master", for master volume / effect automation
    TrackPtr master;
    bool showMaster = false;

    juce::File file;                      // where it was last saved
    bool dirty = false;

    // ---- editing helpers (all take the lock themselves) ----
    TrackPtr addTrack (Track::Type);
    void removeTrack (int trackId);
    ClipPtr addAudioClip (int trackId, const juce::File&, double startBeat);
    ClipPtr addMidiClip (int trackId, double startBeat, double lengthBeats);
    void removeClip (int clipId);
    ClipPtr duplicateClip (int clipId);
    ClipPtr splitClip (int clipId, double atBeat);   // returns the right-hand part
    ClipPtr addClipCopy (int trackId, const Clip&, double startBeat);   // a copy with a new id (paste)
    int newId() { return nextId++; }

    // effects: trackId -1 means the master bus
    void addFx (int trackId, FxPtr, int index = -1);
    void removeFx (int trackId, const FxPtr&);
    void moveFx (int trackId, const FxPtr&, int newIndex);   // moves it to newIndex in its chain
    void setInstrument (int trackId, FxPtr pluginOrNull);
    std::vector<FxPtr>* getFxChain (int trackId);

    // Set by the engine: builds a slot (and its processor) from a type id / plugin description.
    std::function<FxPtr (const juce::String& type, const juce::PluginDescription*, juce::String& error)> createFx;

    TrackPtr findTrack (int trackId) const;
    ClipPtr findClip (int clipId, TrackPtr* ownerOut = nullptr) const;
    double getEndBeat() const;

    // ---- undo / redo: snapshot-based ----
    // Call before making a change. Saves the current state so it can be restored.
    void beginEdit();
    void undo();
    void redo();
    bool canUndo() const { return ! undoStack.empty(); }
    bool canRedo() const { return ! redoStack.empty(); }

    // ---- files ----
    std::unique_ptr<juce::XmlElement> toXml() const;
    void fromXml (const juce::XmlElement&);
    bool save (const juce::File&);
    bool load (const juce::File&);
    void loadImported (const juce::XmlElement&);   // a project converted from another DAW: untitled, unsaved
    void clear();

    // Called when a structural change happened (tracks/clips) so views rebuild
    void changed() { dirty = true; sendChangeMessage(); }

    std::function<void (Track&)> onTrackCreated;   // engine uses this to prepare synths

    AudioCache& audioCache;

private:
    int nextId = 1;
    std::vector<juce::String> undoStack, redoStack;
    void restoreFrom (const juce::String& xmlText);
    static void fxToXml (const FxSlot&, juce::XmlElement& parent, const char* tag);
    FxPtr fxFromXml (const juce::XmlElement&, FxPtr reuse);
};

juce::Colour trackColourFor (int index);

} // namespace rs
