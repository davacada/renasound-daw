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
//    - Simpler, Sampler and Drum Rack samples: the same samples, mapped the
//      same way, played by the Multi Sampler (Live's own synths get the closest
//      RENASOUND sound, see SoundMatch.h)
//    - track volume and pan automation
//  What can't: Live's effects, device automation, and warping (warped audio
//  comes in at its original speed; "Fit to the song" stretches it).
// =============================================================================

namespace rs
{

DawImport importAbletonProject (const juce::File& alsFile, const juce::Array<juce::File>& extraSearchFolders = {});
bool isAbletonProject (const juce::File&);

} // namespace rs
