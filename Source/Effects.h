#pragma once
#include <JuceHeader.h>

// =============================================================================
//  RENASOUND built-in effects. Each one is a regular juce::AudioProcessor, so
//  built-ins and third-party VST3 / AU plugins sit in the same effect chain.
// =============================================================================

namespace rs
{

class BuiltinFx : public juce::AudioProcessor
{
public:
    BuiltinFx (juce::String id, juce::String name, juce::AudioProcessorValueTreeState::ParameterLayout layout);

    const juce::String getName() const override        { return fxName; }
    juce::String getFxId() const                       { return fxId; }

    using juce::AudioProcessor::processBlock;

    bool hasEditor() const override                    { return true; }
    juce::AudioProcessorEditor* createEditor() override;

    bool acceptsMidi() const override                  { return false; }
    bool producesMidi() const override                 { return false; }
    double getTailLengthSeconds() const override       { return 0.0; }
    int getNumPrograms() override                      { return 1; }
    int getCurrentProgram() override                   { return 0; }
    void setCurrentProgram (int) override              {}
    const juce::String getProgramName (int) override   { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo()
            && l.getMainInputChannelSet() == juce::AudioChannelSet::stereo();
    }

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    float param (const char* id) const { return state.getRawParameterValue (id)->load(); }

    juce::AudioProcessorValueTreeState state;

    // optional extras the editor can show
    virtual double getMagnitudeForFrequency (double) const { return 1.0; }
    virtual bool hasResponseCurve() const                  { return false; }
    std::atomic<float> meterValue { 0.0f };                // e.g. gain reduction in dB
    juce::String meterLabel;

    // live readings for the faceplates (written by the audio thread)
    std::atomic<float> inputDb { -100.0f }, outputDb { -100.0f }, phase { 0.0f };
    static constexpr int historySize = 240;
    std::array<std::atomic<float>, historySize> history {};   // one value every ~15 ms, meaning depends on the effect
    std::atomic<int> historyPos { 0 };
    void pushHistory (float v) { const int p = historyPos.load(); history[(size_t) p] = v; historyPos = (p + 1) % historySize; }
    // measures a block before / after processing and keeps the history ticking
    void measureIn (const juce::AudioBuffer<float>& b)  { inputDb = juce::Decibels::gainToDecibels (b.getMagnitude (0, b.getNumSamples()), -100.0f); }
    void measureOut (const juce::AudioBuffer<float>& b) { outputDb = juce::Decibels::gainToDecibels (b.getMagnitude (0, b.getNumSamples()), -100.0f); }
    bool historyTick (int numSamples)
    {
        historyAcc += numSamples;
        if (historyAcc < (int) (currentSampleRate * 0.015)) return false;
        historyAcc = 0;
        return true;
    }

protected:
    juce::String fxId, fxName;
    double currentSampleRate = 44100.0;
    int historyAcc = 0;
    // tempo from the host (RENASOUND's transport)
    double getBpm() const;
    std::optional<double> getPpq() const;
};

struct BuiltinFxInfo { juce::String id, name, description, category; };
const std::vector<BuiltinFxInfo>& getBuiltinFxList();
std::unique_ptr<BuiltinFx> createBuiltinFx (const juce::String& id);
std::unique_ptr<BuiltinFx> createDynamicEq();   // StudioEq.cpp

// Each built-in effect's own faceplate (Faceplates.cpp); nullptr = the generic knob panel
juce::AudioProcessorEditor* createFaceplate (BuiltinFx&);
juce::AudioProcessorEditor* createFaceplate2 (BuiltinFx&);   // Fx2.cpp
juce::AudioProcessorEditor* createFaceplate3 (BuiltinFx&);   // Fx3.cpp
std::unique_ptr<BuiltinFx> createAutoTune();
std::unique_ptr<BuiltinFx> createBusCompressor();
std::unique_ptr<BuiltinFx> createColorEq();
std::unique_ptr<BuiltinFx> createVoiceCleaner();
std::unique_ptr<BuiltinFx> createRotary();
std::unique_ptr<BuiltinFx> createDeEsser();
std::unique_ptr<BuiltinFx> createBitCrusher();
std::unique_ptr<BuiltinFx> createDubDelay();
std::unique_ptr<BuiltinFx> createLimiter();
std::unique_ptr<BuiltinFx> createRingMod();
std::unique_ptr<BuiltinFx> createSubBass();
std::unique_ptr<BuiltinFx> createTubeEq();

} // namespace rs
