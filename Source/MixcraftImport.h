#pragma once
#include <JuceHeader.h>

// =============================================================================
//  Reads Mixcraft projects (.mx9 / .mx10, and likely nearby versions) into a
//  RENASOUND project. Mixcraft's format is undocumented: this is worked out
//  from real project files. What comes across:
//    - tempo
//    - audio tracks and MIDI tracks: names, volume, pan, mute
//    - audio clips: position, length, trim, with crossfades where clips overlap
//    - MIDI clips and their notes (looped clips are unrolled)
//  Mixcraft's bundled organs, electric pianos and 303 bass are played by their
//  RENASOUND counterparts (Organ, Electric Piano, Acid Bass); other instruments
//  get a RENASYNTH sound or RENASOUND Drums.
//  What can't: Mixcraft's effects, automation, Melodyne data.
// =============================================================================

namespace rs
{

// The RENASOUND instrument that stands in for a Mixcraft instrument ("organ", "epiano", "acid"), or empty
juce::String mixcraftCounterpart (const juce::String& mixcraftInstrumentName);

// The result of converting another DAW's project (Mixcraft, Ableton Live...)
struct DawImport
{
    bool ok = false;
    juce::String error;
    std::unique_ptr<juce::XmlElement> project;   // a RENASOUND_PROJECT, ready for Project::fromXml
    juce::StringArray missingFiles;              // audio files that couldn't be found (clips skipped)
    juce::String summary;                        // e.g. "12 tracks, 1040 clips"
    int numTracks = 0, numAudioClips = 0, numMidiClips = 0, numNotes = 0, numAutomationLanes = 0;
    double bpm = 120.0;
    juce::String fromApp = "Mixcraft";
    juce::StringArray notes;                     // things the user should know (e.g. "warped clips play unstretched")
};

using MixcraftImport = DawImport;

// Audio files are looked for next to the project, at their original (relative)
// locations, and anywhere inside the extra folders given.
MixcraftImport importMixcraftProject (const juce::File& projectFile, const juce::Array<juce::File>& extraSearchFolders = {});

bool isMixcraftProject (const juce::File&);

} // namespace rs
