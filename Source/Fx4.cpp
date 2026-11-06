// =============================================================================
//  RENASOUND effects, fourth set:
//    Multiband Compressor  three bands, each with its own compressor
//    Shimmer Reverb        a big space with octaves rising (or falling) inside it
//    Tuner                 the note you're playing and how far off it is
//    Spectrum Analyser     every frequency, live
//    Vocal Remover         takes out (or keeps only) what's in the centre: usually the voice
// =============================================================================
#include "Effects.h"
#include "DspHelpers.h"
#include "UI/Faceplate.h"

namespace rs
{

using namespace facekit;

namespace
{
    std::unique_ptr<juce::AudioParameterChoice> choiceP (const char* id, const char* name, const juce::StringArray& items, int def)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def);
    }
    std::unique_ptr<juce::AudioParameterBool> boolP (const char* id, const char* name, bool def)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def);
    }
    void screenBox (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c) { g.setColour (c); g.fillRoundedRectangle (r, 8.0f); }
    void splitPanel (juce::Rectangle<int> all, juce::Rectangle<int>& screen, juce::Rectangle<int>& controls, int screenH)
    {
        auto r = all.reduced (20, 12);
        r.removeFromTop (40);
        screen = r.removeFromTop (screenH);
        r.removeFromTop (14);
        controls = r;
    }
    const char* noteNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
}

// =============================================================================
//  MULTIBAND COMPRESSOR
// =============================================================================
class MultibandFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    MultibandFx() : BuiltinFx ("multiband", "Multiband Compressor", layout()) {}
    static constexpr const char* thrIds[3] { "thr1", "thr2", "thr3" };
    static constexpr const char* ratioIds[3] { "ratio1", "ratio2", "ratio3" };
    static constexpr const char* gainIds[3] { "gain1", "gain2", "gain3" };

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("xlow", "Low / Mid", skewed (60, 1000, 250), 220, "Hz"));
        l.add (floatParam ("xhigh", "Mid / High", skewed (1000, 12000, 3500), 3200, "Hz"));
        const char* names[3] { "Low", "Mid", "High" };
        for (int b = 0; b < 3; ++b)
        {
            l.add (floatParam (thrIds[b], juce::String (names[b]) + " Threshold", Range (-50, 0), -20, "dB"));
            l.add (floatParam (ratioIds[b], juce::String (names[b]) + " Ratio", skewed (1, 20, 4), 3, ":1"));
            l.add (floatParam (gainIds[b], juce::String (names[b]) + " Gain", Range (-12, 12), 0, "dB"));
        }
        l.add (floatParam ("attack", "Attack", skewed (0.5f, 100, 10), 8, "ms"));
        l.add (floatParam ("release", "Release", skewed (20, 1000, 150), 160, "ms"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 100, "%"));
        l.add (floatParam ("output", "Output", Range (-12, 12), 0, "dB"));
        return l;
    }

    std::array<std::atomic<float>, 3> grNow {}, levelNow {};

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& c : ch) for (auto& f : c.f) f.reset();
        env.fill (0); gr.fill (0);
        lastLow = lastHigh = -1;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const double sr = currentSampleRate;
        const float f1 = param ("xlow"), f2 = juce::jmax (param ("xhigh"), f1 * 1.5f);
        if (! juce::approximatelyEqual (f1, lastLow) || ! juce::approximatelyEqual (f2, lastHigh))
        {
            // Linkwitz-Riley: two Butterworth sections per split, so the bands add back up flat
            const auto lp1 = BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, f1, 0.7071, 0), hp1 = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, f1, 0.7071, 0);
            const auto lp2 = BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, f2, 0.7071, 0), hp2 = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, f2, 0.7071, 0);
            for (auto& c : ch)
            {
                c.f[0].c = c.f[1].c = lp1; c.f[2].c = c.f[3].c = hp1;
                c.f[4].c = c.f[5].c = lp2; c.f[6].c = c.f[7].c = hp2;
                c.f[8].c = c.f[9].c = lp2; c.f[10].c = c.f[11].c = hp2;   // the low band's matching all-pass
            }
            lastLow = f1; lastHigh = f2;
        }
        const float att = (float) std::exp (-1.0 / (param ("attack") * 0.001 * sr));
        const float rel = (float) std::exp (-1.0 / (param ("release") * 0.001 * sr));
        float thr[3], ratio[3], mk[3];
        for (int k = 0; k < 3; ++k) { thr[k] = param (thrIds[k]); ratio[k] = param (ratioIds[k]); mk[k] = juce::Decibels::decibelsToGain (param (gainIds[k])); }
        const float mix = param ("mix") * 0.01f, out = juce::Decibels::decibelsToGain (param ("output"));
        float maxGr[3] {}, maxLvl[3] {};
        for (int i = 0; i < n; ++i)
        {
            float band[2][3];
            for (int c = 0; c < chans; ++c)
            {
                auto& f = ch[(size_t) c].f;
                const float x = b.getSample (c, i);
                float low = f[1].process (f[0].process (x, c), c);
                const float rest = f[3].process (f[2].process (x, c), c);
                const float mid = f[5].process (f[4].process (rest, c), c);
                const float high = f[7].process (f[6].process (rest, c), c);
                low = f[9].process (f[8].process (low, c), c) + f[11].process (f[10].process (low, c), c);   // phase-match the low band to the others
                band[c][0] = low; band[c][1] = mid; band[c][2] = high;
            }
            if (chans == 1) for (int k = 0; k < 3; ++k) band[1][k] = band[0][k];
            float y[2] {};
            for (int k = 0; k < 3; ++k)
            {
                const float lvl = juce::jmax (std::abs (band[0][k]), std::abs (band[1][k]));
                env[(size_t) k] = lvl > env[(size_t) k] ? lvl + att * (env[(size_t) k] - lvl) : lvl + rel * (env[(size_t) k] - lvl);
                const float db = juce::Decibels::gainToDecibels (env[(size_t) k], -100.0f);
                // a soft 6 dB knee
                const float over = db - thr[k];
                float red = 0;
                if (over > 3.0f) red = over * (1.0f - 1.0f / ratio[k]);
                else if (over > -3.0f) red = (1.0f - 1.0f / ratio[k]) * (over + 3.0f) * (over + 3.0f) / 12.0f;
                gr[(size_t) k] = red;
                const float g = juce::Decibels::decibelsToGain (-red) * mk[k];
                y[0] += band[0][k] * g; y[1] += band[1][k] * g;
                maxGr[k] = juce::jmax (maxGr[k], red);
                maxLvl[k] = juce::jmax (maxLvl[k], env[(size_t) k]);
            }
            for (int c = 0; c < chans; ++c) b.setSample (c, i, (mix * y[c] + (1.0f - mix) * b.getSample (c, i)) * out);
        }
        for (int k = 0; k < 3; ++k) { grNow[(size_t) k] = maxGr[k]; levelNow[(size_t) k] = juce::Decibels::gainToDecibels (maxLvl[k], -100.0f); }
        meterValue = -juce::jmax (maxGr[0], juce::jmax (maxGr[1], maxGr[2]));
        measureOut (b);
    }
private:
    struct Chan { std::array<Biquad, 12> f; };
    std::array<Chan, 2> ch;
    std::array<float, 3> env {}, gr {};
    float lastLow = -1, lastHigh = -1;
};

class MultibandFace : public EffectFace
{
public:
    explicit MultibandFace (MultibandFx& f)
        : EffectFace (f, { KnobStyle::Minimal, juce::Colour (0xff121822), juce::Colour (0xff59d0ff), juce::Colour (0xff59d0ff), juce::Colour (0xff222b38),
                           juce::Colour (0xffe6eef8), juce::Colour (0xff7d8ba0), juce::Colour (0xff0a0e14), juce::Colour (0xffe6eef8) }), mb (f)
    {
        for (int k = 0; k < 3; ++k)
        {
            const auto c = bandCol (k).brighter (0.3f);
            knob (MultibandFx::thrIds[k], "THRESHOLD", c, 56);
            knob (MultibandFx::ratioIds[k], "RATIO", c, 56);
            knob (MultibandFx::gainIds[k], "GAIN", c, 56);
        }
        const auto lc = juce::Colour (0xffb8c6d8);
        knob ("xlow", "LOW / MID", lc, 56); knob ("xhigh", "MID / HIGH", lc, 56);
        knob ("attack", "ATTACK", lc, 56); knob ("release", "RELEASE", lc, 56); knob ("mix", "MIX", lc, 56); knob ("output", "OUTPUT", lc, 56);
        setSize (980, 650);
        startTimerHz (30);
    }
    static juce::Colour bandCol (int k) { return k == 0 ? juce::Colour (0xffff7a59) : k == 1 ? juce::Colour (0xffffd25a) : juce::Colour (0xff59d0ff); }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0e131b));
        title (g, { 22, 10, 600, 30 }, "MULTIBAND COMPRESSOR", "LOW  /  MID  /  HIGH", juce::Colour (0xffe6eef8), juce::Colour (0xff59d0ff));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff070a0f));
        auto plot = a.reduced (12, 14);
        const float x1 = logX (mb.param ("xlow"), plot), x2 = logX (mb.param ("xhigh"), plot);
        const float edges[4] { plot.getX(), x1, x2, plot.getRight() };
        for (int k = 0; k < 3; ++k)
        {
            auto col = juce::Rectangle<float> (edges[k], plot.getY(), edges[k + 1] - edges[k], plot.getHeight());
            const auto c = bandCol (k);
            // the level (from below) and the gain reduction (from above)
            const float lvl = juce::jlimit (0.0f, 1.0f, (mb.levelNow[(size_t) k].load() + 60.0f) / 60.0f);
            g.setColour (c.withAlpha (0.18f)); g.fillRect (col.withTop (col.getBottom() - col.getHeight() * lvl));
            const float thrY = col.getBottom() - col.getHeight() * (mb.param (MultibandFx::thrIds[k]) + 60.0f) / 60.0f;
            g.setColour (c.withAlpha (0.8f)); g.drawHorizontalLine ((int) thrY, col.getX() + 4, col.getRight() - 4);
            const float gr = juce::jlimit (0.0f, 24.0f, mb.grNow[(size_t) k].load());
            g.setColour (c.withAlpha (0.75f)); g.fillRect (col.withHeight (col.getHeight() * gr / 24.0f).reduced (col.getWidth() * 0.3f, 0));
            g.setColour (c); g.setFont (theme::font (11.0f, true));
            g.drawText (juce::String (k == 0 ? "LOW" : k == 1 ? "MID" : "HIGH") + "   -" + juce::String (gr, 1) + " dB", col.reduced (8, 6).removeFromBottom (16), juce::Justification::centred);
        }
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        for (float x : { x1, x2 }) { g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom()); g.fillEllipse (x - 5, plot.getY() - 5, 10, 10); }
        g.setColour (juce::Colour (0xff7d8ba0)); g.setFont (theme::font (9.5f, true));
        for (auto [f, t] : { std::pair { 100.0, "100" }, { 1000.0, "1k" }, { 10000.0, "10k" } })
            g.drawText (t, juce::Rectangle<float> (logX (f, plot) - 20, plot.getBottom() + 1, 40, 12), juce::Justification::centred);
        for (int k = 0; k < 3; ++k)
        {
            g.setColour (bandCol (k)); g.setFont (theme::font (11.0f, true));
            g.drawText (k == 0 ? "LOW BAND" : k == 1 ? "MID BAND" : "HIGH BAND", bandLabels[(size_t) k], juce::Justification::centred);
        }
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        splitPanel (getLocalBounds(), screen, c, 200);
        auto top = c.removeFromTop (c.getHeight() / 2);
        const int w = top.getWidth() / 3;
        for (int k = 0; k < 3; ++k)
        {
            auto col = top.removeFromLeft (w).reduced (6, 0);
            bandLabels[(size_t) k] = col.removeFromTop (16);
            col.removeFromTop (6);
            const int kw = col.getWidth() / 3;
            for (int j = 0; j < 3; ++j) knobs[k * 3 + j]->setBounds (col.removeFromLeft (kw));
        }
        c.removeFromTop (16);
        row (c, 9, 15);
    }
    void mouseDown (const juce::MouseEvent& e) override { dragging = -1; auto plot = screen.toFloat().reduced (12, 14); for (int k = 0; k < 2; ++k) if (std::abs (e.position.x - logX (mb.param (k == 0 ? "xlow" : "xhigh"), plot)) < 10 && screen.contains (e.getPosition())) dragging = k; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        auto plot = screen.toFloat().reduced (12, 14);
        if (auto* p = mb.state.getParameter (dragging == 0 ? "xlow" : "xhigh")) p->setValueNotifyingHost (p->convertTo0to1 ((float) xToLogF (e.position.x, plot)));
    }
private:
    MultibandFx& mb;
    juce::Rectangle<int> screen;
    std::array<juce::Rectangle<int>, 3> bandLabels;
    int dragging = -1;
};

// =============================================================================
//  SHIMMER REVERB
// =============================================================================
class ShimmerFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static constexpr int lines = 8;
    ShimmerFx() : BuiltinFx ("shimmer", "Shimmer Reverb", layout()) {}
    double getTailLengthSeconds() const override { return 20.0; }
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("size", "Size", Range (0, 100), 75, "%"));
        l.add (floatParam ("decay", "Decay", skewed (0.5f, 30, 4), 6, "s"));
        l.add (floatParam ("shimmer", "Shimmer", Range (0, 100), 45, "%"));
        l.add (choiceP ("interval", "Interval", { "Octave up", "Fifth up", "Octave down", "Up and down" }, 0));
        l.add (floatParam ("damp", "Damping", skewed (1000, 18000, 5000), 7000, "Hz"));
        l.add (floatParam ("predelay", "Pre-delay", Range (0, 200), 20, "ms"));
        l.add (floatParam ("width", "Width", Range (0, 100), 100, "%"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 35, "%"));
        return l;
    }
    std::atomic<float> shimmerLevel { 0 }, tailLevel { 0 };

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        const int maxLen = (int) (sr * 0.25) + 16;
        for (auto& d : dl) { d.buf.assign ((size_t) maxLen, 0.0f); d.pos = 0; d.lp = 0; }
        pre.assign ((size_t) (sr * 0.25) + 4, 0.0f); prePos = 0;
        for (auto& s : shifters) { s.buf.assign (8192, 0.0f); s.pos = 0; s.ph = 0; }
        shimLp = 0; shimHp = 0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const double sr = currentSampleRate;
        const float size = 0.35f + 0.65f * param ("size") * 0.01f;
        static const float baseMs[lines] { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.1f, 73.3f };
        int len[lines];
        float fbGain[lines];
        const float rt = param ("decay");
        for (int k = 0; k < lines; ++k)
        {
            len[k] = juce::jlimit (16, (int) dl[(size_t) k].buf.size() - 2, (int) (baseMs[k] * size * 0.001 * sr * 2.4));
            fbGain[k] = std::pow (10.0f, -3.0f * (float) len[k] / (float) (rt * sr));   // reaches -60 dB after the decay time
        }
        const float dampK = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmin (param ("damp"), (float) sr * 0.45f) / (float) sr);
        const int preLen = juce::jlimit (1, (int) pre.size() - 1, (int) (param ("predelay") * 0.001 * sr));
        const float shimmer = param ("shimmer") * 0.01f;
        const int interval = (int) param ("interval");
        const float width = param ("width") * 0.01f, mix = param ("mix") * 0.01f;
        float shPeak = 0, tailPeak = 0;
        for (int i = 0; i < n; ++i)
        {
            const float inL = b.getSample (0, i), inR = chans > 1 ? b.getSample (1, i) : inL;
            pre[(size_t) prePos] = 0.5f * (inL + inR);
            const float x = pre[(size_t) ((prePos - preLen + (int) pre.size()) % (int) pre.size())];
            prePos = (prePos + 1) % (int) pre.size();
            // read the lines
            float o[lines];
            for (int k = 0; k < lines; ++k)
            {
                auto& d = dl[(size_t) k];
                const int rp = (d.pos - len[k] + (int) d.buf.size()) % (int) d.buf.size();
                o[k] = d.buf[(size_t) rp];
            }
            // the shimmer: what's in the space, pitched, goes back into the space
            float mixOut = 0;
            for (int k = 0; k < lines; ++k) mixOut += o[k];
            mixOut *= 0.25f;
            float sh = 0;
            if (shimmer > 0.001f)
            {
                if (interval == 0) sh = shift (0, mixOut, 2.0);
                else if (interval == 1) sh = shift (0, mixOut, 1.5);
                else if (interval == 2) sh = shift (0, mixOut, 0.5);
                else sh = 0.6f * (shift (0, mixOut, 2.0) + shift (1, mixOut, 0.5));
                shimHp += 0.002f * (sh - shimHp);
                sh = (sh - shimHp) * shimmer * 0.55f;
                shPeak = juce::jmax (shPeak, std::abs (sh));
            }
            // a Householder mix keeps the energy, and each line darkens a little each time round
            float sum = 0;
            for (int k = 0; k < lines; ++k) sum += o[k];
            sum *= 2.0f / lines;
            for (int k = 0; k < lines; ++k)
            {
                auto& d = dl[(size_t) k];
                float v = (o[k] - sum) * fbGain[k];
                d.lp += dampK * (v - d.lp);
                v = d.lp;
                d.buf[(size_t) d.pos] = v + (x + sh) * (k % 2 == 0 ? 0.5f : -0.5f);
                d.pos = (d.pos + 1) % (int) d.buf.size();
            }
            float wl = (o[0] + o[2] + o[4] + o[6]) * 0.35f, wr = (o[1] + o[3] + o[5] + o[7]) * 0.35f;
            const float m = 0.5f * (wl + wr), s = 0.5f * (wl - wr) * width;
            wl = m + s; wr = m - s;
            tailPeak = juce::jmax (tailPeak, std::abs (wl));
            b.setSample (0, i, inL * (1.0f - mix) + wl * mix);
            if (chans > 1) b.setSample (1, i, inR * (1.0f - mix) + wr * mix);
        }
        shimmerLevel = shPeak; tailLevel = tailPeak;
        if (historyTick (n)) pushHistory (juce::Decibels::gainToDecibels (tailPeak, -80.0f));
        measureOut (b);
    }
private:
    struct Line { std::vector<float> buf; int pos = 0; float lp = 0; };
    struct Shifter { std::vector<float> buf; int pos = 0; double ph = 0; };
    std::array<Line, lines> dl;
    std::array<Shifter, 2> shifters;
    std::vector<float> pre;
    int prePos = 0;
    float shimLp = 0, shimHp = 0;

    // a pitch shifter: two read heads sweeping through a short delay, crossfaded
    float shift (int idx, float x, double ratio)
    {
        auto& s = shifters[(size_t) idx];
        const int size = (int) s.buf.size();
        s.buf[(size_t) s.pos] = x;
        const double window = currentSampleRate * 0.06;
        s.ph += (1.0 - ratio) / window;
        s.ph -= std::floor (s.ph);
        float out = 0;
        for (int h = 0; h < 2; ++h)
        {
            const double p = std::fmod (s.ph + h * 0.5, 1.0);
            const double d = 2.0 + p * window;
            double rp = s.pos - d; while (rp < 0) rp += size;
            const int i0 = (int) rp; const float f = (float) (rp - i0);
            const float v = s.buf[(size_t) (i0 % size)] * (1 - f) + s.buf[(size_t) ((i0 + 1) % size)] * f;
            out += v * (float) std::sin (p * juce::MathConstants<double>::pi);   // each head fades in and out
        }
        s.pos = (s.pos + 1) % size;
        return out * 0.7071f;
    }
};

class ShimmerFace : public EffectFace
{
public:
    explicit ShimmerFace (ShimmerFx& f)
        : EffectFace (f, { KnobStyle::Glossy, juce::Colour (0xff141433), juce::Colour (0xffd9d4ff), juce::Colour (0xffa18cff), juce::Colour (0xff23234a),
                           juce::Colour (0xffeeeaff), juce::Colour (0xff9a96c8), juce::Colour (0xff0b0b1f), juce::Colour (0xffeeeaff) }), sh (f)
    {
        const auto lc = juce::Colour (0xffc4bff0);
        for (auto [id, name] : { std::pair { "size", "SIZE" }, { "decay", "DECAY" }, { "shimmer", "SHIMMER" }, { "damp", "DAMPING" },
                                 { "predelay", "PRE-DELAY" }, { "width", "WIDTH" }, { "mix", "MIX" } })
            knob (id, name, lc, 60);
        interval = choice ("interval");
        for (int i = 0; i < 70; ++i) sparks.push_back ({ rnd.nextFloat(), rnd.nextFloat(), 0.3f + rnd.nextFloat() });
        setSize (900, 500);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff1c1846), 0, 0, juce::Colour (0xff07061a), 0, (float) getHeight(), false);
        g.setGradientFill (bg); g.fillAll();
        title (g, { 22, 10, 600, 30 }, "SHIMMER REVERB", "A SPACE THAT RISES", juce::Colour (0xffeeeaff), juce::Colour (0xffa18cff));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff06051a));
        auto plot = a.reduced (12, 12);
        // the tail over time
        juce::Path tail;
        const int w0 = sh.historyPos.load();
        for (int i = 0; i < BuiltinFx::historySize; ++i)
        {
            const float v = juce::jlimit (0.0f, 1.0f, (sh.history[(size_t) ((w0 + i) % BuiltinFx::historySize)].load() + 60.0f) / 60.0f);
            const float x = plot.getX() + plot.getWidth() * (float) i / (BuiltinFx::historySize - 1);
            if (i == 0) tail.startNewSubPath (x, plot.getBottom());
            tail.lineTo (x, plot.getBottom() - v * plot.getHeight() * 0.7f);
        }
        tail.lineTo (plot.getRight(), plot.getBottom()); tail.closeSubPath();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xffa18cff).withAlpha (0.45f), 0, plot.getBottom(), juce::Colour (0xffa18cff).withAlpha (0.05f), 0, plot.getY(), false));
        g.fillPath (tail);
        // sparks rising (or falling, for an octave down) with the shimmer
        const float amt = sh.param ("shimmer") * 0.01f;
        const float lvl = juce::jlimit (0.0f, 1.0f, sh.shimmerLevel.load() * 8.0f + sh.tailLevel.load() * 2.0f);
        const bool down = sh.param ("interval") > 1.5f && sh.param ("interval") < 2.5f;
        const float t = (float) juce::Time::getMillisecondCounterHiRes() * 0.001f;
        for (auto& s : sparks)
        {
            float y = std::fmod (s.y + t * 0.08f * s.speed * (0.3f + amt), 1.0f);
            if (! down) y = 1.0f - y;
            const float x = plot.getX() + s.x * plot.getWidth();
            const float py = plot.getY() + y * plot.getHeight();
            const float al = amt * (0.2f + 0.8f * lvl) * (1.0f - std::abs (y - 0.5f));
            g.setColour (juce::Colour (0xffe9e3ff).withAlpha (juce::jlimit (0.0f, 1.0f, al)));
            g.fillEllipse (x - 1.5f, py - 1.5f, 3.0f, 3.0f);
        }
        g.setColour (juce::Colour (0xff9a96c8)); g.setFont (theme::font (10.5f, true));
        g.drawText (juce::String (sh.param ("decay"), 1) + " s", plot, juce::Justification::topRight);
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        splitPanel (getLocalBounds(), screen, c, 210);
        interval->setBounds (c.removeFromTop (28).withSizeKeepingCentre (460, 28));
        c.removeFromTop (8);
        row (c, 0, 7);
    }
private:
    ShimmerFx& sh;
    Switch* interval = nullptr;
    juce::Rectangle<int> screen;
    struct Spark { float x, y, speed; };
    std::vector<Spark> sparks;
    juce::Random rnd { 7 };
};

// =============================================================================
//  TUNER
// =============================================================================
class TunerFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    TunerFx() : BuiltinFx ("tuner", "Tuner", layout()) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("ref", "A4 =", Range (420, 460, 0.1f), 440, "Hz"));
        l.add (boolP ("mute", "Mute while tuning", false));
        return l;
    }
    std::atomic<float> hz { 0 }, clarity { 0 };

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        decim = juce::jmax (1, (int) std::round (sr / 24000.0));   // 24 kHz: fine enough for a cent at the top of a guitar
        rate = sr / decim;
        buf.assign (2048, 0.0f); xs.reserve (2048); ds.reserve (2048); cms.reserve (2048); pos = 0; count = 0; acc = 0; accN = 0; lp = 0;
        hz = 0;
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        for (int i = 0; i < n; ++i)
        {
            float x = 0;
            for (int c = 0; c < chans; ++c) x += b.getSample (c, i);
            lp += 0.3f * (x / (float) chans - lp);
            acc += lp;
            if (++accN >= decim)
            {
                buf[(size_t) pos] = acc / (float) accN; acc = 0; accN = 0;
                pos = (pos + 1) % (int) buf.size();
                if (++count >= 512) { count = 0; detect(); }
            }
        }
        if (param ("mute") > 0.5f) b.clear();
        measureOut (b);
    }
private:
    std::vector<float> buf, xs;          // (work space sized in prepareToPlay: detect() runs on the audio thread)
    std::vector<double> ds, cms;
    int pos = 0, count = 0, decim = 4, accN = 0;
    float acc = 0, lp = 0;
    double rate = 12000;
    void detect()
    {
        const int W = 1024, maxLag = juce::jmin ((int) buf.size() - W - 1, (int) (rate / 40.0)), minLag = (int) (rate / 1500.0);
        auto& x = xs; x.resize ((size_t) (W + maxLag));
        for (int i = 0; i < W + maxLag; ++i) x[(size_t) i] = buf[(size_t) ((pos - (W + maxLag) + i + (int) buf.size() * 2) % (int) buf.size())];
        double energy = 0; for (int i = 0; i < W; ++i) energy += x[(size_t) i] * x[(size_t) i];
        if (energy / W < 1.0e-7) { clarity = 0; return; }
        auto& d = ds; auto& cm = cms;
        d.assign ((size_t) maxLag + 1, 0.0); cm.assign ((size_t) maxLag + 1, 1.0);
        double run = 0;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            double s = 0;
            for (int i = 0; i < W; ++i) { const double v = x[(size_t) i] - x[(size_t) (i + tau)]; s += v * v; }
            d[(size_t) tau] = s; run += s;
            cm[(size_t) tau] = run > 0 ? s * tau / run : 1.0;
        }
        int pick = -1;
        for (int tau = minLag; tau < maxLag; ++tau)
            if (cm[(size_t) tau] < 0.12) { while (tau + 1 < maxLag && cm[(size_t) tau + 1] < cm[(size_t) tau]) ++tau; pick = tau; break; }
        if (pick < 2) { clarity = juce::jmax (0.0f, clarity.load() - 0.2f); return; }
        double t = pick;
        const double a = cm[(size_t) pick - 1], bb = cm[(size_t) pick], c = cm[(size_t) pick + 1], den = a - 2 * bb + c;
        if (std::abs (den) > 1e-12) t += 0.5 * (a - c) / den;
        const float f = (float) (rate / t);
        const float prev = hz.load();
        hz = prev > 0 && std::abs (1200.0f * std::log2 (f / prev)) < 30.0f ? prev + 0.35f * (f - prev) : f;   // steady when steady
        clarity = (float) (1.0 - cm[(size_t) pick]);
        meterValue = hz.load();
    }
};

class TunerFace : public EffectFace
{
public:
    explicit TunerFace (TunerFx& f)
        : EffectFace (f, { KnobStyle::Minimal, juce::Colour (0xff0f1110), juce::Colour (0xff46f28a), juce::Colour (0xff46f28a), juce::Colour (0xff1e2420),
                           juce::Colour (0xffe8f5ec), juce::Colour (0xff7f9a88), juce::Colour (0xff080a09), juce::Colour (0xffe8f5ec) }), tu (f)
    {
        knob ("ref", "A4 =", juce::Colour (0xffa9c4b2), 70);
        mute = toggle ("mute", "MUTE WHILE TUNING");
        setSize (720, 480);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0b0d0c));
        title (g, { 22, 10, 600, 30 }, "TUNER", "PLAY ONE NOTE", juce::Colour (0xffe8f5ec), juce::Colour (0xff46f28a));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff050706));
        const float f = tu.hz.load(), cl = tu.clarity.load();
        const float ref = tu.param ("ref");
        const bool have = f > 20.0f && cl > 0.5f;
        float cents = 0; int note = 69;
        if (have)
        {
            const double m = 69.0 + 12.0 * std::log2 (f / ref);
            note = (int) std::lround (m);
            cents = (float) ((m - note) * 100.0);
        }
        const bool inTune = have && std::abs (cents) < 3.0f;
        const auto col = ! have ? juce::Colour (0xff3a4a40) : inTune ? juce::Colour (0xff46f28a) : std::abs (cents) < 15 ? juce::Colour (0xffffd25a) : juce::Colour (0xffff5a5a);
        // the note, big
        g.setColour (col);
        g.setFont (theme::font (96.0f, true));
        const auto name = have ? juce::String (noteNames[(note % 12 + 12) % 12]) : juce::String ("-");
        g.drawText (name, a.withHeight (a.getHeight() * 0.55f), juce::Justification::centred);
        g.setFont (theme::font (16.0f, true));
        if (have) g.drawText (juce::String (note / 12 - 1), a.withHeight (a.getHeight() * 0.55f).translated (a.getWidth() * 0.12f, 18), juce::Justification::centred);
        // the needle scale, -50 .. +50 cents
        auto scale = a.reduced (40, 0).withTrimmedTop (a.getHeight() * 0.6f).withHeight (40);
        for (int c = -50; c <= 50; c += 5)
        {
            const float x = scale.getX() + scale.getWidth() * (c + 50) / 100.0f;
            g.setColour (c == 0 ? juce::Colour (0xff46f28a) : juce::Colour (0xff3a4a40));
            g.fillRect (x - (c % 25 == 0 ? 1.5f : 0.75f), scale.getY() + (c % 25 == 0 ? 0 : 10), c % 25 == 0 ? 3.0f : 1.5f, c % 25 == 0 ? 30.0f : 20.0f);
        }
        if (have)
        {
            const float x = scale.getX() + scale.getWidth() * (juce::jlimit (-50.0f, 50.0f, cents) + 50) / 100.0f;
            g.setColour (col); g.fillRoundedRectangle (x - 4, scale.getY() - 8, 8, 46, 4);
        }
        // a strobe band: still when in tune, drifting the way you're off
        auto strobe = a.reduced (40, 0).withY (scale.getBottom() + 18).withHeight (16);
        g.setColour (juce::Colour (0xff111612)); g.fillRoundedRectangle (strobe, 4);
        strobePos += have ? cents * 0.04f : 0.0f;
        g.setColour (col.withAlpha (0.8f));
        for (float x = std::fmod (strobePos, 24.0f) - 24.0f; x < strobe.getWidth(); x += 24.0f)
            if (x > 0 && x < strobe.getWidth() - 12) g.fillRect (strobe.getX() + x, strobe.getY() + 3, 12.0f, strobe.getHeight() - 6);
        g.setColour (juce::Colour (0xff7f9a88)); g.setFont (theme::font (12.0f, true));
        g.drawText (have ? juce::String (f, 1) + " Hz     " + (cents >= 0 ? "+" : "") + juce::String (cents, 1) + " cents" : juce::String ("listening..."),
                    a.withTrimmedTop (a.getHeight() - 26), juce::Justification::centred);
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        splitPanel (getLocalBounds(), screen, c, 300);
        auto r = c;
        knobs[0]->setBounds (r.removeFromLeft (120));
        mute->setBounds (r.removeFromRight (200).withSizeKeepingCentre (200, 30));
    }
private:
    TunerFx& tu;
    juce::TextButton* mute = nullptr;
    juce::Rectangle<int> screen;
    float strobePos = 0;
};

// =============================================================================
//  SPECTRUM ANALYSER
// =============================================================================
class AnalyzerFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static constexpr int order = 12, size = 1 << order, bands = 400;
    AnalyzerFx() : BuiltinFx ("analyzer", "Spectrum Analyser", layout()), fft (order), window (size, juce::dsp::WindowingFunction<float>::hann) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choiceP ("channel", "Show", { "Stereo", "Left", "Right", "Mid", "Side" }, 0));
        l.add (choiceP ("slope", "Slope", { "Flat", "3 dB/oct", "4.5 dB/oct" }, 2));
        l.add (floatParam ("speed", "Speed", Range (0, 100), 50, "%"));
        l.add (choiceP ("range", "Range", { "60 dB", "90 dB", "120 dB" }, 1));
        return l;
    }
    std::array<std::atomic<float>, bands> level {}, peak {};

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        fifo.assign (size, 0.0f); fifoPos = 0; hop = 0;
        for (auto& v : level) v = -120.0f;
        for (auto& v : peak) v = -120.0f;
        smooth.fill (-120.0f); peakHold.fill (-120.0f); holdAge.fill (0);
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const int mode = (int) param ("channel");
        for (int i = 0; i < n; ++i)
        {
            const float l = b.getSample (0, i), r = chans > 1 ? b.getSample (1, i) : l;
            fifo[(size_t) fifoPos] = mode == 1 ? l : mode == 2 ? r : mode == 4 ? 0.5f * (l - r) : 0.5f * (l + r);
            fifoPos = (fifoPos + 1) % size;
            if (++hop >= size / 4) { hop = 0; analyse(); }
        }
        measureOut (b);
    }
private:
    juce::dsp::FFT fft;
    juce::dsp::WindowingFunction<float> window;
    std::vector<float> fifo;
    std::array<float, size * 2> work {};
    std::array<float, bands> smooth {}, peakHold {};
    std::array<int, bands> holdAge {};
    int fifoPos = 0, hop = 0;
    void analyse()
    {
        for (int i = 0; i < size; ++i) work[(size_t) i] = fifo[(size_t) ((fifoPos + i) % size)];
        std::fill (work.begin() + size, work.end(), 0.0f);
        window.multiplyWithWindowingTable (work.data(), size);
        fft.performFrequencyOnlyForwardTransform (work.data());
        const double sr = currentSampleRate;
        const float slope = param ("slope") < 0.5f ? 0.0f : param ("slope") < 1.5f ? 3.0f : 4.5f;
        const float speed = param ("speed") * 0.01f;
        const float fall = 0.6f + 2.5f * speed;   // dB per frame
        for (int k = 0; k < bands; ++k)
        {
            // each display band covers a slice of the log scale; take the loudest bin in it
            const double f0 = 20.0 * std::pow (1000.0, (double) k / bands), f1 = 20.0 * std::pow (1000.0, (double) (k + 1) / bands);
            const int b0 = juce::jlimit (1, size / 2 - 1, (int) std::floor (f0 * size / sr)), b1 = juce::jlimit (b0, size / 2 - 1, (int) std::ceil (f1 * size / sr));
            float m = 0;
            for (int bin = b0; bin <= b1; ++bin) m = juce::jmax (m, work[(size_t) bin]);
            const double fc = std::sqrt (f0 * f1);
            float db = juce::Decibels::gainToDecibels (m * 4.0f / size, -140.0f) + slope * (float) std::log2 (fc / 1000.0);
            auto& s = smooth[(size_t) k];
            s = db > s ? db : s - fall;
            level[(size_t) k] = s;
            auto& p = peakHold[(size_t) k];
            if (s >= p) { p = s; holdAge[(size_t) k] = 0; } else if (++holdAge[(size_t) k] > 40) p -= 0.5f;
            peak[(size_t) k] = p;
        }
    }
};

class AnalyzerFace : public EffectFace
{
public:
    explicit AnalyzerFace (AnalyzerFx& f)
        : EffectFace (f, { KnobStyle::Minimal, juce::Colour (0xff101317), juce::Colour (0xff6fe3ff), juce::Colour (0xff6fe3ff), juce::Colour (0xff1e242c),
                           juce::Colour (0xffe8eef4), juce::Colour (0xff7d8894), juce::Colour (0xff090b0e), juce::Colour (0xffe8eef4) }), an (f)
    {
        channel = choice ("channel"); slope = choice ("slope"); range = choice ("range");
        knob ("speed", "SPEED", juce::Colour (0xffaab6c2), 56);
        setSize (980, 560);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0c0f12));
        title (g, { 22, 10, 600, 30 }, "SPECTRUM ANALYSER", "EVERY FREQUENCY, LIVE", juce::Colour (0xffe8eef4), juce::Colour (0xff6fe3ff));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff06080a));
        auto plot = a.reduced (40, 14).withTrimmedBottom (10);
        const float rangeDb = an.param ("range") < 0.5f ? 60.0f : an.param ("range") < 1.5f ? 90.0f : 120.0f;
        auto yOf = [&] (float db) { return plot.getY() + (-juce::jlimit (-rangeDb, 0.0f, db)) / rangeDb * plot.getHeight(); };
        g.setFont (theme::font (9.5f, true));
        for (float db = 0; db >= -rangeDb; db -= 12.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.06f)); g.drawHorizontalLine ((int) yOf (db), plot.getX(), plot.getRight());
            g.setColour (juce::Colour (0xff7d8894)); g.drawText (juce::String ((int) db), juce::Rectangle<float> (a.getX() + 4, yOf (db) - 6, 30, 12), juce::Justification::right);
        }
        for (double f : { 30.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            const float x = logX (f, plot);
            g.setColour (juce::Colours::white.withAlpha (0.06f)); g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (juce::Colour (0xff7d8894));
            g.drawText (f >= 1000 ? juce::String (f / 1000.0, 0) + "k" : juce::String ((int) f), juce::Rectangle<float> (x - 20, plot.getBottom() + 2, 40, 12), juce::Justification::centred);
        }
        juce::Path fill, line, pk;
        for (int k = 0; k < AnalyzerFx::bands; ++k)
        {
            const float x = plot.getX() + plot.getWidth() * (k + 0.5f) / AnalyzerFx::bands;
            const float y = yOf (an.level[(size_t) k].load()), py = yOf (an.peak[(size_t) k].load());
            if (k == 0) { fill.startNewSubPath (x, plot.getBottom()); line.startNewSubPath (x, y); pk.startNewSubPath (x, py); }
            fill.lineTo (x, y); line.lineTo (x, y); pk.lineTo (x, py);
        }
        fill.lineTo (plot.getRight(), plot.getBottom()); fill.closeSubPath();
        g.setGradientFill (juce::ColourGradient (juce::Colour (0xff6fe3ff).withAlpha (0.35f), 0, plot.getY(), juce::Colour (0xff6fe3ff).withAlpha (0.03f), 0, plot.getBottom(), false));
        g.fillPath (fill);
        g.setColour (juce::Colour (0xff6fe3ff)); g.strokePath (line, juce::PathStrokeType (1.6f));
        g.setColour (juce::Colour (0xffffd25a).withAlpha (0.5f)); g.strokePath (pk, juce::PathStrokeType (1.0f));
        // under the mouse: frequency, note, level
        if (mouse.x > plot.getX() && mouse.x < plot.getRight() && mouse.y > plot.getY() && mouse.y < plot.getBottom())
        {
            const double f = xToLogF (mouse.x, plot);
            const int k = juce::jlimit (0, AnalyzerFx::bands - 1, (int) ((mouse.x - plot.getX()) / plot.getWidth() * AnalyzerFx::bands));
            const int note = (int) std::lround (69 + 12 * std::log2 (f / 440.0));
            g.setColour (juce::Colours::white.withAlpha (0.3f)); g.drawVerticalLine ((int) mouse.x, plot.getY(), plot.getBottom());
            g.setColour (juce::Colour (0xffe8eef4)); g.setFont (theme::font (11.0f, true));
            g.drawText (juce::String (juce::roundToInt (f)) + " Hz  " + noteNames[(note % 12 + 12) % 12] + juce::String (note / 12 - 1) + "  " + juce::String (an.level[(size_t) k].load(), 1) + " dB",
                        juce::Rectangle<float> (mouse.x + 8, plot.getY() + 4, 200, 16), juce::Justification::left);
        }
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        splitPanel (getLocalBounds(), screen, c, 380);
        auto r = c.removeFromTop (30);
        channel->setBounds (r.removeFromLeft (360)); r.removeFromLeft (12);
        slope->setBounds (r.removeFromLeft (260)); r.removeFromLeft (12);
        range->setBounds (r.removeFromLeft (200));
        knobs[0]->setBounds (c.withTrimmedTop (6).removeFromRight (90).withY (screen.getBottom() + 50).withHeight (80));
        knobs[0]->setBounds (getLocalBounds().removeFromRight (110).removeFromBottom (100).reduced (10));
    }
    void mouseMove (const juce::MouseEvent& e) override { mouse = e.position; }
    void mouseExit (const juce::MouseEvent&) override { mouse = { -1, -1 }; }
private:
    AnalyzerFx& an;
    Switch* channel = nullptr; Switch* slope = nullptr; Switch* range = nullptr;
    juce::Rectangle<int> screen;
    juce::Point<float> mouse { -1, -1 };
};

// =============================================================================
//  VOCAL REMOVER / ISOLATOR
// =============================================================================
class VocalRemoverFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static constexpr int order = 11, fftSize = 1 << order, hop = fftSize / 4, bins = fftSize / 2 + 1;
    static constexpr int mapF = 48, mapP = 33;   // the picture: frequency rows x pan columns
    VocalRemoverFx() : BuiltinFx ("vocalremove", "Vocal Remover", layout()), fft (order)
    {
        window.resize (fftSize);
        for (int i = 0; i < fftSize; ++i) window[(size_t) i] = (float) std::sqrt (0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / fftSize));
    }
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choiceP ("mode", "Mode", { "Remove the centre", "Keep only the centre" }, 0));
        l.add (floatParam ("amount", "Amount", Range (0, 100), 100, "%"));
        l.add (floatParam ("width", "Centre Width", Range (0, 100), 30, "%"));
        l.add (floatParam ("low", "From", skewed (40, 1000, 150), 120, "Hz"));
        l.add (floatParam ("high", "To", skewed (2000, 20000, 8000), 10000, "Hz"));
        return l;
    }
    std::array<std::atomic<float>, mapF * mapP> energy {};

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        setLatencySamples (fftSize);
        for (int c = 0; c < 2; ++c) { inFifo[c].assign (fftSize, 0.0f); outAcc[c].assign (fftSize * 2, 0.0f); }
        fifoPos = 0; hopCount = 0; readPos = 0;
        for (auto& e : energy) e = 0;
        mapAcc.fill (0);
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples();
        if (b.getNumChannels() < 2) return;
        measureIn (b);
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c) inFifo[c][(size_t) fifoPos] = b.getSample (c, i);
            fifoPos = (fifoPos + 1) % fftSize;
            for (int c = 0; c < 2; ++c)
            {
                b.setSample (c, i, outAcc[c][(size_t) readPos]);
                outAcc[c][(size_t) readPos] = 0.0f;
            }
            readPos = (readPos + 1) % (fftSize * 2);
            if (++hopCount >= hop) { hopCount = 0; frame(); }
        }
        measureOut (b);
    }
private:
    juce::dsp::FFT fft;
    std::vector<float> window, inFifo[2], outAcc[2];
    std::array<float, fftSize * 2> L {}, R {};
    std::array<float, mapF * mapP> mapAcc {};
    int fifoPos = 0, hopCount = 0, readPos = 0, frames = 0;

    void frame()
    {
        for (int i = 0; i < fftSize; ++i)
        {
            const int p = (fifoPos + i) % fftSize;
            L[(size_t) i] = inFifo[0][(size_t) p] * window[(size_t) i];
            R[(size_t) i] = inFifo[1][(size_t) p] * window[(size_t) i];
        }
        std::fill (L.begin() + fftSize, L.end(), 0.0f);
        std::fill (R.begin() + fftSize, R.end(), 0.0f);
        fft.performRealOnlyForwardTransform (L.data(), true);
        fft.performRealOnlyForwardTransform (R.data(), true);
        const double sr = currentSampleRate;
        const bool keep = param ("mode") > 0.5f;
        const float amount = param ("amount") * 0.01f;
        const float width = 0.02f + 0.5f * param ("width") * 0.01f;   // how far from dead centre still counts as centre
        const float lo = param ("low"), hi = param ("high");
        for (auto& m : mapAcc) m *= 0.85f;
        for (int k = 0; k < bins; ++k)
        {
            const float lr = L[(size_t) (2 * k)], li = L[(size_t) (2 * k + 1)], rr = R[(size_t) (2 * k)], ri = R[(size_t) (2 * k + 1)];
            const float pl = lr * lr + li * li, pr = rr * rr + ri * ri;
            const float tot = pl + pr;
            const double f = (double) k * sr / fftSize;
            // where it sits: -1 left .. +1 right, and how alike the two sides are (phase)
            const float pan = tot > 1.0e-12f ? (pr - pl) / tot : 0.0f;
            const float cross = lr * rr + li * ri;
            const float alike = tot > 1.0e-12f ? 2.0f * cross / tot : 0.0f;   // 1 = identical, 0 = unrelated, -1 = opposite
            float centre = juce::jlimit (0.0f, 1.0f, 1.0f - std::abs (pan) / width) * juce::jlimit (0.0f, 1.0f, (alike - 0.3f) / 0.6f);
            if (f < lo || f > hi) centre = 0.0f;   // outside the range: left alone
            // the centre part (what both sides share), as complex numbers
            const float cr = 0.5f * (lr + rr), ci = 0.5f * (li + ri);
            const float m = centre * amount;
            float olr, oli, orr, ori;
            if (! keep) { olr = lr - m * cr; oli = li - m * ci; orr = rr - m * cr; ori = ri - m * ci; }
            else
            {
                const float k2 = (f < lo || f > hi) ? 1.0f - amount : 1.0f;   // keep only the centre: everything else goes
                olr = (lr * (1.0f - amount) + m * cr) * (f < lo || f > hi ? k2 : 1.0f);
                oli = (li * (1.0f - amount) + m * ci) * (f < lo || f > hi ? k2 : 1.0f);
                orr = (rr * (1.0f - amount) + m * cr) * (f < lo || f > hi ? k2 : 1.0f);
                ori = (ri * (1.0f - amount) + m * ci) * (f < lo || f > hi ? k2 : 1.0f);
            }
            L[(size_t) (2 * k)] = olr; L[(size_t) (2 * k + 1)] = oli;
            R[(size_t) (2 * k)] = orr; R[(size_t) (2 * k + 1)] = ori;
            // the picture
            if (k > 0 && f > 20 && f < 20000 && tot > 1.0e-10f)
            {
                const int fr = juce::jlimit (0, mapF - 1, (int) (std::log (f / 20.0) / std::log (1000.0) * mapF));
                const int pc = juce::jlimit (0, mapP - 1, (int) ((pan * 0.5f + 0.5f) * (mapP - 1) + 0.5f));
                mapAcc[(size_t) (fr * mapP + pc)] += tot;
            }
        }
        if (++frames % 2 == 0)
            for (size_t i = 0; i < mapAcc.size(); ++i) energy[i] = mapAcc[i];
        fft.performRealOnlyInverseTransform (L.data());
        fft.performRealOnlyInverseTransform (R.data());
        for (int i = 0; i < fftSize; ++i)
        {
            const int p = (readPos + i) % (fftSize * 2);
            outAcc[0][(size_t) p] += L[(size_t) i] * window[(size_t) i] * 0.5f;   // sqrt-Hann in and out at 75% overlap sums to 2
            outAcc[1][(size_t) p] += R[(size_t) i] * window[(size_t) i] * 0.5f;
        }
    }
};

class VocalRemoverFace : public EffectFace
{
public:
    explicit VocalRemoverFace (VocalRemoverFx& f)
        : EffectFace (f, { KnobStyle::Glossy, juce::Colour (0xff1c1220), juce::Colour (0xffff7ad9), juce::Colour (0xffff7ad9), juce::Colour (0xff2c1c33),
                           juce::Colour (0xfffbe9f6), juce::Colour (0xffa98aa6), juce::Colour (0xff110a14), juce::Colour (0xfffbe9f6) }), vr (f)
    {
        const auto lc = juce::Colour (0xffd8b8d4);
        knob ("amount", "AMOUNT", lc, 60); knob ("width", "CENTRE WIDTH", lc, 60); knob ("low", "FROM", lc, 60); knob ("high", "TO", lc, 60);
        mode = choice ("mode");
        setSize (860, 540);
        startTimerHz (30);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff120b15));
        const bool keep = vr.param ("mode") > 0.5f;
        title (g, { 22, 10, 600, 30 }, "VOCAL REMOVER", keep ? "KEEPS ONLY WHAT'S IN THE MIDDLE" : "TAKES OUT WHAT'S IN THE MIDDLE", juce::Colour (0xfffbe9f6), juce::Colour (0xffff7ad9));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff09050b));
        auto plot = a.reduced (40, 14).withTrimmedBottom (8);
        // where the sound sits: left .. right across, low .. high upwards; the pink band is what's taken (or kept)
        float mx = 1.0e-9f;
        for (auto& e : vr.energy) mx = juce::jmax (mx, e.load());
        const float cw = plot.getWidth() / VocalRemoverFx::mapP, ch = plot.getHeight() / VocalRemoverFx::mapF;
        const float width = 0.02f + 0.5f * vr.param ("width") * 0.01f;
        const double lo = vr.param ("low"), hi = vr.param ("high");
        for (int fr = 0; fr < VocalRemoverFx::mapF; ++fr)
        {
            const double f = 20.0 * std::pow (1000.0, (fr + 0.5) / VocalRemoverFx::mapF);
            for (int pc = 0; pc < VocalRemoverFx::mapP; ++pc)
            {
                const float e = vr.energy[(size_t) (fr * VocalRemoverFx::mapP + pc)].load();
                const float v = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (e / mx, -60.0f) + 60.0f) / 60.0f);
                const float pan = (float) pc / (VocalRemoverFx::mapP - 1) * 2.0f - 1.0f;
                const bool centre = std::abs (pan) < width && f >= lo && f <= hi;
                const auto c = centre ? juce::Colour (0xffff7ad9) : juce::Colour (0xff8fb4ff);
                g.setColour (c.withAlpha (v * 0.9f));
                g.fillRect (plot.getX() + pc * cw + 0.5f, plot.getBottom() - (fr + 1) * ch + 0.5f, cw - 1, ch - 1);
            }
        }
        // the band outline
        const float bx0 = plot.getCentreX() - width * plot.getWidth() * 0.5f, bx1 = plot.getCentreX() + width * plot.getWidth() * 0.5f;
        auto yF = [&] (double f) { return plot.getBottom() - (float) (std::log (f / 20.0) / std::log (1000.0)) * plot.getHeight(); };
        g.setColour (juce::Colour (0xffff7ad9).withAlpha (0.8f));
        g.drawRect (juce::Rectangle<float> (bx0, yF (hi), bx1 - bx0, yF (lo) - yF (hi)), 1.5f);
        g.setColour (juce::Colour (0xffa98aa6)); g.setFont (theme::font (10.0f, true));
        g.drawText ("LEFT", plot.withTrimmedTop (plot.getHeight() + 2).withHeight (12), juce::Justification::left);
        g.drawText ("CENTRE", plot.withTrimmedTop (plot.getHeight() + 2).withHeight (12), juce::Justification::centred);
        g.drawText ("RIGHT", plot.withTrimmedTop (plot.getHeight() + 2).withHeight (12), juce::Justification::right);
        for (double f : { 100.0, 1000.0, 10000.0 }) g.drawText (f >= 1000 ? juce::String (f / 1000.0, 0) + "k" : juce::String ((int) f), juce::Rectangle<float> (a.getX() + 2, yF (f) - 6, 34, 12), juce::Justification::right);
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        splitPanel (getLocalBounds(), screen, c, 300);
        mode->setBounds (c.removeFromTop (28).withSizeKeepingCentre (460, 28));
        c.removeFromTop (8);
        row (c, 0, 4);
    }
private:
    VocalRemoverFx& vr;
    Switch* mode = nullptr;
    juce::Rectangle<int> screen;
};

// =============================================================================
std::unique_ptr<BuiltinFx> createMultiband()    { return std::make_unique<MultibandFx>(); }
std::unique_ptr<BuiltinFx> createShimmer()      { return std::make_unique<ShimmerFx>(); }
std::unique_ptr<BuiltinFx> createTuner()        { return std::make_unique<TunerFx>(); }
std::unique_ptr<BuiltinFx> createAnalyzer()     { return std::make_unique<AnalyzerFx>(); }
std::unique_ptr<BuiltinFx> createVocalRemover() { return std::make_unique<VocalRemoverFx>(); }

juce::AudioProcessorEditor* createFaceplate4 (BuiltinFx& fx)
{
    if (auto* p = dynamic_cast<MultibandFx*> (&fx))    return new MultibandFace (*p);
    if (auto* p = dynamic_cast<ShimmerFx*> (&fx))      return new ShimmerFace (*p);
    if (auto* p = dynamic_cast<TunerFx*> (&fx))        return new TunerFace (*p);
    if (auto* p = dynamic_cast<AnalyzerFx*> (&fx))     return new AnalyzerFace (*p);
    if (auto* p = dynamic_cast<VocalRemoverFx*> (&fx)) return new VocalRemoverFace (*p);
    return nullptr;
}

} // namespace rs
