#pragma once
#include "Effects.h"

// =============================================================================
//  RENASYNTH: RENASOUND's own synth. Two oscillators (with shape), a sub and
//  noise, unison, a resonant filter with its own envelope, an amp envelope,
//  an LFO and glide. Every instrument track starts with one.
// =============================================================================

namespace rs
{

struct SynthPatchInfo { juce::String name; };

// the factory sounds; the first seven match the old built-in synth's presets, in order
const std::vector<SynthPatchInfo>& getSynthPatches();

std::unique_ptr<BuiltinFx> createSynth();

// loads factory sound `index` into a RENASYNTH (does nothing for other processors)
void loadSynthPatch (juce::AudioProcessor*, int index);

// the sound's name ("Soft Pad"), or empty if this isn't a RENASYNTH
juce::String getSynthPatchName (const juce::AudioProcessor*);

} // namespace rs
