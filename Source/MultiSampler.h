#pragma once
#include "Effects.h"

// =============================================================================
//  MULTI SAMPLER: a real sampled instrument
//    zones          each sample covers a range of keys and velocities
//    layers         soft to loud recordings, picked by how hard you play
//    round robins   several takes of the same note, rotated so repeats sound natural
//    articulations  sustain / pizzicato / staccato ... chosen by buttons or keyswitches
//    release        sounds that play when you let go of a key
//  Load a folder (mapped from the file names, e.g. "Violin A3 mf RR2"),
//  or an SFZ instrument. NCW sample files are read too.
// =============================================================================

namespace rs::msamp
{

struct SampleData
{
    juce::AudioBuffer<float> buffer;   // always 2 channels
    double rate = 48000.0;
    juce::File file;
    int loopStart = -1, loopEnd = -1;   // from the file itself (WAV loop points), -1 = none
    int unityNote = -1;                 // from the file itself, -1 = unknown
};
using SamplePtr = std::shared_ptr<const SampleData>;

struct Zone
{
    SamplePtr sample;
    int root = 60, loKey = 0, hiKey = 127, loVel = 1, hiVel = 127;
    int rr = 0;              // round robin take (0 = none)
    int articulation = -1;   // -1 = plays in every articulation
    bool release = false;    // plays when the key is let go
    bool keyTrack = true;    // false: the same pitch on every key (drums)
    float tuneCents = 0, gainDb = 0;
    float pan = 0;                         // -1 left .. +1 right (SoundFont stereo pairs)
    int offset = 0, end = -1;              // in samples
    int loopStart = -1, loopEnd = -1;      // -1 = no loop
    bool oneShot = false;                  // plays to the end, ignores note-off
    juce::String name;
};

struct Instrument
{
    juce::String name;
    juce::File source;                     // the folder, SFZ or file it came from
    std::vector<Zone> zones;
    juce::StringArray articulations;       // names; zones point at these by index
    std::vector<int> keyswitches;          // a key per articulation (-1 = none)
    int velocityLayers = 0, roundRobins = 0, samples = 0;
    int octaveCorrection = 0;              // how many octaves the file names were off (found by listening)
    juce::StringArray sf2Presets;          // a SoundFont: all its presets ("000:000 Grand Piano"), and which one this is
    int sf2Preset = -1;
    size_t bytes = 0;
    // envelope found in the SFZ (ms, %): applied once on load, < 0 = not given
    float attack = -1, decay = -1, sustain = -1, release = -1;
};
using InstrumentPtr = std::shared_ptr<const Instrument>;

// ---- loading (can run on any thread; progress 0..1) ----
SamplePtr loadSample (const juce::File& f, juce::AudioFormatManager& formats, juce::String& error);
InstrumentPtr mapFiles (const juce::Array<juce::File>& files, const juce::File& source, std::atomic<float>* progress, juce::String& error);
InstrumentPtr loadFolder (const juce::File& folder, std::atomic<float>* progress, juce::String& error);
InstrumentPtr loadSfz (const juce::File& sfz, std::atomic<float>* progress, juce::String& error);
InstrumentPtr loadSf2 (const juce::File& sf2, int presetIndex, std::atomic<float>* progress, juce::String& error);
struct Sf2PresetInfo { int bank = 0, program = 0; juce::String name; };
std::vector<Sf2PresetInfo> sf2PresetList (const juce::File& sf2);   // in the order the preset menu shows them (bank, then program)
InstrumentPtr load (const juce::File& fileOrFolder, std::atomic<float>* progress, juce::String& error, int sf2Preset = 0);

// what a file name says about a sample (for the mapper, and for tests)
struct NameInfo
{
    int note = -1;            // MIDI note from the name, C4 = 60
    int dyn = -1;             // 0 ppp .. 7 fff, or a layer number + 100
    int rr = 0;
    bool release = false;
    juce::StringArray words;  // what's left: becomes the articulation
    int trailingNumber = -1;
};
NameInfo parseName (const juce::String& fileNameWithoutExtension);
double detectPitchHz (const juce::AudioBuffer<float>& b, double rate);

} // namespace rs::msamp

namespace rs
{
std::unique_ptr<BuiltinFx> createMultiSampler();
bool multiSamplerLoad (juce::AudioProcessor*, const juce::File&, int sf2Preset = 0);   // starts loading in the background
bool multiSamplerIsLoading (juce::AudioProcessor*);
msamp::InstrumentPtr multiSamplerInstrument (juce::AudioProcessor*);
bool multiSamplerLoadNow (juce::AudioProcessor*, const juce::File&, juce::String& error, int sf2Preset = 0);   // on this thread (tests)
std::vector<int> multiSamplerSoundingZones (juce::AudioProcessor*);                      // zone numbers playing now
}
