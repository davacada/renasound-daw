#include "SynthFx.h"
#include "DspHelpers.h"
#include "UI/Faceplate.h"

namespace rs
{

namespace
{
    const juce::StringArray oscWaves { "Sine", "Triangle", "Saw", "Square", "Noise" };
    const juce::StringArray filterTypes { "LP 24", "LP 12", "High-pass", "Band-pass" };
    const juce::StringArray lfoWaves { "Sine", "Triangle", "Saw", "Square", "Random" };
    const juce::StringArray lfoTargets { "Pitch", "Filter", "Volume", "Shape", "Pan" };

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
                               { "lfotarget", 0 }, { "volume", -10 } } } };
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
    };
    std::array<Voice, 16> voices;
    juce::int64 counter = 0;
    double lastFreq = 0;
    double lfoPh = 0;
    float lfoOut = 0, sh = 0;
    int scopeDecim = 0, auditionUntil = 0;
    juce::Random rnd;

    // parameters for this block
    int pW1 = 2, pW2 = 2, pFType = 0, pLfoWave = 0, pLfoTarget = 1, pUnison = 1;
    float pOct1 = 0, pShape1 = 0, pLvl1 = 0.8f, pOct2 = 0, pSemi2 = 0, pFine2 = 0, pShape2 = 0, pLvl2 = 0.5f, pSub = 0, pNoise = 0;
    float pDetune = 15, pWidth = 0.6f, pGlide = 0, pCutoff = 4000, pReso = 0.2f, pFenv = 0, pKey = 0.3f, pDrive = 0;
    float pLfoRate = 2, pLfoDepth = 0, gain = 1;

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
            v->amp.level = 0; v->filt.level = 0;
        }
        v->amp.on(); v->filt.on();
    }

    void noteOff (int note)
    {
        for (auto& v : voices) if (v.note == note && v.amp.stage != Env::Release) { v.amp.off(); v.filt.off(); }
    }

    // every 16 samples: LFO, glide and filter coefficients
    void tickControl()
    {
        const double step = 16.0 / currentSampleRate;
        const double prev = lfoPh;
        lfoPh += pLfoRate * step;
        if (lfoPh >= 1.0) lfoPh -= 1.0;
        switch (pLfoWave)
        {
            case 0: lfoOut = (float) std::sin (lfoPh * juce::MathConstants<double>::twoPi); break;
            case 1: lfoOut = (float) (lfoPh < 0.5 ? 4.0 * lfoPh - 1.0 : 3.0 - 4.0 * lfoPh); break;
            case 2: lfoOut = (float) (1.0 - 2.0 * lfoPh); break;
            case 3: lfoOut = lfoPh < 0.5 ? 1.0f : -1.0f; break;
            default: if (lfoPh < prev) sh = rnd.nextFloat() * 2.0f - 1.0f; lfoOut = sh; break;
        }
        const float glideK = pGlide > 0.5f ? (float) std::exp (-step / (pGlide * 0.001 / 4.6)) : 0.0f;
        for (auto& v : voices)
        {
            if (! v.amp.active()) continue;
            v.freq = v.target + (v.freq - v.target) * glideK;
            float oct = pFenv * 5.0f * v.filt.level + pKey * (float) (v.note - 60) / 12.0f;
            if (pLfoTarget == 1) oct += lfoOut * pLfoDepth * 2.5f;
            const float fc = juce::jlimit (20.0f, (float) currentSampleRate * 0.45f, pCutoff * std::pow (2.0f, oct));
            v.cutoffNow = fc;
            v.g = std::tan (juce::MathConstants<float>::pi * fc / (float) currentSampleRate);
            v.k = 2.0f - 1.96f * pReso;
        }
    }

    float svf (float x, float (&s)[2], float g, float k, int mode)
    {
        const float a1 = 1.0f / (1.0f + g * (g + k));
        const float v3 = x - s[1];
        const float v1 = a1 * s[0] + g * a1 * v3;
        const float v2 = s[1] + g * v1;
        s[0] = 2.0f * v1 - s[0];
        s[1] = 2.0f * v2 - s[1];
        return mode == 0 ? v2 : mode == 1 ? x - k * v1 - v2 : v1;   // low / high / band
    }

    void renderVoice (Voice& v, float& outL, float& outR)
    {
        const double sr = currentSampleRate;
        double pitchMul = 1.0;
        if (pLfoTarget == 0) pitchMul = std::pow (2.0, lfoOut * pLfoDepth * 2.0 / 12.0);
        const double f1 = v.freq * std::pow (2.0, pOct1) * pitchMul;
        const double f2 = v.freq * std::pow (2.0, pOct2 + (pSemi2 + pFine2 * 0.01) / 12.0) * pitchMul;
        const float shapeMod = pLfoTarget == 3 ? lfoOut * pLfoDepth * 0.5f : 0.0f;
        const float sh1 = juce::jlimit (0.0f, 1.0f, pShape1 + shapeMod), sh2 = juce::jlimit (0.0f, 1.0f, pShape2 + shapeMod);

        // white noise, softened a little for the noise oscillator
        const float white = rnd.nextFloat() * 2.0f - 1.0f;
        v.noiseLp += 0.5f * (white - v.noiseLp);

        float l = 0, r = 0;
        for (int u = 0; u < pUnison; ++u)
        {
            const float pos = pUnison > 1 ? (float) u / (float) (pUnison - 1) * 2.0f - 1.0f : 0.0f;
            const double det = std::pow (2.0, pos * pDetune * 0.01 / 12.0);
            auto& a = v.ph1[(size_t) u];
            auto& b = v.ph2[(size_t) u];
            const double d1 = f1 * det / sr, d2 = f2 * det / sr;
            float s = 0;
            if (pLvl1 > 0) s += pLvl1 * oscSample (pW1, a, d1, sh1, v.noiseLp);
            if (pLvl2 > 0) s += pLvl2 * oscSample (pW2, b, d2, sh2, white);
            a += d1; if (a >= 1.0) a -= 1.0;
            b += d2; if (b >= 1.0) b -= 1.0;
            const float pan = pos * pWidth;
            l += s * (1.0f - juce::jmax (0.0f, pan));
            r += s * (1.0f + juce::jmin (0.0f, pan));
        }
        // sub (an octave below osc 1) and noise sit in the middle
        const float sub = pSub > 0 ? pSub * (float) std::sin (v.phSub * juce::MathConstants<double>::twoPi) * std::sqrt ((float) pUnison) : 0.0f;
        v.phSub += f1 * 0.5 / sr; if (v.phSub >= 1.0) v.phSub -= 1.0;
        const float nz = pNoise * white * 0.5f * std::sqrt ((float) pUnison);
        l += sub + nz; r += sub + nz;

        if (pDrive > 0) { const float dr = 1.0f + pDrive * 6.0f; l = std::tanh (l * dr) / std::sqrt (dr); r = std::tanh (r * dr) / std::sqrt (dr); }

        const int mode = pFType <= 1 ? 0 : pFType == 2 ? 1 : 2;
        float yl = svf (l, v.s1[0], v.g, v.k, mode), yr = svf (r, v.s1[1], v.g, v.k, mode);
        if (pFType == 0) { yl = svf (yl, v.s2[0], v.g, v.k * 1.3f, 0); yr = svf (yr, v.s2[1], v.g, v.k * 1.3f, 0); }

        v.filt.next();
        float env = v.amp.next() * (0.3f + 0.7f * v.vel);
        if (pLfoTarget == 2) env *= 1.0f - pLfoDepth * 0.5f * (1.0f - lfoOut);
        float panL = 1, panR = 1;
        if (pLfoTarget == 4) { const float p = lfoOut * pLfoDepth; panL = 1.0f - juce::jmax (0.0f, p); panR = 1.0f + juce::jmin (0.0f, p); }
        outL += yl * env * panL;
        outR += yr * env * panR;
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
    const juce::Colour oscA (0xffff6b5b), oscB (0xff3fd6c8), filtCol (0xffffb547), envCol (0xffa58bff), lfoCol (0xffb6f24a), ampCol (0xff6fb7ff);

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

class SynthEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit SynthEditor (SynthFx& p) : AudioProcessorEditor (p), fx (p),
        lookA (skinFor (oscA)), lookB (skinFor (oscB)), lookF (skinFor (filtCol)), lookE (skinFor (envCol)),
        lookL (skinFor (lfoCol)), lookM (skinFor (ampCol)), lookS (skinFor (juce::Colour (0xffd8a96e)))
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
        volume = k ("volume", "VOLUME", lookS);

        setSize (1120, 760);
        timerCallback();   // show the current sound's name straight away
        startTimerHz (40);
    }

    ~SynthEditor() override
    {
        for (auto* kn : knobs) kn->setLookAndFeel (nullptr);
        for (auto* s : switches) s->setLookAndFeel (nullptr);
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
        g.drawText ("2 OSC  /  SUB  /  NOISE  /  UNISON  /  FILTER  /  2 ENV  /  LFO", 210, 10, 420, 36, juce::Justification::centredLeft);
        g.drawText ("SOUND", patchBox.getBounds().translated (-50, 0).withWidth (46), juce::Justification::centredRight);

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
        drawLfo (g, lfoScreen);
        drawScope (g, scopeScreen);
    }

    void resized() override
    {
        sections.clear();
        auto r = getLocalBounds().reduced (14, 10);
        auto head = r.removeFromTop (44);
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
        // row 3: LFO, scope, volume
        {
            auto a = section (row3.removeFromLeft (620), "LFO", lfoCol); row3.removeFromLeft (8);
            auto ctl = a.removeFromRight (300);
            lfoScreen = a.reduced (0, 2).toFloat();
            ctl.removeFromLeft (10);
            lfoWave->setBounds (ctl.removeFromTop (24));
            ctl.removeFromTop (6);
            lfoTarget->setBounds (ctl.removeFromTop (24));
            ctl.removeFromTop (4);
            knobRow (ctl, lfoKnobs);

            auto b = section (row3.removeFromLeft (row3.getWidth() - 130), "OUTPUT", juce::Colour (0xffd8a96e)); row3.removeFromLeft (8);
            scopeScreen = b.reduced (0, 2).toFloat();
            auto c = section (row3, "MASTER", juce::Colour (0xffd8a96e));
            volume->setBounds (c.withSizeKeepingCentre (80, 96));
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
    SkinLook lookA, lookB, lookF, lookE, lookL, lookM, lookS;
    juce::ComboBox patchBox;
    juce::TextButton playButton { juce::String::fromUTF8 ("\xe2\x96\xb6") };
    juce::OwnedArray<Knob> knobs;
    juce::OwnedArray<Switch> switches;
    struct Osc { Switch* wave = nullptr; std::vector<Knob*> knobs; } o1, o2;
    std::vector<Knob*> voiceKnobs, filterKnobs, fEnvKnobs, aEnvKnobs, lfoKnobs;
    Switch* filterType = nullptr; Switch* lfoWave = nullptr; Switch* lfoTarget = nullptr;
    Knob* volume = nullptr;
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
        // magnitude of the 2-pole state-variable filter (and squared for the 24 dB low-pass)
        const double w = f / fc;
        const double k = 2.0 - 1.96 * reso;
        const double den = std::sqrt ((1 - w * w) * (1 - w * w) + (k * w) * (k * w));
        double m = type <= 1 ? 1.0 / den : type == 2 ? (w * w) / den : (k * w) / den;
        if (type == 0) m = m * (1.0 / std::sqrt ((1 - w * w) * (1 - w * w) + (k * 1.3 * w) * (k * 1.3 * w)));
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

    void drawLfo (juce::Graphics& g, juce::Rectangle<float> r)
    {
        screen (g, r);
        auto plot = r.reduced (12, 14);
        const int wave = (int) fx.param ("lfowave");
        const float depth = fx.param ("lfodepth") * 0.01f;
        const float amp = plot.getHeight() * 0.45f * (0.08f + 0.92f * depth);
        auto val = [&] (double ph)
        {
            switch (wave)
            {
                case 0: return (float) std::sin (ph * juce::MathConstants<double>::twoPi);
                case 1: return (float) (ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph);
                case 2: return (float) (1.0 - 2.0 * ph);
                case 3: return ph < 0.5 ? 1.0f : -1.0f;
                default: { juce::Random rr ((int) (ph * 8.0) * 97 + 3); return rr.nextFloat() * 2.0f - 1.0f; }
            }
        };
        juce::Path p;
        const double cycles = 2.0;
        for (int x = 0; x <= (int) plot.getWidth(); ++x)
        {
            const double ph = std::fmod ((double) x / plot.getWidth() * cycles, 1.0);
            const float y = plot.getCentreY() - val (ph) * amp;
            if (x == 0) p.startNewSubPath (plot.getX(), y); else p.lineTo (plot.getX() + x, y);
        }
        glowPath (g, p, lfoCol.withMultipliedAlpha (depth > 0 ? 1.0f : 0.35f));
        // a dot riding the wave at the LFO's position
        const float ph = fx.lfoPhase.load();
        const float dx = plot.getX() + ph / (float) cycles * plot.getWidth();
        const float dy = plot.getCentreY() - val (ph) * amp;
        g.setColour (juce::Colours::white); g.fillEllipse (dx - 5, dy - 5, 10, 10);
        g.setColour (lfoCol.withAlpha (0.3f)); g.fillEllipse (dx - 11, dy - 11, 22, 22);
        g.setColour (dimCol); g.setFont (theme::font (10.0f, true));
        g.drawText (juce::String (fx.param ("lforate"), 2) + " Hz  >  " + lfoTargets[(int) fx.param ("lfotarget")].toUpperCase(), r.reduced (10, 6), juce::Justification::topRight);
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
