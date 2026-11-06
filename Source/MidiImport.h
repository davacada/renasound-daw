#pragma once
#include "MixcraftImport.h"

// =============================================================================
//  Reads Standard MIDI Files (.mid / .midi / .kar) into a RENASOUND project:
//    - tempo and time signature (the first ones in the file)
//    - one instrument track per MIDI track and channel, named from the file
//    - the instrument each part asks for (General MIDI program changes, the
//      drum channel), matched to the closest RENASOUND sound (see SoundMatch.h)
//    - starting volume and pan (controllers 7 and 10)
// =============================================================================

namespace rs
{

bool isMidiFile (const juce::File&);
DawImport importMidiFile (const juce::File&);

// Adds an imported project's tracks to an existing project XML (ids renumbered from
// firstFreeId, every clip moved later by startBeat). Returns how many tracks were added.
int appendImportedTracks (juce::XmlElement& projectXml, const juce::XmlElement& imported, double startBeat, int& firstFreeId);

} // namespace rs
