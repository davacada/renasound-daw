#pragma once
#include <JuceHeader.h>

// =============================================================================
//  Renasound built-in synth: 2 oscillators -> resonant low-pass -> amp ADSR.
//  Presets cover the usual "starter" sounds you'd reach for in Mixcraft.
// =============================================================================

namespace rs
{

enum class Wave { Sine, Saw, Square, Triangle };

struct SynthPreset
{
    juce::String name;
    Wave osc1 = Wave::Saw, osc2 = Wave::Saw;
    float osc2Detune = 0.08f;     // semitones
    int   osc2Octave = 0;
    float osc2Level  = 0.5f;
    float cutoff     = 4000.0f;   // Hz
    float resonance  = 0.2f;      // 0..1
    float envToCutoff = 0.0f;     // Hz added at envelope peak
    juce::ADSR::Parameters amp { 0.005f, 0.2f, 0.8f, 0.25f };
    float gain = 0.25f;
};

inline const std::vector<SynthPreset>& getSynthPresets()
{
    static const std::vector<SynthPreset> presets = []
    {
        std::vector<SynthPreset> p;
        { SynthPreset s; s.name = "Saw Lead";  s.cutoff = 3500; s.resonance = 0.3f; s.envToCutoff = 2500; s.amp = { 0.005f, 0.15f, 0.8f, 0.2f };  p.push_back (s); }
        { SynthPreset s; s.name = "Soft Pad";  s.osc2Detune = 0.15f; s.cutoff = 1400; s.resonance = 0.1f; s.amp = { 0.6f, 0.5f, 0.85f, 1.2f }; s.gain = 0.18f; p.push_back (s); }
        { SynthPreset s; s.name = "Pluck";     s.osc1 = Wave::Square; s.osc2 = Wave::Saw; s.cutoff = 600; s.envToCutoff = 5000; s.amp = { 0.001f, 0.3f, 0.0f, 0.2f }; s.gain = 0.3f; p.push_back (s); }
        { SynthPreset s; s.name = "Bass";      s.osc1 = Wave::Saw; s.osc2 = Wave::Square; s.osc2Octave = -1; s.osc2Detune = 0; s.osc2Level = 0.45f; s.cutoff = 500; s.resonance = 0.35f; s.envToCutoff = 900; s.amp = { 0.002f, 0.25f, 0.7f, 0.1f }; s.gain = 0.35f; p.push_back (s); }
        { SynthPreset s; s.name = "E-Piano";   s.osc1 = Wave::Sine; s.osc2 = Wave::Triangle; s.osc2Octave = 1; s.osc2Detune = 0; s.osc2Level = 0.25f; s.cutoff = 6000; s.amp = { 0.002f, 1.2f, 0.25f, 0.4f }; s.gain = 0.35f; p.push_back (s); }
        { SynthPreset s; s.name = "Square Keys"; s.osc1 = Wave::Square; s.osc2 = Wave::Square; s.osc2Detune = 0.05f; s.cutoff = 2500; s.amp = { 0.003f, 0.3f, 0.6f, 0.3f }; s.gain = 0.2f; p.push_back (s); }
        { SynthPreset s; s.name = "Sub Bass";  s.osc1 = Wave::Sine; s.osc2 = Wave::Triangle; s.osc2Level = 0.15f; s.osc2Detune = 0; s.cutoff = 300; s.amp = { 0.003f, 0.1f, 1.0f, 0.12f }; s.gain = 0.5f; p.push_back (s); }
        return p;
    }();
    return presets;
}

// ---------------------------------------------------------------------------
class SynthSound : public juce::SynthesiserSound
{
public:
    bool appliesToNote (int) override    { return true; }
    bool appliesToChannel (int) override { return true; }
};

// ---------------------------------------------------------------------------
class SynthVoice : public juce::SynthesiserVoice
{
public:
    explicit SynthVoice (const std::atomic<int>& presetIndexRef) : presetIndex (presetIndexRef) {}

    bool canPlaySound (juce::SynthesiserSound* s) override { return dynamic_cast<SynthSound*> (s) != nullptr; }

    void startNote (int midiNote, float velocity, juce::SynthesiserSound*, int) override
    {
        preset = getSynthPresets()[(size_t) juce::jlimit (0, (int) getSynthPresets().size() - 1, presetIndex.load())];
        level = velocity;
        const auto sr = getSampleRate();
        freq1 = juce::MidiMessage::getMidiNoteInHertz (midiNote);
        freq2 = freq1 * std::pow (2.0, preset.osc2Octave + preset.osc2Detune / 12.0);
        inc1 = freq1 / sr;
        inc2 = freq2 / sr;
        if (! adsr.isActive()) { phase1 = 0; phase2 = 0.25; ic1 = ic2 = 0; }
        adsr.setSampleRate (sr);
        adsr.setParameters (preset.amp);
        filterEnv.setSampleRate (sr);
        filterEnv.setParameters ({ 0.002f, 0.35f, 0.15f, 0.3f });
        adsr.noteOn();
        filterEnv.noteOn();
    }

    void stopNote (float, bool allowTailOff) override
    {
        if (allowTailOff) { adsr.noteOff(); filterEnv.noteOff(); }
        else              { adsr.reset(); filterEnv.reset(); clearCurrentNote(); }
    }

    void pitchWheelMoved (int) override {}
    void controllerMoved (int, int) override {}

    using juce::SynthesiserVoice::renderNextBlock;

    void renderNextBlock (juce::AudioBuffer<float>& out, int start, int num) override
    {
        if (! adsr.isActive()) return;
        const auto sr = (float) getSampleRate();

        for (int i = 0; i < num; ++i)
        {
            float s = osc (preset.osc1, phase1, inc1) + preset.osc2Level * osc (preset.osc2, phase2, inc2);
            phase1 += inc1; if (phase1 >= 1.0) phase1 -= 1.0;
            phase2 += inc2; if (phase2 >= 1.0) phase2 -= 1.0;

            // TPT state-variable low-pass
            const float fc = juce::jlimit (30.0f, sr * 0.45f, preset.cutoff + preset.envToCutoff * filterEnv.getNextSample());
            const float g = std::tan (juce::MathConstants<float>::pi * fc / sr);
            const float k = 2.0f - 1.9f * preset.resonance;
            const float a1 = 1.0f / (1.0f + g * (g + k));
            const float v3 = s - ic2;
            const float v1 = a1 * ic1 + g * a1 * v3;
            const float v2 = ic2 + g * v1;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;

            const float v = v2 * adsr.getNextSample() * level * preset.gain;
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.addSample (ch, start + i, v);

            if (! adsr.isActive()) { clearCurrentNote(); break; }
        }
    }

private:
    static float polyBlep (double t, double dt)
    {
        if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }

    static float osc (Wave w, double p, double dt)
    {
        switch (w)
        {
            case Wave::Sine:     return (float) std::sin (p * juce::MathConstants<double>::twoPi);
            case Wave::Saw:      return (float) (2.0 * p - 1.0) - polyBlep (p, dt);
            case Wave::Square:
            {
                float v = p < 0.5 ? 1.0f : -1.0f;
                v += polyBlep (p, dt);
                v -= polyBlep (std::fmod (p + 0.5, 1.0), dt);
                return v * 0.8f;
            }
            case Wave::Triangle: return (float) (p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p);
        }
        return 0.0f;
    }

    const std::atomic<int>& presetIndex;
    SynthPreset preset;
    juce::ADSR adsr, filterEnv;
    double phase1 = 0, phase2 = 0, inc1 = 0, inc2 = 0, freq1 = 0, freq2 = 0;
    float ic1 = 0, ic2 = 0, level = 1;
};

// ---------------------------------------------------------------------------
class RenaSynth
{
public:
    RenaSynth()
    {
        for (int i = 0; i < 16; ++i)
            synth.addVoice (new SynthVoice (presetIndex));
        synth.addSound (new SynthSound());
        synth.setNoteStealingEnabled (true);
    }

    void prepare (double sampleRate) { synth.setCurrentPlaybackSampleRate (sampleRate); }

    void render (juce::AudioBuffer<float>& buffer, const juce::MidiBuffer& midi, int start, int num)
    {
        synth.renderNextBlock (buffer, midi, start, num);
    }

    void allNotesOff() { synth.allNotesOff (0, false); }

    std::atomic<int> presetIndex { 0 };

private:
    juce::Synthesiser synth;
};

} // namespace rs
