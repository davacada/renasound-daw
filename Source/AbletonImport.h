#pragma once
#include "MixcraftImport.h"

// =============================================================================
//  Reads Ableton Live sets (.als, Live 9 to 12) into a RENASOUND project.
//  An .als file is gzip-compressed XML. What comes across:
//    - tempo
//    - audio and MIDI tracks: names, volume, pan, mute (track on/off)
//    - arrangement clips with their positions, lengths and loops, MIDI notes,
//      and audio offsets worked out through Live's warp markers
//    - if the arrangement is empty, the Session view's scenes are laid out
//      one after another so nothing is lost
//    - Drum Racks, Impulse and drum-named tracks become RENASOUND Drums
//    - track volume and pan automation
//  What can't: Live's devices (instruments / effects), device automation, and
//  time-stretching (warped audio plays at its original speed).
// =============================================================================

namespace rs
{

DawImport importAbletonProject (const juce::File& alsFile, const juce::Array<juce::File>& extraSearchFolders = {});
bool isAbletonProject (const juce::File&);

} // namespace rs
