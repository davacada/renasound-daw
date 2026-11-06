#pragma once
#include "Model.h"

// =============================================================================
//  FITTING AUDIO TO THE SONG
//    analyse   the tempo (from the file name if it says, else from the beats),
//              whether it's a loop (a whole number of bars), and the key
//    render    a new version stretched to the song's tempo (WSOLA: keeps the
//              pitch and the attack of drums) and moved to the song's key
//  Used when a loop is imported (automatic) and from a clip's menu (any audio).
// =============================================================================

namespace rs::audiofit
{

struct Analysis
{
    double bpm = 0;              // 0 = no steady beat found
    float tempoConfidence = 0;   // 0..1
    bool tempoFromName = false;
    bool isLoop = false;         // a whole number of bars at that tempo, and short
    int bars = 0;
    int key = -1;                // 0 = C .. 11 = B, -1 = no clear key (drums, noise)
    bool minor = false;
    float keyConfidence = 0;
    bool keyFromName = false;
};

juce::String keyName (int key, bool minor);   // "A minor"

Analysis analyse (const AudioData&, const juce::String& fileName = {}, int beatsPerBar = 4);

// tempo / key a file name states: "Funky Loop 120bpm Am.wav", "128_Gmin_Bass.wav"
double bpmFromName (const juce::String& name);
int keyFromName (const juce::String& name, bool& minor);

// the song's key from its MIDI notes (-1 if there aren't enough)
int songKey (const Project&, bool& minor);

// how far to move `key` to reach `target` (-6..+5 semitones), comparing like with like (relative major/minor)
int semitonesBetween (int key, bool minor, int targetKey, bool targetMinor);

// stretch: new length / old length (2 = twice as long, slower); semitones: pitch change
std::shared_ptr<AudioData> render (const AudioData&, double stretch, double semitones, const std::atomic<bool>* cancel = nullptr);

struct Fit
{
    bool tempo = false, key = false;   // what will change
    double stretch = 1.0, semitones = 0.0;
    double sourceBpm = 0;
    juce::String description;          // "92 -> 120 BPM, A minor -> C minor (+3)"
};

// what fitting this audio into `project` means. adoptTempo: the song is empty, so the song takes the loop's tempo instead.
Fit plan (const Analysis&, const Project&, bool& adoptTempo, bool onlyIfLoop);

// renders and saves the fitted audio next to RENASOUND's other files; returns the new file (or {} on failure)
juce::File renderToFile (const AudioData&, const Fit&, const juce::String& baseName, juce::String& error);

} // namespace rs::audiofit
