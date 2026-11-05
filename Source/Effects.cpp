#include "Effects.h"
#include "UI/Theme.h"
#include "DspHelpers.h"
#include "Devices.h"

namespace rs
{

using APVTS = juce::AudioProcessorValueTreeState;
// =============================================================================
BuiltinFx::BuiltinFx (juce::String id, juce::String name, APVTS::ParameterLayout layout)
    : AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo())
                                       .withOutput ("Output", juce::AudioChannelSet::stereo())),
      state (*this, nullptr, "STATE", std::move (layout)),
      fxId (std::move (id)), fxName (std::move (name))
{
}

void BuiltinFx::getStateInformation (juce::MemoryBlock& mb)
{
    if (auto xml = state.copyState().createXml())
        copyXmlToBinary (*xml, mb);
}

void BuiltinFx::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (state.state.getType()))
            state.replaceState (juce::ValueTree::fromXml (*xml));
}

double BuiltinFx::getBpm() const
{
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
            if (auto b = pos->getBpm())
                return *b;
    return 120.0;
}

std::optional<double> BuiltinFx::getPpq() const
{
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition(); pos && pos->getIsPlaying())
            if (auto p = pos->getPpqPosition())
                return *p;
    return std::nullopt;
}

// =============================================================================
//  EQ: low cut, low shelf, mid bell, high shelf, high cut
// =============================================================================
class EqFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    EqFx() : BuiltinFx ("eq", "EQ", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("lowCut", "Low Cut", skewed (20, 1000, 120), 20, "Hz"));
        l.add (floatParam ("lowFreq", "Low Freq", skewed (30, 600, 120), 100, "Hz"));
        l.add (floatParam ("lowGain", "Low", Range (-18, 18), 0, "dB"));
        l.add (floatParam ("midFreq", "Mid Freq", skewed (150, 10000, 1000), 1000, "Hz"));
        l.add (floatParam ("midGain", "Mid", Range (-18, 18), 0, "dB"));
        l.add (floatParam ("midQ", "Mid Q", skewed (0.3f, 8.0f, 1.0f), 1.0f));
        l.add (floatParam ("highFreq", "High Freq", skewed (1500, 18000, 6000), 8000, "Hz"));
        l.add (floatParam ("highGain", "High", Range (-18, 18), 0, "dB"));
        l.add (floatParam ("highCut", "High Cut", skewed (1500, 20000, 8000), 20000, "Hz"));
        return l;
    }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& ch : filters) for (auto& f : ch) f.reset();
        lastValues.fill (-1.0f);
        updateCoefficients();
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        measureIn (buffer);
        updateCoefficients();
        for (int ch = 0; ch < juce::jmin (2, buffer.getNumChannels()); ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (int k = 0; k < numBands; ++k)
            {
                if (! active[(size_t) k]) continue;
                auto& f = filters[(size_t) ch][(size_t) k];
                for (int i = 0; i < buffer.getNumSamples(); ++i)
                    d[i] = f.processSample (d[i]);
            }
        }
    }

    bool hasResponseCurve() const override { return true; }

    // Called by the editor (message thread): uses its own copy of the coefficients
    // so the curve follows the knobs even while the audio is stopped.
    double getMagnitudeForFrequency (double freq) const override
    {
        const auto v = currentValues();
        if (v != displayValues)
        {
            displayValues = v;
            displayCoefs = makeCoefficients (v, currentSampleRate);
            displayActive = whichActive (v);
        }
        double m = 1.0;
        for (int k = 0; k < numBands; ++k)
            if (displayActive[(size_t) k])
                m *= displayCoefs[(size_t) k]->getMagnitudeForFrequency (freq, currentSampleRate);
        return m;
    }

private:
    static constexpr int numBands = 5;
    std::array<std::array<juce::dsp::IIR::Filter<float>, numBands>, 2> filters;
    std::array<bool, numBands> active {};
    std::array<float, 9> lastValues {};
    mutable std::array<float, 9> displayValues { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    mutable std::array<juce::dsp::IIR::Coefficients<float>::Ptr, numBands> displayCoefs;
    mutable std::array<bool, numBands> displayActive {};

    using C = juce::dsp::IIR::Coefficients<float>;

    std::array<float, 9> currentValues() const
    {
        return { param ("lowCut"), param ("lowFreq"), param ("lowGain"), param ("midFreq"), param ("midGain"),
                 param ("midQ"), param ("highFreq"), param ("highGain"), param ("highCut") };
    }

    static std::array<C::Ptr, numBands> makeCoefficients (const std::array<float, 9>& v, double sr)
    {
        const float nyq = (float) sr * 0.49f;
        return { C::makeHighPass (sr, v[0], 0.707f),
                 C::makeLowShelf (sr, v[1], 0.707f, juce::Decibels::decibelsToGain (v[2])),
                 C::makePeakFilter (sr, juce::jmin (v[3], nyq), v[5], juce::Decibels::decibelsToGain (v[4])),
                 C::makeHighShelf (sr, juce::jmin (v[6], nyq), 0.707f, juce::Decibels::decibelsToGain (v[7])),
                 C::makeLowPass (sr, juce::jmin (v[8], nyq), 0.707f) };
    }

    static std::array<bool, numBands> whichActive (const std::array<float, 9>& v)
    {
        return { v[0] > 20.5f, std::abs (v[2]) > 0.05f, std::abs (v[4]) > 0.05f, std::abs (v[7]) > 0.05f, v[8] < 19900.0f };
    }

    void updateCoefficients()
    {
        const auto v = currentValues();
        if (v == lastValues) return;
        lastValues = v;
        const auto c = makeCoefficients (v, currentSampleRate);
        active = whichActive (v);
        for (int k = 0; k < numBands; ++k)
            for (auto& ch : filters)
                *ch[(size_t) k].coefficients = *c[(size_t) k];
    }
};

// =============================================================================
class CompressorFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    CompressorFx() : BuiltinFx ("compressor", "Compressor", layout()) { meterLabel = "Gain reduction"; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("threshold", "Threshold", Range (-60, 0), -18, "dB"));
        l.add (floatParam ("ratio", "Ratio", skewed (1, 20, 4), 4, ":1"));
        l.add (floatParam ("attack", "Attack", skewed (0.1f, 100, 10), 10, "ms"));
        l.add (floatParam ("release", "Release", skewed (10, 1000, 150), 120, "ms"));
        l.add (floatParam ("makeup", "Makeup", Range (0, 24), 0, "dB"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 100, "%"));
        return l;
    }

    void prepareToPlay (double sr, int block) override
    {
        currentSampleRate = sr;
        comp.prepare ({ sr, (juce::uint32) block, 2 });
        dry.setSize (2, block);
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        comp.setThreshold (param ("threshold"));
        comp.setRatio (param ("ratio"));
        comp.setAttack (param ("attack"));
        comp.setRelease (param ("release"));

        const float mix = param ("mix") / 100.0f;
        if (dry.getNumSamples() < n) dry.setSize (2, n, false, false, true);
        for (int ch = 0; ch < 2; ++ch) dry.copyFrom (ch, 0, buffer, ch, 0, n);

        measureIn (buffer);
        const float inLevel = buffer.getRMSLevel (0, 0, n);
        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), 2, (size_t) n);
        comp.process (juce::dsp::ProcessContextReplacing<float> (block));
        const float outLevel = buffer.getRMSLevel (0, 0, n);
        const float gr = (inLevel > 1.0e-4f && outLevel > 0) ? juce::Decibels::gainToDecibels (outLevel / inLevel) : 0.0f;
        meterValue = juce::jmin (0.0f, 0.7f * meterValue.load() + 0.3f * gr);
        if (historyTick (n)) pushHistory (meterValue.load());

        buffer.applyGain (juce::Decibels::decibelsToGain (param ("makeup")));
        if (mix < 0.999f)
            for (int ch = 0; ch < 2; ++ch)
            {
                buffer.applyGain (ch, 0, n, mix);
                buffer.addFrom (ch, 0, dry, ch, 0, n, 1.0f - mix);
            }
        measureOut (buffer);
    }

private:
    juce::dsp::Compressor<float> comp;
    juce::AudioBuffer<float> dry;
};

// =============================================================================
class ReverbFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    ReverbFx() : BuiltinFx ("reverb", "Reverb", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("size", "Size", Range (0, 100), 55, "%"));
        l.add (floatParam ("damping", "Damping", Range (0, 100), 45, "%"));
        l.add (floatParam ("width", "Width", Range (0, 100), 100, "%"));
        l.add (floatParam ("predelay", "Pre-delay", Range (0, 150), 10, "ms"));
        l.add (floatParam ("lowcut", "Low Cut", skewed (20, 1000, 200), 150, "Hz"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 25, "%"));
        return l;
    }

    double getTailLengthSeconds() const override { return 5.0; }

    void prepareToPlay (double sr, int block) override
    {
        currentSampleRate = sr;
        reverb.prepare ({ sr, (juce::uint32) block, 2 });
        reverb.reset();
        wet.setSize (2, block);
        pre.setSize (2, (int) (sr * 0.2) + 1);
        pre.clear();
        prePos = 0;
        for (auto& f : lowCut) f.reset();
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        measureIn (buffer);
        if (historyTick (n)) pushHistory (inputDb.load());
        juce::dsp::Reverb::Parameters p;
        p.roomSize = param ("size") / 100.0f;
        p.damping = param ("damping") / 100.0f;
        p.width = param ("width") / 100.0f;
        p.wetLevel = 1.0f;
        p.dryLevel = 0.0f;
        reverb.setParameters (p);

        if (wet.getNumSamples() < n) wet.setSize (2, n, false, false, true);
        const int preSamples = juce::jlimit (0, pre.getNumSamples() - 1, (int) (param ("predelay") * 0.001 * currentSampleRate));
        auto lc = juce::dsp::IIR::Coefficients<float>::makeHighPass (currentSampleRate, param ("lowcut"));

        // pre-delay + low cut into the reverb
        for (int ch = 0; ch < 2; ++ch)
        {
            *lowCut[(size_t) ch].coefficients = *lc;
            auto* in = buffer.getReadPointer (ch);
            auto* w = wet.getWritePointer (ch);
            auto* line = pre.getWritePointer (ch);
            int pos = prePos;
            for (int i = 0; i < n; ++i)
            {
                line[pos] = in[i];
                int rd = pos - preSamples; if (rd < 0) rd += pre.getNumSamples();
                w[i] = lowCut[(size_t) ch].processSample (line[rd]);
                if (++pos >= pre.getNumSamples()) pos = 0;
            }
        }
        prePos = (prePos + n) % pre.getNumSamples();

        juce::dsp::AudioBlock<float> block (wet.getArrayOfWritePointers(), 2, (size_t) n);
        reverb.process (juce::dsp::ProcessContextReplacing<float> (block));

        const float mix = param ("mix") / 100.0f;
        for (int ch = 0; ch < 2; ++ch)
        {
            buffer.applyGain (ch, 0, n, std::cos (mix * juce::MathConstants<float>::halfPi));
            buffer.addFrom (ch, 0, wet, ch, 0, n, std::sin (mix * juce::MathConstants<float>::halfPi));
        }
        outputDb = juce::Decibels::gainToDecibels (wet.getMagnitude (0, n), -100.0f);   // the reverb tail itself
    }

private:
    juce::dsp::Reverb reverb;
    juce::AudioBuffer<float> wet, pre;
    int prePos = 0;
    std::array<juce::dsp::IIR::Filter<float>, 2> lowCut;
};

// =============================================================================
class DelayFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    DelayFx() : BuiltinFx ("delay", "Delay", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "sync", 1 }, "Time Sync",
                   juce::StringArray { "Free (ms)", "1/4", "1/8", "1/8 dotted", "1/8 triplet", "1/16", "1/2" }, 2));
        l.add (floatParam ("time", "Time", skewed (10, 2000, 350), 350, "ms"));
        l.add (floatParam ("feedback", "Feedback", Range (0, 95), 35, "%"));
        l.add (floatParam ("tone", "Tone", skewed (500, 20000, 4000), 7000, "Hz"));
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "pingpong", 1 }, "Ping-pong", false));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 30, "%"));
        return l;
    }

    double getTailLengthSeconds() const override { return 4.0; }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        line.setSize (2, (int) (sr * 4.0) + 1);
        line.clear();
        writePos = 0;
        lp.fill (0.0f);
        smoothedDelay.reset (sr, 0.08);
        smoothedDelay.setCurrentAndTargetValue ((float) (0.35 * sr));
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        static const double beats[] = { 0, 1.0, 0.5, 0.75, 1.0 / 3.0, 0.25, 2.0 };
        const int sync = (int) param ("sync");
        const double seconds = sync == 0 ? param ("time") * 0.001 : beats[sync] * 60.0 / getBpm();
        smoothedDelay.setTargetValue ((float) juce::jlimit (1.0, (double) line.getNumSamples() - 2, seconds * currentSampleRate));

        measureIn (buffer);
        if (historyTick (buffer.getNumSamples())) pushHistory (inputDb.load());
        phase = (float) seconds;   // the actual delay time, for the faceplate
        const float fb = param ("feedback") / 100.0f;
        const float mix = param ("mix") / 100.0f;
        const bool pingPong = param ("pingpong") > 0.5f;
        const float a = (float) std::exp (-juce::MathConstants<double>::twoPi * param ("tone") / currentSampleRate);
        const int len = line.getNumSamples();
        auto* L = line.getWritePointer (0);
        auto* R = line.getWritePointer (1);
        auto* bl = buffer.getWritePointer (0);
        auto* br = buffer.getWritePointer (1);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const float d = smoothedDelay.getNextValue();
            float rp = (float) writePos - d;
            if (rp < 0) rp += (float) len;
            const int i0 = (int) rp;
            const int i1 = (i0 + 1) % len;
            const float fr = rp - (float) i0;
            float dl = L[i0] + fr * (L[i1] - L[i0]);
            float dr = R[i0] + fr * (R[i1] - R[i0]);
            lp[0] = dl + a * (lp[0] - dl);
            lp[1] = dr + a * (lp[1] - dr);
            dl = lp[0]; dr = lp[1];

            const float inL = bl[i], inR = br[i];
            if (pingPong)
            {
                L[writePos] = 0.5f * (inL + inR) + fb * dr;
                R[writePos] = fb * dl;
            }
            else
            {
                L[writePos] = inL + fb * dl;
                R[writePos] = inR + fb * dr;
            }
            bl[i] = inL * (1.0f - mix * 0.5f) + mix * dl;
            br[i] = inR * (1.0f - mix * 0.5f) + mix * dr;
            if (++writePos >= len) writePos = 0;
        }
    }

private:
    juce::AudioBuffer<float> line;
    int writePos = 0;
    std::array<float, 2> lp {};
    juce::SmoothedValue<float> smoothedDelay;
};

// =============================================================================
class NoiseGateFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    NoiseGateFx() : BuiltinFx ("noisegate", "Noise Gate", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("threshold", "Threshold", Range (-80, 0), -45, "dB"));
        l.add (floatParam ("ratio", "Ratio", skewed (1, 50, 10), 20, ":1"));
        l.add (floatParam ("attack", "Attack", skewed (0.1f, 50, 2), 1, "ms"));
        l.add (floatParam ("release", "Release", skewed (5, 1000, 100), 120, "ms"));
        return l;
    }

    void prepareToPlay (double sr, int block) override
    {
        currentSampleRate = sr;
        gate.prepare ({ sr, (juce::uint32) block, 2 });
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        gate.setThreshold (param ("threshold"));
        gate.setRatio (param ("ratio"));
        gate.setAttack (param ("attack"));
        gate.setRelease (param ("release"));
        measureIn (buffer);
        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), 2, (size_t) buffer.getNumSamples());
        gate.process (juce::dsp::ProcessContextReplacing<float> (block));
        measureOut (buffer);
        if (historyTick (buffer.getNumSamples())) pushHistory (inputDb.load());
        meterValue = outputDb.load() - inputDb.load();   // how much the gate is closing
    }

private:
    juce::dsp::NoiseGate<float> gate;
};

// =============================================================================
//  Gater: tempo-synced rhythmic volume gate (the classic "trance gate")
// =============================================================================
class GaterFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    GaterFx() : BuiltinFx ("gater", "Gater", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "rate", 1 }, "Rate",
                   juce::StringArray { "1/4", "1/8", "1/16", "1/32", "1/8 triplet", "1/16 triplet" }, 2));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "pattern", 1 }, "Pattern",
                   juce::StringArray { "Every step", "Off-beats", "Gallop", "Stutter", "Half" }, 0));
        l.add (floatParam ("depth", "Depth", Range (0, 100), 100, "%"));
        l.add (floatParam ("length", "Gate Length", Range (5, 100), 50, "%"));
        l.add (floatParam ("smooth", "Smooth", skewed (0.5f, 30, 4), 3, "ms"));
        return l;
    }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        freeBeats = 0;
        env = 1.0f;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        static const double stepBeats[] = { 1.0, 0.5, 0.25, 0.125, 1.0 / 3.0, 1.0 / 6.0 };
        static const std::array<std::array<bool, 8>, 5> patterns {{
            {{ 1, 1, 1, 1, 1, 1, 1, 1 }},
            {{ 0, 1, 0, 1, 0, 1, 0, 1 }},
            {{ 1, 0, 1, 1, 1, 0, 1, 1 }},
            {{ 1, 1, 1, 0, 1, 1, 0, 1 }},
            {{ 1, 1, 1, 1, 0, 0, 0, 0 }} }};

        const double step = stepBeats[(int) param ("rate")];
        const auto& pat = patterns[(size_t) param ("pattern")];
        const float depth = param ("depth") / 100.0f;
        const double gateLen = param ("length") / 100.0;
        const float coef = (float) std::exp (-1.0 / (param ("smooth") * 0.001 * currentSampleRate));
        const double beatsPerSample = getBpm() / 60.0 / currentSampleRate;

        double beat = freeBeats;
        if (auto ppq = getPpq()) beat = *ppq;

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const double stepPos = beat / step;
            const auto stepIndex = (long long) std::floor (stepPos);
            const bool on = pat[(size_t) (stepIndex % 8)] && (stepPos - (double) stepIndex) < gateLen;
            const float target = on ? 1.0f : 1.0f - depth;
            env = target + coef * (env - target);
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
                buffer.getWritePointer (ch)[i] *= env;
            beat += beatsPerSample;
        }
        freeBeats = beat;
        phase = (float) std::fmod (beat / step, 8.0);   // which of the 8 steps is playing
        meterValue = env;
    }

private:
    double freeBeats = 0;
    float env = 1.0f;
};

// =============================================================================
class PhaserFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    PhaserFx() : BuiltinFx ("phaser", "Phaser", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("rate", "Rate", skewed (0.02f, 10, 0.6f), 0.4f, "Hz"));
        l.add (floatParam ("depth", "Depth", Range (0, 100), 60, "%"));
        l.add (floatParam ("centre", "Centre", skewed (200, 5000, 1000), 1000, "Hz"));
        l.add (floatParam ("feedback", "Feedback", Range (-90, 90), 40, "%"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 50, "%"));
        return l;
    }

    void prepareToPlay (double sr, int block) override
    {
        currentSampleRate = sr;
        phaser.prepare ({ sr, (juce::uint32) block, 2 });
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        phaser.setRate (param ("rate"));
        phaser.setDepth (param ("depth") / 100.0f);
        phaser.setCentreFrequency (param ("centre"));
        phaser.setFeedback (param ("feedback") / 100.0f);
        phaser.setMix (param ("mix") / 100.0f);
        measureIn (buffer);
        phase = (float) std::fmod (phase.load() + param ("rate") * buffer.getNumSamples() / currentSampleRate, 1.0);
        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), 2, (size_t) buffer.getNumSamples());
        phaser.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

private:
    juce::dsp::Phaser<float> phaser;
};

// =============================================================================
class ChorusFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    ChorusFx() : BuiltinFx ("chorus", "Chorus", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("rate", "Rate", skewed (0.05f, 8, 1), 0.8f, "Hz"));
        l.add (floatParam ("depth", "Depth", Range (0, 100), 35, "%"));
        l.add (floatParam ("delay", "Delay", Range (2, 30), 8, "ms"));
        l.add (floatParam ("feedback", "Feedback", Range (-90, 90), 0, "%"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 45, "%"));
        return l;
    }

    void prepareToPlay (double sr, int block) override
    {
        currentSampleRate = sr;
        chorus.prepare ({ sr, (juce::uint32) block, 2 });
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        chorus.setRate (param ("rate"));
        chorus.setDepth (param ("depth") / 100.0f);
        chorus.setCentreDelay (param ("delay"));
        chorus.setFeedback (param ("feedback") / 100.0f);
        chorus.setMix (param ("mix") / 100.0f);
        measureIn (buffer);
        phase = (float) std::fmod (phase.load() + param ("rate") * buffer.getNumSamples() / currentSampleRate, 1.0);
        juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), 2, (size_t) buffer.getNumSamples());
        chorus.process (juce::dsp::ProcessContextReplacing<float> (block));
    }

private:
    juce::dsp::Chorus<float> chorus;
};

// =============================================================================
const std::vector<BuiltinFxInfo>& getBuiltinFxList()
{
    static const std::vector<BuiltinFxInfo> list {
        { "eq",         "EQ",          "Shape the tone: cuts, shelves and a bell" },
        { "dyneq",      "Dynamic EQ",  "6 bands that can react to the level, with an analyser" },
        { "tubeeq",     "Tube EQ",     "Vintage passive EQ: boost and cut the lows together, tube warmth" },
        { "punch",      "Punch",       "Transient shaper and HDR enhancer: more snap, more detail" },
        { "widener",    "Stereo Widener", "Wider stereo that still sums to mono cleanly" },
        { "amp",        "Amp",         "Guitar / bass amp with three channels and cabinets" },
        { "compressor", "Compressor",  "Even out dynamics and add punch" },
        { "reverb",     "Reverb",      "Put the sound in a room or hall" },
        { "delay",      "Delay",       "Tempo-synced echoes, with ping-pong" },
        { "noisegate",  "Noise Gate",  "Silence hiss and bleed between phrases" },
        { "gater",      "Gater",       "Rhythmic tempo-synced volume chopping" },
        { "phaser",     "Phaser",      "Sweeping, swirling notches" },
        { "chorus",     "Chorus",      "Thicken and widen" } };
    return list;
}

std::unique_ptr<BuiltinFx> createBuiltinFx (const juce::String& id)
{
    if (id == "eq")         return std::make_unique<EqFx>();
    if (id == "dyneq")      return createDynamicEq();
    if (id == "tubeeq")     return createTubeEq();
    if (id == "punch")      return createPunch();
    if (id == "widener")    return createWidener();
    if (id == "amp")        return createAmp();
    if (id == "kick")       return createKickSynth();
    if (id == "vocoder")    return createVocoder();
    if (id == "sampler")    return createSampler();
    if (id == "compressor") return std::make_unique<CompressorFx>();
    if (id == "reverb")     return std::make_unique<ReverbFx>();
    if (id == "delay")      return std::make_unique<DelayFx>();
    if (id == "noisegate")  return std::make_unique<NoiseGateFx>();
    if (id == "gater")      return std::make_unique<GaterFx>();
    if (id == "phaser")     return std::make_unique<PhaserFx>();
    if (id == "chorus")     return std::make_unique<ChorusFx>();
    return nullptr;
}

// =============================================================================
//  Knob-based editor shared by all built-in effects
// =============================================================================
class BuiltinFxEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit BuiltinFxEditor (BuiltinFx& p) : AudioProcessorEditor (p), fx (p)
    {
        for (auto* param : fx.getParameters())
        {
            auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);
            if (ranged == nullptr) continue;
            auto c = std::make_unique<Control>();
            c->label.setText (ranged->getName (32), juce::dontSendNotification);
            c->label.setJustificationType (juce::Justification::centred);
            c->label.setFont (theme::font (12.0f, true));
            c->label.setColour (juce::Label::textColourId, theme::textDim);
            addAndMakeVisible (c->label);

            if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (ranged))
            {
                c->combo.addItemList (choice->choices, 1);
                addAndMakeVisible (c->combo);
                c->comboAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (fx.state, ranged->paramID, c->combo);
            }
            else if (dynamic_cast<juce::AudioParameterBool*> (ranged) != nullptr)
            {
                c->toggle.setButtonText ("On");
                c->toggle.setClickingTogglesState (true);
                addAndMakeVisible (c->toggle);
                c->toggleAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (fx.state, ranged->paramID, c->toggle);
            }
            else
            {
                c->knob.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
                c->knob.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 74, 18);
                addAndMakeVisible (c->knob);
                c->knobAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (fx.state, ranged->paramID, c->knob);
                c->knob.setDoubleClickReturnValue (true, ranged->convertFrom0to1 (ranged->getDefaultValue()));
            }
            controls.push_back (std::move (c));
        }

        const int cols = juce::jmin (5, (int) controls.size());
        const int rows = ((int) controls.size() + cols - 1) / cols;
        graphHeight = fx.hasResponseCurve() ? 150 : (fx.meterLabel.isNotEmpty() ? 46 : 0);
        setSize (juce::jmax (360, cols * 96 + 24), 52 + graphHeight + rows * 120 + 12);
        startTimerHz (20);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        auto r = getLocalBounds().reduced (12, 8);
        auto title = r.removeFromTop (34);
        g.setColour (theme::accent);
        g.fillRoundedRectangle (title.removeFromLeft (4).toFloat().reduced (0, 6), 2.0f);
        title.removeFromLeft (8);
        g.setColour (theme::text);
        g.setFont (theme::font (17.0f, true));
        g.drawText (fx.getName().toUpperCase(), title, juce::Justification::centredLeft);

        if (graphHeight <= 0) return;
        auto area = r.removeFromTop (graphHeight - 8).toFloat();
        g.setColour (theme::bg);
        g.fillRoundedRectangle (area, 6.0f);

        if (fx.hasResponseCurve())
        {
            // grid
            g.setFont (theme::font (10.0f));
            for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
            {
                const float x = freqToX (f, area);
                g.setColour (theme::grid);
                g.drawVerticalLine ((int) x, area.getY(), area.getBottom());
                g.setColour (theme::textDim);
                g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f), (int) x + 2, (int) area.getBottom() - 14, 30, 12, juce::Justification::left);
            }
            for (float db : { -12.0f, 0.0f, 12.0f })
            {
                g.setColour (std::abs (db) < 0.1f ? theme::gridBar : theme::grid);
                g.drawHorizontalLine ((int) dbToY (db, area), area.getX(), area.getRight());
            }

            juce::Path p;
            for (int x = 0; x <= (int) area.getWidth(); x += 2)
            {
                const double f = 20.0 * std::pow (1000.0, x / area.getWidth());
                const float db = juce::Decibels::gainToDecibels ((float) fx.getMagnitudeForFrequency (f), -60.0f);
                const float y = dbToY (juce::jlimit (-24.0f, 24.0f, db), area);
                if (x == 0) p.startNewSubPath (area.getX() + (float) x, y); else p.lineTo (area.getX() + (float) x, y);
            }
            juce::Path fill (p);
            fill.lineTo (area.getRight(), dbToY (0, area));
            fill.lineTo (area.getX(), dbToY (0, area));
            fill.closeSubPath();
            g.setColour (theme::accent.withAlpha (0.15f));
            g.fillPath (fill);
            g.setColour (theme::accent);
            g.strokePath (p, juce::PathStrokeType (2.0f));
        }
        else
        {
            // gain-reduction style meter
            auto bar = area.reduced (10, 12);
            g.setColour (theme::textDim);
            g.setFont (theme::font (11.0f, true));
            g.drawText (fx.meterLabel.toUpperCase(), bar.removeFromLeft (120), juce::Justification::centredLeft);
            const float v = juce::jlimit (0.0f, 24.0f, -fx.meterValue.load());
            g.setColour (theme::grid);
            g.fillRoundedRectangle (bar, 3.0f);
            g.setColour (theme::accent);
            g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * v / 24.0f), 3.0f);
            g.setColour (theme::text);
            g.drawText (juce::String (-v, 1) + " dB", bar, juce::Justification::centredRight);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        r.removeFromTop (34 + graphHeight + 6);
        const int cols = juce::jmin (5, (int) controls.size());
        const int w = r.getWidth() / juce::jmax (1, cols);
        for (size_t i = 0; i < controls.size(); ++i)
        {
            auto cell = juce::Rectangle<int> (r.getX() + (int) (i % (size_t) cols) * w, r.getY() + (int) (i / (size_t) cols) * 120, w, 112).reduced (4, 0);
            auto& c = *controls[i];
            c.label.setBounds (cell.removeFromTop (18));
            if (c.comboAttach) c.combo.setBounds (cell.withSizeKeepingCentre (cell.getWidth(), 26));
            else if (c.toggleAttach) c.toggle.setBounds (cell.withSizeKeepingCentre (60, 26));
            else c.knob.setBounds (cell);
        }
    }

private:
    struct Control
    {
        juce::Label label;
        juce::Slider knob;
        juce::ComboBox combo;
        juce::TextButton toggle;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> knobAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> comboAttach;
        std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> toggleAttach;
    };

    static float freqToX (double f, juce::Rectangle<float> a) { return a.getX() + a.getWidth() * (float) (std::log (f / 20.0) / std::log (1000.0)); }
    static float dbToY (float db, juce::Rectangle<float> a)   { return a.getCentreY() - db / 24.0f * (a.getHeight() * 0.5f - 6); }

    void timerCallback() override { if (graphHeight > 0) repaint (0, 40, getWidth(), graphHeight + 10); }

    BuiltinFx& fx;
    std::vector<std::unique_ptr<Control>> controls;
    int graphHeight = 0;
};

juce::AudioProcessorEditor* BuiltinFx::createEditor()
{
    if (auto* face = createFaceplate (*this)) return face;
    return new BuiltinFxEditor (*this);
}

} // namespace rs
