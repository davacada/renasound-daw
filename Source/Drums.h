#pragma once
#include <JuceHeader.h>

// =============================================================================
//  RENASOUND Drums: a 16-pad drum machine.
//  Every kit is synthesised from scratch (808, 909, Trap, Lo-Fi, House), so no
//  sample licences are involved. Any pad can be replaced with your own sample,
//  or a whole folder of samples can be loaded as a kit.
//  Pads respond to the General MIDI drum notes, so drum parts imported from
//  other DAWs land on the right sounds.
// =============================================================================

namespace rs
{

class DrumMachine : public juce::AudioProcessor,
                    private juce::AudioProcessorValueTreeState::Listener,
                    private juce::Timer
{
public:
    static constexpr int numPads = 16;

    DrumMachine();
    ~DrumMachine() override;

    // ---- kits & pads ----
    static juce::StringArray getKitNames();
    void loadKit (int kitIndex);                       // replaces every pad with a synthesised sound
    bool loadKitFolder (const juce::File& folder);     // matches files to pads by name (kick, snare, hat...)
    bool loadPadSample (int pad, const juce::File&);

    // Kit files (.rskit): a small XML file naming a sample (relative to the
    // file) and optional level / tune / pan for each pad note. RENASOUND lists
    // every kit file found in the user's kit folders; the samples stay there.
    bool loadKitFile (const juce::File& rskit);
    static juce::Array<juce::File> getKitFolders();
    static void addKitFolder (const juce::File&);
    static juce::Array<juce::File> findKitFiles();
    void resetPad (int pad);                           // back to the kit's own sound
    int getKitIndex() const { return kitIndex.load(); }
    juce::String getPadName (int pad) const;
    int getPadNote (int pad) const;
    int padForNote (int midiNote) const;               // -1 if unmapped
    bool padHasCustomSample (int pad) const;
    void triggerPad (int pad, float velocity);         // from the UI (audition)
    double getLastHitTime (int pad) const { return lastHit[(size_t) pad].load(); }
    juce::String getKitLabel() const;

    // ---- sound shaping ----
    // Every pad: Decay, Tone, Attack, Drive (applied as it plays, so they work on samples too).
    // Synthesised pads also have two "character" controls that rebuild the sound (see getCharacterNames).
    static juce::StringArray getCharacterNames (int pad);
    bool padIsSynthesised (int pad) const { return ! padHasCustomSample (pad); }
    std::vector<float> getPadPeaks (int pad, int columns) const;   // waveform outline, 0..1, for the editor
    double getPadSeconds (int pad) const;

    juce::AudioProcessorValueTreeState state;

    // ---- AudioProcessor ----
    const juce::String getName() const override { return "Drums: " + getKitLabel(); }
    void prepareToPlay (double sampleRate, int blockSize) override;
    void releaseResources() override {}
    using juce::AudioProcessor::processBlock;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    bool isBusesLayoutSupported (const BusesLayout& l) const override
    {
        return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo() && l.getMainInputChannelSet().isDisabled();
    }
    bool acceptsMidi() const override  { return true; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 2.0; }
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override;
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    std::function<void()> onPadsChanged;   // UI refresh when samples change

private:
    struct Sample { juce::AudioBuffer<float> buffer; double sampleRate = 48000.0; };
    using SamplePtr = std::shared_ptr<const Sample>;

    struct Pad
    {
        juce::String name;
        int note = 36;
        int chokeGroup = 0;        // pads in the same non-zero group cut each other (hi-hats)
        SamplePtr sample;
        juce::File customFile;     // set when the user loaded their own sample
    };

    struct Voice
    {
        SamplePtr sample;
        int pad = -1;
        double pos = 0, step = 1;
        float gainL = 0, gainR = 0;
        float fade = 1.0f, fadeStep = 0.0f;   // fadeStep < 0 while being cut off
        bool active = false;
        // shaping
        int age = 0, fadeIn = 0, hold = 0;
        float env = 1.0f, envCoef = 1.0f, boost = 0.0f, boostCoef = 0.0f;
        float lpA = 1.0f, hpA = 0.0f, toneGain = 1.0f, drive = 1.0f, driveNorm = 1.0f;
        float lp1[2] {}, lp2[2] {}, hpIn[2] {}, hpOut[2] {};
    };

    std::array<Pad, numPads> pads;
    std::array<Voice, 32> voices;
    std::array<std::atomic<float>, numPads> pendingHits;
    std::array<std::atomic<double>, numPads> lastHit;
    std::atomic<int> kitIndex { 0 };
    juce::String kitFolderName;            // when a sample folder is loaded as the kit
    juce::SpinLock padLock;                // guards pad samples between UI and audio
    double hostRate = 44100.0;
    juce::int64 samplePosition = 0;

    void startVoice (int pad, float velocity);
    void parameterChanged (const juce::String& id, float) override;
    void timerCallback() override;
    void resynthesise (int pad);
    std::array<std::atomic<bool>, numPads> needsResynth;
    static SamplePtr loadSampleFile (const juce::File&);
};

} // namespace rs
