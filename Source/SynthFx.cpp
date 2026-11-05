#include "SynthFx.h"
#include <complex>
#include "DspHelpers.h"
#include "UI/Faceplate.h"

namespace rs
{

namespace
{
    const juce::StringArray oscWaves { "Sine", "Triangle", "Saw", "Square", "Noise" };
    // (new types go at the end: saved sounds store the type's number)
    const juce::StringArray filterTypes { "LP 24", "LP 12", "High-pass", "Band-pass", "Ladder", "Warm LP", "Notch" };
    const juce::StringArray lfoWaves { "Sine", "Triangle", "Saw", "Square", "Random" };
    const juce::StringArray lfoTargets { "Pitch", "Filter", "Volume", "Shape", "Pan" };

    // ---- the mod matrix: any source to any destination ----
    enum ModSrc { SrcNone, SrcLfo1, SrcLfo2, SrcFiltEnv, SrcAmpEnv, SrcVelocity, SrcNote, SrcModWheel, SrcAftertouch, SrcRandom, NumSrc };
    enum ModDst { DstNone, DstPitch, DstFilter, DstReso, DstVolume, DstPan, DstShape1, DstShape2, DstOsc2Pitch, DstOscMix,
                  DstDetune, DstDrive, DstLfo1Rate, DstLfo2Rate, DstLfo1Amount, DstLfo2Amount, NumDst };
    const juce::StringArray modSources { "None", "LFO 1", "LFO 2", "Filter Env", "Amp Env", "Velocity", "Note", "Mod Wheel", "Aftertouch", "Random" };
    const juce::StringArray modDests { "None", "Pitch", "Filter", "Resonance", "Volume", "Pan", "Osc 1 Shape", "Osc 2 Shape", "Osc 2 Pitch",
                                       "Osc Mix", "Detune", "Drive", "LFO 1 Rate", "LFO 2 Rate", "LFO 1 Amount", "LFO 2 Amount" };
    constexpr int modSlots = 4;
    const char* const modSrcIds[] { "m1src", "m2src", "m3src", "m4src" };
    const char* const modDstIds[] { "m1dst", "m2dst", "m3dst", "m4dst" };
    const char* const modAmtIds[] { "m1amt", "m2amt", "m3amt", "m4amt" };

    float lfoShape (int wave, double ph, float held)
    {
        switch (wave)
        {
            case 0: return (float) std::sin (ph * juce::MathConstants<double>::twoPi);
            case 1: return (float) (ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph);
            case 2: return (float) (1.0 - 2.0 * ph);
            case 3: return ph < 0.5 ? 1.0f : -1.0f;
            default: return held;
        }
    }
    // a quick tanh for the filters' saturation
    inline float softClip (float x) { x = juce::jlimit (-3.0f, 3.0f, x); return x * (27.0f + x * x) / (27.0f + 9.0f * x * x); }

    std::unique_ptr<juce::AudioParameterChoice> choiceParam (const char* id, const char* name, const juce::StringArray& items, int def)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def);
    }
    Range stepped (float lo, float hi) { return Range (lo, hi, 1.0f); }

    // whole-number settings shown as whole numbers ("+1", "5")
    std::unique_ptr<juce::AudioParameterFloat> intParam (const char* id, const char* name, float lo, float hi, float def, const char* unit, bool sign)
    {
        const juce::String u (unit);
        return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id, 1 }, name, Range (lo, hi, 1.0f), def,
            juce::AudioParameterFloatAttributes().withStringFromValueFunction ([u, sign] (float v, int)
            {
                const int n = juce::roundToInt (v);
                return (sign && n > 0 ? "+" : "") + juce::String (n) + (u.isNotEmpty() ? " " + u : juce::String());
            }));
    }

    // one cycle of an oscillator at phase p (0..1); dt = phase step for anti-aliasing (0 = off)
    float polyBlep (double t, double dt)
    {
        if (dt <= 0) return 0.0f;
        if (t < dt)       { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }

    float oscSample (int wave, double p, double dt, float shape, float noise)
    {
        constexpr double twoPi = juce::MathConstants<double>::twoPi;
        auto fold = [shape] (float s) { return (1.0f - shape) * s + shape * std::sin (juce::MathConstants<float>::halfPi * s * 5.0f); };
        switch (wave)
        {
            case 0: return fold ((float) std::sin (p * twoPi));
            case 1: return fold ((float) (p < 0.5 ? 4.0 * p - 1.0 : 3.0 - 4.0 * p));
            case 2:
            {
                const float a = (float) (2.0 * p - 1.0) - polyBlep (p, dt);
                const double p2 = std::fmod (p * 2.0, 1.0);
                const float b = (float) (2.0 * p2 - 1.0) - polyBlep (p2, dt * 2.0);
                return (1.0f - shape) * a + shape * b;   // shape brightens the saw towards an octave-up saw
            }
            case 3:
            {
                const double pw = 0.5 - 0.45 * shape;   // shape narrows the pulse
                float v = p < pw ? 1.0f : -1.0f;
                v += polyBlep (p, dt);
                v -= polyBlep (std::fmod (p + 1.0 - pw, 1.0), dt);
                return v * 0.8f;
            }
            default: return noise;
        }
    }

    struct Env
    {
        enum Stage { Idle, Attack, Decay, Sustain, Release };
        Stage stage = Idle;
        float level = 0.0f;
        float a = 0.01f, d = 0.2f, s = 0.8f, r = 0.2f;   // seconds, sustain 0..1
        double sr = 44100.0;

        void set (float att, float dec, float sus, float rel) { a = att; d = dec; s = sus; r = rel; }
        void on()  { stage = Attack; }
        void off() { if (stage != Idle) stage = Release; }
        bool active() const { return stage != Idle; }
        float next()
        {
            switch (stage)
            {
                case Attack:
                    level += (float) (1.0 / juce::jmax (1.0, a * sr));
                    if (level >= 1.0f) { level = 1.0f; stage = Decay; }
                    break;
                case Decay:
                {
                    const float k = (float) std::exp (-4.6 / juce::jmax (1.0, d * sr));   // ~99% of the way in d seconds
                    level = s + (level - s) * k;
                    if (std::abs (level - s) < 1.0e-4f) { level = s; stage = Sustain; }
                    break;
                }
                case Sustain: level = s; break;
                case Release:
                {
                    const float k = (float) std::exp (-4.6 / juce::jmax (1.0, r * sr));
                    level *= k;
                    if (level < 1.0e-4f) { level = 0.0f; stage = Idle; }
                    break;
                }
                case Idle: level = 0.0f; break;
            }
            return level;
        }
    };

    struct Patch { const char* name; std::vector<std::pair<const char*, float>> values; };

    const std::vector<Patch>& patches()
    {
        // wave indices: 0 sine, 1 triangle, 2 saw, 3 square, 4 noise
        static const std::vector<Patch> list {
            { "Saw Lead",    { { "o1wave", 2 }, { "o2wave", 2 }, { "o2fine", 8 }, { "unison", 3 }, { "detune", 12 }, { "cutoff", 3500 }, { "reso", 30 },
                               { "fenv", 35 }, { "fd", 300 }, { "fs", 20 }, { "aa", 5 }, { "ad", 150 }, { "as", 80 }, { "ar", 200 }, { "volume", -8 } } },
            { "Soft Pad",    { { "o1wave", 2 }, { "o2wave", 2 }, { "o2fine", 15 }, { "unison", 5 }, { "detune", 25 }, { "width", 80 }, { "cutoff", 1400 },
                               { "reso", 10 }, { "aa", 600 }, { "ad", 500 }, { "as", 85 }, { "ar", 1200 }, { "lfowave", 0 }, { "lforate", 0.3f },
                               { "lfodepth", 25 }, { "lfotarget", 1 }, { "volume", -10 } } },
            { "Pluck",       { { "o1wave", 3 }, { "o2wave", 2 }, { "cutoff", 600 }, { "fenv", 60 }, { "fd", 300 }, { "fs", 0 },
                               { "aa", 1 }, { "ad", 300 }, { "as", 0 }, { "ar", 200 }, { "volume", -6 } } },
            { "Bass",        { { "o1wave", 2 }, { "o2wave", 3 }, { "o2oct", -1 }, { "o2level", 45 }, { "cutoff", 500 }, { "reso", 35 }, { "fenv", 25 },
                               { "fd", 250 }, { "fs", 10 }, { "aa", 2 }, { "ad", 250 }, { "as", 70 }, { "ar", 100 }, { "volume", -5 } } },
            { "E-Piano",     { { "o1wave", 0 }, { "o2wave", 1 }, { "o2oct", 1 }, { "o2level", 25 }, { "cutoff", 6000 },
                               { "aa", 2 }, { "ad", 1200 }, { "as", 25 }, { "ar", 400 }, { "volume", -5 } } },
            { "Square Keys", { { "o1wave", 3 }, { "o2wave", 3 }, { "o2fine", 5 }, { "cutoff", 2500 }, { "aa", 3 }, { "ad", 300 }, { "as", 60 },
                               { "ar", 300 }, { "volume", -9 } } },
            { "Sub Bass",    { { "o1wave", 0 }, { "o2wave", 1 }, { "o2level", 15 }, { "cutoff", 300 }, { "aa", 3 }, { "ad", 100 }, { "as", 100 },
                               { "ar", 120 }, { "volume", -3 } } },
            { "Super Saw",   { { "o1wave", 2 }, { "o2wave", 2 }, { "o2oct", 1 }, { "o2level", 30 }, { "unison", 7 }, { "detune", 35 }, { "width", 100 },
                               { "cutoff", 8000 }, { "aa", 10 }, { "ad", 300 }, { "as", 90 }, { "ar", 400 }, { "volume", -11 } } },
            { "Wobble Bass", { { "o1wave", 2 }, { "o2wave", 3 }, { "o2oct", -1 }, { "o2level", 60 }, { "cutoff", 300 }, { "reso", 55 }, { "drive", 40 },
                               { "lfowave", 0 }, { "lforate", 4 }, { "lfodepth", 80 }, { "lfotarget", 1 }, { "aa", 2 }, { "as", 100 }, { "ar", 80 },
                               { "volume", -6 } } },
            { "Bell",        { { "o1wave", 0 }, { "o2wave", 0 }, { "o2oct", 1 }, { "o2semi", 7 }, { "o2level", 60 }, { "o2shape", 30 }, { "cutoff", 9000 },
                               { "aa", 1 }, { "ad", 1800 }, { "as", 0 }, { "ar", 1500 }, { "volume", -7 } } },
            { "Brass",       { { "o1wave", 2 }, { "o2wave", 2 }, { "o2fine", 6 }, { "cutoff", 900 }, { "fenv", 45 }, { "fa", 80 }, { "fd", 400 },
                               { "fs", 40 }, { "aa", 40 }, { "ad", 200 }, { "as", 80 }, { "ar", 250 }, { "lfowave", 0 }, { "lforate", 5 },
                               { "lfodepth", 8 }, { "lfotarget", 0 }, { "volume", -8 } } },
            { "Warm Strings",{ { "o1wave", 2 }, { "o2wave", 2 }, { "o2fine", 9 }, { "unison", 5 }, { "detune", 18 }, { "width", 70 }, { "cutoff", 2500 },
                               { "aa", 300 }, { "ad", 400 }, { "as", 90 }, { "ar", 900 }, { "lfowave", 0 }, { "lforate", 5.5f }, { "lfodepth", 6 },
                               { "lfotarget", 0 }, { "volume", -10 } } },
            { "Acid Ladder", { { "o1wave", 2 }, { "o2level", 0 }, { "ftype", 4 }, { "cutoff", 380 }, { "reso", 78 }, { "fenv", 55 }, { "fd", 220 },
                               { "fs", 0 }, { "drive", 35 }, { "glide", 60 }, { "aa", 1 }, { "ad", 300 }, { "as", 70 }, { "ar", 90 },
                               { "m1src", SrcVelocity }, { "m1dst", DstFilter }, { "m1amt", 25 }, { "volume", -7 } } },
            { "Evolving Pad",{ { "o1wave", 2 }, { "o2wave", 1 }, { "o2oct", 1 }, { "o2level", 40 }, { "unison", 5 }, { "detune", 22 }, { "width", 90 },
                               { "ftype", 5 }, { "cutoff", 1100 }, { "reso", 25 }, { "aa", 900 }, { "ad", 800 }, { "as", 85 }, { "ar", 1800 },
                               { "lfowave", 1 }, { "lforate", 0.15f }, { "lfodepth", 35 }, { "lfotarget", 1 },
                               { "lfo2wave", 0 }, { "lfo2rate", 0.08f }, { "lfo2depth", 0 },
                               { "m1src", SrcLfo2 }, { "m1dst", DstShape1 }, { "m1amt", 60 },
                               { "m2src", SrcRandom }, { "m2dst", DstPan }, { "m2amt", 45 },
                               { "m3src", SrcLfo2 }, { "m3dst", DstOscMix }, { "m3amt", 50 }, { "volume", -6 } } },
            { "Vibrato Lead",{ { "o1wave", 3 }, { "o1shape", 30 }, { "o2wave", 2 }, { "o2fine", 7 }, { "o2level", 40 }, { "ftype", 4 }, { "cutoff", 2400 },
                               { "reso", 30 }, { "fenv", 20 }, { "glide", 80 }, { "aa", 8 }, { "ad", 200 }, { "as", 85 }, { "ar", 220 },
                               { "lfo2wave", 0 }, { "lfo2rate", 5.5f }, { "lfo2depth", 0 },
                               { "m1src", SrcLfo2 }, { "m1dst", DstPitch }, { "m1amt", 3 },
                               { "m2src", SrcModWheel }, { "m2dst", DstLfo2Amount }, { "m2amt", 100 },
                               { "m3src", SrcAftertouch }, { "m3dst", DstFilter }, { "m3amt", 30 }, { "volume", -9 } } } };
        return list;
    }
}

const std::vector<SynthPatchInfo>& getSynthPatches()
{
    static const std::vector<SynthPatchInfo> names = []
    {
        std::vector<SynthPatchInfo> v;
        for (auto& p : patches()) v.push_back ({ p.name });
        return v;
    }();
    return names;
}

// =============================================================================
class SynthFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    SynthFx() : BuiltinFx ("synth", "Synth", layout())
    {
        state.state.setProperty ("patch", "Saw Lead", nullptr);
        loadPatch (0);
    }

    bool acceptsMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 8.0; }
    juce::AudioProcessorEditor* createEditor() override;

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choiceParam ("o1wave", "Osc 1 Wave", oscWaves, 2));
        l.add (intParam ("o1oct", "Osc 1 Octave", -2, 2, 0, "oct", true));
        l.add (floatParam ("o1shape", "Osc 1 Shape", Range (0, 100), 0, "%"));
        l.add (floatParam ("o1level", "Osc 1 Level", Range (0, 100), 80, "%"));
        l.add (choiceParam ("o2wave", "Osc 2 Wave", oscWaves, 2));
        l.add (intParam ("o2oct", "Osc 2 Octave", -2, 2, 0, "oct", true));
        l.add (intParam ("o2semi", "Osc 2 Semitones", -12, 12, 0, "st", true));
        l.add (floatParam ("o2fine", "Osc 2 Fine", Range (-50, 50), 0, "ct"));
        l.add (floatParam ("o2shape", "Osc 2 Shape", Range (0, 100), 0, "%"));
        l.add (floatParam ("o2level", "Osc 2 Level", Range (0, 100), 50, "%"));
        l.add (floatParam ("sub", "Sub", Range (0, 100), 0, "%"));
        l.add (floatParam ("noise", "Noise", Range (0, 100), 0, "%"));
        l.add (intParam ("unison", "Unison", 1, 7, 1, "", false));
        l.add (floatParam ("detune", "Detune", Range (0, 100), 15, "ct"));
        l.add (floatParam ("width", "Width", Range (0, 100), 60, "%"));
        l.add (floatParam ("glide", "Glide", skewed (0, 1000, 120), 0, "ms"));
        l.add (choiceParam ("ftype", "Filter Type", filterTypes, 0));
        l.add (floatParam ("cutoff", "Cutoff", skewed (20, 20000, 1000), 4000, "Hz"));
        l.add (floatParam ("reso", "Resonance", Range (0, 100), 20, "%"));
        l.add (floatParam ("fenv", "Filter Env", Range (-100, 100), 0, "%"));
        l.add (floatParam ("fkey", "Key Track", Range (0, 100), 30, "%"));
        l.add (floatParam ("drive", "Drive", Range (0, 100), 0, "%"));
        l.add (floatParam ("fa", "Filter Attack", skewed (0, 5000, 200), 2, "ms"));
        l.add (floatParam ("fd", "Filter Decay", skewed (1, 5000, 300), 350, "ms"));
        l.add (floatParam ("fs", "Filter Sustain", Range (0, 100), 15, "%"));
        l.add (floatParam ("fr", "Filter Release", skewed (1, 8000, 400), 300, "ms"));
        l.add (floatParam ("aa", "Attack", skewed (0, 5000, 200), 5, "ms"));
        l.add (floatParam ("ad", "Decay", skewed (1, 5000, 300), 200, "ms"));
        l.add (floatParam ("as", "Sustain", Range (0, 100), 80, "%"));
        l.add (floatParam ("ar", "Release", skewed (1, 8000, 400), 250, "ms"));
        l.add (choiceParam ("lfowave", "LFO Wave", lfoWaves, 0));
        l.add (floatParam ("lforate", "LFO Rate", skewed (0.05f, 20, 2), 2, "Hz"));
        l.add (floatParam ("lfodepth", "LFO Depth", Range (0, 100), 0, "%"));
        l.add (choiceParam ("lfotarget", "LFO Target", lfoTargets, 1));
        l.add (choiceParam ("lfo2wave", "LFO 2 Wave", lfoWaves, 0));
        l.add (floatParam ("lfo2rate", "LFO 2 Rate", skewed (0.05f, 20, 2), 0.5f, "Hz"));
        l.add (floatParam ("lfo2depth", "LFO 2 Depth", Range (0, 100), 0, "%"));
        l.add (choiceParam ("lfo2target", "LFO 2 Target", lfoTargets, 3));
        for (int i = 0; i < modSlots; ++i)
        {
            const juce::String n ("Mod " + juce::String (i + 1));
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { modSrcIds[i], 1 }, n + " Source", modSources, 0));
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { modDstIds[i], 1 }, n + " Destination", modDests, 0));
            l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { modAmtIds[i], 1 }, n + " Amount", Range (-100, 100), 0.0f,
                       juce::AudioParameterFloatAttributes().withLabel ("%")));
        }
        l.add (floatParam ("volume", "Volume", Range (-36, 6), -8, "dB"));
        return l;
    }

    void loadPatch (int index)
    {
        const auto& list = patches();
        if (list.empty()) return;
        const auto& patch = list[(size_t) juce::jlimit (0, (int) list.size() - 1, index)];
        for (auto* p : getParameters())
            if (auto* rp = dynamic_cast<juce::RangedAudioParameter*> (p)) rp->setValueNotifyingHost (rp->getDefaultValue());
        for (auto& [id, v] : patch.values)
            if (auto* rp = state.getParameter (id)) rp->setValueNotifyingHost (rp->convertTo0to1 (v));
        state.state.setProperty ("patch", patch.name, nullptr);
    }
    juce::String patchName() const { return state.state.getProperty ("patch", "Synth").toString(); }

    // ---- live readings for the editor ----
    static constexpr int scopeSize = 1024;
    std::array<std::atomic<float>, scopeSize> scope {};
    std::atomic<int> scopePos { 0 };
    std::atomic<float> ampLevel { 0 }, filterLevel { 0 }, liveCutoff { 0 }, lfoValue { 0 }, lfoPhase { 0 }, oscPhase { 0 };
    std::atomic<int> ampStage { 0 }, voicesPlaying { 0 };
    std::atomic<float> lfo2Value { 0 }, lfo2Phase { 0 }, modWheelNow { 0 }, aftertouchNow { 0 };
    std::array<std::atomic<float>, 4> slotNow {};   // what each mod slot is doing right now (newest note)
    std::atomic<bool> audition { false };
    std::atomic<int> auditionNote { 60 };

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& v : voices) v = Voice();
        lastFreq = 0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        buffer.clear();
        if (buffer.getNumChannels() < 2) return;
        readParams();

        if (audition.exchange (false)) { noteOn (auditionNote, 0.8f); auditionUntil = (int) (currentSampleRate * 0.6); }
        if (auditionUntil > 0 && (auditionUntil -= n) <= 0) noteOff (auditionNote);

        auto it = midi.cbegin();
        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            while (it != midi.cend() && (*it).samplePosition <= i)
            {
                const auto m = (*it).getMessage();
                if (m.isNoteOn()) noteOn (m.getNoteNumber(), m.getFloatVelocity());
                else if (m.isNoteOff()) noteOff (m.getNoteNumber());
                else if (m.isAllNotesOff() || m.isAllSoundOff()) for (auto& v : voices) { v.amp.off(); v.filt.off(); }
                else if (m.isControllerOfType (1)) modWheel = m.getControllerValue() / 127.0f;
                else if (m.isChannelPressure()) chanPressure = m.getChannelPressureValue() / 127.0f;
                else if (m.isAftertouch()) { for (auto& v : voices) if (v.note == m.getNoteNumber()) v.pressure = m.getAfterTouchValue() / 127.0f; }
                ++it;
            }
            if ((i & 15) == 0) tickControl();

            float l = 0, r = 0;
            for (auto& v : voices)
                if (v.amp.active()) renderVoice (v, l, r);
            L[i] = std::tanh (l * gain);
            R[i] = std::tanh (r * gain);

            if (++scopeDecim >= 2)
            {
                scopeDecim = 0;
                const int p = scopePos.load();
                scope[(size_t) p] = 0.5f * (L[i] + R[i]);
                scopePos = (p + 1) % scopeSize;
            }
        }

        // what the editor shows
        int playing = 0;
        const Voice* newest = nullptr;
        for (auto& v : voices)
            if (v.amp.active()) { ++playing; if (newest == nullptr || v.age > newest->age) newest = &v; }
        voicesPlaying = playing;
        if (newest != nullptr)
        {
            ampLevel = newest->amp.level; ampStage = (int) newest->amp.stage;
            filterLevel = newest->filt.level; liveCutoff = newest->cutoffNow;
            oscPhase = (float) newest->ph1[0];
        }
        else { ampLevel = 0; ampStage = 0; filterLevel = 0; liveCutoff = pCutoff; }
        lfoValue = lfoOut;
        lfoPhase = (float) lfoPh;
        lfo2Value = lfo2Out;
        lfo2Phase = (float) lfo2Ph;
        modWheelNow = modWheel; aftertouchNow = chanPressure;
        for (int sl = 0; sl < modSlots; ++sl) slotNow[(size_t) sl] = newest != nullptr ? newest->slotVal[sl] : globalSlotVal[sl];
    }

private:
    struct Voice
    {
        int note = -1;
        float vel = 0;
        juce::int64 age = 0;
        Env amp, filt;
        double freq = 440, target = 440;
        std::array<double, 7> ph1 {}, ph2 {};
        double phSub = 0;
        float s1[2][2] {}, s2[2][2] {};   // filter state [channel][stage]
        float g = 0.1f, k = 1.0f, cutoffNow = 1000;
        float noiseLp = 0;
        float ld[2][4] {};        // ladder stages
        float ladderOut[2] {};
        float pressure = 0, rndVal = 0;
        // set every 16 samples from the LFOs, envelopes and the mod matrix
        double mul1 = 1, mul2 = 1;
        float sh1 = 0, sh2 = 0, lvl1 = 0.8f, lvl2 = 0.5f, det = 15, volMod = 1, panL = 1, panR = 1, drive = 0;
        float slotVal[4] {};
    };
    std::array<Voice, 16> voices;
    juce::int64 counter = 0;
    double lastFreq = 0;
    double lfoPh = 0, lfo2Ph = 0;
    float lfoOut = 0, sh = 0, lfo2Out = 0, sh2 = 0;
    float modWheel = 0, chanPressure = 0;
    float globalSlotVal[4] {};
    int scopeDecim = 0, auditionUntil = 0;
    juce::Random rnd;

    // parameters for this block
    int pW1 = 2, pW2 = 2, pFType = 0, pLfoWave = 0, pLfoTarget = 1, pUnison = 1;
    float pOct1 = 0, pShape1 = 0, pLvl1 = 0.8f, pOct2 = 0, pSemi2 = 0, pFine2 = 0, pShape2 = 0, pLvl2 = 0.5f, pSub = 0, pNoise = 0;
    float pDetune = 15, pWidth = 0.6f, pGlide = 0, pCutoff = 4000, pReso = 0.2f, pFenv = 0, pKey = 0.3f, pDrive = 0;
    float pLfoRate = 2, pLfoDepth = 0, gain = 1;
    int pLfo2Wave = 0, pLfo2Target = 3;
    float pLfo2Rate = 0.5f, pLfo2Depth = 0;
    int mSrc[4] {}, mDst[4] {};
    float mAmt[4] {};

    void readParams()
    {
        pW1 = (int) param ("o1wave"); pOct1 = param ("o1oct"); pShape1 = param ("o1shape") * 0.01f; pLvl1 = param ("o1level") * 0.01f;
        pW2 = (int) param ("o2wave"); pOct2 = param ("o2oct"); pSemi2 = param ("o2semi"); pFine2 = param ("o2fine");
        pShape2 = param ("o2shape") * 0.01f; pLvl2 = param ("o2level") * 0.01f;
        pSub = param ("sub") * 0.01f; pNoise = param ("noise") * 0.01f;
        pUnison = juce::jlimit (1, 7, (int) std::lround (param ("unison"))); pDetune = param ("detune"); pWidth = param ("width") * 0.01f;
        pGlide = param ("glide"); pFType = (int) param ("ftype"); pCutoff = param ("cutoff"); pReso = param ("reso") * 0.01f;
        pFenv = param ("fenv") * 0.01f; pKey = param ("fkey") * 0.01f; pDrive = param ("drive") * 0.01f;
        pLfoWave = (int) param ("lfowave"); pLfoRate = param ("lforate"); pLfoDepth = param ("lfodepth") * 0.01f; pLfoTarget = (int) param ("lfotarget");
        pLfo2Wave = (int) param ("lfo2wave"); pLfo2Rate = param ("lfo2rate"); pLfo2Depth = param ("lfo2depth") * 0.01f; pLfo2Target = (int) param ("lfo2target");
        for (int i = 0; i < modSlots; ++i)
        {
            mSrc[i] = (int) param (modSrcIds[i]); mDst[i] = (int) param (modDstIds[i]); mAmt[i] = param (modAmtIds[i]) * 0.01f;
            if (mSrc[i] == SrcNone || mDst[i] == DstNone) mAmt[i] = 0;
        }
        gain = juce::Decibels::decibelsToGain (param ("volume")) / std::sqrt ((float) pUnison) * 0.7f;
        for (auto& v : voices)
        {
            v.amp.set (param ("aa") * 0.001f, param ("ad") * 0.001f, param ("as") * 0.01f, param ("ar") * 0.001f);
            v.filt.set (param ("fa") * 0.001f, param ("fd") * 0.001f, param ("fs") * 0.01f, param ("fr") * 0.001f);
            v.amp.sr = v.filt.sr = currentSampleRate;
        }
    }

    void noteOn (int note, float vel)
    {
        Voice* v = nullptr;
        for (auto& x : voices) if (x.note == note && x.amp.active()) v = &x;                 // same note: retrigger
        if (v == nullptr) for (auto& x : voices) if (! x.amp.active()) { v = &x; break; }
        if (v == nullptr)                                                                    // steal: released first, then oldest
            for (auto& x : voices)
                if (v == nullptr || (x.amp.stage == Env::Release) > (v->amp.stage == Env::Release)
                    || ((x.amp.stage == Env::Release) == (v->amp.stage == Env::Release) && x.age < v->age)) v = &x;
        const bool fresh = ! v->amp.active();
        v->note = note;
        v->vel = vel;
        v->age = ++counter;
        v->target = juce::MidiMessage::getMidiNoteInHertz (note);
        v->freq = (pGlide > 0.5f && lastFreq > 0) ? lastFreq : v->target;
        lastFreq = v->target;
        if (fresh)
        {
            for (int u = 0; u < 7; ++u) { v->ph1[(size_t) u] = rnd.nextDouble(); v->ph2[(size_t) u] = rnd.nextDouble(); }
            v->phSub = 0;
            std::memset (v->s1, 0, sizeof (v->s1)); std::memset (v->s2, 0, sizeof (v->s2));
            std::memset (v->ld, 0, sizeof (v->ld)); v->ladderOut[0] = v->ladderOut[1] = 0;
            v->pressure = 0;
            v->amp.level = 0; v->filt.level = 0;
        }
        v->rndVal = rnd.nextFloat() * 2.0f - 1.0f;   // the "Random" source: new for every note
        v->amp.on(); v->filt.on();
        updateVoiceMods (*v);   // so the first samples already sound right
    }

    void noteOff (int note)
    {
        for (auto& v : voices) if (v.note == note && v.amp.stage != Env::Release) { v.amp.off(); v.filt.off(); }
    }

    // the global sources (the same for every note)
    float globalSource (int src) const
    {
        switch (src)
        {
            case SrcLfo1: return lfoOut * lfo1Amount;
            case SrcLfo2: return lfo2Out * lfo2Amount;
            case SrcModWheel: return modWheel;
            case SrcAftertouch: return chanPressure;
            default: return 0.0f;
        }
    }
    float voiceSource (const Voice& v, int src) const
    {
        switch (src)
        {
            case SrcFiltEnv: return v.filt.level;
            case SrcAmpEnv: return v.amp.level;
            case SrcVelocity: return v.vel;
            case SrcNote: return (float) (v.note - 60) / 24.0f;
            case SrcAftertouch: return juce::jmax (chanPressure, v.pressure);
            case SrcRandom: return v.rndVal;
            default: return globalSource (src);
        }
    }
    float lfo1Amount = 0, lfo2Amount = 0;

    // every 16 samples: LFOs, glide, filter coefficients and everything the matrix moves
    void tickControl()
    {
        const double step = 16.0 / currentSampleRate;

        // LFO rates and amounts can be moved by the global sources (not per note)
        float r1 = 0, r2 = 0, a1 = 0, a2 = 0;
        for (int i = 0; i < modSlots; ++i)
        {
            if (mAmt[i] == 0.0f) continue;
            const float m = globalSource (mSrc[i]) * mAmt[i];
            globalSlotVal[i] = m;
            switch (mDst[i])
            {
                case DstLfo1Rate: r1 += m; break;
                case DstLfo2Rate: r2 += m; break;
                case DstLfo1Amount: a1 += m; break;
                case DstLfo2Amount: a2 += m; break;
                default: break;
            }
        }
        // LFO amount: normally full. A positive "Amount" route fades it in from nothing (mod wheel vibrato);
        // a negative one turns it down from full.
        bool fadeIn1 = false, fadeIn2 = false;
        for (int i = 0; i < modSlots; ++i)
        {
            fadeIn1 = fadeIn1 || (mDst[i] == DstLfo1Amount && mAmt[i] > 0);
            fadeIn2 = fadeIn2 || (mDst[i] == DstLfo2Amount && mAmt[i] > 0);
        }
        lfo1Amount = juce::jlimit (0.0f, 1.0f, (fadeIn1 ? 0.0f : 1.0f) + a1);
        lfo2Amount = juce::jlimit (0.0f, 1.0f, (fadeIn2 ? 0.0f : 1.0f) + a2);

        auto advance = [&] (double& ph, float rate, int wave, float& held, float& out)
        {
            const double prev = ph;
            ph += rate * step;
            if (ph >= 1.0) ph -= 1.0;
            if (wave == 4 && ph < prev) held = rnd.nextFloat() * 2.0f - 1.0f;
            out = lfoShape (wave, ph, held);
        };
        advance (lfoPh, pLfoRate * std::pow (2.0f, r1 * 3.0f), pLfoWave, sh, lfoOut);
        advance (lfo2Ph, pLfo2Rate * std::pow (2.0f, r2 * 3.0f), pLfo2Wave, sh2, lfo2Out);

        const float glideK = pGlide > 0.5f ? (float) std::exp (-step / (pGlide * 0.001 / 4.6)) : 0.0f;
        for (auto& v : voices)
        {
            if (! v.amp.active()) continue;
            v.freq = v.target + (v.freq - v.target) * glideK;
            updateVoiceMods (v);
        }
    }

    void updateVoiceMods (Voice& v)
    {
        float m[NumDst] {};
        for (int i = 0; i < modSlots; ++i)
        {
            if (mAmt[i] == 0.0f) { v.slotVal[i] = 0; continue; }
            const float val = voiceSource (v, mSrc[i]) * mAmt[i];
            v.slotVal[i] = val;
            m[mDst[i]] += val;
        }
        // the two LFOs' own quick routes (amounts as before)
        float trem = 1.0f;
        auto quick = [&] (int target, float out, float depth)
        {
            if (depth <= 0) return;
            switch (target)
            {
                case 0: m[DstPitch] += out * depth * 2.0f / 12.0f; break;   // up to 2 semitones
                case 1: m[DstFilter] += out * depth * 0.5f; break;          // up to 2.5 octaves
                case 2: trem *= 1.0f - depth * 0.5f * (1.0f - out); break;
                case 3: m[DstShape1] += out * depth * 0.5f; m[DstShape2] += out * depth * 0.5f; break;
                case 4: m[DstPan] += out * depth; break;
                default: break;
            }
        };
        quick (pLfoTarget, lfoOut * lfo1Amount, pLfoDepth);
        quick (pLfo2Target, lfo2Out * lfo2Amount, pLfo2Depth);

        // pitch: matrix 100% = an octave
        v.mul1 = std::pow (2.0, (double) pOct1 + m[DstPitch]);
        v.mul2 = std::pow (2.0, (double) pOct2 + (pSemi2 + pFine2 * 0.01) / 12.0 + m[DstPitch] + m[DstOsc2Pitch]);
        v.sh1 = juce::jlimit (0.0f, 1.0f, pShape1 + m[DstShape1]);
        v.sh2 = juce::jlimit (0.0f, 1.0f, pShape2 + m[DstShape2]);
        const float mix = juce::jlimit (-1.0f, 1.0f, m[DstOscMix]);   // + towards osc 2, - towards osc 1
        v.lvl1 = pLvl1 * (1.0f - juce::jmax (0.0f, mix));
        v.lvl2 = pLvl2 * (1.0f + juce::jmin (0.0f, mix));
        v.det = juce::jlimit (0.0f, 100.0f, pDetune + m[DstDetune] * 100.0f);
        v.drive = juce::jlimit (0.0f, 1.0f, pDrive + m[DstDrive]);
        v.volMod = trem * juce::jlimit (0.0f, 2.0f, 1.0f + m[DstVolume]);
        const float pan = juce::jlimit (-1.0f, 1.0f, m[DstPan]);
        v.panL = 1.0f - juce::jmax (0.0f, pan); v.panR = 1.0f + juce::jmin (0.0f, pan);

        // filter: matrix 100% = 5 octaves
        const float oct = pFenv * 5.0f * v.filt.level + pKey * (float) (v.note - 60) / 12.0f + m[DstFilter] * 5.0f;
        const float fc = juce::jlimit (20.0f, (float) currentSampleRate * 0.45f, pCutoff * std::pow (2.0f, oct));
        v.cutoffNow = fc;
        v.g = std::tan (juce::MathConstants<float>::pi * fc / (float) currentSampleRate);
        const float reso = juce::jlimit (0.0f, 1.0f, pReso + m[DstReso]);
        v.k = pFType == 4 ? 3.9f * reso : 2.0f - 1.96f * reso;   // the ladder uses feedback (4 = it sings), the others damping
    }

    // a 4-stage transistor-ladder style low-pass: saturating input and feedback, sings at high resonance
    float ladder (float x, Voice& v, int ch, float scale)
    {
        const float G = v.g / (1.0f + v.g);
        float u = softClip ((x - v.k * v.ladderOut[ch]) * scale) / scale;
        auto& st = v.ld[ch];
        for (int i = 0; i < 4; ++i)
        {
            const float vv = (u - st[i]) * G;
            const float y = vv + st[i];
            st[i] = y + vv;
            u = y;
        }
        v.ladderOut[ch] = u;
        return u * (1.0f + v.k * 0.45f);   // keep the low end as resonance goes up
    }

    float svf (float x, float (&s)[2], float g, float k, int mode)
    {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float v3 = x - s[1];
        const float v1 = a1 * s[0] + g * a1 * v3;
        const float v2 = s[1] + g * v1;
        s[0] = 2.0f * v1 - s[0];
        s[1] = 2.0f * v2 - s[1];
        return mode == 0 ? v2 : mode == 1 ? x - k * v1 - v2 : mode == 2 ? v1 : x - k * v1;   // low / high / band / notch
    }

    void renderVoice (Voice& v, float& outL, float& outR)
    {
        const double sr = currentSampleRate;
        const double f1 = v.freq * v.mul1;
        const double f2 = v.freq * v.mul2;

        // white noise, softened a little for the noise oscillator
        const float white = rnd.nextFloat() * 2.0f - 1.0f;
        v.noiseLp += 0.5f * (white - v.noiseLp);

        float l = 0, r = 0;
        for (int u = 0; u < pUnison; ++u)
        {
            const float pos = pUnison > 1 ? (float) u / (float) (pUnison - 1) * 2.0f - 1.0f : 0.0f;
            const double det = pUnison > 1 ? std::exp2 (pos * v.det * 0.01 / 12.0) : 1.0;
            auto& a = v.ph1[(size_t) u];
            auto& b = v.ph2[(size_t) u];
            const double d1 = f1 * det / sr, d2 = f2 * det / sr;
            float s = 0;
            if (v.lvl1 > 0) s += v.lvl1 * oscSample (pW1, a, d1, v.sh1, v.noiseLp);
            if (v.lvl2 > 0) s += v.lvl2 * oscSample (pW2, b, d2, v.sh2, white);
            a += d1; if (a >= 1.0) a -= 1.0;
            b += d2; if (b >= 1.0) b -= 1.0;
            const float pan = pos * pWidth;
            l += s * (1.0f - juce::jmax (0.0f, pan));
            r += s * (1.0f + juce::jmin (0.0f, pan));
        }
        // sub (an octave below osc 1) and noise sit in the middle
        const float uniScale = std::sqrt ((float) pUnison);
        const float sub = pSub > 0 ? pSub * (float) std::sin (v.phSub * juce::MathConstants<double>::twoPi) * uniScale : 0.0f;
        v.phSub += f1 * 0.5 / sr; if (v.phSub >= 1.0) v.phSub -= 1.0;
        const float nz = pNoise * white * 0.5f * uniScale;
        l += sub + nz; r += sub + nz;

        if (v.drive > 0) { const float dr = 1.0f + v.drive * 6.0f; l = std::tanh (l * dr) / std::sqrt (dr); r = std::tanh (r * dr) / std::sqrt (dr); }

        float yl, yr;
        const float scale = 1.0f / uniScale;   // the saturating filters see one voice's level, however many unison voices
        switch (pFType)
        {
            case 4:   // Ladder
                yl = ladder (l, v, 0, scale); yr = ladder (r, v, 1, scale);
                break;
            case 5:   // Warm LP: two stages with soft saturation going into each
                yl = svf (softClip (l * scale) / scale, v.s1[0], v.g, v.k, 0); yr = svf (softClip (r * scale) / scale, v.s1[1], v.g, v.k, 0);
                yl = svf (softClip (yl * scale) / scale, v.s2[0], v.g, v.k * 1.3f, 0); yr = svf (softClip (yr * scale) / scale, v.s2[1], v.g, v.k * 1.3f, 0);
                break;
            default:
            {
                const int mode = pFType <= 1 ? 0 : pFType == 2 ? 1 : pFType == 3 ? 2 : 3;
                yl = svf (l, v.s1[0], v.g, v.k, mode); yr = svf (r, v.s1[1], v.g, v.k, mode);
                if (pFType == 0) { yl = svf (yl, v.s2[0], v.g, v.k * 1.3f, 0); yr = svf (yr, v.s2[1], v.g, v.k * 1.3f, 0); }
            }
        }

        v.filt.next();
        const float env = v.amp.next() * (0.3f + 0.7f * v.vel) * v.volMod;
        outL += yl * env * v.panL;
        outR += yr * env * v.panR;
        if (! v.amp.active()) v.note = -1;
    }

    friend class SynthEditor;
};

std::unique_ptr<BuiltinFx> createSynth() { return std::make_unique<SynthFx>(); }

void loadSynthPatch (juce::AudioProcessor* p, int index)
{
    if (auto* s = dynamic_cast<SynthFx*> (p)) s->loadPatch (index);
}

juce::String getSynthPatchName (const juce::AudioProcessor* p)
{
    if (auto* s = dynamic_cast<const SynthFx*> (p)) return s->patchName();
    return {};
}

// =============================================================================
//  The editor: every section draws what it does, live
// =============================================================================
namespace
{
    const juce::Colour bgCol (0xff0e0f12), panelCol (0xff16181d), screenCol (0xff07080a), lineCol (0xff2a2d35),
                       textCol (0xffeceff4), dimCol (0xff8a8f9c);
    const juce::Colour oscA (0xffff6b5b), oscB (0xff3fd6c8), filtCol (0xffffb547), envCol (0xffa58bff), lfoCol (0xffb6f24a), ampCol (0xff6fb7ff),
                       lfo2Col (0xfff2d24a), modCol (0xffff5fb8);

    Skin skinFor (juce::Colour c)
    {
        return { KnobStyle::Neon, juce::Colour (0xff1b1d23), c.brighter (0.4f), c, juce::Colour (0xff262932),
                 textCol, dimCol, juce::Colour (0xff0b0c0f), textCol };
    }

    void screen (juce::Graphics& g, juce::Rectangle<float> r)
    {
        g.setColour (screenCol);
        g.fillRoundedRectangle (r, 6.0f);
        g.setColour (juce::Colours::white.withAlpha (0.035f));
        for (float y = r.getY() + 3; y < r.getBottom(); y += 3) g.drawHorizontalLine ((int) y, r.getX() + 2, r.getRight() - 2);
    }

    void glowPath (juce::Graphics& g, const juce::Path& p, juce::Colour c, float w = 2.0f)
    {
        g.setColour (c.withAlpha (0.22f)); g.strokePath (p, juce::PathStrokeType (w * 3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);                   g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    float norm (BuiltinFx& fx, const char* id) { auto* p = fx.state.getParameter (id); return p->getValue(); }
    void setNorm (BuiltinFx& fx, const char* id, float v) { auto* p = fx.state.getParameter (id); p->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, v)); }
}

// a mod amount: a bar from the middle, left = negative, right = positive, plus a line for what it is doing now
class ModAmountSlider : public juce::Slider
{
public:
    ModAmountSlider() : juce::Slider (juce::Slider::LinearHorizontal, juce::Slider::NoTextBox) {}
    float live = 0;   // -1..1
    juce::Colour colour { 0xffb6f24a };
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1, 4);
        g.setColour (juce::Colour (0xff0b0c0f)); g.fillRoundedRectangle (r, 4.0f);
        const float v = (float) getValue() / 100.0f, mid = r.getCentreX();
        auto bar = v >= 0 ? juce::Rectangle<float> (mid, r.getY(), r.getWidth() * 0.5f * v, r.getHeight())
                          : juce::Rectangle<float> (mid + r.getWidth() * 0.5f * v, r.getY(), -r.getWidth() * 0.5f * v, r.getHeight());
        g.setColour (colour.withAlpha (isEnabled() ? 0.55f : 0.2f)); g.fillRoundedRectangle (bar, 3.0f);
        g.setColour (juce::Colours::white.withAlpha (0.25f)); g.drawVerticalLine ((int) mid, r.getY(), r.getBottom());
        if (std::abs (live) > 0.001f)
        {
            const float x = mid + juce::jlimit (-1.0f, 1.0f, live) * r.getWidth() * 0.5f;
            g.setColour (juce::Colours::white); g.fillRoundedRectangle (x - 1.5f, r.getY() - 2, 3.0f, r.getHeight() + 4, 1.5f);
        }
        g.setColour (juce::Colour (0xffeceff4)); g.setFont (theme::font (10.5f, true));
        g.drawText ((v > 0 ? "+" : "") + juce::String (juce::roundToInt (v * 100)) + "%", r.reduced (6, 0), v >= 0 ? juce::Justification::centredLeft : juce::Justification::centredRight);
    }
};

class SynthEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SynthEditor (SynthFx& p) : AudioProcessorEditor (p), fx (p),
        lookA (skinFor (oscA)), lookB (skinFor (oscB)), lookF (skinFor (filtCol)), lookE (skinFor (envCol)),
        lookL (skinFor (lfoCol)), lookM (skinFor (ampCol)), lookS (skinFor (juce::Colour (0xffd8a96e))), lookL2 (skinFor (lfo2Col)), lookX (skinFor (modCol))
    {
        setLookAndFeel (&lookS);
        // patch picker
        addAndMakeVisible (patchBox);
        for (int i = 0; i < (int) getSynthPatches().size(); ++i) patchBox.addItem (getSynthPatches()[(size_t) i].name, i + 1);
        patchBox.setTextWhenNothingSelected ("Choose a sound");
        patchBox.onChange = [this] { if (patchBox.getSelectedId() > 0) fx.loadPatch (patchBox.getSelectedId() - 1); };
        addAndMakeVisible (playButton);
        playButton.setTooltip ("Hear the sound (C4)");
        playButton.onClick = [this] { fx.auditionNote = 60; fx.audition = true; };

        // knobs, coloured by section
        auto k = [this] (const char* id, const char* name, juce::LookAndFeel& lf) -> Knob*
        {
            auto* kn = knobs.add (new Knob (fx, id, name, 56));
            kn->setLookAndFeel (&lf);
            kn->label.setColour (juce::Label::textColourId, dimCol);
            kn->label.setFont (theme::font (10.0f, true));
            addAndMakeVisible (kn);
            return kn;
        };
        auto sw = [this] (const char* id, juce::LookAndFeel& lf) -> Switch*
        {
            auto* s = switches.add (new Switch (fx, id));
            s->setLookAndFeel (&lf);
            addAndMakeVisible (s);
            return s;
        };
        o1 = { sw ("o1wave", lookA), { k ("o1oct", "OCTAVE", lookA), k ("o1shape", "SHAPE", lookA), k ("o1level", "LEVEL", lookA) } };
        o2 = { sw ("o2wave", lookB), { k ("o2oct", "OCTAVE", lookB), k ("o2semi", "SEMI", lookB), k ("o2fine", "FINE", lookB),
                                       k ("o2shape", "SHAPE", lookB), k ("o2level", "LEVEL", lookB) } };
        voiceKnobs = { k ("unison", "VOICES", lookS), k ("detune", "DETUNE", lookS), k ("width", "WIDTH", lookS),
                       k ("sub", "SUB", lookS), k ("noise", "NOISE", lookS), k ("glide", "GLIDE", lookS) };
        filterType = sw ("ftype", lookF);
        filterKnobs = { k ("cutoff", "CUTOFF", lookF), k ("reso", "RESONANCE", lookF), k ("fenv", "ENV AMT", lookF),
                        k ("fkey", "KEY TRACK", lookF), k ("drive", "DRIVE", lookF) };
        fEnvKnobs = { k ("fa", "ATTACK", lookE), k ("fd", "DECAY", lookE), k ("fs", "SUSTAIN", lookE), k ("fr", "RELEASE", lookE) };
        aEnvKnobs = { k ("aa", "ATTACK", lookM), k ("ad", "DECAY", lookM), k ("as", "SUSTAIN", lookM), k ("ar", "RELEASE", lookM) };
        lfoWave = sw ("lfowave", lookL);
        lfoTarget = sw ("lfotarget", lookL);
        lfoKnobs = { k ("lforate", "RATE", lookL), k ("lfodepth", "DEPTH", lookL) };
        lfo2Wave = sw ("lfo2wave", lookL2);
        lfo2Target = sw ("lfo2target", lookL2);
        lfo2Knobs = { k ("lfo2rate", "RATE", lookL2), k ("lfo2depth", "DEPTH", lookL2) };
        volume.setSliderStyle (juce::Slider::LinearHorizontal);
        volume.setTextBoxStyle (juce::Slider::TextBoxRight, false, 62, 20);
        volume.setLookAndFeel (&lookS);
        volume.setColour (juce::Slider::trackColourId, juce::Colour (0xffd8a96e));
        volume.setColour (juce::Slider::backgroundColourId, juce::Colour (0xff262932));
        volume.setColour (juce::Slider::thumbColourId, textCol);
        addAndMakeVisible (volume);
        volumeAttach = std::make_unique<APVTS::SliderAttachment> (fx.state, "volume", volume);

        // LFO 1 / LFO 2 tabs
        for (int i = 0; i < 2; ++i)
        {
            auto& t = lfoTab[i];
            t.setButtonText (i == 0 ? "LFO 1" : "LFO 2");
            t.setClickingTogglesState (true);
            t.setRadioGroupId (4711);
            t.setLookAndFeel (i == 0 ? &lookL : &lookL2);
            t.onClick = [this] { showLfo (lfoTab[1].getToggleState() ? 1 : 0); };
            addAndMakeVisible (t);
        }
        lfoTab[0].setToggleState (true, juce::dontSendNotification);
        showLfo (0);

        // the mod matrix
        for (int i = 0; i < modSlots; ++i)
        {
            auto& sl = slots[(size_t) i];
            sl.src.addItemList (modSources, 1);
            sl.dst.addItemList (modDests, 1);
            for (auto* cb : { &sl.src, &sl.dst }) { cb->setLookAndFeel (&lookX); addAndMakeVisible (*cb); }
            sl.srcAttach = std::make_unique<APVTS::ComboBoxAttachment> (fx.state, modSrcIds[i], sl.src);
            sl.dstAttach = std::make_unique<APVTS::ComboBoxAttachment> (fx.state, modDstIds[i], sl.dst);
            sl.amount.colour = modCol;
            sl.amount.setDoubleClickReturnValue (true, 0.0);
            addAndMakeVisible (sl.amount);
            sl.amtAttach = std::make_unique<APVTS::SliderAttachment> (fx.state, modAmtIds[i], sl.amount);
        }

        setSize (1240, 770);
        timerCallback();   // show the current sound's name straight away
        startTimerHz (40);
    }

    ~SynthEditor() override
    {
        for (auto* kn : knobs) kn->setLookAndFeel (nullptr);
        for (auto* s : switches) s->setLookAndFeel (nullptr);
        for (auto& t : lfoTab) t.setLookAndFeel (nullptr);
        volume.setLookAndFeel (nullptr);
        for (auto& sl : slots) { sl.src.setLookAndFeel (nullptr); sl.dst.setLookAndFeel (nullptr); }
        setLookAndFeel (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (bgCol);
        g.setColour (juce::Colours::white.withAlpha (0.015f));
        for (int x = 0; x < getWidth(); x += 4) g.drawVerticalLine (x, 0, (float) getHeight());

        // header
        g.setFont (juce::Font (juce::FontOptions (26.0f, juce::Font::bold)).withExtraKerningFactor (0.12f));
        juce::ColourGradient tg (oscA, 24, 0, oscB, 260, 0, false);
        g.setGradientFill (tg);
        g.drawText ("RENASYNTH", 22, 10, 260, 36, juce::Justification::centredLeft);
        g.setColour (dimCol);
        g.setFont (theme::font (10.5f, true));
        g.drawText ("2 OSC  /  SUB  /  NOISE  /  UNISON  /  7 FILTERS  /  2 ENV  /  2 LFO  /  MOD MATRIX", 210, 10, 560, 36, juce::Justification::centredLeft);
        g.drawText ("SOUND", patchBox.getBounds().translated (-50, 0).withWidth (46), juce::Justification::centredRight);
        g.drawText ("VOLUME", volume.getBounds().translated (-56, 0).withWidth (52), juce::Justification::centredRight);

        for (auto& s : sections)
        {
            g.setColour (panelCol);
            g.fillRoundedRectangle (s.area.toFloat(), 8.0f);
            g.setColour (s.colour);
            g.fillRoundedRectangle (s.area.toFloat().withHeight (3.0f).reduced (8, 0), 1.5f);
            g.setFont (theme::font (11.5f, true));
            g.drawText (s.title, s.area.withHeight (24).reduced (12, 0), juce::Justification::centredLeft);
        }

        drawOsc (g, oscScreen1, (int) fx.param ("o1wave"), fx.param ("o1shape") * 0.01f, fx.param ("o1level") * 0.01f, oscA, 2.0 + fx.param ("o1oct"));
        drawOsc (g, oscScreen2, (int) fx.param ("o2wave"), fx.param ("o2shape") * 0.01f, fx.param ("o2level") * 0.01f, oscB,
                 2.0 + fx.param ("o2oct") + fx.param ("o2semi") / 12.0);
        drawVoices (g, voiceScreen);
        drawFilter (g, filterScreen);
        drawEnv (g, fEnvScreen, "fa", "fd", "fs", "fr", envCol, fx.filterLevel.load(), -1);
        drawEnv (g, aEnvScreen, "aa", "ad", "as", "ar", ampCol, fx.ampLevel.load(), fx.ampStage.load());
        drawLfo (g, lfoScreen, shownLfo);
        drawScope (g, scopeScreen);
        drawMatrix (g);
    }

    void resized() override
    {
        sections.clear();
        auto r = getLocalBounds().reduced (14, 10);
        auto head = r.removeFromTop (44);
        volume.setBounds (head.removeFromRight (200).reduced (0, 10));
        head.removeFromRight (64);
        playButton.setBounds (head.removeFromRight (40).reduced (2, 6));
        head.removeFromRight (6);
        patchBox.setBounds (head.removeFromRight (230).reduced (0, 8));
        r.removeFromTop (6);

        const int rowH = (r.getHeight() - 16) / 3;
        auto row1 = r.removeFromTop (rowH); r.removeFromTop (8);
        auto row2 = r.removeFromTop (rowH); r.removeFromTop (8);
        auto row3 = r;

        auto knobRow = [] (juce::Rectangle<int> area, std::vector<Knob*> ks)
        {
            const int w = area.getWidth() / juce::jmax (1, (int) ks.size());
            for (auto* kn : ks) kn->setBounds (area.removeFromLeft (w).withSizeKeepingCentre (juce::jmin (w, 74), area.getHeight()));
        };
        auto section = [this] (juce::Rectangle<int> area, const char* title, juce::Colour c)
        {
            sections.push_back ({ area, title, c });
            return area.reduced (10, 0).withTrimmedTop (26).withTrimmedBottom (8);
        };

        // row 1: oscillators and voices
        {
            auto a = section (row1.removeFromLeft (330), "OSCILLATOR 1", oscA); row1.removeFromLeft (8);
            oscScreen1 = a.removeFromTop (a.getHeight() - 112).toFloat();
            a.removeFromTop (4);
            o1.wave->setBounds (a.removeFromTop (24));
            a.removeFromTop (4);
            knobRow (a, o1.knobs);

            auto b = section (row1.removeFromLeft (430), "OSCILLATOR 2", oscB); row1.removeFromLeft (8);
            oscScreen2 = b.removeFromTop (b.getHeight() - 112).toFloat();
            b.removeFromTop (4);
            o2.wave->setBounds (b.removeFromTop (24));
            b.removeFromTop (4);
            knobRow (b, o2.knobs);

            auto c = section (row1, "VOICES  /  SUB  /  NOISE", juce::Colour (0xffd8a96e));
            voiceScreen = c.removeFromTop (c.getHeight() - 84).toFloat();
            c.removeFromTop (6);
            knobRow (c, voiceKnobs);
        }
        // row 2: filter and its envelope, amp envelope
        {
            auto a = section (row2.removeFromLeft (440), "FILTER", filtCol); row2.removeFromLeft (8);
            filterScreen = a.removeFromTop (a.getHeight() - 112).toFloat();
            a.removeFromTop (4);
            filterType->setBounds (a.removeFromTop (24));
            a.removeFromTop (4);
            knobRow (a, filterKnobs);

            auto b = section (row2.removeFromLeft ((row2.getWidth() - 8) / 2), "FILTER ENVELOPE", envCol); row2.removeFromLeft (8);
            fEnvScreen = b.removeFromTop (b.getHeight() - 84).toFloat();
            b.removeFromTop (6);
            knobRow (b, fEnvKnobs);

            auto c = section (row2, "AMP ENVELOPE", ampCol);
            aEnvScreen = c.removeFromTop (c.getHeight() - 84).toFloat();
            c.removeFromTop (6);
            knobRow (c, aEnvKnobs);
        }
        // row 3: LFOs, mod matrix, output
        {
            auto a = section (row3.removeFromLeft (520), "", lfoCol); row3.removeFromLeft (8);
            auto tabs = sections.back().area.withHeight (26).reduced (10, 3).removeFromLeft (150);
            lfoTab[0].setBounds (tabs.removeFromLeft (72)); tabs.removeFromLeft (6);
            lfoTab[1].setBounds (tabs.removeFromLeft (72));
            auto ctl = a.removeFromRight (270);
            lfoScreen = a.reduced (0, 2).toFloat();
            ctl.removeFromLeft (10);
            for (int i = 0; i < 2; ++i)
            {
                auto c2 = ctl;
                (i == 0 ? lfoWave : lfo2Wave)->setBounds (c2.removeFromTop (24));
                c2.removeFromTop (6);
                (i == 0 ? lfoTarget : lfo2Target)->setBounds (c2.removeFromTop (24));
                c2.removeFromTop (4);
                knobRow (c2, i == 0 ? lfoKnobs : lfo2Knobs);
            }

            auto m = section (row3.removeFromLeft (440), "MOD MATRIX", modCol); row3.removeFromLeft (8);
            matrixArea = m;
            m.removeFromTop (16);   // column titles
            const int rowH2 = m.getHeight() / modSlots;
            for (int i = 0; i < modSlots; ++i)
            {
                auto rr = m.removeFromTop (rowH2).reduced (0, 3);
                auto& sl = slots[(size_t) i];
                rr.removeFromLeft (18);   // slot number
                sl.src.setBounds (rr.removeFromLeft (112));
                rr.removeFromLeft (22);   // arrow
                sl.dst.setBounds (rr.removeFromLeft (118));
                rr.removeFromLeft (8);
                sl.amount.setBounds (rr);
            }

            auto b = section (row3, "OUTPUT", juce::Colour (0xffd8a96e));
            scopeScreen = b.reduced (0, 2).toFloat();
        }
    }

    // drag on the envelope and filter screens
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragTarget = None;
        const auto p = e.position;
        if (filterScreen.contains (p)) { dragTarget = Filter; begin ({ "cutoff", "reso" }); }
        else if (fEnvScreen.contains (p)) { dragTarget = FEnv; dragHandle = nearestEnvHandle (fEnvScreen, "fa", "fd", "fs", "fr", p); begin ({ "fa", "fd", "fs", "fr" }); }
        else if (aEnvScreen.contains (p)) { dragTarget = AEnv; dragHandle = nearestEnvHandle (aEnvScreen, "aa", "ad", "as", "ar", p); begin ({ "aa", "ad", "as", "ar" }); }
        else if (scopeScreen.contains (p) || oscScreen1.contains (p) || oscScreen2.contains (p)) { fx.auditionNote = 60; fx.audition = true; }
        mouseDrag (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const auto p = e.position;
        if (dragTarget == Filter)
        {
            auto plot = filterScreen.reduced (8, 10);
            setNorm (fx, "cutoff", (p.x - plot.getX()) / plot.getWidth());
            setNorm (fx, "reso", 1.0f - (p.y - plot.getY()) / plot.getHeight());
        }
        else if (dragTarget == FEnv) dragEnv (fEnvScreen, "fa", "fd", "fs", "fr", p);
        else if (dragTarget == AEnv) dragEnv (aEnvScreen, "aa", "ad", "as", "ar", p);
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override { for (auto* prm : gesture) prm->endChangeGesture(); gesture.clear(); dragTarget = None; }

private:
    SynthFx& fx;
    SkinLook lookA, lookB, lookF, lookE, lookL, lookM, lookS, lookL2, lookX;
    juce::ComboBox patchBox;
    juce::TextButton playButton { juce::String::fromUTF8 ("\xe2\x96\xb6") };
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<Switch> switches;
    struct Osc { Switch* wave = nullptr; std::vector<Knob*> knobs; } o1, o2;
    std::vector<Knob*> voiceKnobs, filterKnobs, fEnvKnobs, aEnvKnobs, lfoKnobs;
    Switch* filterType = nullptr; Switch* lfoWave = nullptr; Switch* lfoTarget = nullptr;
    Switch* lfo2Wave = nullptr; Switch* lfo2Target = nullptr;
    std::vector<Knob*> lfo2Knobs;
    juce::TextButton lfoTab[2];
    int shownLfo = 0;
    struct Slot
    {
        juce::ComboBox src, dst;
        ModAmountSlider amount;
        std::unique_ptr<APVTS::ComboBoxAttachment> srcAttach, dstAttach;
        std::unique_ptr<APVTS::SliderAttachment> amtAttach;
    };
    std::array<Slot, 4> slots;
    juce::Rectangle<int> matrixArea;

    void showLfo (int which)
    {
        shownLfo = which;
        for (auto* c : std::initializer_list<juce::Component*> { lfoWave, lfoTarget, lfoKnobs[0], lfoKnobs[1] }) c->setVisible (which == 0);
        for (auto* c : std::initializer_list<juce::Component*> { lfo2Wave, lfo2Target, lfo2Knobs[0], lfo2Knobs[1] }) c->setVisible (which == 1);
        repaint();
    }
    juce::Slider volume;
    std::unique_ptr<APVTS::SliderAttachment> volumeAttach;
    struct Section { juce::Rectangle<int> area; juce::String title; juce::Colour colour; };
    std::vector<Section> sections;
    juce::Rectangle<float> oscScreen1, oscScreen2, voiceScreen, filterScreen, fEnvScreen, aEnvScreen, lfoScreen, scopeScreen;
    enum Drag { None, Filter, FEnv, AEnv } dragTarget = None;
    int dragHandle = 0;
    std::vector<juce::RangedAudioParameter*> gesture;
    float drift = 0;

    void begin (std::initializer_list<const char*> ids)
    {
        for (auto* id : ids) if (auto* p = fx.state.getParameter (id)) { p->beginChangeGesture(); gesture.push_back (p); }
    }

    void timerCallback() override
    {
        drift += 0.02f + 0.08f * (fx.voicesPlaying.load() > 0 ? 1.0f : 0.0f);
        const int idx = patchIndex();
        if (patchBox.getSelectedId() != idx + 1) patchBox.setSelectedId (idx + 1, juce::dontSendNotification);
        if (idx < 0) patchBox.setText (fx.patchName(), juce::dontSendNotification);
        for (int i = 0; i < modSlots; ++i)
        {
            auto& sl = slots[(size_t) i];
            sl.amount.live = fx.slotNow[(size_t) i].load();
            const bool on = sl.src.getSelectedId() > 1 && sl.dst.getSelectedId() > 1;
            sl.amount.setAlpha (on ? 1.0f : 0.5f);
        }
        repaint();
    }
    int patchIndex() const
    {
        const auto n = fx.patchName();
        for (int i = 0; i < (int) getSynthPatches().size(); ++i) if (getSynthPatches()[(size_t) i].name == n) return i;
        return -1;
    }

    // ---- the pictures ----
    void drawOsc (juce::Graphics& g, juce::Rectangle<float> r, int wave, float shape, float level, juce::Colour c, double cycles)
    {
        screen (g, r);
        auto plot = r.reduced (10, 10);
        const float mid = plot.getCentreY();
        g.setColour (lineCol);
        g.drawHorizontalLine ((int) mid, plot.getX(), plot.getRight());
        if (fx.param ("lfotarget") > 2.5f && fx.param ("lfotarget") < 3.5f)
            shape = juce::jlimit (0.0f, 1.0f, shape + fx.lfoValue.load() * fx.param ("lfodepth") * 0.005f);
        cycles = juce::jlimit (0.5, 6.0, std::pow (2.0, cycles - 2.0) * 2.0);   // octave up = more cycles on screen
        juce::Path p;
        juce::Random rnd (wave * 31 + 7);
        const float amp = plot.getHeight() * 0.45f * (0.15f + 0.85f * level);
        const int steps = (int) plot.getWidth();
        for (int x = 0; x <= steps; ++x)
        {
            const double ph = std::fmod ((double) x / steps * cycles + drift * 0.25, 1.0);
            const float v = oscSample (wave, ph, 0.0, shape, rnd.nextFloat() * 2.0f - 1.0f);
            const float y = mid - v * amp;
            if (x == 0) p.startNewSubPath (plot.getX(), y); else p.lineTo (plot.getX() + (float) x, y);
        }
        glowPath (g, p, c.withMultipliedAlpha (level > 0.001f ? 1.0f : 0.3f));
        g.setColour (dimCol);
        g.setFont (theme::font (10.0f, true));
        g.drawText (oscWaves[wave].toUpperCase(), r.reduced (10, 6), juce::Justification::topRight);
    }

    void drawVoices (juce::Graphics& g, juce::Rectangle<float> r)
    {
        screen (g, r);
        auto plot = r.reduced (12, 10);
        const int n = juce::jlimit (1, 7, (int) std::lround (fx.param ("unison")));
        const float det = fx.param ("detune"), width = fx.param ("width") * 0.01f;
        const auto c = juce::Colour (0xffd8a96e);
        // each voice: a line fanning out by width (left / right) and detune (height of its wobble)
        for (int u = 0; u < n; ++u)
        {
            const float pos = n > 1 ? (float) u / (float) (n - 1) * 2.0f - 1.0f : 0.0f;
            const float x = plot.getCentreX() + pos * width * plot.getWidth() * 0.45f;
            juce::Path p;
            for (int k = 0; k <= 40; ++k)
            {
                const float t = (float) k / 40.0f;
                const float y = plot.getBottom() - t * plot.getHeight();
                const float wob = std::sin (t * 12.0f + drift * (1.0f + pos * det * 0.02f) * 3.0f) * (2.0f + det * 0.06f) * t;
                if (k == 0) p.startNewSubPath (plot.getCentreX(), plot.getBottom()); else p.lineTo (plot.getCentreX() + (x - plot.getCentreX()) * t + wob, y);
            }
            glowPath (g, p, c.withAlpha (0.5f + 0.5f / (float) n), 1.6f);
        }
        // sub and noise as a floor glow
        const float sub = fx.param ("sub") * 0.01f, noise = fx.param ("noise") * 0.01f;
        if (sub > 0) { g.setColour (oscA.withAlpha (0.35f * sub)); g.fillEllipse (plot.getCentreX() - 40, plot.getBottom() - 10, 80, 18); }
        if (noise > 0)
        {
            juce::Random rnd ((int) (drift * 50));
            g.setColour (textCol.withAlpha (0.4f * noise));
            for (int i = 0; i < (int) (noise * 120); ++i) g.fillRect (plot.getX() + rnd.nextFloat() * plot.getWidth(), plot.getY() + rnd.nextFloat() * plot.getHeight(), 1.5f, 1.5f);
        }
        g.setColour (dimCol);
        g.setFont (theme::font (10.0f, true));
        g.drawText (juce::String (n) + (n == 1 ? " VOICE" : " VOICES"), r.reduced (10, 6), juce::Justification::topRight);
    }

    float filterMag (double f, double fc, float reso, int type) const
    {
        const double w = f / fc;
        if (type == 4)   // ladder: four one-pole stages in a feedback loop
        {
            const double k = 3.9 * reso;
            const std::complex<double> s1 (1.0, w);
            return (float) ((1.0 + k * 0.45) / std::abs (s1 * s1 * s1 * s1 + k));
        }
        // the 2-pole state-variable filter (twice over for the 24 dB types)
        const double k = 2.0 - 1.96 * reso;
        const double den = std::sqrt ((1 - w * w) * (1 - w * w) + (k * w) * (k * w));
        double m = (type <= 1 || type == 5) ? 1.0 / den : type == 2 ? (w * w) / den : type == 3 ? (k * w) / den : std::abs (1 - w * w) / den;
        if (type == 0 || type == 5) m = m * (1.0 / std::sqrt ((1 - w * w) * (1 - w * w) + (k * 1.3 * w) * (k * 1.3 * w)));
        return (float) m;
    }

    void drawFilter (juce::Graphics& g, juce::Rectangle<float> r)
    {
        screen (g, r);
        auto plot = r.reduced (8, 10);
        auto xToF = [&] (float x) { return 20.0 * std::pow (1000.0, (x - plot.getX()) / plot.getWidth()); };
        auto fToX = [&] (double f) { return plot.getX() + plot.getWidth() * (float) (std::log (f / 20.0) / std::log (1000.0)); };
        auto dbToY = [&] (float db) { return plot.getY() + plot.getHeight() * (1.0f - (juce::jlimit (-36.0f, 18.0f, db) + 36.0f) / 54.0f); };
        g.setColour (lineCol);
        for (double f : { 100.0, 1000.0, 10000.0 }) g.drawVerticalLine ((int) fToX (f), plot.getY(), plot.getBottom());
        g.drawHorizontalLine ((int) dbToY (0), plot.getX(), plot.getRight());

        const int type = (int) fx.param ("ftype");
        const float reso = fx.param ("reso") * 0.01f;
        auto curve = [&] (double fc)
        {
            juce::Path p;
            for (int x = 0; x <= (int) plot.getWidth(); x += 2)
            {
                const float y = dbToY (juce::Decibels::gainToDecibels (filterMag (xToF (plot.getX() + x), fc, reso, type), -60.0f));
                if (x == 0) p.startNewSubPath (plot.getX(), y); else p.lineTo (plot.getX() + x, y);
            }
            return p;
        };
        const double set = fx.param ("cutoff");
        auto base = curve (set);
        juce::Path fill (base);
        fill.lineTo (plot.getRight(), plot.getBottom()); fill.lineTo (plot.getX(), plot.getBottom()); fill.closeSubPath();
        g.setColour (filtCol.withAlpha (0.12f)); g.fillPath (fill);
        glowPath (g, base, filtCol);

        // where the envelope / LFO has pushed the cutoff right now
        if (fx.voicesPlaying.load() > 0)
        {
            const double live = juce::jlimit (20.0, 20000.0, (double) fx.liveCutoff.load());
            glowPath (g, curve (live), juce::Colours::white.withAlpha (0.55f), 1.2f);
        }
        const auto handle = juce::Point<float> (fToX (set), plot.getY() + (1.0f - norm (fx, "reso")) * plot.getHeight());
        g.setColour (filtCol); g.fillEllipse (handle.x - 6, handle.y - 6, 12, 12);
        g.setColour (screenCol); g.fillEllipse (handle.x - 2.5f, handle.y - 2.5f, 5, 5);
        g.setColour (dimCol); g.setFont (theme::font (10.0f, true));
        g.drawText (filterTypes[type].toUpperCase() + "   " + juce::String (juce::roundToInt (set)) + " Hz", r.reduced (10, 6), juce::Justification::topRight);
    }

    // the envelope's corners: attack peak, decay/sustain, release start, end
    std::array<juce::Point<float>, 4> envPoints (juce::Rectangle<float> r, const char* a, const char* d, const char* s, const char* rl)
    {
        auto plot = r.reduced (10, 12);
        const float seg = plot.getWidth() * 0.27f;
        const float x1 = plot.getX() + norm (fx, a) * seg;
        const float x2 = x1 + juce::jmax (0.02f, norm (fx, d)) * seg;
        const float sy = plot.getBottom() - fx.param (s) * 0.01f * plot.getHeight();
        const float x3 = x2 + plot.getWidth() * 0.16f;
        const float x4 = x3 + juce::jmax (0.02f, norm (fx, rl)) * seg;
        return { juce::Point<float> (x1, plot.getY()), { x2, sy }, { x3, sy }, { x4, plot.getBottom() } };
    }

    void drawEnv (juce::Graphics& g, juce::Rectangle<float> r, const char* a, const char* d, const char* s, const char* rl, juce::Colour c, float level, int stage)
    {
        screen (g, r);
        auto plot = r.reduced (10, 12);
        const auto pts = envPoints (r, a, d, s, rl);
        juce::Path p;
        p.startNewSubPath (plot.getX(), plot.getBottom());
        p.quadraticTo ((plot.getX() + pts[0].x) * 0.5f, plot.getY() + plot.getHeight() * 0.25f, pts[0].x, pts[0].y);
        p.quadraticTo (pts[0].x + (pts[1].x - pts[0].x) * 0.2f, pts[1].y, pts[1].x, pts[1].y);
        p.lineTo (pts[2]);
        p.quadraticTo (pts[2].x + (pts[3].x - pts[2].x) * 0.2f, pts[3].y, pts[3].x, pts[3].y);
        juce::Path fill (p);
        fill.lineTo (plot.getX(), plot.getBottom()); fill.closeSubPath();
        g.setColour (c.withAlpha (0.12f)); g.fillPath (fill);
        glowPath (g, p, c);
        for (auto pt : pts) { g.setColour (c); g.fillEllipse (pt.x - 5, pt.y - 5, 10, 10); g.setColour (screenCol); g.fillEllipse (pt.x - 2, pt.y - 2, 4, 4); }

        // the newest note's envelope right now: a bar on the left and a line across
        if (level > 0.001f)
        {
            const float y = plot.getBottom() - level * plot.getHeight();
            g.setColour (juce::Colours::white.withAlpha (0.35f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (c.brighter (0.4f));
            g.fillRoundedRectangle (r.getX() + 3, y, 4, plot.getBottom() - y, 2.0f);
        }
        const char* names[] { "", "ATTACK", "DECAY", "SUSTAIN", "RELEASE" };
        g.setColour (dimCol); g.setFont (theme::font (10.0f, true));
        if (stage > 0) g.drawText (names[stage], r.reduced (10, 6), juce::Justification::topRight);
    }

    int nearestEnvHandle (juce::Rectangle<float> r, const char* a, const char* d, const char* s, const char* rl, juce::Point<float> p)
    {
        const auto pts = envPoints (r, a, d, s, rl);
        int best = 0;
        for (int i = 1; i < 4; ++i) if (pts[(size_t) i].getDistanceFrom (p) < pts[(size_t) best].getDistanceFrom (p)) best = i;
        return best;
    }

    void dragEnv (juce::Rectangle<float> r, const char* a, const char* d, const char* s, const char* rl, juce::Point<float> p)
    {
        auto plot = r.reduced (10, 12);
        const float seg = plot.getWidth() * 0.27f;
        const auto pts = envPoints (r, a, d, s, rl);
        switch (dragHandle)
        {
            case 0: setNorm (fx, a, (p.x - plot.getX()) / seg); break;
            case 1: setNorm (fx, d, (p.x - pts[0].x) / seg);
                    setNorm (fx, s, (plot.getBottom() - p.y) / plot.getHeight()); break;
            case 2: setNorm (fx, s, (plot.getBottom() - p.y) / plot.getHeight()); break;
            default: setNorm (fx, rl, (p.x - pts[2].x) / seg); break;
        }
    }

    void drawLfo (juce::Graphics& g, juce::Rectangle<float> r, int which)
    {
        screen (g, r);
        auto plot = r.reduced (12, 14);
        const juce::String pre (which == 0 ? "lfo" : "lfo2");
        const auto col = which == 0 ? lfoCol : lfo2Col;
        const int wave = (int) fx.param ((pre + "wave").toRawUTF8());
        // depth: its own quick route, or full if the matrix uses it
        bool usedByMatrix = false;
        for (int i = 0; i < modSlots; ++i) usedByMatrix = usedByMatrix || ((int) fx.param (modSrcIds[i]) == (which == 0 ? SrcLfo1 : SrcLfo2) && fx.param (modAmtIds[i]) != 0.0f);
        const float depth = juce::jmax (fx.param ((pre + "depth").toRawUTF8()) * 0.01f, usedByMatrix ? 0.7f : 0.0f);
        const float amp = plot.getHeight() * 0.45f * (0.08f + 0.92f * depth);
        auto val = [&] (double ph)
        {
            if (wave == 4) { juce::Random rr ((int) (ph * 8.0) * 97 + 3); return rr.nextFloat() * 2.0f - 1.0f; }
            return lfoShape (wave, ph, 0.0f);
        };
        juce::Path p;
        const double cycles = 2.0;
        for (int x = 0; x <= (int) plot.getWidth(); ++x)
        {
            const double ph = std::fmod ((double) x / plot.getWidth() * cycles, 1.0);
            const float y = plot.getCentreY() - val (ph) * amp;
            if (x == 0) p.startNewSubPath (plot.getX(), y); else p.lineTo (plot.getX() + x, y);
        }
        glowPath (g, p, col.withMultipliedAlpha (depth > 0 ? 1.0f : 0.35f));
        // a dot riding the wave at the LFO's position
        const float ph = which == 0 ? fx.lfoPhase.load() : fx.lfo2Phase.load();
        const float dx = plot.getX() + ph / (float) cycles * plot.getWidth();
        const float dy = plot.getCentreY() - val (ph) * amp;
        g.setColour (juce::Colours::white); g.fillEllipse (dx - 5, dy - 5, 10, 10);
        g.setColour (col.withAlpha (0.3f)); g.fillEllipse (dx - 11, dy - 11, 22, 22);
        g.setColour (dimCol); g.setFont (theme::font (10.0f, true));
        juce::String where = fx.param ((pre + "depth").toRawUTF8()) > 0 ? lfoTargets[(int) fx.param ((pre + "target").toRawUTF8())].toUpperCase() : juce::String();
        if (usedByMatrix) where += (where.isEmpty() ? "" : " + ") + juce::String ("MATRIX");
        if (where.isEmpty()) where = "OFF";
        g.drawText (juce::String (fx.param ((pre + "rate").toRawUTF8()), 2) + " Hz  >  " + where, r.reduced (10, 6), juce::Justification::topRight);
    }

    void drawMatrix (juce::Graphics& g)
    {
        auto m = matrixArea;
        auto head = m.removeFromTop (16);
        g.setColour (dimCol); g.setFont (theme::font (9.5f, true));
        head.removeFromLeft (18);
        g.drawText ("SOURCE", head.removeFromLeft (112), juce::Justification::centredLeft);
        head.removeFromLeft (22);
        g.drawText ("DESTINATION", head.removeFromLeft (118), juce::Justification::centredLeft);
        head.removeFromLeft (8);
        g.drawText ("AMOUNT  (LIVE)", head, juce::Justification::centredLeft);
        const int rowH2 = m.getHeight() / modSlots;
        for (int i = 0; i < modSlots; ++i)
        {
            auto rr = m.removeFromTop (rowH2);
            const bool on = slots[(size_t) i].src.getSelectedId() > 1 && slots[(size_t) i].dst.getSelectedId() > 1 && fx.param (modAmtIds[i]) != 0.0f;
            const float live = std::abs (fx.slotNow[(size_t) i].load());
            g.setColour (on ? modCol.withAlpha (0.5f + 0.5f * juce::jmin (1.0f, live * 3.0f)) : lineCol);
            g.fillEllipse ((float) rr.getX() + 2, (float) rr.getCentreY() - 5, 10, 10);
            g.setColour (on ? textCol : dimCol); g.setFont (theme::font (10.0f, true));
            // the arrow between source and destination
            const float ax = (float) rr.getX() + 18 + 112 + 4, ay = (float) rr.getCentreY();
            juce::Path arrow;
            arrow.startNewSubPath (ax, ay); arrow.lineTo (ax + 13, ay);
            arrow.addTriangle (ax + 15, ay, ax + 9, ay - 4, ax + 9, ay + 4);
            g.setColour (on ? modCol : lineCol);
            g.strokePath (arrow, juce::PathStrokeType (1.5f)); g.fillPath (arrow);
        }
    }

    void drawScope (juce::Graphics& g, juce::Rectangle<float> r)
    {
        screen (g, r);
        auto plot = r.reduced (10, 10);
        // find a rising zero crossing so the wave stands still
        const int pos = fx.scopePos.load();
        const int span = 400;
        int start = pos - span - 1;
        for (int k = 0; k < 300; ++k)
        {
            const int i0 = (start - k + SynthFx::scopeSize * 4) % SynthFx::scopeSize, i1 = (i0 + 1) % SynthFx::scopeSize;
            if (fx.scope[(size_t) i0].load() <= 0.0f && fx.scope[(size_t) i1].load() > 0.0f) { start = start - k; break; }
        }
        juce::Path p;
        float peak = 0.05f;
        for (int x = 0; x < span; ++x) peak = juce::jmax (peak, std::abs (fx.scope[(size_t) ((start + x + SynthFx::scopeSize * 4) % SynthFx::scopeSize)].load()));
        for (int x = 0; x < span; ++x)
        {
            const float v = fx.scope[(size_t) ((start + x + SynthFx::scopeSize * 4) % SynthFx::scopeSize)].load();
            const float px = plot.getX() + plot.getWidth() * (float) x / (float) (span - 1);
            const float py = plot.getCentreY() - v / juce::jmax (peak, 0.3f) * plot.getHeight() * 0.45f;
            if (x == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
        }
        juce::ColourGradient gr (oscA, plot.getX(), 0, oscB, plot.getRight(), 0, false);
        g.setColour (oscA.withAlpha (0.18f));
        g.strokePath (p, juce::PathStrokeType (6.0f));
        g.setGradientFill (gr);
        g.strokePath (p, juce::PathStrokeType (1.8f));
        g.setColour (dimCol); g.setFont (theme::font (10.0f, true));
        g.drawText (fx.voicesPlaying.load() > 0 ? juce::String (fx.voicesPlaying.load()) + " PLAYING" : juce::String ("CLICK TO HEAR"), r.reduced (10, 6), juce::Justification::topRight);
    }
};

juce::AudioProcessorEditor* SynthFx::createEditor() { return new SynthEditor (*this); }

} // namespace rs
