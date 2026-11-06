#pragma once
#include <JuceHeader.h>

// =============================================================================
//  SOUND MATCHING: when a project or MIDI file comes in from somewhere else,
//  give every instrument track the closest sound RENASOUND has.
//
//    1. the same plugin, if it's installed here (with its settings when the
//       file carries them in a form we can read)
//    2. otherwise what the names say: the preset ("Acoustic Piano"), the track,
//       the category ("Keyboard - Piano"), the instrument ("Alien 303")
//    3. otherwise what the notes say: low and one at a time = bass, long chords
//       = pad, short chords = keys, high single notes = lead, drum-map hits = drums
//
//  Everything is first placed on the General MIDI map (128 instruments + drums),
//  which then picks the RENASOUND instrument and sound. With a General MIDI
//  SoundFont chosen (Multi Sampler > a .sf2 with a full bank), the real sampled
//  instrument from that bank is used instead.
// =============================================================================

namespace rs::soundmatch
{

struct NoteInfo { int pitch = 60; double start = 0, length = 1; float velocity = 0.8f; };

struct Hints
{
    juce::String trackName, preset, category, device, icon;   // any can be empty
    int gmProgram = -1;              // from a MIDI file's program change (-1 = none)
    bool gmDrums = false;            // MIDI channel 10
    std::vector<NoteInfo> notes;     // what the track plays
    juce::MemoryBlock pluginState;   // the original plugin's settings, in JUCE's format (if the file had them)
};

struct Choice
{
    juce::String type;                // "builtin:<id>", "plugin", or empty = RENASYNTH with synthPatch
    int synthPatch = 0;
    juce::MemoryBlock state;          // for the INSTRUMENT element (may be empty)
    juce::PluginDescription plugin;   // type == "plugin"
    int gmProgram = -1;               // where the track landed on the General MIDI map (-1 unknown, 128 = drums)
    juce::String soundName;           // "Organ", "Warm Strings", "Grand Piano (SoundFont)" ...
    juce::String because;             // "the preset name", "the notes it plays" ...
};

Choice match (const Hints&);

// The Multi Sampler playing a folder / SFZ / SoundFont preset (e.g. an Ableton sampler's own samples)
Choice multiSamplerFrom (const juce::File& source, int sf2Preset, const juce::String& because);

// Writes the choice into a RENASOUND_PROJECT <TRACK> element (preset + INSTRUMENT)
void applyToTrackXml (const Choice&, juce::XmlElement& track);

// "Electric Piano   (from the preset name)"
juce::String describe (const Hints&, const Choice&);

// ---- what the matcher can use on this computer (set by the app) ----
void setInstalledPlugins (const juce::Array<juce::PluginDescription>&);
void setGmSoundFont (const juce::File&);   // remembered by the app; empty = none
juce::File getGmSoundFont();
// Called when the Multi Sampler loads a SoundFont: a full General MIDI bank becomes the one imports use
// (if none is chosen yet), and onGmSoundFontChanged is called on the message thread so the app can remember it.
void noticeSoundFont (const juce::File&);
bool isGeneralMidiBank (const juce::File&);
extern std::function<void (const juce::File&)> onGmSoundFontChanged;

// ---- the General MIDI map ----
const char* gmName (int program);           // 0..127, 128 = "Drum Kit"
int gmFromText (const juce::String& text, int* strength = nullptr);   // -1 if nothing recognised, 128 = drums
int gmFromNotes (const std::vector<NoteInfo>&);                        // always gives an answer
bool notesLookLikeDrums (const std::vector<NoteInfo>&);

} // namespace rs::soundmatch
