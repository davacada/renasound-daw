#pragma once
#include "Effects.h"

// =============================================================================
//  RENASOUND devices with their own faceplates:
//    effects:      Stereo Widener, Amp (guitar amp + cabinet), Punch (transient / HDR enhancer)
//    instruments:  Kick Synth, Vocoder (plays chords through another track's voice)
// =============================================================================

namespace rs
{

// A device that listens to another track's audio (the vocoder's modulator).
// The engine runs the source track first and hands its post-effects,
// pre-fader audio over each block.
class SidechainUser
{
public:
    virtual ~SidechainUser() = default;
    std::atomic<int> sidechainTrackId { 0 };            // 0 = none
    std::atomic<int> ownerTrackId { 0 };                // the track this device sits on (set by the engine)
    const juce::AudioBuffer<float>* sidechain = nullptr; // valid during processBlock (audio thread)

    // filled in by the app: the tracks a device can listen to (message thread)
    static std::function<juce::Array<std::pair<int, juce::String>>()> listTracks;
};

const std::vector<BuiltinFxInfo>& getBuiltinInstrumentList();

std::unique_ptr<BuiltinFx> createWidener();
std::unique_ptr<BuiltinFx> createAmp();
std::unique_ptr<BuiltinFx> createPunch();
std::unique_ptr<BuiltinFx> createKickSynth();
std::unique_ptr<BuiltinFx> createVocoder();
std::unique_ptr<BuiltinFx> createSampler();

} // namespace rs
