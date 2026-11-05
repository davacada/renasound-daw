// =============================================================================
//  RENASOUND effects, third set (classic studio tricks):
//    Rotary      a spinning speaker cabinet: horn and drum, slow / fast / brake
//    De-esser    tames harsh "s" and "t" sounds
//    Bit Crusher fewer bits, lower sample rate: lo-fi grit
//    Dub Delay   tape echo that can run away into feedback
//    Limiter     a loudness ceiling that nothing gets past
//    Ring Mod    multiplies the sound with a tone: metallic, robotic
//    Sub Bass    adds a sub an octave below the bass
//  Each one draws what it's doing, live.
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
    void screenBox (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float corner = 8.0f)
    {
        g.setColour (c);
        g.fillRoundedRectangle (r, corner);
    }
    void glow (juce::Graphics& g, const juce::Path& p, juce::Colour c, float w = 2.0f)
    {
        g.setColour (c.withAlpha (0.22f)); g.strokePath (p, juce::PathStrokeType (w * 3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);                   g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    // a short memory of the output for drawing waveforms
    struct Scope
    {
        static constexpr int size = 1024;
        std::array<std::atomic<float>, size> data {};
        std::atomic<int> pos { 0 };
        int decim = 0, every = 1;
        void push (float v) { if (++decim < every) return; decim = 0; const int p = pos.load(); data[(size_t) p] = v; pos = (p + 1) % size; }
        juce::Path path (juce::Rectangle<float> r, int count, float scale) const
        {
            juce::Path p;
            const int start = pos.load() - count;
            for (int i = 0; i < count; ++i)
            {
                const float v = data[(size_t) ((start + i + size * 4) % size)].load();
                const float x = r.getX() + r.getWidth() * (float) i / (float) (count - 1);
                const float y = r.getCentreY() - juce::jlimit (-1.0f, 1.0f, v * scale) * r.getHeight() * 0.48f;
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            return p;
        }
    };

    // a basic panel with a screen on top and a row of knobs, shared by this set
    struct PanelLayout
    {
        static void split (juce::Rectangle<int> all, juce::Rectangle<int>& screen, juce::Rectangle<int>& controls, int screenH)
        {
            auto r = all.reduced (20, 12);
            r.removeFromTop (40);
            screen = r.removeFromTop (screenH);
            r.removeFromTop (14);
            controls = r;
        }
    };
}

// =============================================================================
//  ROTARY
// =============================================================================
class RotaryFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    RotaryFx() : BuiltinFx ("rotary", "Rotary", layout()) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choiceP ("speed", "Speed", { "Slow", "Fast", "Brake" }, 0));
        l.add (floatParam ("ramp", "Ramp", Range (0, 100), 50, "%"));
        l.add (floatParam ("balance", "Horn / Drum", Range (-100, 100), 0, ""));
        l.add (floatParam ("drive", "Drive", Range (0, 100), 20, "%"));
        l.add (floatParam ("width", "Mic Width", Range (0, 100), 70, "%"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 100, "%"));
        return l;
    }
    std::atomic<float> hornAngle { 0 }, drumAngle { 0 }, hornHz { 0 }, drumHz { 0 };

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        xo.c = BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, 800, 0.707, 0); xo.reset();
        xo2.c = xo.c; xo2.reset();
        line.assign (4096, 0.0f); wp = 0; hRate = 0.8; dRate = 0.7; hPh = 0; dPh = 0.3;
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const int sp = (int) param ("speed");
        const double hornT = sp == 0 ? 0.8 : sp == 1 ? 6.7 : 0.0, drumT = sp == 0 ? 0.65 : sp == 1 ? 5.8 : 0.0;
        const double ramp = 0.3 + 3.0 * param ("ramp") * 0.01;   // seconds to get up to speed
        const double hk = 1.0 - std::exp (-1.0 / (ramp * 0.35 * currentSampleRate)), dk = 1.0 - std::exp (-1.0 / (ramp * currentSampleRate));   // the horn is lighter
        const float bal = param ("balance") * 0.01f, drive = 1.0f + param ("drive") * 0.05f, width = param ("width") * 0.01f, mix = param ("mix") * 0.01f;
        const float hornG = juce::jmin (1.0f, 1.0f + bal), drumG = juce::jmin (1.0f, 1.0f - bal);
        const double sr = currentSampleRate;
        for (int i = 0; i < n; ++i)
        {
            const float l = b.getSample (0, i), r = chans > 1 ? b.getSample (1, i) : l;
            const float x = std::tanh ((l + r) * 0.5f * drive) / drive;
            hRate += hk * (hornT - hRate); dRate += dk * (drumT - dRate);
            hPh = std::fmod (hPh + hRate / sr, 1.0); dPh = std::fmod (dPh + dRate / sr, 1.0);
            const float low = xo2.process (xo.process (x, 0), 0), high = x - low;
            line[(size_t) wp] = high;
            const double ha = hPh * juce::MathConstants<double>::twoPi, da = dPh * juce::MathConstants<double>::twoPi;
            // the horn: Doppler (a moving delay) and loudness changing as it points at / away from each mic
            auto horn = [&] (double micAngle)
            {
                const double rel = ha - micAngle;
                const double d = 18.0 + 14.0 * std::cos (rel) * sr / 48000.0;   // ~0.3 ms swing
                double pos = wp - d; while (pos < 0) pos += line.size();
                const int i0 = (int) pos; const float f = (float) (pos - i0);
                const float s = line[(size_t) (i0 % line.size())] * (1 - f) + line[(size_t) ((i0 + 1) % line.size())] * f;
                return s * (0.65f + 0.35f * (float) std::cos (rel));
            };
            auto drum = [&] (double micAngle) { return low * (0.8f + 0.2f * (float) std::cos (da - micAngle)); };
            const double micL = -juce::MathConstants<double>::halfPi * width, micR = juce::MathConstants<double>::halfPi * width;
            const float outL = 0.8f * (hornG * horn (micL) + drumG * drum (micL)), outR = 0.8f * (hornG * horn (micR) + drumG * drum (micR));
            wp = (wp + 1) % (int) line.size();
            b.setSample (0, i, mix * outL + (1 - mix) * l);
            if (chans > 1) b.setSample (1, i, mix * outR + (1 - mix) * r);
        }
        hornAngle = (float) hPh; drumAngle = (float) dPh; hornHz = (float) hRate; drumHz = (float) dRate;
        measureOut (b);
    }
private:
    Biquad xo, xo2;
    std::vector<float> line;
    int wp = 0;
    double hRate = 0.8, dRate = 0.7, hPh = 0, dPh = 0;
};

class RotaryFace : public EffectFace
{
public:
    explicit RotaryFace (RotaryFx& f)
        : EffectFace (f, { KnobStyle::ChickenHead, juce::Colour (0xff1a1a1a), juce::Colour (0xfff3e7cf), juce::Colour (0xffe0a25a), juce::Colour (0xff3a2a1c),
                           juce::Colour (0xfff3e7cf), juce::Colour (0xffbfa98a), juce::Colour (0xff1e140d), juce::Colour (0xfff3e7cf) }), rf (f)
    {
        const auto lc = juce::Colour (0xffe8d6b6);
        knob ("ramp", "RAMP", lc); knob ("balance", "HORN / DRUM", lc); knob ("drive", "DRIVE", lc); knob ("width", "MIC WIDTH", lc); knob ("mix", "MIX", lc);
        speed = choice ("speed");
        setSize (820, 520);
        startTimerHz (60);
    }
    void paint (juce::Graphics& g) override
    {
        // walnut cabinet
        juce::ColourGradient wood (juce::Colour (0xff6b3f22), 0, 0, juce::Colour (0xff3a2112), (float) getWidth(), (float) getHeight(), false);
        g.setGradientFill (wood); g.fillAll();
        juce::Random rnd (4);
        for (int i = 0; i < 70; ++i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.05f + rnd.nextFloat() * 0.06f));
            const float y = rnd.nextFloat() * getHeight();
            g.drawLine (0, y, (float) getWidth(), y + rnd.nextFloat() * 30 - 15, 1.0f + rnd.nextFloat() * 2);
        }
        title (g, { 22, 12, 600, 30 }, "ROTARY", "SPINNING SPEAKER CABINET", juce::Colour (0xfff3e7cf), juce::Colour (0xffe0a25a));

        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff140c07));
        // top view: the horn above, the drum below, two microphones at the sides
        // (horn on the left, drum on the right, seen from above)
        const auto hc = juce::Point<float> (a.getCentreX() - a.getHeight() * 0.42f, a.getCentreY());
        const auto dc = juce::Point<float> (a.getCentreX() + a.getHeight() * 0.42f, a.getCentreY());
        const float hornR = a.getHeight() * 0.36f, drumR = a.getHeight() * 0.3f;
        g.setColour (juce::Colour (0xff2b1b10)); g.fillEllipse (hc.x - hornR, hc.y - hornR, hornR * 2, hornR * 2);
        const float ha = rf.hornAngle.load() * juce::MathConstants<float>::twoPi;
        for (int k = 0; k < 2; ++k)   // the two horn bells
        {
            const float ang = ha + k * juce::MathConstants<float>::pi;
            juce::Path bell;
            bell.addTriangle (hc.x, hc.y, hc.x + std::cos (ang - 0.18f) * hornR, hc.y + std::sin (ang - 0.18f) * hornR,
                              hc.x + std::cos (ang + 0.18f) * hornR, hc.y + std::sin (ang + 0.18f) * hornR);
            g.setColour (k == 0 ? juce::Colour (0xffe0a25a) : juce::Colour (0xff8a6a46)); g.fillPath (bell);
        }
        // motion blur rings when fast
        const float hz = rf.hornHz.load();
        g.setColour (juce::Colour (0xffe0a25a).withAlpha (juce::jlimit (0.0f, 0.35f, hz / 20.0f)));
        g.drawEllipse (hc.x - hornR, hc.y - hornR, hornR * 2, hornR * 2, 3.0f);
        g.setColour (juce::Colour (0xff2b1b10)); g.fillEllipse (dc.x - drumR, dc.y - drumR, drumR * 2, drumR * 2);
        const float da = rf.drumAngle.load() * juce::MathConstants<float>::twoPi;
        juce::Path baffle;
        baffle.addPieSegment (dc.x - drumR, dc.y - drumR, drumR * 2, drumR * 2, da, da + juce::MathConstants<float>::pi, 0.25f);
        g.setColour (juce::Colour (0xffb98a5a)); g.fillPath (baffle);
        const float w = fx.param ("width") * 0.01f;
        for (int s : { -1, 1 })
        {
            const float mx = a.getCentreX() + s * (a.getHeight() * 0.42f + hornR + 30 + 70 * w);
            g.setColour (juce::Colour (0xffcfd3d6)); g.fillRoundedRectangle (mx - 6, hc.y - 30, 12, 30, 4);
            g.setColour (juce::Colour (0xff6c7277)); g.fillRect (mx - 1.0f, hc.y, 2.0f, 50.0f);
        }
        g.setColour (juce::Colour (0xffe8d6b6)); g.setFont (theme::font (11.0f, true));
        g.drawText ("HORN  " + juce::String (rf.hornHz.load(), 1) + " Hz", juce::Rectangle<float> (hc.x - 80, a.getBottom() - 24, 160, 16), juce::Justification::centred);
        g.drawText ("DRUM  " + juce::String (rf.drumHz.load(), 1) + " Hz", juce::Rectangle<float> (dc.x - 80, a.getBottom() - 24, 160, 16), juce::Justification::centred);
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        PanelLayout::split (getLocalBounds(), screen, c, 260);
        speed->setBounds (c.removeFromTop (30).withSizeKeepingCentre (360, 30));
        c.removeFromTop (8);
        row (c, 0, 5);
    }
private:
    RotaryFx& rf;
    Switch* speed = nullptr;
    juce::Rectangle<int> screen;
};

// =============================================================================
//  DE-ESSER
// =============================================================================
class DeEsserFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    DeEsserFx() : BuiltinFx ("deesser", "De-esser", layout()) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("freq", "Frequency", skewed (2000, 12000, 6000), 6000, "Hz"));
        l.add (floatParam ("threshold", "Threshold", Range (-50, 0), -24, "dB"));
        l.add (floatParam ("range", "Range", Range (0, 24), 10, "dB"));
        l.add (choiceP ("mode", "Mode", { "High band", "Whole signal" }, 0));
        l.add (boolP ("listen", "Listen", false));
        return l;
    }
    std::atomic<float> bandDb { -100 };
    void prepareToPlay (double sr, int) override { currentSampleRate = sr; for (auto& h : hp) h.reset(); for (auto& h : split) h.reset(); env = 0; gr = 0; }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const double sr = currentSampleRate;
        const float f = juce::jmin (param ("freq"), (float) sr * 0.45f);
        for (auto& h : hp) h.c = BiquadCoefs::make (BiquadCoefs::Type::BandPass, sr, f, 1.2, 0);
        for (auto& h : split) h.c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, f * 0.7, 0.707, 0);
        const float thr = param ("threshold"), range = param ("range");
        const bool whole = param ("mode") > 0.5f, listen = param ("listen") > 0.5f;
        const float att = (float) std::exp (-1.0 / (0.0005 * sr)), rel = (float) std::exp (-1.0 / (0.06 * sr));
        float maxGr = 0, maxBand = 0;
        for (int i = 0; i < n; ++i)
        {
            const float mono = 0.5f * (b.getSample (0, i) + (chans > 1 ? b.getSample (1, i) : b.getSample (0, i)));
            const float s = std::abs (hp[0].process (mono, 0));
            env = s > env ? s + (env - s) * att : s + (env - s) * rel;
            maxBand = juce::jmax (maxBand, env);
            const float over = juce::Decibels::gainToDecibels (env, -100.0f) - thr;
            const float want = juce::jlimit (0.0f, range, over * 0.75f);
            gr += 0.02f * (want - gr);
            maxGr = juce::jmax (maxGr, gr);
            const float g = juce::Decibels::decibelsToGain (-gr);
            for (int c = 0; c < chans; ++c)
            {
                const float x = b.getSample (c, i);
                const float hi = split[c].process (x, c), lo = x - hi;
                float y = whole ? x * g : lo + hi * g;
                if (listen) y = hp[1].process (x, c);   // hear what it listens to
                b.setSample (c, i, y);
            }
        }
        meterValue = -maxGr;
        bandDb = juce::Decibels::gainToDecibels (maxBand, -100.0f);
        if (historyTick (n)) pushHistory (-maxGr);
        measureOut (b);
    }
private:
    Biquad hp[2], split[2];
    float env = 0, gr = 0;
};

class DeEsserFace : public EffectFace
{
public:
    explicit DeEsserFace (DeEsserFx& f)
        : EffectFace (f, { KnobStyle::Minimal, juce::Colour (0xffeef1f6), juce::Colour (0xff2f6bff), juce::Colour (0xff2f6bff), juce::Colour (0xffd5dbe6),
                           juce::Colour (0xff1b2130), juce::Colour (0xff6b7488), juce::Colour (0xffffffff), juce::Colour (0xff1b2130) }), de (f)
    {
        const auto lc = juce::Colour (0xff4c5568);
        knob ("freq", "FREQUENCY", lc); knob ("threshold", "THRESHOLD", lc); knob ("range", "RANGE", lc);
        mode = choice ("mode"); listen = toggle ("listen", "LISTEN");
        setSize (760, 460);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xfff4f6fa));
        title (g, { 22, 12, 600, 30 }, "DE-ESSER", "SOFTER  S  AND  T", juce::Colour (0xff1b2130), juce::Colour (0xff2f6bff));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xffffffff));
        g.setColour (juce::Colour (0xffd5dbe6)); g.drawRoundedRectangle (a, 8.0f, 1.0f);
        auto plot = a.reduced (14, 12);
        // the "s" energy over time, the threshold, and the reduction (blue, from the top)
        const float thr = fx.param ("threshold");
        auto toY = [&] (float db) { return plot.getBottom() - (juce::jlimit (-60.0f, 0.0f, db) + 60.0f) / 60.0f * plot.getHeight(); };
        sHist[(size_t) sPos] = de.bandDb.load(); sPos = (sPos + 1) % (int) sHist.size();
        juce::Path e;
        for (int i = 0; i < (int) sHist.size(); ++i)
        {
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) (sHist.size() - 1);
            const float y = toY (sHist[(size_t) ((sPos + i) % (int) sHist.size())]);
            if (i == 0) e.startNewSubPath (x, y); else e.lineTo (x, y);
        }
        g.setColour (juce::Colour (0xff9aa6bd)); g.strokePath (e, juce::PathStrokeType (1.5f));
        g.setColour (juce::Colour (0xffff5a5a)); g.drawHorizontalLine ((int) toY (thr), plot.getX(), plot.getRight());
        juce::Path h;
        const int w0 = fx.historyPos.load();
        for (int i = 0; i < BuiltinFx::historySize; ++i)
        {
            const float v = juce::jlimit (0.0f, 24.0f, -fx.history[(size_t) ((w0 + i) % BuiltinFx::historySize)].load());
            const float x = plot.getX() + plot.getWidth() * (float) i / (BuiltinFx::historySize - 1);
            const float y = plot.getY() + v / 24.0f * plot.getHeight() * 0.6f;
            if (i == 0) { h.startNewSubPath (x, plot.getY()); h.lineTo (x, y); } else h.lineTo (x, y);
        }
        h.lineTo (plot.getRight(), plot.getY()); h.closeSubPath();
        g.setColour (juce::Colour (0xff2f6bff).withAlpha (0.35f)); g.fillPath (h);
        g.setColour (juce::Colour (0xff6b7488)); g.setFont (theme::font (10.0f, true));
        g.drawText ("S  ENERGY", plot, juce::Justification::bottomLeft);
        g.drawText ("REDUCTION  " + juce::String (-fx.meterValue.load(), 1) + " dB", plot, juce::Justification::topRight);
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        PanelLayout::split (getLocalBounds(), screen, c, 200);
        auto top = c.removeFromTop (28);
        mode->setBounds (top.removeFromLeft (300)); top.removeFromLeft (12);
        listen->setBounds (top.removeFromLeft (100));
        c.removeFromTop (8);
        row (c, 0, 3);
    }
private:
    DeEsserFx& de;
    Switch* mode = nullptr; juce::TextButton* listen = nullptr;
    juce::Rectangle<int> screen;
    std::array<float, 200> sHist {}; int sPos = 0;
};

// =============================================================================
//  BIT CRUSHER
// =============================================================================
class BitCrusherFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    BitCrusherFx() : BuiltinFx ("bitcrusher", "Bit Crusher", layout()) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("bits", "Bits", Range (1, 16), 8, "bit"));
        l.add (floatParam ("rate", "Sample Rate", skewed (500, 48000, 8000), 11025, "Hz"));
        l.add (floatParam ("drive", "Drive", Range (0, 24), 0, "dB"));
        l.add (floatParam ("tone", "Tone", skewed (500, 20000, 4000), 20000, "Hz"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 100, "%"));
        return l;
    }
    Scope scope;
    void prepareToPlay (double sr, int) override { currentSampleRate = sr; phase = 0; held[0] = held[1] = 0; for (auto& t : lp) t.reset(); scope.every = 1; }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const float bits = param ("bits"), levels = std::pow (2.0f, bits - 1.0f);
        const double step = juce::jmin (1.0, param ("rate") / currentSampleRate);
        const float drive = juce::Decibels::decibelsToGain (param ("drive")), mix = param ("mix") * 0.01f;
        for (auto& t : lp) t.c = BiquadCoefs::make (BiquadCoefs::Type::LowPass, currentSampleRate, juce::jmin (param ("tone"), (float) currentSampleRate * 0.45f), 0.707, 0);
        for (int i = 0; i < n; ++i)
        {
            phase += step;
            const bool sample = phase >= 1.0;
            if (sample) phase -= 1.0;
            for (int c = 0; c < chans; ++c)
            {
                const float x = b.getSample (c, i);
                if (sample) held[c] = std::round (juce::jlimit (-1.0f, 1.0f, x * drive) * levels) / levels;   // hold, then quantise
                const float y = lp[c].process (held[c], c);
                b.setSample (c, i, mix * y + (1 - mix) * x);
            }
            scope.push (b.getSample (0, i));
        }
        measureOut (b);
    }
private:
    double phase = 0;
    float held[2] {};
    Biquad lp[2];
};

class BitCrusherFace : public EffectFace
{
public:
    explicit BitCrusherFace (BitCrusherFx& f)
        : EffectFace (f, { KnobStyle::Neon, juce::Colour (0xff120a1e), juce::Colour (0xffff4fd8), juce::Colour (0xffff4fd8), juce::Colour (0xff2a1740),
                           juce::Colour (0xfff9e6ff), juce::Colour (0xffa88bc0), juce::Colour (0xff0a0612), juce::Colour (0xfff9e6ff) }), bc (f)
    {
        const auto lc = juce::Colour (0xffc7a8de);
        knob ("bits", "BITS", lc); knob ("rate", "SAMPLE RATE", lc); knob ("drive", "DRIVE", lc); knob ("tone", "TONE", lc); knob ("mix", "MIX", lc);
        setSize (780, 460);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0d0716));
        // a pixel grid, coarser with fewer bits
        const int cell = juce::jlimit (4, 24, 26 - (int) fx.param ("bits") * 2);
        g.setColour (juce::Colour (0xffff4fd8).withAlpha (0.05f));
        for (int x = 0; x < getWidth(); x += cell) g.drawVerticalLine (x, 0, (float) getHeight());
        for (int y = 0; y < getHeight(); y += cell) g.drawHorizontalLine (y, 0, (float) getWidth());
        g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 24.0f, juce::Font::bold)));
        g.setColour (juce::Colour (0xfff9e6ff));
        g.drawText ("BIT CRUSHER", 22, 12, 300, 30, juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xffff4fd8)); g.setFont (theme::font (10.5f, true));
        g.drawText (juce::String (juce::roundToInt (fx.param ("bits"))) + " BIT  /  " + juce::String (juce::roundToInt (fx.param ("rate"))) + " HZ", 250, 12, 300, 30, juce::Justification::centredLeft);
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff07040c));
        auto plot = a.reduced (12, 12);
        // the crushed waveform: steps you can see
        const float peak = juce::jmax (0.05f, juce::Decibels::decibelsToGain (fx.outputDb.load()));
        glow (g, bc.scope.path (plot, 300, 0.9f / peak), juce::Colour (0xffff4fd8), 1.6f);
    }
    void resized() override { juce::Rectangle<int> c; PanelLayout::split (getLocalBounds(), screen, c, 210); row (c, 0, 5); }
private:
    BitCrusherFx& bc;
    juce::Rectangle<int> screen;
};

// =============================================================================
//  DUB DELAY
// =============================================================================
class DubDelayFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    DubDelayFx() : BuiltinFx ("dubdelay", "Dub Delay", layout()) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choiceP ("sync", "Sync", { "Free", "1/4", "1/8 dotted", "1/8", "1/4 triplet", "1/2" }, 2));
        l.add (floatParam ("time", "Time", skewed (20, 2000, 350), 375, "ms"));
        l.add (floatParam ("feedback", "Feedback", Range (0, 110), 65, "%"));
        l.add (floatParam ("tone", "Tone", skewed (300, 12000, 2500), 2500, "Hz"));
        l.add (floatParam ("wow", "Wow", Range (0, 100), 25, "%"));
        l.add (floatParam ("drive", "Drive", Range (0, 100), 30, "%"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 35, "%"));
        return l;
    }
    std::atomic<float> reel { 0 }, delaySec { 0.375f }, loopLevel { 0 };
    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& l : line) l.assign ((size_t) (sr * 4.2), 0.0f);
        wp = 0; for (auto& t : lp) t.reset(); for (auto& t : hp) t.reset(); wowPh = 0; smoothDelay = 0.375 * sr; reelPh = 0;
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const double sr = currentSampleRate;
        double sec = param ("time") * 0.001;
        if (const int s = (int) param ("sync"); s > 0)
        {
            double bpm = 120;
            if (auto* ph = getPlayHead()) if (auto pos = ph->getPosition()) if (auto t = pos->getBpm()) bpm = *t;
            const double beats[] { 0, 1.0, 0.75, 0.5, 2.0 / 3.0, 2.0 };
            sec = beats[s] * 60.0 / bpm;
        }
        sec = juce::jlimit (0.01, 4.0, sec);
        delaySec = (float) sec;
        const float fb = param ("feedback") * 0.01f, wow = param ("wow") * 0.01f, drive = 1.0f + param ("drive") * 0.04f, mix = param ("mix") * 0.01f;
        for (int c = 0; c < 2; ++c)
        {
            lp[c].c = BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, juce::jmin (param ("tone"), (float) sr * 0.45f), 0.6, 0);
            hp[c].c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, 120, 0.6, 0);
        }
        const int size = (int) line[0].size();
        float lvl = 0;
        for (int i = 0; i < n; ++i)
        {
            smoothDelay += 0.0005 * (sec * sr - smoothDelay);   // changing the time pitches the echoes, like tape
            wowPh += (0.6 + 0.3 * std::sin (wowPh * 0.13)) / sr;
            const double d = smoothDelay * (1.0 + wow * 0.004 * std::sin (wowPh * juce::MathConstants<double>::twoPi));
            for (int c = 0; c < chans; ++c)
            {
                double pos = wp - d; while (pos < 0) pos += size;
                const int i0 = (int) pos; const float f = (float) (pos - i0);
                const float echo = line[c][(size_t) (i0 % size)] * (1 - f) + line[c][(size_t) ((i0 + 1) % size)] * f;
                const float x = b.getSample (c, i);
                // the loop: filtered, and saturated so feedback above 100% blooms instead of exploding
                float fbSig = hp[c].process (lp[c].process (echo, c), c) * fb;
                fbSig = std::tanh (fbSig * drive) / drive;
                line[c][(size_t) wp] = x + fbSig;
                lvl = juce::jmax (lvl, std::abs (echo));
                b.setSample (c, i, (mix > 0.99f ? 0.0f : x) + mix * echo);
            }
            wp = (wp + 1) % size;
            reelPh += 1.0 / sr;
        }
        reel = (float) std::fmod (reelPh * 0.8, 1.0);
        loopLevel = lvl;
        measureOut (b);
    }
private:
    std::array<std::vector<float>, 2> line;
    int wp = 0;
    Biquad lp[2], hp[2];
    double wowPh = 0, smoothDelay = 0, reelPh = 0;
};

class DubDelayFace : public EffectFace
{
public:
    explicit DubDelayFace (DubDelayFx& f)
        : EffectFace (f, { KnobStyle::Vintage, juce::Colour (0xff111111), juce::Colour (0xfff2d27a), juce::Colour (0xfff2d27a), juce::Colour (0xff3b3b2a),
                           juce::Colour (0xfff1ecd8), juce::Colour (0xffb8b096), juce::Colour (0xff1e1d14), juce::Colour (0xfff1ecd8) }), dd (f)
    {
        const auto lc = juce::Colour (0xffe9e1c2);
        knob ("time", "TIME", lc); knob ("feedback", "FEEDBACK", lc); knob ("tone", "TONE", lc); knob ("wow", "WOW", lc); knob ("drive", "DRIVE", lc); knob ("mix", "MIX", lc);
        sync = choice ("sync");
        setSize (840, 500);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        // dub colours: green, gold, red stripes
        g.fillAll (juce::Colour (0xff1f2a1c));
        const juce::Colour stripes[] { juce::Colour (0xff1f7a3a), juce::Colour (0xfff2c230), juce::Colour (0xffc8322b) };
        for (int i = 0; i < 3; ++i) { g.setColour (stripes[i]); g.fillRect (0, getHeight() - 24 + i * 8, getWidth(), 8); }
        title (g, { 22, 12, 600, 30 }, "DUB DELAY", "TAPE ECHO", juce::Colour (0xfff1ecd8), juce::Colour (0xfff2c230));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff141b12));
        // two tape reels and a loop of tape with the echoes on it
        const float rr = a.getHeight() * 0.32f;
        const juce::Point<float> c1 (a.getX() + a.getWidth() * 0.18f, a.getCentreY()), c2 (a.getRight() - a.getWidth() * 0.18f, a.getCentreY());
        const float ang = dd.reel.load() * juce::MathConstants<float>::twoPi;
        for (auto c : { c1, c2 })
        {
            g.setColour (juce::Colour (0xff3a3426)); g.fillEllipse (c.x - rr, c.y - rr, rr * 2, rr * 2);
            g.setColour (juce::Colour (0xff6b5f45)); g.fillEllipse (c.x - rr * 0.35f, c.y - rr * 0.35f, rr * 0.7f, rr * 0.7f);
            for (int k = 0; k < 3; ++k)
            {
                const float t = ang + k * juce::MathConstants<float>::twoPi / 3.0f;
                g.setColour (juce::Colour (0xff141b12)); g.fillEllipse (c.x + std::cos (t) * rr * 0.62f - 10, c.y + std::sin (t) * rr * 0.62f - 10, 20, 20);
            }
        }
        g.setColour (juce::Colour (0xff4a3f2a));
        g.drawLine (c1.x, c1.y - rr, c2.x, c2.y - rr, 3.0f);
        g.drawLine (c1.x, c1.y + rr, c2.x, c2.y + rr, 3.0f);
        // the echoes as pulses running along the tape, fading by the feedback
        const float fb = fx.param ("feedback") * 0.01f;
        const float span = c2.x - c1.x;
        const float level = juce::jlimit (0.0f, 1.0f, dd.loopLevel.load() * 2.0f);
        for (int k = 0; k < 10; ++k)
        {
            const float amp = std::pow (juce::jmin (fb, 1.05f), (float) k);
            const float x = c1.x + std::fmod (dd.reel.load() * span * 0.5f + k * span / 10.0f, span);
            const float h = 6 + 40 * amp * (0.3f + 0.7f * level);
            g.setColour (juce::Colour (0xfff2c230).withAlpha (juce::jlimit (0.05f, 0.9f, amp)));
            g.fillRoundedRectangle (x - 3, c1.y + rr - h - 4, 6, h, 3);
        }
        g.setColour (juce::Colour (0xffb8b096)); g.setFont (theme::font (11.0f, true));
        g.drawText (juce::String (juce::roundToInt (dd.delaySec.load() * 1000.0f)) + " ms" + (fb > 1.0f ? "   RUNAWAY" : ""), a.reduced (12, 8), juce::Justification::topRight);
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        PanelLayout::split (getLocalBounds().withTrimmedBottom (24), screen, c, 200);
        sync->setBounds (c.removeFromTop (28).withSizeKeepingCentre (520, 28));
        c.removeFromTop (8);
        row (c, 0, 6);
    }
    void timerCallback() override
    {
        knobs[0]->setAlpha (fx.param ("sync") < 0.5f ? 1.0f : 0.45f);
        repaint();
    }
private:
    DubDelayFx& dd;
    Switch* sync = nullptr;
    juce::Rectangle<int> screen;
};

// =============================================================================
//  LIMITER
// =============================================================================
class LimiterFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    LimiterFx() : BuiltinFx ("limiter", "Limiter", layout()) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("gain", "Gain", Range (0, 24), 0, "dB"));
        l.add (floatParam ("ceiling", "Ceiling", Range (-12, 0), -0.3f, "dB"));
        l.add (floatParam ("release", "Release", skewed (5, 1000, 80), 60, "ms"));
        return l;
    }
    static constexpr int look = 64;   // samples of look-ahead (~1.3 ms)
    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        setLatencySamples (look);
        for (auto& d : dl) d.assign (look + 1, 0.0f);
        pos = 0; env = 1; peakWin.assign (look + 1, 0.0f);
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const float gain = juce::Decibels::decibelsToGain (param ("gain")), ceil = juce::Decibels::decibelsToGain (param ("ceiling"));
        const float rel = (float) std::exp (-1.0 / (param ("release") * 0.001 * currentSampleRate));
        float minG = 1;
        for (int i = 0; i < n; ++i)
        {
            float pk = 0;
            for (int c = 0; c < chans; ++c) pk = juce::jmax (pk, std::abs (b.getSample (c, i) * gain));
            peakWin[(size_t) pos] = pk;
            float winMax = 0;
            for (auto v : peakWin) winMax = juce::jmax (winMax, v);   // the loudest sample about to arrive
            const float want = winMax > ceil ? ceil / winMax : 1.0f;
            env = want < env ? env + (want - env) * 0.25f : want + (env - want) * rel;   // quick down, smooth up
            minG = juce::jmin (minG, env);
            for (int c = 0; c < chans; ++c)
            {
                auto& d = dl[(size_t) c];
                const float delayed = d[(size_t) pos];
                d[(size_t) pos] = b.getSample (c, i) * gain;
                b.setSample (c, i, juce::jlimit (-ceil, ceil, delayed * env));   // the ceiling is absolute
            }
            pos = (pos + 1) % (look + 1);
        }
        meterValue = juce::Decibels::gainToDecibels (minG);
        if (historyTick (n)) pushHistory (outputDb.load());
        measureOut (b);
        if (historyPos.load() >= 0) grHist[(size_t) ((historyPos.load() + BuiltinFx::historySize - 1) % BuiltinFx::historySize)] = meterValue.load();
    }
    std::array<std::atomic<float>, BuiltinFx::historySize> grHist {};
private:
    std::array<std::vector<float>, 2> dl;
    std::vector<float> peakWin;
    int pos = 0;
    float env = 1;
};

class LimiterFace : public EffectFace
{
public:
    explicit LimiterFace (LimiterFx& f)
        : EffectFace (f, { KnobStyle::Glossy, juce::Colour (0xff2a0f0f), juce::Colour (0xffffffff), juce::Colour (0xffff3b3b), juce::Colour (0xff3a1a1a),
                           juce::Colour (0xfffff0f0), juce::Colour (0xffc99a9a), juce::Colour (0xff160606), juce::Colour (0xfffff0f0) }), lm (f)
    {
        const auto lc = juce::Colour (0xffe8b9b9);
        knob ("gain", "GAIN", lc, 70); knob ("ceiling", "CEILING", lc, 70); knob ("release", "RELEASE", lc, 70);
        setSize (760, 460);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff2b0d0d), 0, 0, juce::Colour (0xff0e0404), 0, (float) getHeight(), false);
        g.setGradientFill (bg); g.fillAll();
        title (g, { 22, 12, 600, 30 }, "LIMITER", "NOTHING GETS PAST THE CEILING", juce::Colour (0xfffff0f0), juce::Colour (0xffff3b3b));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff120505));
        auto plot = a.reduced (12, 12).withTrimmedRight (90);
        auto toY = [&] (float db) { return plot.getY() + (-juce::jlimit (-36.0f, 0.0f, db)) / 36.0f * plot.getHeight(); };
        // the output level over time, squashed against the ceiling line
        juce::Path lvl;
        const int w0 = fx.historyPos.load();
        for (int i = 0; i < BuiltinFx::historySize; ++i)
        {
            const float v = fx.history[(size_t) ((w0 + i) % BuiltinFx::historySize)].load();
            const float x = plot.getX() + plot.getWidth() * (float) i / (BuiltinFx::historySize - 1);
            if (i == 0) { lvl.startNewSubPath (x, plot.getBottom()); }
            lvl.lineTo (x, toY (v));
        }
        lvl.lineTo (plot.getRight(), plot.getBottom()); lvl.closeSubPath();
        g.setColour (juce::Colour (0xffff3b3b).withAlpha (0.35f)); g.fillPath (lvl);
        const float ceil = fx.param ("ceiling");
        g.setColour (juce::Colours::white); g.drawHorizontalLine ((int) toY (ceil), plot.getX(), plot.getRight());
        g.setFont (theme::font (10.0f, true));
        g.drawText ("CEILING " + juce::String (ceil, 1) + " dB", plot.withHeight (14).withY (toY (ceil) - 16), juce::Justification::right);
        // the gain reduction meter on the right
        auto m = a.reduced (12, 12).removeFromRight (70);
        g.setColour (juce::Colour (0xff2a0d0d)); g.fillRoundedRectangle (m, 6);
        const float gr = juce::jlimit (0.0f, 24.0f, -fx.meterValue.load());
        g.setColour (juce::Colour (0xffff3b3b)); g.fillRoundedRectangle (m.withHeight (m.getHeight() * gr / 24.0f), 6);
        g.setColour (juce::Colour (0xfffff0f0));
        g.drawText ("-" + juce::String (gr, 1), m.withTrimmedTop (m.getHeight() - 20), juce::Justification::centred);
        g.drawText ("GR dB", m.withHeight (16), juce::Justification::centred);
    }
    void resized() override { juce::Rectangle<int> c; PanelLayout::split (getLocalBounds(), screen, c, 210); row (c, 0, 3); }
private:
    LimiterFx& lm;
    juce::Rectangle<int> screen;
};

// =============================================================================
//  RING MOD
// =============================================================================
class RingModFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    RingModFx() : BuiltinFx ("ringmod", "Ring Mod", layout()) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("freq", "Frequency", skewed (1, 5000, 300), 440, "Hz"));
        l.add (choiceP ("wave", "Wave", { "Sine", "Triangle", "Square" }, 0));
        l.add (floatParam ("lforate", "Sweep Rate", skewed (0.01f, 10, 1), 0.3f, "Hz"));
        l.add (floatParam ("lfodepth", "Sweep Depth", Range (0, 100), 0, "%"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 100, "%"));
        return l;
    }
    Scope scope, carrier;
    std::atomic<float> nowFreq { 440 };
    void prepareToPlay (double sr, int) override { currentSampleRate = sr; ph = 0; lph = 0; scope.every = 2; carrier.every = 2; }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const int wave = (int) param ("wave");
        const float base = param ("freq"), depth = param ("lfodepth") * 0.01f, mix = param ("mix") * 0.01f;
        const double lr = param ("lforate") / currentSampleRate;
        float f = base;
        for (int i = 0; i < n; ++i)
        {
            lph = std::fmod (lph + lr, 1.0);
            f = base * std::pow (2.0f, depth * 2.0f * (float) std::sin (lph * juce::MathConstants<double>::twoPi));   // sweep up to 2 octaves
            ph = std::fmod (ph + f / currentSampleRate, 1.0);
            float m;
            switch (wave)
            {
                case 1:  m = (float) (ph < 0.5 ? 4.0 * ph - 1.0 : 3.0 - 4.0 * ph); break;
                case 2:  m = ph < 0.5 ? 1.0f : -1.0f; break;
                default: m = (float) std::sin (ph * juce::MathConstants<double>::twoPi);
            }
            for (int c = 0; c < chans; ++c)
            {
                const float x = b.getSample (c, i);
                b.setSample (c, i, mix * x * m + (1 - mix) * x);
            }
            scope.push (b.getSample (0, i)); carrier.push (m);
        }
        nowFreq = f;
        measureOut (b);
    }
private:
    double ph = 0, lph = 0;
};

class RingModFace : public EffectFace
{
public:
    explicit RingModFace (RingModFx& f)
        : EffectFace (f, { KnobStyle::Neon, juce::Colour (0xff0a1410), juce::Colour (0xff9dff6b), juce::Colour (0xff4cff9a), juce::Colour (0xff17302a),
                           juce::Colour (0xffe9ffe9), juce::Colour (0xff86b39a), juce::Colour (0xff06100c), juce::Colour (0xffe9ffe9) }), rm (f)
    {
        const auto lc = juce::Colour (0xffa8d9bc);
        knob ("freq", "FREQUENCY", lc); knob ("lforate", "SWEEP RATE", lc); knob ("lfodepth", "SWEEP DEPTH", lc); knob ("mix", "MIX", lc);
        wave = choice ("wave");
        setSize (780, 480);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff07110d));
        // concentric rings pulsing with the carrier
        const float f = rm.nowFreq.load();
        for (int k = 0; k < 9; ++k)
        {
            const float rad = 30.0f + k * 34.0f + std::fmod ((float) juce::Time::getMillisecondCounter() * 0.0004f * std::log2 (f + 2.0f) * 10.0f, 34.0f);
            g.setColour (juce::Colour (0xff4cff9a).withAlpha (0.05f));
            g.drawEllipse ((float) getWidth() - 140 - rad, 60 - rad, rad * 2, rad * 2, 2.0f);
        }
        title (g, { 22, 12, 600, 30 }, "RING MOD", juce::String (juce::roundToInt (f)) + " HZ", juce::Colour (0xffe9ffe9), juce::Colour (0xff4cff9a));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff040b08));
        auto top = a.reduced (12, 10).removeFromTop (a.getHeight() * 0.35f);
        auto bot = a.reduced (12, 10).withTrimmedTop (a.getHeight() * 0.4f);
        g.setColour (juce::Colour (0xff86b39a)); g.setFont (theme::font (10.0f, true));
        g.drawText ("TONE", top, juce::Justification::topLeft);
        g.drawText ("RESULT", bot, juce::Justification::topLeft);
        glow (g, rm.carrier.path (top, 300, 0.9f), juce::Colour (0xff9dff6b).withAlpha (0.7f), 1.2f);
        const float peak = juce::jmax (0.05f, juce::Decibels::decibelsToGain (fx.outputDb.load()));
        glow (g, rm.scope.path (bot, 300, 0.9f / peak), juce::Colour (0xff4cff9a), 1.6f);
    }
    void resized() override
    {
        juce::Rectangle<int> c;
        PanelLayout::split (getLocalBounds(), screen, c, 210);
        wave->setBounds (c.removeFromTop (28).withSizeKeepingCentre (300, 28));
        c.removeFromTop (8);
        row (c, 0, 4);
    }
private:
    RingModFx& rm;
    Switch* wave = nullptr;
    juce::Rectangle<int> screen;
};

// =============================================================================
//  SUB BASS: an octave below what's playing
// =============================================================================
class SubBassFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    SubBassFx() : BuiltinFx ("subbass", "Sub Bass", layout()) {}
    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("level", "Sub Level", Range (0, 100), 60, "%"));
        l.add (floatParam ("tune", "Listen Below", skewed (40, 300, 120), 140, "Hz"));
        l.add (floatParam ("smooth", "Smooth", Range (0, 100), 60, "%"));
        l.add (floatParam ("dry", "Dry", Range (0, 100), 100, "%"));
        return l;
    }
    Scope scopeIn, scopeSub;
    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& f : lp) f.reset(); for (auto& f : out) f.reset();
        sign = 1; last = 0; env = 0; scopeIn.every = 8; scopeSub.every = 8;
    }
    void releaseResources() override {}
    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const double sr = currentSampleRate;
        const float tune = param ("tune");
        for (auto& f : lp) f.c = BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, tune, 0.707, 0);
        const float sm = param ("smooth") * 0.01f;
        for (auto& f : out) f.c = BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, tune * (1.2f - 0.7f * sm), 0.6, 0);
        const float level = param ("level") * 0.01f * 2.0f, dry = param ("dry") * 0.01f;
        const float er = (float) std::exp (-1.0 / (0.02 * sr));
        for (int i = 0; i < n; ++i)
        {
            const float mono = 0.5f * (b.getSample (0, i) + (chans > 1 ? b.getSample (1, i) : b.getSample (0, i)));
            const float low = lp[1].process (lp[0].process (mono, 0), 0);
            env = juce::jmax (std::abs (low), env * er);
            // flip on every second upward zero crossing: a square an octave down, following the bass ...
            if (last <= 0 && low > 0) sign = -sign;
            last = low;
            // ... smoothed into a round sub, as loud as the bass it follows
            const float sub = out[1].process (out[0].process ((float) sign * env, 0), 0);
            for (int c = 0; c < chans; ++c) b.setSample (c, i, b.getSample (c, i) * dry + sub * level);
            scopeIn.push (low); scopeSub.push (sub * level);
        }
        measureOut (b);
    }
private:
    Biquad lp[2], out[2];
    int sign = 1;
    float last = 0, env = 0;
};

class SubBassFace : public EffectFace
{
public:
    explicit SubBassFace (SubBassFx& f)
        : EffectFace (f, { KnobStyle::Glossy, juce::Colour (0xff1a1240), juce::Colour (0xffb9a6ff), juce::Colour (0xff7b5cff), juce::Colour (0xff221a4a),
                           juce::Colour (0xffece6ff), juce::Colour (0xff9b90c9), juce::Colour (0xff0d0820), juce::Colour (0xffece6ff) }), sb (f)
    {
        const auto lc = juce::Colour (0xffb8addf);
        knob ("level", "SUB LEVEL", lc, 70); knob ("tune", "LISTEN BELOW", lc, 70); knob ("smooth", "SMOOTH", lc, 70); knob ("dry", "DRY", lc, 70);
        setSize (760, 470);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff1c1446), getWidth() * 0.5f, (float) getHeight(), juce::Colour (0xff07041a), getWidth() * 0.5f, 0, true);
        g.setGradientFill (bg); g.fillAll();
        // a speaker cone breathing with the sub
        const float pump = juce::jlimit (0.0f, 1.0f, (fx.outputDb.load() + 40.0f) / 40.0f);
        const auto c = juce::Point<float> ((float) getWidth() - 60, 30);
        for (int k = 4; k >= 1; --k)
        {
            const float rad = k * 6.0f * (1.0f + 0.06f * pump * (float) k / 4.0f);
            g.setColour (juce::Colour (0xff7b5cff).withAlpha (0.08f + 0.05f * (float) (5 - k)));
            g.fillEllipse (c.x - rad, c.y - rad, rad * 2, rad * 2);
        }
        title (g, { 22, 12, 600, 30 }, "SUB BASS", "AN OCTAVE BELOW", juce::Colour (0xffece6ff), juce::Colour (0xff7b5cff));
        auto a = screen.toFloat();
        screenBox (g, a, juce::Colour (0xff0a0618));
        auto plot = a.reduced (12, 12);
        g.setColour (juce::Colour (0xff9b90c9)); g.setFont (theme::font (10.0f, true));
        g.setColour (juce::Colour (0xffece6ff).withAlpha (0.6f)); g.drawText ("YOUR BASS", plot, juce::Justification::topRight);
        g.setColour (juce::Colour (0xff9b7cff)); g.drawText ("THE SUB", plot, juce::Justification::bottomRight);
        const float peak = juce::jmax (0.02f, juce::Decibels::decibelsToGain (fx.inputDb.load()));
        glow (g, sb.scopeIn.path (plot, 500, 0.6f / peak), juce::Colour (0xffece6ff).withAlpha (0.5f), 1.2f);
        glow (g, sb.scopeSub.path (plot, 500, 0.6f / peak), juce::Colour (0xff7b5cff), 2.4f);
    }
    void resized() override { juce::Rectangle<int> c; PanelLayout::split (getLocalBounds(), screen, c, 210); row (c, 0, 4); }
private:
    SubBassFx& sb;
    juce::Rectangle<int> screen;
};

// =============================================================================
std::unique_ptr<BuiltinFx> createRotary()     { return std::make_unique<RotaryFx>(); }
std::unique_ptr<BuiltinFx> createDeEsser()    { return std::make_unique<DeEsserFx>(); }
std::unique_ptr<BuiltinFx> createBitCrusher() { return std::make_unique<BitCrusherFx>(); }
std::unique_ptr<BuiltinFx> createDubDelay()   { return std::make_unique<DubDelayFx>(); }
std::unique_ptr<BuiltinFx> createLimiter()    { return std::make_unique<LimiterFx>(); }
std::unique_ptr<BuiltinFx> createRingMod()    { return std::make_unique<RingModFx>(); }
std::unique_ptr<BuiltinFx> createSubBass()    { return std::make_unique<SubBassFx>(); }

juce::AudioProcessorEditor* createFaceplate3 (BuiltinFx& fx)
{
    if (auto* p = dynamic_cast<RotaryFx*> (&fx))     return new RotaryFace (*p);
    if (auto* p = dynamic_cast<DeEsserFx*> (&fx))    return new DeEsserFace (*p);
    if (auto* p = dynamic_cast<BitCrusherFx*> (&fx)) return new BitCrusherFace (*p);
    if (auto* p = dynamic_cast<DubDelayFx*> (&fx))   return new DubDelayFace (*p);
    if (auto* p = dynamic_cast<LimiterFx*> (&fx))    return new LimiterFace (*p);
    if (auto* p = dynamic_cast<RingModFx*> (&fx))    return new RingModFace (*p);
    if (auto* p = dynamic_cast<SubBassFx*> (&fx))    return new SubBassFace (*p);
    return nullptr;
}

} // namespace rs
