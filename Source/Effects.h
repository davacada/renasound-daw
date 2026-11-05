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

protected:
    juce::String fxId, fxName;
    double currentSampleRate = 44100.0;
    // tempo from the host (RENASOUND's transport)
    double getBpm() const;
    std::optional<double> getPpq() const;
};

struct BuiltinFxInfo { juce::String id, name, description; };
const std::vector<BuiltinFxInfo>& getBuiltinFxList();
std::unique_ptr<BuiltinFx> createBuiltinFx (const juce::String& id);
std::unique_ptr<BuiltinFx> createDynamicEq();   // StudioEq.cpp
std::unique_ptr<BuiltinFx> createTubeEq();

} // namespace rs
