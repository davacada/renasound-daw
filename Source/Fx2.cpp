// =============================================================================
//  RENASOUND effects, second set:
//    Auto-Tune       live pitch correction to a key / scale (or notes you pick)
//    Bus Compressor  clean "glue" compression with separate peak and body release
//    Color EQ        three-band mixing EQ with four curve characters and saturation
//    Voice Cleaner   takes the room noise and reverb out of a voice recording
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
    std::unique_ptr<juce::AudioParameterChoice> choice (const char* id, const char* name, const juce::StringArray& items, int def)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def);
    }
    std::unique_ptr<juce::AudioParameterBool> toggleParam (const juce::String& id, const juce::String& name, bool def)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def);
    }
    const char* noteNames[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    void screenBox (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c, float corner = 8.0f)
    {
        g.setColour (c);
        g.fillRoundedRectangle (r, corner);
        g.setColour (juce::Colours::white.withAlpha (0.03f));
        for (float y = r.getY() + 3; y < r.getBottom(); y += 3) g.drawHorizontalLine ((int) y, r.getX() + 3, r.getRight() - 3);
    }
    void glow (juce::Graphics& g, const juce::Path& p, juce::Colour c, float w = 2.0f)
    {
        g.setColour (c.withAlpha (0.22f)); g.strokePath (p, juce::PathStrokeType (w * 3.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.setColour (c);                   g.strokePath (p, juce::PathStrokeType (w, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
}

// =============================================================================
//  AUTO-TUNE
// =============================================================================
class AutoTuneFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static const juce::StringArray& scales()
    {
        static const juce::StringArray s { "Chromatic", "Major", "Minor", "Harmonic Minor", "Major Pentatonic", "Minor Pentatonic", "Blues", "Dorian", "Custom" };
        return s;
    }

    AutoTuneFx() : BuiltinFx ("autotune", "Auto-Tune", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choice ("key", "Key", juce::StringArray (noteNames, 12), 0));
        l.add (choice ("scale", "Scale", scales(), 0));
        l.add (floatParam ("speed", "Retune Speed", skewed (0, 400, 60), 20, "ms"));
        l.add (floatParam ("humanize", "Humanize", Range (0, 100), 20, "%"));
        l.add (floatParam ("amount", "Amount", Range (0, 100), 100, "%"));
        l.add (floatParam ("transpose", "Transpose", Range (-12, 12, 1), 0, "st"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 100, "%"));
        for (int i = 0; i < 12; ++i)
            l.add (toggleParam (("n" + juce::String (i)).toRawUTF8(), juce::String ("Note ") + noteNames[i], true));
        return l;
    }

    // which of the 12 notes are allowed
    bool allowed (int pc) const
    {
        static const bool table[8][12] {
            { 1,1,1,1,1,1,1,1,1,1,1,1 },   // chromatic
            { 1,0,1,0,1,1,0,1,0,1,0,1 },   // major
            { 1,0,1,1,0,1,0,1,1,0,1,0 },   // minor
            { 1,0,1,1,0,1,0,1,1,0,0,1 },   // harmonic minor
            { 1,0,1,0,1,0,0,1,0,1,0,0 },   // major pentatonic
            { 1,0,0,1,0,1,0,1,0,0,1,0 },   // minor pentatonic
            { 1,0,0,1,0,1,1,1,0,0,1,0 },   // blues
            { 1,0,1,1,0,1,0,1,0,1,1,0 } }; // dorian
        const int sc = (int) param ("scale");
        if (sc >= 8) return param (("n" + juce::String (pc)).toRawUTF8()) > 0.5f;
        return table[sc][((pc - (int) param ("key")) % 12 + 12) % 12];
    }

    float nearestAllowed (float midi) const
    {
        const int base = (int) std::round (midi);
        for (int d = 0; d < 12; ++d)
        {
            const int a = base + d, b = base - d;
            const bool ai = allowed (((a % 12) + 12) % 12), bi = allowed (((b % 12) + 12) % 12);
            if (ai && bi) return (float) (std::abs (a - midi) < std::abs (b - midi) ? a : b);
            if (ai) return (float) a;
            if (bi) return (float) b;
        }
        return midi;
    }

    // ---- live readings ----
    static constexpr int histSize = 300;
    std::array<std::atomic<float>, histSize> detHist {}, outHist {};
    std::atomic<int> histPos { 0 };
    std::atomic<float> nowDetected { -1 }, nowTarget { -1 };

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        dec = juce::jmax (1, (int) std::round (sr / 12000.0));
        decRate = sr / dec;
        for (auto& b : aa) { b.c = BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, decRate * 0.4, 0.707, 0); b.reset(); }
        ana.assign (anaSize, 0.0f); anaPos = 0; decCount = 0; hopCount = 0;
        for (auto& b : buf) b.assign (bufSize, 0.0f);
        w = 0; delay = 600; xfade = -1; ratio = 1; period = 300;
        detected = -1; slow = -1; corrected = -1; recent.fill (-1);
        for (auto& h : detHist) h = -1;
        for (auto& h : outHist) h = -1;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const float mix = param ("mix") * 0.01f;
        for (int i = 0; i < n; ++i)
        {
            const float l = b.getSample (0, i), r = chans > 1 ? b.getSample (1, i) : l;
            analyse (0.5f * (l + r));

            // pitch-synchronous delay-line shifter: the read point drifts at the new speed and jumps
            // back or forward by whole periods (with a short crossfade) to stay in range
            buf[0][(size_t) w] = l; buf[1][(size_t) w] = r;
            delay += 1.0 - ratio;
            const double lo = period + 64, hi = lo + 2.0 * period;
            if (xfade < 0 && (delay < lo || delay > hi))
            {
                oldDelay = delay;
                const double k = std::ceil ((delay < lo ? lo - delay : delay - hi) / period);
                delay += (delay < lo ? 1.0 : -1.0) * k * period;
                xfade = 0; xfadeLen = juce::jlimit (64, 1024, (int) period);
            }
            float outL, outR;
            if (xfade >= 0)
            {
                oldDelay += 1.0 - ratio;
                const float t = (float) xfade / (float) xfadeLen;
                const float g1 = std::sin (t * juce::MathConstants<float>::halfPi), g0 = std::cos (t * juce::MathConstants<float>::halfPi);
                outL = g0 * read (0, oldDelay) + g1 * read (0, delay);
                outR = g0 * read (1, oldDelay) + g1 * read (1, delay);
                if (++xfade >= xfadeLen) xfade = -1;
            }
            else { outL = read (0, delay); outR = read (1, delay); }
            w = (w + 1) & (bufSize - 1);

            b.setSample (0, i, mix * outL + (1 - mix) * l);
            if (chans > 1) b.setSample (1, i, mix * outR + (1 - mix) * r);
        }
        measureOut (b);
    }

    void setStateInformation (const void* d, int size) override { BuiltinFx::setStateInformation (d, size); }

private:
    static constexpr int anaSize = 512, bufSize = 16384;
    int dec = 4, decCount = 0, anaPos = 0, hopCount = 0;
    double decRate = 12000;
    Biquad aa[2];
    std::vector<float> ana;
    std::array<std::vector<float>, 2> buf;
    int w = 0, xfade = -1, xfadeLen = 256;
    double delay = 600, oldDelay = 600, ratio = 1, period = 300;
    float detected = -1, slow = -1, corrected = -1;
    std::array<float, 3> recent {};
    std::vector<float> dbuf, frame;

    float read (int ch, double d) const
    {
        double pos = w - d;
        while (pos < 0) pos += bufSize;
        const int i1 = (int) pos;
        const float f = (float) (pos - i1);
        const auto& v = buf[(size_t) ch];
        const float y0 = v[(size_t) ((i1 - 1) & (bufSize - 1))], y1 = v[(size_t) (i1 & (bufSize - 1))],
                    y2 = v[(size_t) ((i1 + 1) & (bufSize - 1))], y3 = v[(size_t) ((i1 + 2) & (bufSize - 1))];
        const float c1 = 0.5f * (y2 - y0), c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3, c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * f + c2) * f + c1) * f + y1;   // cubic (Hermite) interpolation
    }

    void analyse (float x)
    {
        float y = aa[1].process (aa[0].process (x, 0), 0);
        if (++decCount < dec) return;
        decCount = 0;
        ana[(size_t) anaPos] = y;
        anaPos = (anaPos + 1) % anaSize;
        if (++hopCount < 128) return;   // ~10 ms
        hopCount = 0;
        detect();
        steer();
    }

    // YIN pitch detection on the decimated signal
    void detect()
    {
        frame.resize (anaSize);
        float energy = 0;
        for (int i = 0; i < anaSize; ++i) { frame[(size_t) i] = ana[(size_t) ((anaPos + i) % anaSize)]; energy += frame[(size_t) i] * frame[(size_t) i]; }
        const float rms = std::sqrt (energy / anaSize);
        float f0 = -1;
        if (rms > 0.003f)
        {
            const int tauMin = juce::jmax (2, (int) (decRate / 1100.0)), tauMax = juce::jmin (anaSize / 2, (int) (decRate / 65.0));
            const int win = anaSize - tauMax;
            dbuf.assign ((size_t) tauMax + 1, 0.0f);
            double running = 0;
            int best = -1;
            for (int tau = 1; tau <= tauMax; ++tau)
            {
                float s = 0;
                for (int j = 0; j < win; ++j) { const float d = frame[(size_t) j] - frame[(size_t) (j + tau)]; s += d * d; }
                running += s;
                dbuf[(size_t) tau] = running > 0 ? (float) (s * tau / running) : 1.0f;
            }
            for (int tau = tauMin; tau < tauMax; ++tau)
                if (dbuf[(size_t) tau] < 0.15f)
                {
                    while (tau + 1 < tauMax && dbuf[(size_t) tau + 1] < dbuf[(size_t) tau]) ++tau;
                    best = tau; break;
                }
            if (best > 0)
            {
                const float a = dbuf[(size_t) best - 1], c = dbuf[(size_t) best + 1], m = dbuf[(size_t) best];
                const float shift = (a + c - 2 * m) != 0 ? 0.5f * (a - c) / (a + c - 2 * m) : 0.0f;
                f0 = (float) (decRate / (best + juce::jlimit (-1.0f, 1.0f, shift)));
            }
        }
        // a median of the last three readings, to ignore one-off octave slips
        recent[0] = recent[1]; recent[1] = recent[2];
        recent[2] = f0 > 0 ? 69.0f + 12.0f * std::log2 (f0 / 440.0f) : -1.0f;
        auto v = recent;
        std::sort (v.begin(), v.end());
        detected = v[1] > 0 ? v[1] : (recent[2] > 0 && recent[1] > 0 ? recent[2] : -1.0f);
        if (detected > 0) period = juce::jlimit (40.0, 2000.0, currentSampleRate / (440.0 * std::pow (2.0, (detected - 69.0) / 12.0)));
    }

    // where the voice should go, and how fast
    void steer()
    {
        const double hopSec = 128.0 / decRate;
        float targetRatio = 1.0f;
        if (detected > 0)
        {
            slow = slow < 0 ? detected : slow + (float) (1.0 - std::exp (-hopSec / 0.15)) * (detected - slow);
            const float note = nearestAllowed (slow);
            const float human = param ("humanize") * 0.01f;
            const float target = note + (detected - slow) * human;   // humanize keeps vibrato and scoops
            const float speedMs = param ("speed");
            corrected = corrected < 0 ? target
                      : speedMs < 1.0f ? target : corrected + (float) (1.0 - std::exp (-hopSec * 1000.0 / speedMs)) * (target - corrected);
            const float out = detected + (corrected - detected) * param ("amount") * 0.01f + param ("transpose");
            targetRatio = std::pow (2.0f, (out - detected) / 12.0f);
            nowTarget = out;
        }
        else { slow = -1; corrected = -1; nowTarget = -1; targetRatio = std::pow (2.0f, param ("transpose") / 12.0f); }
        ratio = ratio + 0.5 * (targetRatio - ratio);
        nowDetected = detected;
        const int p = histPos.load();
        detHist[(size_t) p] = detected;
        outHist[(size_t) p] = nowTarget.load();
        histPos = (p + 1) % histSize;
    }
};

class AutoTuneFace : public EffectFace
{
public:
    explicit AutoTuneFace (AutoTuneFx& f)
        : EffectFace (f, { KnobStyle::Neon, juce::Colour (0xff12141c), juce::Colour (0xff7ef9ff), juce::Colour (0xff36d1ff), juce::Colour (0xff1e2433),
                           juce::Colour (0xffe8f7ff), juce::Colour (0xff7d8aa5), juce::Colour (0xff0a0c12), juce::Colour (0xffe8f7ff) }), at (f)
    {
        const auto lc = juce::Colour (0xff98a6c4);
        knob ("speed", "RETUNE SPEED", lc); knob ("humanize", "HUMANIZE", lc); knob ("amount", "AMOUNT", lc);
        knob ("transpose", "TRANSPOSE", lc); knob ("mix", "MIX", lc);
        for (auto* cb : { &keyBox, &scaleBox }) addAndMakeVisible (*cb);
        keyBox.addItemList (juce::StringArray (noteNames, 12), 1);
        scaleBox.addItemList (AutoTuneFx::scales(), 1);
        keyAttach = std::make_unique<APVTS::ComboBoxAttachment> (fx.state, "key", keyBox);
        scaleAttach = std::make_unique<APVTS::ComboBoxAttachment> (fx.state, "scale", scaleBox);
        setSize (900, 520);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff141a2a), 0, 0, juce::Colour (0xff06070c), 0, (float) getHeight(), false);
        g.setGradientFill (bg); g.fillAll();
        title (g, { 22, 12, 600, 30 }, "AUTO-TUNE", "LIVE PITCH CORRECTION", juce::Colour (0xffe8f7ff), juce::Colour (0xff36d1ff));

        // the pitch graph: what you sang (white) and where it's going (cyan)
        auto a = graph.toFloat();
        screenBox (g, a, juce::Colour (0xff070910));
        auto plot = a.reduced (40, 12).withTrimmedLeft (0);
        const float now = at.nowDetected.load();
        if (now > 0) centre += 0.15f * (now - centre);
        const float range = 7.0f;
        auto toY = [&] (float m) { return plot.getCentreY() - (m - centre) / range * plot.getHeight() * 0.5f; };
        g.setFont (theme::font (10.0f, true));
        for (int m = (int) std::floor (centre - range); m <= (int) std::ceil (centre + range); ++m)
        {
            const float y = toY ((float) m);
            if (y < plot.getY() || y > plot.getBottom()) continue;
            const bool ok = at.allowed (((m % 12) + 12) % 12);
            g.setColour (ok ? juce::Colour (0xff36d1ff).withAlpha (0.18f) : juce::Colours::white.withAlpha (0.04f));
            g.drawHorizontalLine ((int) y, plot.getX(), plot.getRight());
            g.setColour (ok ? juce::Colour (0xff98a6c4) : juce::Colour (0xff3a4255));
            g.drawText (juce::String (noteNames[((m % 12) + 12) % 12]) + juce::String (m / 12 - 1), (int) a.getX() + 4, (int) y - 7, 34, 14, juce::Justification::left);
        }
        auto line = [&] (const std::array<std::atomic<float>, AutoTuneFx::histSize>& h, juce::Colour c, float wdt)
        {
            juce::Path p;
            bool pen = false;
            const int start = at.histPos.load();
            for (int i = 0; i < AutoTuneFx::histSize; ++i)
            {
                const float v = h[(size_t) ((start + i) % AutoTuneFx::histSize)].load();
                const float x = plot.getX() + plot.getWidth() * (float) i / (AutoTuneFx::histSize - 1);
                if (v <= 0) { pen = false; continue; }
                const float y = juce::jlimit (plot.getY(), plot.getBottom(), toY (v));
                if (! pen) { p.startNewSubPath (x, y); pen = true; } else p.lineTo (x, y);
            }
            glow (g, p, c, wdt);
        };
        line (at.detHist, juce::Colours::white.withAlpha (0.55f), 1.4f);
        line (at.outHist, juce::Colour (0xff36d1ff), 2.2f);

        // the note right now
        auto info = side.toFloat();
        screenBox (g, info, juce::Colour (0xff070910));
        const float target = at.nowTarget.load();
        g.setColour (juce::Colour (0xffe8f7ff));
        g.setFont (juce::Font (juce::FontOptions (44.0f, juce::Font::bold)));
        if (target > 0)
        {
            const int n = (int) std::round (target);
            g.drawText (juce::String (noteNames[((n % 12) + 12) % 12]) + juce::String (n / 12 - 1), info.withHeight (info.getHeight() * 0.55f), juce::Justification::centred);
            const float cents = (now - (float) n) * 100.0f;
            g.setFont (theme::font (12.0f, true));
            g.setColour (juce::Colour (0xff98a6c4));
            g.drawText ("you: " + juce::String (cents > 0 ? "+" : "") + juce::String (juce::roundToInt (cents)) + " cents", info.withTrimmedTop (info.getHeight() * 0.5f).withHeight (18), juce::Justification::centred);
            // the correction as a needle
            auto bar = info.reduced (18, 0).withTrimmedTop (info.getHeight() * 0.72f).withHeight (8);
            g.setColour (juce::Colour (0xff1e2433)); g.fillRoundedRectangle (bar, 4);
            const float x = bar.getCentreX() + juce::jlimit (-1.0f, 1.0f, cents / 50.0f) * bar.getWidth() * 0.5f;
            g.setColour (std::abs (cents) < 10 ? juce::Colour (0xff6dffb0) : juce::Colour (0xffffb35a));
            g.fillEllipse (x - 6, bar.getCentreY() - 6, 12, 12);
        }
        else
        {
            g.setFont (theme::font (14.0f, true));
            g.setColour (juce::Colour (0xff5d6782));
            g.drawText ("Sing or play\none note at a time", info, juce::Justification::centred);
        }

        // the 12 notes: lit = allowed (click to choose your own)
        for (int i = 0; i < 12; ++i)
        {
            auto k = keyRect (i).toFloat();
            const bool black = i == 1 || i == 3 || i == 6 || i == 8 || i == 10;
            const bool ok = at.allowed (i);
            const bool sounding = target > 0 && ((int) std::round (target) % 12 + 12) % 12 == i;
            // in the scale: bright blue; not in it: dark, so the scale reads at a glance
            g.setColour (ok ? (black ? juce::Colour (0xff1f8fc0) : juce::Colour (0xff8fdcff)) : (black ? juce::Colour (0xff141824) : juce::Colour (0xff2a3248)));
            g.fillRoundedRectangle (k, 4.0f);
            if (sounding) { g.setColour (juce::Colour (0xff6dffb0)); g.drawRoundedRectangle (k.reduced (1), 4.0f, 2.5f); }
            g.setColour (ok ? (black ? juce::Colour (0xffeaf6ff) : juce::Colour (0xff0d1424)) : juce::Colour (0xff5d6a85));
            g.setFont (theme::font (11.0f, true));
            g.drawText (noteNames[i], k.removeFromBottom (18), juce::Justification::centred);
        }
        g.setColour (juce::Colour (0xff7d8aa5));
        g.setFont (theme::font (10.0f, true));
        g.drawText ("KEY", keyBox.getBounds().translated (0, -15).withHeight (13), juce::Justification::left);
        g.drawText ("SCALE", scaleBox.getBounds().translated (0, -15).withHeight (13), juce::Justification::left);
        g.drawText ("NOTES  (CLICK TO CHOOSE YOUR OWN)", keyRect (0).translated (0, -15).withHeight (13).withWidth (300), juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (40);
        auto top = r.removeFromTop (220);
        side = top.removeFromRight (180);
        top.removeFromRight (12);
        graph = top;
        r.removeFromTop (28);
        auto strip = r.removeFromTop (44);
        keyBox.setBounds (strip.removeFromLeft (80).withHeight (28)); strip.removeFromLeft (10);
        scaleBox.setBounds (strip.removeFromLeft (170).withHeight (28)); strip.removeFromLeft (24);
        keysArea = strip.withHeight (44);
        r.removeFromTop (10);
        row (r, 0, 5);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        for (int i = 0; i < 12; ++i)
            if (keyRect (i).contains (e.getPosition()))
            {
                // switching to your own notes starts from what the current scale allows
                if ((int) fx.param ("scale") != 8)
                    for (int k = 0; k < 12; ++k) setBool (k, at.allowed (k));
                setBool (i, ! at.allowed (i));
                auto* sc = fx.state.getParameter ("scale");
                sc->setValueNotifyingHost (sc->convertTo0to1 (8.0f));
                repaint();
            }
    }

private:
    AutoTuneFx& at;
    juce::ComboBox keyBox, scaleBox;
    std::unique_ptr<APVTS::ComboBoxAttachment> keyAttach, scaleAttach;
    juce::Rectangle<int> graph, side, keysArea;
    float centre = 64;

    juce::Rectangle<int> keyRect (int i) const
    {
        const int w = keysArea.getWidth() / 12;
        return { keysArea.getX() + i * w + 1, keysArea.getY(), w - 3, keysArea.getHeight() };
    }
    void setBool (int i, bool v)
    {
        auto* p = fx.state.getParameter (("n" + juce::String (i)).toRawUTF8());
        p->setValueNotifyingHost (v ? 1.0f : 0.0f);
    }
};

// =============================================================================
//  BUS COMPRESSOR
// =============================================================================
class BusCompFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    BusCompFx() : BuiltinFx ("buscomp", "Bus Compressor", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("threshold", "Threshold", Range (-40, 0), -12, "dB"));
        l.add (floatParam ("ratio", "Ratio", skewed (1, 20, 4), 3, ":1"));
        l.add (floatParam ("knee", "Knee", Range (0, 18), 6, "dB"));
        l.add (floatParam ("crest", "Peak Crest", Range (0, 18), 9, "dB"));
        l.add (floatParam ("attack", "Attack", skewed (0.1f, 100, 10), 8, "ms"));
        l.add (floatParam ("relpeak", "Peak Release", skewed (10, 1000, 120), 100, "ms"));
        l.add (floatParam ("relrms", "Body Release", skewed (30, 3000, 400), 400, "ms"));
        l.add (floatParam ("schp", "Sidechain HP", skewed (20, 500, 100), 60, "Hz"));
        l.add (floatParam ("link", "Stereo Link", Range (0, 100), 80, "%"));
        l.add (floatParam ("makeup", "Makeup", Range (0, 24), 0, "dB"));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 100, "%"));
        l.add (toggleParam ("delta", "Delta", false));
        return l;
    }

    float curve (float in) const   // static curve with soft knee (dB in, dB of gain change)
    {
        const float thr = param ("threshold"), ratio = param ("ratio"), knee = param ("knee");
        const float over = in - thr;
        if (2 * over < -knee) return 0.0f;
        if (2 * std::abs (over) <= knee && knee > 0) { const float x = over + knee * 0.5f; return (1.0f / ratio - 1.0f) * x * x / (2.0f * knee); }
        return (1.0f / ratio - 1.0f) * over;
    }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& h : hp) h.reset();
        pk[0] = pk[1] = ms[0] = ms[1] = 0; gr[0] = gr[1] = 0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const double sr = currentSampleRate;
        hp[0].c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, param ("schp"), 0.707, 0);
        hp[1].c = hp[0].c;
        const float relP = (float) std::exp (-1.0 / (param ("relpeak") * 0.001 * sr));
        const float relR = (float) (1.0 - std::exp (-1.0 / (param ("relrms") * 0.001 * sr)));
        const float att = (float) std::exp (-1.0 / (param ("attack") * 0.001 * sr));
        const float rel = (float) std::exp (-1.0 / (0.005 * sr));
        const float crest = param ("crest"), link = param ("link") * 0.01f;
        const float makeup = juce::Decibels::decibelsToGain (param ("makeup")), mix = param ("mix") * 0.01f;
        const bool delta = param ("delta") > 0.5f;
        float maxGr = 0;
        for (int i = 0; i < n; ++i)
        {
            float lev[2] {};
            for (int c = 0; c < chans; ++c)
            {
                const float x = hp[c].process (b.getSample (c, i), c);
                pk[c] = juce::jmax (std::abs (x), pk[c] * relP);       // peaks: instant attack, their own release
                ms[c] += relR * (x * x - ms[c]);                        // the body (RMS) with a slower release
                const float rmsDb = juce::Decibels::gainToDecibels (std::sqrt (ms[c]) * 1.4142f, -120.0f);
                const float peakDb = juce::Decibels::gainToDecibels (pk[c], -120.0f) - crest;   // peaks only count once they stand out
                lev[c] = juce::jmax (rmsDb, peakDb);
            }
            if (chans == 1) lev[1] = lev[0];
            const float linked = juce::jmax (lev[0], lev[1]);
            for (int c = 0; c < chans; ++c)
            {
                const float want = -curve (lev[c] + (linked - lev[c]) * link);   // dB of reduction wanted
                gr[c] = want > gr[c] ? want + (gr[c] - want) * att : want + (gr[c] - want) * rel;
                const float dry = b.getSample (c, i);
                const float wet = dry * juce::Decibels::decibelsToGain (-gr[c]) * makeup;
                const float out = mix * wet + (1 - mix) * dry;
                b.setSample (c, i, delta ? dry - wet / makeup : out);
                maxGr = juce::jmax (maxGr, gr[c]);
            }
        }
        meterValue = -maxGr;
        if (historyTick (n)) pushHistory (-maxGr);
        measureOut (b);
    }

private:
    Biquad hp[2];
    float pk[2] {}, ms[2] {}, gr[2] {};
};

class BusCompFace : public EffectFace
{
public:
    explicit BusCompFace (BusCompFx& f)
        : EffectFace (f, { KnobStyle::Minimal, juce::Colour (0xff2a2e33), juce::Colour (0xffffffff), juce::Colour (0xffb8ff5a), juce::Colour (0xff30353b),
                           juce::Colour (0xffe9ecef), juce::Colour (0xff8a9199), juce::Colour (0xff15181b), juce::Colour (0xffe9ecef) }), bc (f)
    {
        const auto lc = juce::Colour (0xff9aa3ad);
        for (auto [id, name] : { std::pair { "threshold", "THRESHOLD" }, { "ratio", "RATIO" }, { "knee", "KNEE" }, { "crest", "PEAK CREST" },
                                 { "attack", "ATTACK" }, { "relpeak", "PEAK REL" }, { "relrms", "BODY REL" }, { "schp", "SC HIGH-PASS" },
                                 { "link", "STEREO LINK" }, { "makeup", "MAKEUP" }, { "mix", "MIX" } })
            knob (id, name, lc, 58);
        deltaButton = toggle ("delta", "DELTA");
        deltaButton->setTooltip ("Hear only what the compressor takes away");
        setSize (980, 540);
        startTimerHz (40);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff1c1f23));
        g.setColour (juce::Colours::white.withAlpha (0.02f));
        for (int y = 0; y < getHeight(); y += 2) g.drawHorizontalLine (y, 0, (float) getWidth());
        title (g, { 22, 12, 600, 30 }, "BUS COMPRESSOR", "GLUE  /  PEAK + BODY", juce::Colour (0xffe9ecef), juce::Colour (0xffb8ff5a));

        // gain reduction: a long bar meter
        auto m = meter.toFloat();
        screenBox (g, m, juce::Colour (0xff0d0f11), 6.0f);
        auto bar = m.reduced (12, 10);
        const float grNow = juce::jlimit (0.0f, 20.0f, -fx.meterValue.load());
        grShown = juce::jmax (grNow, grShown * 0.92f);
        g.setFont (theme::font (10.0f, true));
        for (int db : { 0, 1, 2, 3, 4, 6, 8, 10, 12, 15, 20 })
        {
            const float x = bar.getRight() - std::sqrt ((float) db / 20.0f) * bar.getWidth();
            g.setColour (juce::Colour (0xff4a5058)); g.drawVerticalLine ((int) x, bar.getY(), bar.getY() + 5);
            g.setColour (juce::Colour (0xff8a9199)); g.drawText (juce::String (-db), (int) x - 12, (int) bar.getBottom() - 12, 24, 12, juce::Justification::centred);
        }
        const float x0 = bar.getRight() - std::sqrt (grNow / 20.0f) * bar.getWidth();
        juce::ColourGradient gg (juce::Colour (0xffb8ff5a), bar.getRight(), 0, juce::Colour (0xffff8a3a), bar.getX(), 0, false);
        g.setGradientFill (gg);
        g.fillRoundedRectangle (juce::Rectangle<float> (x0, bar.getY() + 8, bar.getRight() - x0, 12), 3.0f);
        const float xh = bar.getRight() - std::sqrt (grShown / 20.0f) * bar.getWidth();
        g.setColour (juce::Colours::white); g.fillRect (xh - 1, bar.getY() + 6, 2.0f, 16.0f);
        g.setColour (juce::Colour (0xff8a9199));
        g.drawText ("GAIN REDUCTION", bar.withHeight (8).translated (0, -10), juce::Justification::left);

        // the transfer curve with the knee
        auto tc = curveArea.toFloat();
        screenBox (g, tc, juce::Colour (0xff0d0f11));
        auto plot = tc.reduced (12, 12);
        auto toX = [&] (float db) { return plot.getX() + (db + 48.0f) / 48.0f * plot.getWidth(); };
        auto toY = [&] (float db) { return plot.getBottom() - (juce::jlimit (-48.0f, 6.0f, db) + 48.0f) / 54.0f * plot.getHeight(); };
        g.setColour (juce::Colours::white.withAlpha (0.08f));
        g.drawLine (toX (-48), toY (-48), toX (0), toY (0));
        juce::Path p;
        for (float in = -48; in <= 0.01f; in += 0.5f)
        {
            const float out = in + bc.curve (in) + fx.param ("makeup");
            if (in < -47.9f) p.startNewSubPath (toX (in), toY (out)); else p.lineTo (toX (in), toY (out));
        }
        glow (g, p, juce::Colour (0xffb8ff5a));
        const float in = juce::jlimit (-48.0f, 0.0f, fx.inputDb.load());
        if (fx.inputDb.load() > -47)
        {
            const float out = in + bc.curve (in) + fx.param ("makeup");
            g.setColour (juce::Colours::white); g.fillEllipse (toX (in) - 4, toY (out) - 4, 8, 8);
        }

        // level and gain reduction over time
        auto hs = histArea.toFloat();
        screenBox (g, hs, juce::Colour (0xff0d0f11));
        auto hp = hs.reduced (10, 10);
        juce::Path h;
        const int w0 = fx.historyPos.load();
        for (int i = 0; i < BuiltinFx::historySize; ++i)
        {
            const float v = juce::jlimit (0.0f, 20.0f, -fx.history[(size_t) ((w0 + i) % BuiltinFx::historySize)].load());
            const float x = hp.getX() + hp.getWidth() * (float) i / (BuiltinFx::historySize - 1);
            const float y = hp.getY() + std::sqrt (v / 20.0f) * hp.getHeight();
            if (i == 0) { h.startNewSubPath (x, hp.getY()); h.lineTo (x, y); } else h.lineTo (x, y);
        }
        h.lineTo (hp.getRight(), hp.getY()); h.closeSubPath();
        g.setColour (juce::Colour (0xffb8ff5a).withAlpha (0.35f)); g.fillPath (h);
        g.setColour (juce::Colour (0xff8a9199)); g.setFont (theme::font (9.5f, true));
        g.drawText ("GAIN REDUCTION OVER TIME", hp, juce::Justification::bottomLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (40);
        meter = r.removeFromTop (56);
        r.removeFromTop (10);
        auto mid = r.removeFromTop (170);
        curveArea = mid.removeFromLeft (220);
        mid.removeFromLeft (10);
        histArea = mid;
        r.removeFromTop (14);
        auto k1 = r.removeFromTop (r.getHeight() / 2);
        row (k1, 0, 6);
        auto k2 = r;
        deltaButton->setBounds (k2.removeFromRight (110).withSizeKeepingCentre (100, 30));
        row (k2, 6, 11);
    }

private:
    BusCompFx& bc;
    juce::TextButton* deltaButton = nullptr;
    juce::Rectangle<int> meter, curveArea, histArea;
    float grShown = 0;
};

// =============================================================================
//  COLOR EQ
// =============================================================================
class ColorEqFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static const juce::StringArray& models() { static const juce::StringArray m { "Silk", "Punch", "Tilt", "Bloom" }; return m; }

    ColorEqFx() : BuiltinFx ("coloreq", "Color EQ", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choice ("model", "Character", models(), 0));
        l.add (floatParam ("hpf", "High-pass", skewed (10, 300, 60), 10, "Hz"));
        l.add (choice ("lowtype", "Low Type", { "Shelf", "Bell" }, 0));
        l.add (floatParam ("lowfreq", "Low Freq", skewed (20, 400, 100), 80, "Hz"));
        l.add (floatParam ("lowgain", "Low Gain", Range (-15, 15), 0, "dB"));
        l.add (floatParam ("midfreq", "Mid Freq", skewed (150, 8000, 1000), 1000, "Hz"));
        l.add (floatParam ("midgain", "Mid Gain", Range (-15, 15), 0, "dB"));
        l.add (choice ("hightype", "High Type", { "Shelf", "Bell" }, 0));
        l.add (floatParam ("highfreq", "High Freq", skewed (1500, 22000, 6000), 8000, "Hz"));
        l.add (floatParam ("highgain", "High Gain", Range (-15, 15), 0, "dB"));
        l.add (toggleParam ("eqsat", "EQ Saturation", false));
        l.add (choice ("outsat", "Output", { "Clean", "Tape", "Tube", "Iron" }, 0));
        l.add (toggleParam ("autogain", "Auto Gain", true));
        l.add (floatParam ("trim", "Trim", Range (-12, 12), 0, "dB"));
        return l;
    }

    bool hasResponseCurve() const override { return true; }
    double getMagnitudeForFrequency (double f) const override
    {
        auto c = coefsFor (currentSampleRate > 0 ? currentSampleRate : 48000.0);
        double m = 1.0;
        for (auto& b : c.bands) m *= b.magnitude (f, c.sr);
        if (c.hpOn)
        {
            const double w = f / c.hpf;
            m *= (w * w * w) / std::sqrt (1.0 + std::pow (w, 6.0));   // 3rd order Butterworth
        }
        return m;
    }

    struct Coefs { std::array<BiquadCoefs, 3> bands; double sr; bool hpOn; double hpf; };
    Coefs coefsFor (double sr) const
    {
        const int model = (int) param ("model");
        auto bandQ = [model] (float gain, bool boost)
        {
            const float g = std::abs (gain);
            switch (model)
            {
                case 0:  return 0.45 + 0.05 * g;                                  // Silk: gentle, narrows a little as you push
                case 1:  return (0.75 + 0.08 * g) * (boost ? 1.0 : 1.3);         // Punch: tighter, cuts tighter than boosts
                case 2:  return 0.4;                                              // Tilt: broad and even
                default: return 1.6 / (1.0 + 0.12 * g);                          // Bloom: opens up as you push
            }
        };
        const double shelfQ = model == 1 ? 1.1 : model == 2 ? 0.35 : model == 3 ? 0.55 : 0.6;
        using T = BiquadCoefs::Type;
        const float lg = param ("lowgain"), mg = param ("midgain"), hg = param ("highgain");
        Coefs c;
        c.sr = sr;
        c.bands[0] = param ("lowtype") < 0.5f ? BiquadCoefs::make (T::LowShelf, sr, param ("lowfreq"), shelfQ, lg)
                                              : BiquadCoefs::make (T::Peak, sr, param ("lowfreq"), bandQ (lg, lg > 0), lg);
        c.bands[1] = BiquadCoefs::make (T::Peak, sr, param ("midfreq"), bandQ (mg, mg > 0), mg);
        c.bands[2] = param ("hightype") < 0.5f ? BiquadCoefs::make (T::HighShelf, sr, juce::jmin (param ("highfreq"), (float) sr * 0.45f), shelfQ, hg)
                                               : BiquadCoefs::make (T::Peak, sr, juce::jmin (param ("highfreq"), (float) sr * 0.45f), bandQ (hg, hg > 0), hg);
        c.hpf = param ("hpf");
        c.hpOn = c.hpf > 11.0;
        return c;
    }

    // the loudness change of the curve, for auto gain
    float curveLoudness() const
    {
        double sum = 0, wsum = 0;
        for (int i = 0; i < 48; ++i)
        {
            const double f = 40.0 * std::pow (300.0, i / 47.0);   // 40 Hz .. 12 kHz
            const double weight = f < 200 ? 0.5 : f > 6000 ? 0.6 : 1.0;
            sum += weight * juce::Decibels::gainToDecibels (getMagnitudeForFrequency (f), -60.0);
            wsum += weight;
        }
        return (float) (sum / wsum);
    }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& b : bands) b.reset();
        hp2.reset(); hp1[0] = hp1[1] = 0; hpx[0] = hpx[1] = 0;
        autoGainDb = 0; tapeLp[0] = tapeLp[1] = 0; ironLp[0] = ironLp[1] = 0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        const double sr = currentSampleRate;
        const auto c = coefsFor (sr);
        for (int k = 0; k < 3; ++k) bands[k].c = c.bands[(size_t) k];
        hp2.c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, c.hpf, 1.0, 0);
        const float a1 = (float) std::exp (-juce::MathConstants<double>::twoPi * c.hpf / sr);
        const float maxBoost = juce::jmax (0.0f, param ("lowgain"), param ("midgain"), param ("highgain"));
        const bool eqSat = param ("eqsat") > 0.5f;
        const float satAmt = eqSat ? juce::jlimit (0.0f, 1.0f, maxBoost / 12.0f) : 0.0f;   // no boost, no saturation
        const int outSat = (int) param ("outsat");
        const float target = param ("autogain") > 0.5f ? -0.9f * curveLoudness() : 0.0f;
        autoGainDb += 0.2f * (target - autoGainDb);
        const float gain = juce::Decibels::decibelsToGain (autoGainDb + param ("trim"));
        const float tapeK = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 16000.0 / sr));
        const float ironK = (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * 180.0 / sr));
        for (int ch = 0; ch < chans; ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
            {
                float x = d[i];
                if (c.hpOn)
                {
                    const float y1 = a1 * (hp1[ch] + x - hpx[ch]);   // one-pole high-pass ...
                    hpx[ch] = x; hp1[ch] = y1;
                    x = hp2.process (y1, ch);                       // ... and a two-pole: 18 dB / octave
                }
                float y = bands[2].process (bands[1].process (bands[0].process (x, ch), ch), ch);
                if (satAmt > 0) y = y + satAmt * (std::tanh (y * 1.6f) / 1.6f - y) * 0.8f;
                y *= gain;
                switch (outSat)
                {
                    case 1: { tapeLp[ch] += tapeK * (y - tapeLp[ch]); y = std::tanh (tapeLp[ch] * 1.15f) / 1.15f; break; }
                    case 2: { const float t = y + 0.12f * y * y; y = std::tanh (t * 1.3f) / 1.3f - 0.02f * std::tanh (t * t); break; }
                    case 3: { ironLp[ch] += ironK * (y - ironLp[ch]); const float low = ironLp[ch]; y = (y - low) + std::tanh (low * 1.8f) / 1.8f; break; }
                    default: break;
                }
                d[i] = y;
            }
        }
        measureOut (b);
    }

    float autoGainDb = 0;

private:
    Biquad bands[3], hp2;
    float hp1[2] {}, hpx[2] {}, tapeLp[2] {}, ironLp[2] {};
};

class ColorEqFace : public EffectFace
{
public:
    explicit ColorEqFace (ColorEqFx& f)
        : EffectFace (f, { KnobStyle::Glossy, juce::Colour (0xff2b2f36), juce::Colour (0xfff6f1e8), juce::Colour (0xfff6f1e8), juce::Colour (0x33000000),
                           juce::Colour (0xfff6f1e8), juce::Colour (0xccf6f1e8), juce::Colour (0x40000000), juce::Colour (0xfff6f1e8) }), eq (f)
    {
        const auto lc = juce::Colour (0xddf6f1e8);
        knob ("hpf", "HIGH-PASS", lc, 60);
        knob ("lowfreq", "LOW FREQ", lc, 60); knob ("lowgain", "LOW", lc, 60);
        knob ("midfreq", "MID FREQ", lc, 60); knob ("midgain", "MID", lc, 60);
        knob ("highfreq", "HIGH FREQ", lc, 60); knob ("highgain", "HIGH", lc, 60);
        knob ("trim", "TRIM", lc, 60);
        modelSw = choice ("model"); lowType = choice ("lowtype"); highType = choice ("hightype"); outSw = choice ("outsat");
        eqSat = toggle ("eqsat", "EQ SAT"); autoGain = toggle ("autogain", "AUTO GAIN");
        setSize (960, 520);
        startTimerHz (30);
    }

    juce::Colour modelColour() const
    {
        const juce::Colour c[] { juce::Colour (0xff3b5d7a), juce::Colour (0xff7a3b3b), juce::Colour (0xff4f6b3a), juce::Colour (0xff6a4a7d) };
        return c[juce::jlimit (0, 3, (int) fx.param ("model"))];
    }

    void paint (juce::Graphics& g) override
    {
        // the panel's colour follows the character
        target = modelColour();
        shown = shown.interpolatedWith (target, 0.2f);
        juce::ColourGradient bg (shown.brighter (0.25f), 0, 0, shown.darker (0.7f), 0, (float) getHeight(), false);
        g.setGradientFill (bg); g.fillAll();
        noiseTexture (g, getLocalBounds(), 9, 0.04f);
        title (g, { 22, 12, 600, 30 }, "COLOR EQ", ColorEqFx::models()[(int) fx.param ("model")].toUpperCase() + " CHARACTER", juce::Colour (0xfff6f1e8), juce::Colour (0xccf6f1e8));

        auto a = graph.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.35f)); g.fillRoundedRectangle (a, 10.0f);
        auto plot = a.reduced (14, 12);
        g.setFont (theme::font (10.0f, true));
        for (double fr : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = logX (fr, plot);
            g.setColour (juce::Colours::white.withAlpha (0.07f)); g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (juce::Colours::white.withAlpha (0.5f));
            g.drawText (fr >= 1000 ? juce::String ((int) (fr / 1000)) + "k" : juce::String ((int) fr), (int) x + 3, (int) plot.getBottom() - 13, 30, 12, juce::Justification::left);
        }
        auto dbY = [&] (float db) { return plot.getCentreY() - db / 18.0f * plot.getHeight() * 0.5f; };
        g.setColour (juce::Colours::white.withAlpha (0.15f)); g.drawHorizontalLine ((int) dbY (0), plot.getX(), plot.getRight());
        juce::Path c;
        for (int x = 0; x <= (int) plot.getWidth(); x += 2)
        {
            const double fr = xToLogF (plot.getX() + (float) x, plot);
            const float y = dbY (juce::jlimit (-24.0f, 24.0f, juce::Decibels::gainToDecibels ((float) eq.getMagnitudeForFrequency (fr), -60.0f)));
            if (x == 0) c.startNewSubPath (plot.getX(), y); else c.lineTo (plot.getX() + (float) x, y);
        }
        juce::Path fill (c);
        fill.lineTo (plot.getRight(), dbY (0)); fill.lineTo (plot.getX(), dbY (0)); fill.closeSubPath();
        g.setColour (juce::Colours::white.withAlpha (0.12f)); g.fillPath (fill);
        // saturation shows as a warm haze on the curve
        if (fx.param ("eqsat") > 0.5f || fx.param ("outsat") > 0.5f)
            glow (g, c, juce::Colour (0xffffb36b).withAlpha (0.4f + 0.3f * juce::jlimit (0.0f, 1.0f, (fx.outputDb.load() + 40.0f) / 40.0f)), 4.0f);
        glow (g, c, juce::Colour (0xfff6f1e8), 2.2f);

        for (int i = 0; i < 4; ++i)
        {
            const auto pt = node (i, plot);
            g.setColour (juce::Colours::black.withAlpha (0.4f)); g.fillEllipse (pt.x - 9, pt.y - 9, 18, 18);
            g.setColour (juce::Colour (0xfff6f1e8)); g.fillEllipse (pt.x - 6, pt.y - 6, 12, 12);
        }
        g.setColour (juce::Colour (0xddf6f1e8)); g.setFont (theme::font (11.0f, true));
        if (fx.param ("autogain") > 0.5f)
            g.drawText ("AUTO GAIN  " + juce::String (eq.autoGainDb, 1) + " dB", a.reduced (12, 8), juce::Justification::topRight);
        g.setFont (theme::font (10.0f, true));
        g.drawText ("CHARACTER", modelSw->getBounds().translated (0, -14).withHeight (12), juce::Justification::left);
        g.drawText ("OUTPUT", outSw->getBounds().translated (0, -14).withHeight (12), juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (40);
        auto ctl = r.removeFromTop (44).withTrimmedTop (14);
        modelSw->setBounds (ctl.removeFromLeft (300)); ctl.removeFromLeft (20);
        outSw->setBounds (ctl.removeFromLeft (260)); ctl.removeFromLeft (20);
        eqSat->setBounds (ctl.removeFromLeft (100)); ctl.removeFromLeft (8);
        autoGain->setBounds (ctl.removeFromLeft (110));
        r.removeFromTop (10);
        graph = r.removeFromTop (220);
        r.removeFromTop (10);
        auto types = r.removeFromTop (24);
        const int w = types.getWidth() / 8;
        lowType->setBounds (types.withX (types.getX() + w).withWidth (w * 2).reduced (8, 0));
        highType->setBounds (types.withX (types.getX() + w * 5).withWidth (w * 2).reduced (8, 0));
        r.removeFromTop (6);
        row (r, 0, 8);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto plot = graph.toFloat().reduced (14, 12);
        dragging = -1;
        for (int i = 0; i < 4; ++i) if (node (i, plot).getDistanceFrom (e.position) < 14) dragging = i;
        for (auto* p : params()) if (p) p->beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        auto plot = graph.toFloat().reduced (14, 12);
        auto ps = params();
        ps[0]->setValueNotifyingHost (ps[0]->convertTo0to1 ((float) xToLogF (e.position.x, plot)));
        if (ps[1]) ps[1]->setValueNotifyingHost (ps[1]->convertTo0to1 (juce::jlimit (-15.0f, 15.0f, (plot.getCentreY() - e.position.y) / (plot.getHeight() * 0.5f) * 18.0f)));
    }
    void mouseUp (const juce::MouseEvent&) override { for (auto* p : params()) if (p) p->endChangeGesture(); dragging = -1; }

private:
    ColorEqFx& eq;
    Switch* modelSw = nullptr; Switch* lowType = nullptr; Switch* highType = nullptr; Switch* outSw = nullptr;
    juce::TextButton* eqSat = nullptr; juce::TextButton* autoGain = nullptr;
    juce::Rectangle<int> graph;
    juce::Colour shown { 0xff3b5d7a }, target { 0xff3b5d7a };
    int dragging = -1;

    std::array<juce::RangedAudioParameter*, 2> params()
    {
        const char* f[] { "hpf", "lowfreq", "midfreq", "highfreq" };
        const char* gn[] { nullptr, "lowgain", "midgain", "highgain" };
        if (dragging < 0) return { nullptr, nullptr };
        return { fx.state.getParameter (f[dragging]), gn[dragging] ? fx.state.getParameter (gn[dragging]) : nullptr };
    }
    juce::Point<float> node (int i, juce::Rectangle<float> plot) const
    {
        auto dbY = [&] (float db) { return plot.getCentreY() - db / 18.0f * plot.getHeight() * 0.5f; };
        switch (i)
        {
            case 0:  return { logX (p ("hpf"), plot), dbY (0) };
            case 1:  return { logX (p ("lowfreq"), plot), dbY (p ("lowgain")) };
            case 2:  return { logX (p ("midfreq"), plot), dbY (p ("midgain")) };
            default: return { logX (p ("highfreq"), plot), dbY (p ("highgain")) };
        }
    }
};

// =============================================================================
//  VOICE CLEANER: spectral noise and reverb reduction
// =============================================================================
class VoiceCleanerFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static constexpr int order = 10, fftSize = 1 << order, hop = fftSize / 4, bins = fftSize / 2 + 1, bands = 96;

    VoiceCleanerFx() : BuiltinFx ("voiceclean", "Voice Cleaner", layout()), fft (order)
    {
        for (int i = 0; i < fftSize; ++i) window[(size_t) i] = std::sqrt (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / fftSize));
    }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("noise", "Noise", Range (0, 100), 60, "%"));
        l.add (floatParam ("reverb", "Reverb", Range (0, 100), 30, "%"));
        l.add (floatParam ("voice", "Voice", Range (-24, 12), 0, "dB"));
        l.add (floatParam ("smooth", "Smoothness", Range (0, 100), 50, "%"));
        l.add (choice ("listen", "Listen", { "Clean voice", "What's removed" }, 0));
        return l;
    }

    // what the editor draws, in 96 log-spaced bands (dB)
    std::array<std::atomic<float>, bands> inBand {}, outBand {}, noiseBand {};
    std::atomic<bool> learning { false };

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        setLatencySamples (fftSize);
        for (int c = 0; c < 2; ++c) { inFifo[c].assign (fftSize, 0.0f); outAcc[c].assign (fftSize * 2, 0.0f); }
        fifoPos = 0; hopCount = 0;
        noise.assign (bins, 1.0e-6f); power.assign (bins, 0.0f); slow.assign (bins, 0.0f);
        for (auto& m : subMin) m.assign (bins, 0.0f);
        subFilled.fill (false); subIdx = 0; subCount = 0;
        subLen = juce::jmax (4, (int) std::round (2.5 * sr / hop / subWindows));   // 2.5 s: long held notes are not mistaken for noise
        gainS.assign (bins, 1.0f);
        histFrames = juce::jmax (1, (int) std::round (0.05 * sr / hop));   // reverb arrives ~50 ms after the sound
        past.assign ((size_t) histFrames, std::vector<float> (bins, 0.0f)); pastPos = 0;
        warm = 0;
        for (auto& b : inBand) b = -100; for (auto& b : outBand) b = -100; for (auto& b : noiseBand) b = -100;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& b, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = b.getNumSamples(), chans = juce::jmin (2, b.getNumChannels());
        measureIn (b);
        for (int i = 0; i < n; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                const float x = b.getSample (juce::jmin (c, chans - 1), i);
                inFifo[c][(size_t) fifoPos] = x;
                const float y = outAcc[c][(size_t) fifoPos];
                outAcc[c][(size_t) fifoPos] = 0;
                if (c < chans) b.setSample (c, i, y);
            }
            fifoPos = (fifoPos + 1) % fftSize;
            if (++hopCount >= hop) { hopCount = 0; frame(); }
        }
        measureOut (b);
    }

private:
    juce::dsp::FFT fft;
    std::array<float, fftSize> window {};
    std::array<std::vector<float>, 2> inFifo, outAcc;
    std::array<std::array<float, fftSize * 2>, 2> spec {};
    std::vector<float> noise, power, gainS, slow;
    static constexpr int subWindows = 8;
    std::array<std::vector<float>, subWindows> subMin;
    std::array<bool, subWindows> subFilled {};
    int subIdx = 0, subCount = 0, subLen = 24;
    std::vector<std::vector<float>> past;
    int fifoPos = 0, hopCount = 0, histFrames = 9, pastPos = 0, warm = 0;

    void frame()
    {
        // analyse both channels
        for (int c = 0; c < 2; ++c)
        {
            auto& s = spec[(size_t) c];
            std::fill (s.begin(), s.end(), 0.0f);
            for (int i = 0; i < fftSize; ++i) s[(size_t) i] = inFifo[c][(size_t) ((fifoPos + i) % fftSize)] * window[(size_t) i];
            fft.performRealOnlyForwardTransform (s.data());
        }
        const float noiseAmt = param ("noise") * 0.01f, revAmt = param ("reverb") * 0.01f;
        const float smooth = param ("smooth") * 0.01f;
        const bool removed = param ("listen") > 0.5f;
        const float voice = juce::Decibels::decibelsToGain (param ("voice"));
        const float floorG = juce::Decibels::decibelsToGain (-6.0f - 30.0f * noiseAmt);
        const float revFloor = juce::Decibels::decibelsToGain (-3.0f - 18.0f * revAmt);
        // the reverb's assumed decay: more Reverb = assume a longer room
        const float rt60 = 0.3f + 1.2f * revAmt;
        const float revScale = std::exp (-2.0f * (6.9f / rt60) * 0.05f);
        auto& late = past[(size_t) pastPos];
        ++warm;

        std::vector<float> g (bins);
        for (int k = 0; k < bins; ++k)
        {
            const float re = 0.5f * (spec[0][(size_t) (2 * k)] + spec[1][(size_t) (2 * k)]), im = 0.5f * (spec[0][(size_t) (2 * k + 1)] + spec[1][(size_t) (2 * k + 1)]);
            const float p = re * re + im * im + 1.0e-12f;
            power[(size_t) k] = 0.7f * power[(size_t) k] + 0.3f * p;
            // the noise floor: follows quiet moments quickly, rises only slowly (so the voice isn't learned as noise)
            auto& nz = noise[(size_t) k];
            auto& sl = slow[(size_t) k];
            sl = 0.85f * sl + 0.15f * p;   // a steadier view of the level, for finding the floor
            // the noise floor = the quietest this bin has been over the last 2.5 seconds
            // (kept as 8 short windows, so it forgets old minimums and adapts quickly)
            auto& cur = subMin[(size_t) subIdx][(size_t) k];
            cur = subCount == 0 ? sl : juce::jmin (cur, sl);
            float mn = cur;
            for (int w = 0; w < subWindows; ++w) if (subFilled[(size_t) w]) mn = juce::jmin (mn, subMin[(size_t) w][(size_t) k]);
            if (learning.load()) nz = 0.8f * nz + 0.2f * p;
            else nz = warm <= 4 ? p : juce::jmax (nz * 0.7f, 2.0f * mn);   // (the minimum sits below the average, hence x2)
            float gn = 1.0f - (1.0f + 2.0f * noiseAmt) * nz / juce::jmax (power[(size_t) k], 1.0e-12f);
            gn = juce::jmax (floorG, gn);
            // the late reverb: what was there ~50 ms ago, decayed
            const float rev = revScale * late[(size_t) k];
            float gr = 1.0f - (0.5f + 1.5f * revAmt) * rev / p;
            gr = juce::jmax (revFloor, gr);
            if (revAmt <= 0.001f) gr = 1.0f;
            if (noiseAmt <= 0.001f) gn = 1.0f;
            float gg = gn * gr;
            // smooth over time (less "chirping")
            auto& s = gainS[(size_t) k];
            const float up = 0.6f - 0.45f * smooth, down = 0.85f - 0.55f * smooth;
            s += (gg > s ? up : down) * (gg - s);
            g[(size_t) k] = s;
            late[(size_t) k] = power[(size_t) k];
        }
        pastPos = (pastPos + 1) % histFrames;
        if (warm > 4 && ++subCount >= subLen) { subCount = 0; subFilled[(size_t) subIdx] = true; subIdx = (subIdx + 1) % subWindows; }
        // and a little across frequency
        for (int k = 1; k + 1 < bins; ++k) g[(size_t) k] = 0.25f * g[(size_t) k - 1] + 0.5f * g[(size_t) k] + 0.25f * g[(size_t) k + 1];

        for (int c = 0; c < 2; ++c)
        {
            auto& s = spec[(size_t) c];
            for (int k = 0; k < bins; ++k)
            {
                const float gg = removed ? (1.0f - g[(size_t) k]) : g[(size_t) k] * voice;
                s[(size_t) (2 * k)] *= gg; s[(size_t) (2 * k + 1)] *= gg;
            }
            fft.performRealOnlyInverseTransform (s.data());
            for (int i = 0; i < fftSize; ++i)
                outAcc[c][(size_t) ((fifoPos + i) % fftSize)] += s[(size_t) i] * window[(size_t) i] * 0.5f;   // sqrt-Hann in and out at 75% overlap sums to 2
        }

        // the picture, in bands
        if (warm % 2 == 0)
            for (int bnd = 0; bnd < bands; ++bnd)
            {
                const double f0 = 40.0 * std::pow (500.0, bnd / (double) bands), f1 = 40.0 * std::pow (500.0, (bnd + 1) / (double) bands);
                const int k0 = juce::jlimit (1, bins - 1, (int) (f0 * fftSize / currentSampleRate)), k1 = juce::jlimit (k0 + 1, bins, (int) (f1 * fftSize / currentSampleRate) + 1);
                float pin = 0, pout = 0, pn = 0;
                for (int k = k0; k < k1; ++k) { pin += power[(size_t) k]; pout += power[(size_t) k] * g[(size_t) k] * g[(size_t) k]; pn += noise[(size_t) k]; }
                const float norm = 1.0f / (float) (k1 - k0) / (fftSize * 0.25f);
                inBand[(size_t) bnd] = juce::Decibels::gainToDecibels (std::sqrt (pin * norm), -100.0f);
                outBand[(size_t) bnd] = juce::Decibels::gainToDecibels (std::sqrt (pout * norm) * voice, -100.0f);
                noiseBand[(size_t) bnd] = juce::Decibels::gainToDecibels (std::sqrt (pn * norm), -100.0f);
            }
    }
};

class VoiceCleanerFace : public EffectFace
{
public:
    explicit VoiceCleanerFace (VoiceCleanerFx& f)
        : EffectFace (f, { KnobStyle::Glossy, juce::Colour (0xff1d2a2a), juce::Colour (0xffbff8e6), juce::Colour (0xff3ee0b4), juce::Colour (0xff1b3330),
                           juce::Colour (0xffe6fff8), juce::Colour (0xff7fa89f), juce::Colour (0xff0b1615), juce::Colour (0xffe6fff8) }), vc (f)
    {
        const auto lc = juce::Colour (0xff9cc7bd);
        knob ("noise", "NOISE", lc, 66); knob ("reverb", "REVERB", lc, 66); knob ("voice", "VOICE", lc, 66); knob ("smooth", "SMOOTHNESS", lc, 66);
        listenSw = choice ("listen");
        addAndMakeVisible (learn);
        learn.setTooltip ("Hold while only the background plays, to teach it the noise (it also learns by itself in quiet moments)");
        learn.onStateChange = [this] { vc.learning = learn.isDown(); };
        setSize (860, 500);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff10201f), 0, 0, juce::Colour (0xff050a0a), 0, (float) getHeight(), false);
        g.setGradientFill (bg); g.fillAll();
        title (g, { 22, 12, 600, 30 }, "VOICE CLEANER", "NOISE  /  ROOM  /  VOICE", juce::Colour (0xffe6fff8), juce::Colour (0xff3ee0b4));

        auto a = spectrum.toFloat();
        screenBox (g, a, juce::Colour (0xff061010));
        auto plot = a.reduced (12, 12);
        auto toY = [&] (float db) { return plot.getBottom() - (juce::jlimit (-90.0f, -6.0f, db) + 90.0f) / 84.0f * plot.getHeight(); };
        auto shape = [&] (const std::array<std::atomic<float>, VoiceCleanerFx::bands>& v, bool close)
        {
            juce::Path p;
            for (int i = 0; i < VoiceCleanerFx::bands; ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / (VoiceCleanerFx::bands - 1);
                const float y = toY (v[(size_t) i].load());
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            if (close) { p.lineTo (plot.getRight(), plot.getBottom()); p.lineTo (plot.getX(), plot.getBottom()); p.closeSubPath(); }
            return p;
        };
        // what came in (grey), what's left (green), the noise it hears (red)
        g.setColour (juce::Colour (0xff9fb2ae).withAlpha (0.18f)); g.fillPath (shape (vc.inBand, true));
        g.setColour (juce::Colour (0xff3ee0b4).withAlpha (0.35f)); g.fillPath (shape (vc.outBand, true));
        glow (g, shape (vc.outBand, false), juce::Colour (0xff3ee0b4), 1.6f);
        glow (g, shape (vc.noiseBand, false), juce::Colour (0xffff6b6b).withAlpha (0.8f), 1.2f);
        g.setFont (theme::font (10.0f, true));
        for (auto [f, t] : { std::pair { 100.0, "100" }, { 1000.0, "1k" }, { 10000.0, "10k" } })
        {
            const float x = plot.getX() + plot.getWidth() * (float) (std::log (f / 40.0) / std::log (500.0));
            g.setColour (juce::Colours::white.withAlpha (0.06f)); g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (juce::Colour (0xff7fa89f)); g.drawText (t, (int) x + 3, (int) plot.getBottom() - 12, 30, 12, juce::Justification::left);
        }
        auto legend = a.reduced (14, 8).removeFromTop (14);
        g.setColour (juce::Colour (0xff3ee0b4)); g.drawText ("YOUR VOICE", legend, juce::Justification::right);
        g.setColour (juce::Colour (0xffff6b6b)); g.drawText ("NOISE IT HEARS", legend.withTrimmedRight (90), juce::Justification::right);
        g.setColour (juce::Colour (0xff9fb2ae)); g.drawText ("BEFORE", legend.withTrimmedRight (210), juce::Justification::right);

        // how much is being taken away
        const float removedDb = juce::jlimit (0.0f, 30.0f, fx.inputDb.load() - fx.outputDb.load());
        auto m = removal.toFloat();
        screenBox (g, m, juce::Colour (0xff061010), 6.0f);
        auto bar = m.reduced (8, 10);
        const float h = bar.getHeight() * removedDb / 30.0f;
        g.setColour (juce::Colour (0xffff6b6b).withAlpha (0.8f));
        g.fillRoundedRectangle (bar.withTop (bar.getY()).withHeight (h), 3.0f);
        g.setColour (juce::Colour (0xff7fa89f)); g.setFont (theme::font (9.5f, true));
        g.drawText ("REMOVED", m.withTrimmedTop (m.getHeight() - 16), juce::Justification::centred);
        g.drawText (juce::String (removedDb, 1) + " dB", m.withHeight (16).translated (0, -18), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (40);
        auto top = r.removeFromTop (240);
        removal = top.removeFromRight (70);
        top.removeFromRight (10);
        spectrum = top;
        r.removeFromTop (14);
        auto ctl = r.removeFromTop (28);
        listenSw->setBounds (ctl.removeFromLeft (320)); ctl.removeFromLeft (16);
        learn.setBounds (ctl.removeFromLeft (180));
        r.removeFromTop (10);
        row (r, 0, 4);
    }

private:
    VoiceCleanerFx& vc;
    Switch* listenSw = nullptr;
    juce::TextButton learn { "HOLD TO LEARN NOISE" };
    juce::Rectangle<int> spectrum, removal;
};

// =============================================================================
std::unique_ptr<BuiltinFx> createAutoTune()      { return std::make_unique<AutoTuneFx>(); }
std::unique_ptr<BuiltinFx> createBusCompressor() { return std::make_unique<BusCompFx>(); }
std::unique_ptr<BuiltinFx> createColorEq()       { return std::make_unique<ColorEqFx>(); }
std::unique_ptr<BuiltinFx> createVoiceCleaner()  { return std::make_unique<VoiceCleanerFx>(); }

juce::AudioProcessorEditor* createFaceplate2 (BuiltinFx& fx)
{
    if (auto* p = dynamic_cast<AutoTuneFx*> (&fx))     return new AutoTuneFace (*p);
    if (auto* p = dynamic_cast<BusCompFx*> (&fx))      return new BusCompFace (*p);
    if (auto* p = dynamic_cast<ColorEqFx*> (&fx))      return new ColorEqFace (*p);
    if (auto* p = dynamic_cast<VoiceCleanerFx*> (&fx)) return new VoiceCleanerFace (*p);
    return nullptr;
}

} // namespace rs
