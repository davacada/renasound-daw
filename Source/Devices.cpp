// =============================================================================
//  RENASOUND devices with their own faceplates (see Devices.h)
// =============================================================================
#include "Devices.h"
#include "DspHelpers.h"
#include "UI/Theme.h"
#include "UI/Faceplate.h"

namespace rs
{

std::function<juce::Array<std::pair<int, juce::String>>()> SidechainUser::listTracks;

using APVTS = juce::AudioProcessorValueTreeState;

namespace
{
    inline float dbOf (float lin) { return 20.0f * std::log10 (lin + 1.0e-9f); }
    inline float coefFor (double ms, double sr) { return (float) (1.0 - std::exp (-1.0 / (juce::jmax (0.01, ms) * 0.001 * sr))); }

    struct Follower
    {
        float env = 0;
        inline float process (float x, float att, float rel) noexcept { env += (x > env ? att : rel) * (x - env); return env; }
    };

    std::unique_ptr<juce::AudioParameterChoice> choiceParam (const char* id, const char* name, juce::StringArray items, int def)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def);
    }
}

// =============================================================================
//  PUNCH: transient shaper + HDR (upward compression for detail) + crisp
// =============================================================================
class PunchFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    PunchFx() : BuiltinFx ("punch", "Punch", layout()) { for (auto& h : history) for (auto& v : h) v = -100.0f; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("attack", "Attack", Range (-100, 100), 30, "%"));
        l.add (floatParam ("sustain", "Sustain", Range (-100, 100), 0, "%"));
        l.add (floatParam ("hdr", "HDR", Range (0, 100), 25, "%"));
        l.add (floatParam ("crisp", "Crisp", Range (0, 100), 0, "%"));
        l.add (choiceParam ("mode", "Mode", { "Gentle", "Punchy", "Smash" }, 1));
        l.add (floatParam ("mix", "Mix", Range (0, 100), 100, "%"));
        l.add (floatParam ("output", "Output", Range (-12, 12), 0, "dB"));
        return l;
    }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        fast = slowA = slowS = {};
        for (auto& h : hp) h.reset();
        for (auto& h : hp) h.c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, 3500, 0.707, 0);
        acc = 0; accIn = accOut = accGain = 0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples(), chans = juce::jmin (2, buffer.getNumChannels());
        if (chans == 0) return;
        const double sr = currentSampleRate;
        const int mode = juce::jlimit (0, 2, (int) std::lround (param ("mode")));
        const double fastAtt[] { 1.0, 0.4, 0.15 }, fastRel[] { 70, 45, 25 }, slowAtt[] { 30, 15, 7 }, susRel[] { 450, 300, 180 };
        const float fa = coefFor (fastAtt[mode], sr), fr = coefFor (fastRel[mode], sr);
        const float sa = coefFor (slowAtt[mode], sr), sRel = coefFor (susRel[mode], sr);
        const float attack = param ("attack") / 100.0f, sustain = param ("sustain") / 100.0f;
        const float hdr = param ("hdr") / 100.0f * (mode == 2 ? 1.3f : mode == 0 ? 0.7f : 1.0f);
        const float crisp = param ("crisp") / 100.0f, mix = param ("mix") / 100.0f;
        const float out = juce::Decibels::decibelsToGain (param ("output"));

        auto* L = buffer.getWritePointer (0);
        auto* R = chans > 1 ? buffer.getWritePointer (1) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float l = L[i], r = R != nullptr ? R[i] : l;
            const float d = juce::jmax (std::abs (l), std::abs (r));
            const float ef = fast.process (d, fa, fr), ea = slowA.process (d, sa, fr), es = slowS.process (d, fa, sRel);
            const float dbF = dbOf (ef), dbA = dbOf (ea), dbS = dbOf (es);
            // sustain and HDR look at the instant level too, so a new hit is never boosted
            const float dbNow = dbOf (juce::jmax (d, ef));

            float gainDb = 0.0f;
            if (dbF > -70.0f)
            {
                gainDb += juce::jlimit (-18.0f, 18.0f, attack * juce::jmax (0.0f, dbF - dbA));
                gainDb += juce::jlimit (-18.0f, 18.0f, sustain * 0.7f * juce::jmax (0.0f, dbS - dbNow));
                // HDR: quiet detail comes up (upward compression), loud peaks stay put;
                // nothing below -60 dB, so the noise floor isn't dragged up
                const float lift = dbNow < -20.0f ? juce::jmin (18.0f, (-20.0f - dbNow) * 0.7f) : 0.0f;
                const float floorFade = juce::jlimit (0.0f, 1.0f, (dbNow + 72.0f) / 12.0f);
                gainDb += hdr * lift * floorFade;
            }
            const float g = juce::Decibels::decibelsToGain (gainDb);

            float wl = l * g, wr = r * g;
            if (crisp > 0.001f)
            {
                const float hl = hp[0].process (wl, 0), hr = hp[1].process (wr, 0);
                wl += crisp * 1.5f * std::tanh (hl * 4.0f) * 0.25f;
                wr += crisp * 1.5f * std::tanh (hr * 4.0f) * 0.25f;
            }
            // boosted transients can go over; a soft ceiling at -1 dB keeps them from clipping
            auto ceiling = [] (float x) { const float ax = std::abs (x); return ax < 0.8f ? x : std::copysign (0.8f + 0.09f * std::tanh ((ax - 0.8f) / 0.09f), x); };
            const float ol = ceiling ((l + mix * (wl - l)) * out), orr = ceiling ((r + mix * (wr - r)) * out);
            L[i] = ol;
            if (R != nullptr) R[i] = orr;

            // history for the display, one point per ~10 ms
            accIn = juce::jmax (accIn, d);
            accOut = juce::jmax (accOut, juce::jmax (std::abs (ol), std::abs (orr)));
            accGain += gainDb;
            if (++acc >= (int) (sr * 0.01))
            {
                const int w = writePos.load();
                history[0][(size_t) w] = dbOf (accIn);
                history[1][(size_t) w] = dbOf (accOut);
                history[2][(size_t) w] = accGain / (float) acc;
                writePos = (w + 1) % historySize;
                acc = 0; accIn = accOut = accGain = 0;
            }
        }
    }

    static constexpr int historySize = 300;
    std::array<std::array<std::atomic<float>, historySize>, 3> history;   // in dB, out dB, gain change dB
    std::atomic<int> writePos { 0 };

    juce::AudioProcessorEditor* createEditor() override;

private:
    Follower fast, slowA, slowS;
    std::array<Biquad, 2> hp;
    int acc = 0;
    float accIn = 0, accOut = 0, accGain = 0;
};

class PunchEditor : public FaceEditor
{
public:
    explicit PunchEditor (PunchFx& p)
        : FaceEditor (p, { KnobStyle::Minimal, juce::Colours::white, juce::Colour (0xff1d2b2a), juce::Colour (0xff00a08a), juce::Colour (0xffd6d9d4),
                           juce::Colour (0xff1d2b2a), juce::Colour (0xff7a827f), juce::Colour (0xffe2e5e0), juce::Colour (0xff1d2b2a) }),
          fx (p), mode (p, "mode")
    {
        for (auto [id, name] : { std::pair { "attack", "ATTACK" }, { "sustain", "SUSTAIN" }, { "hdr", "HDR" }, { "crisp", "CRISP" }, { "mix", "MIX" }, { "output", "OUTPUT" } })
        {
            auto* k = knobs.add (new Knob (p, id, name));
            k->label.setColour (juce::Label::textColourId, look.s.textDim);
            addAndMakeVisible (k);
        }
        addAndMakeVisible (mode);
        setSize (620, 400);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        juce::ColourGradient bg (juce::Colour (0xfff6f6f3), 0, 0, juce::Colour (0xffe6e8e3), 0, r.getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();

        auto head = getLocalBounds().reduced (20, 14).removeFromTop (30);
        g.setColour (look.s.text);
        g.setFont (theme::font (22.0f, true));
        g.drawText ("PUNCH", head, juce::Justification::centredLeft);
        g.setColour (look.s.arc);
        g.fillRect (head.getX() + 84, head.getCentreY() - 2, 18, 4);
        g.setColour (look.s.textDim);
        g.setFont (theme::font (11.0f, true));
        g.drawText ("TRANSIENT + HDR ENHANCER", head.withTrimmedLeft (110), juce::Justification::centredLeft);

        // history: input (grey), output (teal), gain change (orange)
        auto a = display.toFloat();
        g.setColour (juce::Colour (0xff1d2b2a));
        g.fillRoundedRectangle (a, 8.0f);
        auto plot = a.reduced (10, 8);
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        for (float db : { -12.0f, -24.0f, -36.0f, -48.0f })
            g.drawHorizontalLine ((int) yFor (db, plot), plot.getX(), plot.getRight());
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());

        const int w0 = fx.writePos.load();
        auto line = [&] (int which, juce::Colour col, bool fill, bool isGain)
        {
            juce::Path p;
            for (int i = 0; i < PunchFx::historySize; ++i)
            {
                const float v = fx.history[(size_t) which][(size_t) ((w0 + i) % PunchFx::historySize)].load();
                const float x = plot.getX() + plot.getWidth() * (float) i / (PunchFx::historySize - 1);
                const float y = isGain ? plot.getCentreY() - juce::jlimit (-18.0f, 18.0f, v) / 18.0f * plot.getHeight() * 0.45f : yFor (v, plot);
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            if (fill)
            {
                juce::Path f (p);
                f.lineTo (plot.getRight(), plot.getBottom()); f.lineTo (plot.getX(), plot.getBottom()); f.closeSubPath();
                g.setColour (col.withAlpha (0.25f));
                g.fillPath (f);
            }
            g.setColour (col);
            g.strokePath (p, juce::PathStrokeType (isGain ? 1.6f : 1.4f));
        };
        line (0, juce::Colour (0xff8d9894), true, false);
        line (1, look.s.arc, false, false);
        line (2, juce::Colour (0xffff8a3d), false, true);
        g.setFont (theme::font (10.0f, true));
        g.setColour (juce::Colour (0xff8d9894)); g.drawText ("IN", (int) plot.getX(), (int) plot.getY(), 30, 12, juce::Justification::left);
        g.setColour (look.s.arc);                g.drawText ("OUT", (int) plot.getX() + 26, (int) plot.getY(), 30, 12, juce::Justification::left);
        g.setColour (juce::Colour (0xffff8a3d)); g.drawText ("GAIN", (int) plot.getX() + 58, (int) plot.getY(), 40, 12, juce::Justification::left);

        g.setColour (look.s.textDim);
        g.drawText ("MODE", mode.getBounds().translated (0, -15).withHeight (14), juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 14);
        r.removeFromTop (40);
        display = r.removeFromTop (150);
        r.removeFromTop (22);
        mode.setBounds (r.removeFromTop (28).removeFromLeft (270));
        r.removeFromTop (12);
        const int w = r.getWidth() / knobs.size();
        for (auto* k : knobs) k->setBounds (r.removeFromLeft (w).reduced (4, 0));
    }

private:
    PunchFx& fx;
    Switch mode;
    juce::OwnedArray<Knob> knobs;
    juce::Rectangle<int> display;
    static float yFor (float db, juce::Rectangle<float> a) { return juce::jmap (juce::jlimit (-60.0f, 0.0f, db), -60.0f, 0.0f, a.getBottom(), a.getY() + 14); }
};

juce::AudioProcessorEditor* PunchFx::createEditor() { return new PunchEditor (*this); }

// =============================================================================
//  STEREO WIDENER: mid/side width, mono-safe spread, bass kept in mono
// =============================================================================
class WidenerFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    WidenerFx() : BuiltinFx ("widener", "Stereo Widener", layout()) { for (auto& v : scopeL) v = 0; for (auto& v : scopeR) v = 0; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("width", "Width", Range (0, 200), 140, "%"));
        l.add (floatParam ("spread", "Spread", Range (0, 100), 35, "%"));
        l.add (floatParam ("bassMono", "Bass Mono", skewed (20, 500, 120), 120, "Hz"));
        l.add (floatParam ("output", "Output", Range (-12, 12), 0, "dB"));
        return l;
    }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        delay.assign ((size_t) juce::nextPowerOfTwo ((int) (sr * 0.05)), 0.0f);
        dpos = 0;
        for (auto& b : bass) b.reset();
        spreadHp.reset();
        spreadHp.c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, 300, 0.707, 0);
        sLL = sRR = sLR = 0;
        lastBass = -1;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        if (buffer.getNumChannels() < 2 || delay.empty()) return;
        const double sr = currentSampleRate;
        const float width = param ("width") / 100.0f, spread = param ("spread") / 100.0f;
        const float bassF = param ("bassMono"), out = juce::Decibels::decibelsToGain (param ("output"));
        const bool bassOn = bassF > 20.5f;
        if (bassOn && ! juce::approximatelyEqual (bassF, lastBass))
        {
            for (auto& b : bass) b.c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, bassF, 0.707, 0);
            lastBass = bassF;
        }
        const int mask = (int) delay.size() - 1;
        const int dly = (int) (sr * 0.011);
        const float corrCoef = coefFor (300, sr);

        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getWritePointer (1);
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (L[i] + R[i]);
            float s = 0.5f * (L[i] - R[i]) * width;

            // a delayed, filtered copy of the middle goes into the sides only:
            // it widens, and cancels out completely when summed to mono
            delay[(size_t) dpos] = m;
            const float delayed = delay[(size_t) ((dpos - dly) & mask)];
            dpos = (dpos + 1) & mask;
            s += spread * 0.7f * spreadHp.process (delayed, 0);

            if (bassOn) s = bass[1].process (bass[0].process (s, 0), 0);   // 24 dB/oct: lows stay centred

            const float l = (m + s) * out, r = (m - s) * out;
            L[i] = l; R[i] = r;

            sLL += corrCoef * (l * l - sLL); sRR += corrCoef * (r * r - sRR); sLR += corrCoef * (l * r - sLR);
            if ((i & 3) == 0)
            {
                const int w = scopePos.load();
                scopeL[(size_t) w] = l; scopeR[(size_t) w] = r;
                scopePos = (w + 1) % scopeSize;
            }
        }
        const float den = std::sqrt (sLL * sRR);
        correlation = den > 1.0e-9f ? juce::jlimit (-1.0f, 1.0f, sLR / den) : 1.0f;
    }

    static constexpr int scopeSize = 1024;
    std::array<std::atomic<float>, scopeSize> scopeL, scopeR;
    std::atomic<int> scopePos { 0 };
    std::atomic<float> correlation { 1.0f };

    juce::AudioProcessorEditor* createEditor() override;

private:
    std::vector<float> delay;
    int dpos = 0;
    std::array<Biquad, 2> bass;
    Biquad spreadHp;
    float sLL = 0, sRR = 0, sLR = 0, lastBass = -1;
};

class WidenerEditor : public FaceEditor
{
public:
    explicit WidenerEditor (WidenerFx& p)
        : FaceEditor (p, { KnobStyle::Neon, juce::Colour (0xff120d22), juce::Colour (0xffff4fd8), juce::Colour (0xff35e0ff), juce::Colour (0xff2a2145),
                           juce::Colour (0xffe9e4ff), juce::Colour (0xff8f86b8), juce::Colour (0xff1d1636), juce::Colour (0xffe9e4ff) }),
          fx (p), width (p, "width", "WIDTH", 80), spread (p, "spread", "SPREAD"), bass (p, "bassMono", "BASS MONO"), output (p, "output", "OUTPUT")
    {
        for (auto* k : { &width, &spread, &bass, &output })
        {
            k->label.setColour (juce::Label::textColourId, look.s.textDim);
            addAndMakeVisible (*k);
        }
        width.label.setFont (theme::font (13.0f, true));
        setSize (600, 400);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        juce::ColourGradient bg (juce::Colour (0xff2a1450), r.getCentreX(), r.getHeight() * 0.35f, juce::Colour (0xff07050d), 0, r.getHeight() * 1.2f, true);
        g.setGradientFill (bg);
        g.fillAll();

        g.setFont (theme::font (21.0f, true));
        juce::ColourGradient title (look.s.arc, 20, 0, look.s.pointer, 260, 0, false);
        g.setGradientFill (title);
        g.drawText ("STEREO WIDENER", 20, 14, 300, 28, juce::Justification::centredLeft);

        // goniometer: mono content stands up, wide content spreads sideways
        auto sc = scope.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillEllipse (sc);
        g.setColour (look.s.arcBg.brighter (0.2f));
        g.drawEllipse (sc, 1.0f);
        g.drawLine (sc.getCentreX(), sc.getY(), sc.getCentreX(), sc.getBottom(), 0.7f);
        g.drawLine (sc.getX(), sc.getCentreY(), sc.getRight(), sc.getCentreY(), 0.7f);
        g.setFont (theme::font (10.0f, true));
        g.setColour (look.s.textDim);
        g.drawText ("M", juce::Rectangle<float> (sc.getCentreX() - 8, sc.getY() + 3, 16, 12), juce::Justification::centred);
        g.drawText ("L", juce::Rectangle<float> (sc.getX() + 18, sc.getY() + 18, 14, 12), juce::Justification::centred);
        g.drawText ("R", juce::Rectangle<float> (sc.getRight() - 32, sc.getY() + 18, 14, 12), juce::Justification::centred);
        const float rad = sc.getWidth() * 0.5f;
        const auto c = sc.getCentre();
        const int w0 = fx.scopePos.load();
        for (int i = 0; i < WidenerFx::scopeSize; ++i)
        {
            const int k = (w0 + i) % WidenerFx::scopeSize;
            const float l = fx.scopeL[(size_t) k].load(), rr = fx.scopeR[(size_t) k].load();
            float x = (l - rr) * 0.7071f, y = (l + rr) * 0.7071f;   // rotated 45°: side across, mid up
            const float mag = std::sqrt (x * x + y * y);
            if (mag > 1.0f) { x /= mag; y /= mag; }
            const float age = (float) i / WidenerFx::scopeSize;
            g.setColour (look.s.arc.interpolatedWith (look.s.pointer, std::abs (x) * 1.4f).withAlpha (0.15f + 0.7f * age));
            g.fillRect (c.x + x * rad * 0.92f, c.y - y * rad * 0.92f, 1.6f, 1.6f);
        }

        // correlation: +1 mono, 0 wide, -1 out of phase
        auto cm = corr.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.45f));
        g.fillRoundedRectangle (cm, 4.0f);
        const float v = fx.correlation.load();
        const float x = cm.getX() + cm.getWidth() * (v + 1.0f) * 0.5f;
        juce::ColourGradient bar (juce::Colour (0xffff4060), cm.getX(), 0, juce::Colour (0xff35e0ff), cm.getRight(), 0, false);
        g.setGradientFill (bar);
        g.fillRoundedRectangle (juce::Rectangle<float> (juce::jmin (x, cm.getCentreX()), cm.getY() + 3, std::abs (x - cm.getCentreX()), cm.getHeight() - 6), 2.0f);
        g.setColour (juce::Colours::white);
        g.fillRect (x - 1.0f, cm.getY(), 2.0f, cm.getHeight());
        g.setColour (look.s.textDim);
        g.setFont (theme::font (10.0f, true));
        g.drawText ("-1", cm.withY (cm.getBottom() + 2).withHeight (12), juce::Justification::left);
        g.drawText ("CORRELATION " + juce::String (v, 2), cm.withY (cm.getBottom() + 2).withHeight (12), juce::Justification::centred);
        g.drawText ("+1", cm.withY (cm.getBottom() + 2).withHeight (12), juce::Justification::right);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 14);
        r.removeFromTop (40);
        auto left = r.removeFromLeft (250);
        scope = left.removeFromTop (250).reduced (6);
        left.removeFromTop (16);
        corr = left.removeFromTop (16).reduced (6, 0);
        r.removeFromLeft (20);
        width.setBounds (r.removeFromTop (190).withSizeKeepingCentre (190, 190));
        r.removeFromTop (14);
        const int w = r.getWidth() / 3;
        for (auto* k : { &spread, &bass, &output }) k->setBounds (r.removeFromLeft (w).reduced (4, 0));
    }

private:
    WidenerFx& fx;
    Knob width, spread, bass, output;
    juce::Rectangle<int> scope, corr;
};

juce::AudioProcessorEditor* WidenerFx::createEditor() { return new WidenerEditor (*this); }

// =============================================================================
//  AMP: noise gate, preamp (3 channels, 4x oversampled), tone stack, cabinets
// =============================================================================
class AmpFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    AmpFx() : BuiltinFx ("amp", "Amp", layout()) {}

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        auto ten = [] (const char* id, const char* name, float def) { return floatParam (id, name, Range (0, 10), def); };
        l.add (choiceParam ("channel", "Channel", { "Clean", "Crunch", "Lead" }, 1));
        l.add (ten ("gain", "Gain", 5));
        l.add (ten ("bass", "Bass", 5));
        l.add (ten ("mid", "Middle", 5));
        l.add (ten ("treble", "Treble", 5));
        l.add (ten ("presence", "Presence", 5));
        l.add (ten ("master", "Master", 7));
        l.add (choiceParam ("cab", "Cabinet", { "No cabinet", "1x12 Open", "2x12 Combo", "4x12 Closed" }, 3));
        l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "gate", 1 }, "Gate", Range (-90, -30), -90.0f,
                   juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction ([] (float v, int)
                   { return v <= -89.5f ? juce::String ("Off") : juce::String (juce::roundToInt (v)) + " dB"; })));
        return l;
    }

    void prepareToPlay (double sr, int maxBlock) override
    {
        currentSampleRate = sr;
        prepared = juce::jmax (1, maxBlock);
        mono.setSize (1, prepared);
        oversampling.initProcessing ((size_t) prepared);
        oversampling.reset();
        for (auto& f : pre) f.reset();
        for (auto& f : tone) f.reset();
        for (auto& f : cab) f.reset();
        gateEnv = {}; gateGain = 1.0f;
        lastKey = {};
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples(), chans = juce::jmin (2, buffer.getNumChannels());
        if (chans == 0 || n > prepared) return;
        const double sr = currentSampleRate;
        updateFilters();
        const int channel = juce::jlimit (0, 2, (int) std::lround (param ("channel")));
        const float gain = param ("gain");
        const float driveDb = channel == 0 ? gain * 2.4f : channel == 1 ? 6.0f + gain * 3.4f : 14.0f + gain * 3.6f;
        const float k = juce::Decibels::decibelsToGain (driveDb);
        const float master = juce::Decibels::decibelsToGain (-24.0f + param ("master") * 3.0f);
        const float gateThr = param ("gate");
        const bool gateOn = gateThr > -89.5f;
        const float ga = coefFor (1, sr), gr = coefFor (80, sr), open = coefFor (2, sr), close = coefFor (60, sr);

        auto* m = mono.getWritePointer (0);
        for (int i = 0; i < n; ++i)
        {
            float x = chans > 1 ? 0.5f * (buffer.getSample (0, i) + buffer.getSample (1, i)) : buffer.getSample (0, i);
            if (gateOn)
            {
                const float lvl = dbOf (gateEnv.process (std::abs (x), ga, gr));
                const float target = lvl > gateThr ? 1.0f : lvl < gateThr - 4.0f ? 0.0f : gateGain;
                gateGain += (target > gateGain ? open : close) * (target - gateGain);
                x *= gateGain;
            }
            for (auto& f : pre) x = f.process (x, 0);
            m[i] = x;
        }

        // preamp at 4x the sample rate so the distortion doesn't alias
        {
            float* chan[] { m };
            juce::dsp::AudioBlock<float> block (chan, 1, (size_t) n);
            auto up = oversampling.processSamplesUp (block);
            auto* d = up.getChannelPointer (0);
            const size_t un = up.getNumSamples();
            for (size_t i = 0; i < un; ++i)
            {
                float x = d[i] * k;
                if (channel == 0)
                    x = std::tanh (x * 0.8f) / 0.8f / juce::jmax (1.0f, std::sqrt (k));
                else if (channel == 1)
                {
                    x = std::tanh (x + 0.2f) - 0.19738f;
                    x = std::tanh (2.0f * x) * 0.6f;
                }
                else
                {
                    x = std::tanh (x + 0.25f) - 0.24492f;
                    stageLp += 0.35f * (x - stageLp);           // a little smoothing between stages
                    x = std::tanh (3.0f * stageLp + 0.1f) - 0.09967f;
                    x = std::tanh (2.0f * x) * 0.5f;
                }
                d[i] = x;
            }
            oversampling.processSamplesDown (block);
        }

        for (int i = 0; i < n; ++i)
        {
            float x = m[i];
            for (auto& f : tone) x = f.process (x, 0);
            if (cabCount > 0) for (int c = 0; c < cabCount; ++c) x = cab[(size_t) c].process (x, 0);
            x *= master;
            for (int ch = 0; ch < chans; ++ch) buffer.setSample (ch, i, x);
        }
        level = juce::jmax (level.load() * 0.9f, buffer.getMagnitude (0, 0, n));
    }

    std::atomic<float> level { 0.0f };
    juce::AudioProcessorEditor* createEditor() override;

private:
    int prepared = 0;
    juce::AudioBuffer<float> mono;
    juce::dsp::Oversampling<float> oversampling { 1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true };
    std::array<Biquad, 2> pre;
    std::array<Biquad, 4> tone;
    std::array<Biquad, 6> cab;
    int cabCount = 0;
    Follower gateEnv;
    float gateGain = 1.0f, stageLp = 0.0f;
    std::array<float, 7> lastKey {};

    void updateFilters()
    {
        const std::array<float, 7> key { param ("channel"), param ("bass"), param ("mid"), param ("treble"), param ("presence"), param ("cab"), (float) currentSampleRate };
        if (key == lastKey) return;
        lastKey = key;
        using T = BiquadCoefs::Type;
        const double sr = currentSampleRate;
        const int channel = juce::jlimit (0, 2, (int) std::lround (key[0]));
        const double preHp[] { 60, 100, 140 }, preBoost[] { 0, 4, 7 };
        pre[0].c = BiquadCoefs::make (T::HighPass, sr, preHp[channel], 0.707, 0);
        pre[1].c = BiquadCoefs::make (T::Peak, sr, 900, 0.8, preBoost[channel]);
        tone[0].c = BiquadCoefs::make (T::LowShelf, sr, 110, 0.7, (key[1] - 5) * 2.4);
        tone[1].c = BiquadCoefs::make (T::Peak, sr, 650, 0.8, (key[2] - 5) * 2.4);
        tone[2].c = BiquadCoefs::make (T::HighShelf, sr, 2800, 0.7, (key[3] - 5) * 2.4);
        tone[3].c = BiquadCoefs::make (T::HighShelf, sr, 5000, 0.7, (key[4] - 5) * 1.8);

        struct F { T t; double f, q, g; };
        std::vector<F> spec;
        switch (juce::jlimit (0, 3, (int) std::lround (key[5])))
        {
            case 1: spec = { { T::HighPass, 70, 0.7, 0 }, { T::Peak, 110, 1.2, 3 }, { T::Peak, 400, 1.0, -3 }, { T::Peak, 2500, 1.6, 2 }, { T::LowPass, 5500, 0.7, 0 }, { T::LowPass, 6500, 0.7, 0 } }; break;
            case 2: spec = { { T::HighPass, 80, 0.7, 0 }, { T::Peak, 120, 1.3, 4 }, { T::Peak, 1800, 1.5, -3 }, { T::Peak, 3200, 2.0, 3 }, { T::LowPass, 5000, 0.7, 0 }, { T::LowPass, 5800, 0.7, 0 } }; break;
            case 3: spec = { { T::HighPass, 60, 0.7, 0 }, { T::Peak, 100, 1.4, 6 }, { T::Peak, 500, 0.9, -4 }, { T::Peak, 2800, 1.8, 4 }, { T::LowPass, 4500, 0.7, 0 }, { T::LowPass, 5200, 0.7, 0 } }; break;
            default: break;
        }
        cabCount = (int) spec.size();
        for (size_t i = 0; i < spec.size(); ++i) cab[i].c = BiquadCoefs::make (spec[i].t, sr, spec[i].f, spec[i].q, spec[i].g);
    }
};

class AmpEditor : public FaceEditor
{
public:
    explicit AmpEditor (AmpFx& p)
        : FaceEditor (p, { KnobStyle::ChickenHead, juce::Colour (0xff141413), juce::Colour (0xfff3ead2), juce::Colour (0xffb2232a), juce::Colour (0xff8a7b5a),
                           juce::Colour (0xff221f1a), juce::Colour (0xff4a4134), juce::Colour (0xff2b2722), juce::Colour (0xfff3ead2) }),
          fx (p), channel (p, "channel")
    {
        for (auto [id, name] : { std::pair { "gain", "GAIN" }, { "bass", "BASS" }, { "mid", "MIDDLE" }, { "treble", "TREBLE" }, { "presence", "PRESENCE" }, { "master", "MASTER" } })
        {
            auto* k = knobs.add (new Knob (p, id, name, 46));
            k->label.setFont (theme::font (12.0f, true));
            k->label.setColour (juce::Label::textColourId, look.s.text);
            addAndMakeVisible (k);
        }
        gate = std::make_unique<Knob> (p, "gate", "GATE", 58);
        gate->label.setColour (juce::Label::textColourId, juce::Colour (0xffd8c9a3));
        addAndMakeVisible (*gate);
        cabBox.addItemList ({ "No cabinet", "1x12 Open", "2x12 Combo", "4x12 Closed" }, 1);
        cabAttach = std::make_unique<APVTS::ComboBoxAttachment> (p.state, "cab", cabBox);
        addAndMakeVisible (cabBox);
        addAndMakeVisible (channel);
        setSize (820, 360);
        startTimerHz (20);

        // the tolex texture, drawn once
        tolex = juce::Image (juce::Image::RGB, 820, 360, false);
        juce::Graphics tg (tolex);
        tg.fillAll (juce::Colour (0xff191816));
        juce::Random rnd (7);
        for (int i = 0; i < 26000; ++i)
        {
            tg.setColour (juce::Colour (0xff000000).withAlpha (rnd.nextFloat() * 0.5f).interpolatedWith (juce::Colour (0xff3a3833), rnd.nextFloat() * 0.5f));
            tg.fillRect ((float) rnd.nextInt (820), (float) rnd.nextInt (360), 1.5f, 1.5f);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.drawImageAt (tolex, 0, 0);
        auto r = getLocalBounds().toFloat().reduced (8);
        // gold piping
        g.setColour (juce::Colour (0xffc9a75a));
        g.drawRoundedRectangle (r, 10.0f, 2.0f);

        // logo plate
        g.setColour (juce::Colour (0xffd9b86a));
        g.setFont (juce::Font (juce::FontOptions ("Georgia", 30.0f, juce::Font::bold | juce::Font::italic)));
        g.drawText ("Renasound", 34, 22, 300, 40, juce::Justification::centredLeft);
        g.setFont (theme::font (11.0f, true));
        g.setColour (juce::Colour (0xffb39a62));
        g.drawText ("ALL-TUBE STYLE GUITAR AMPLIFIER", 36, 60, 300, 14, juce::Justification::centredLeft);

        // grille cloth hint behind the logo row
        auto grille = juce::Rectangle<float> (360, 26, 430, 54);
        g.setColour (juce::Colour (0xff2a2620));
        g.fillRoundedRectangle (grille, 4.0f);
        g.setColour (juce::Colour (0xff3d372d));
        for (float x = grille.getX() - 60; x < grille.getRight(); x += 6)
            g.drawLine (x, grille.getBottom(), x + 54, grille.getY(), 1.0f);
        g.setColour (juce::Colour (0xffc9a75a).withAlpha (0.6f));
        g.drawRoundedRectangle (grille, 4.0f, 1.0f);
        g.setColour (juce::Colour (0xffd8c9a3));
        g.setFont (theme::font (10.0f, true));
        g.drawText ("CHANNEL", channel.getBounds().translated (0, -14).withHeight (12), juce::Justification::left);
        g.drawText ("CABINET", cabBox.getBounds().translated (0, -14).withHeight (12), juce::Justification::left);

        // the cream control panel
        auto panel = panelArea.toFloat();
        juce::ColourGradient cream (juce::Colour (0xfff1e7cc), 0, panel.getY(), juce::Colour (0xffd9cba8), 0, panel.getBottom(), false);
        g.setGradientFill (cream);
        g.fillRoundedRectangle (panel, 6.0f);
        g.setColour (juce::Colour (0xff8a7b5a));
        g.drawRoundedRectangle (panel, 6.0f, 1.2f);

        // pilot light
        const float lvl = juce::jlimit (0.0f, 1.0f, fx.level.load() * 3.0f);
        auto jewel = juce::Rectangle<float> (panel.getRight() - 46, panel.getCentreY() - 13, 26, 26);
        g.setColour (juce::Colour (0xff3b0a0c));
        g.fillEllipse (jewel);
        juce::ColourGradient glow (juce::Colour (0xffff4040).withAlpha (0.45f + 0.55f * lvl), jewel.getCentreX(), jewel.getCentreY(),
                                   juce::Colour (0xff6a0d10), jewel.getRight(), jewel.getBottom(), true);
        g.setGradientFill (glow);
        g.fillEllipse (jewel.reduced (3));
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.fillEllipse (jewel.getX() + 8, jewel.getY() + 6, 6, 4);
        g.setColour (look.s.textDim);
        g.setFont (theme::font (9.0f, true));
        g.drawText ("ON", jewel.translated (0, 26).withHeight (10), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (24, 18);
        auto top = r.removeFromTop (84);
        top.removeFromLeft (360);
        r.removeFromTop (10);
        auto controls = r.removeFromTop (34);
        channel.setBounds (controls.removeFromLeft (300).withTrimmedTop (8));
        controls.removeFromLeft (20);
        cabBox.setBounds (controls.removeFromLeft (170).withTrimmedTop (8));
        controls.removeFromLeft (20);
        gate->setBounds (controls.getX(), controls.getY() - 46, 90, 92);
        juce::ignoreUnused (top);
        r.removeFromTop (12);
        panelArea = r;
        auto kr = r.reduced (10, 6).withTrimmedRight (60);
        const int w = kr.getWidth() / knobs.size();
        for (auto* k : knobs) k->setBounds (kr.removeFromLeft (w).reduced (2, 0));
    }

private:
    AmpFx& fx;
    Switch channel;
    juce::OwnedArray<Knob> knobs;
    std::unique_ptr<Knob> gate;
    juce::ComboBox cabBox;
    std::unique_ptr<APVTS::ComboBoxAttachment> cabAttach;
    juce::Rectangle<int> panelArea;
    juce::Image tolex;
};

juce::AudioProcessorEditor* AmpFx::createEditor() { return new AmpEditor (*this); }

// =============================================================================
//  KICK SYNTH (instrument): pitch-swept sine body, click, punch, drive
// =============================================================================
struct KickParams
{
    float tune = 50, pitch = 24, sweep = 45, decay = 450, click = 40, clickTone = 3500, punch = 40, drive = 20, level = 0;
    bool keyTrack = false;
};

struct KickVoice
{
    bool active = false;
    double t = 0, phase = 0, freqEnd = 50;
    float vel = 1, fade = 1, fadeStep = 0;
    Biquad clickFilter;
    juce::Random rnd;

    void start (const KickParams& p, double sr, int note, float velocity)
    {
        active = true; t = 0; phase = 0; vel = velocity; fade = 1; fadeStep = 0;
        freqEnd = p.keyTrack ? p.tune * std::pow (2.0, (note - 36) / 12.0) : p.tune;
        clickFilter.reset();
        clickFilter.c = BiquadCoefs::make (BiquadCoefs::Type::BandPass, sr, p.clickTone, 1.2, 0);
    }

    float next (const KickParams& p, double sr)
    {
        if (! active) return 0.0f;
        const double dt = 1.0 / sr;
        const double sweepTau = juce::jmax (0.001, p.sweep / 1000.0 / 3.0);
        const double f = freqEnd * std::pow (2.0, p.pitch / 12.0 * std::exp (-t / sweepTau));
        phase += f * dt;
        if (phase >= 1.0) phase -= 1.0;
        const double ampTau = juce::jmax (0.005, p.decay / 1000.0 / 6.9);
        float amp = (float) std::exp (-t / ampTau);
        if (t < 0.0006) amp *= (float) (t / 0.0006);
        const float punchGain = 1.0f + p.punch / 100.0f * 1.6f * (float) std::exp (-t / 0.022);
        const float body = (float) std::sin (juce::MathConstants<double>::twoPi * phase) * amp * punchGain;
        const float d = 1.0f + p.drive / 100.0f * 6.0f;
        float y = std::tanh (body * d) / std::tanh (d);

        if (p.click > 0.1f && t < 0.03)
        {
            const float noise = clickFilter.process (rnd.nextFloat() * 2.0f - 1.0f, 0);
            const float blip = (float) std::sin (juce::MathConstants<double>::twoPi * p.clickTone * 0.5 * t) * (float) std::exp (-t / 0.0012);
            y += p.click / 100.0f * (noise * 2.2f * (float) std::exp (-t / 0.0025) + 0.5f * blip);
        }

        t += dt;
        if (fadeStep > 0) { fade -= fadeStep; if (fade <= 0) active = false; }
        if (t > p.decay / 1000.0 * 1.6 + 0.05) active = false;
        return y * vel * fade;
    }
};

class KickSynthFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    KickSynthFx() : BuiltinFx ("kick", "Kick Synth", layout()) {}

    bool acceptsMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 2.5; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("tune", "Tune", skewed (30, 120, 55), 50, "Hz"));
        l.add (floatParam ("pitch", "Pitch", Range (0, 48), 24, "st"));
        l.add (floatParam ("sweep", "Sweep", skewed (5, 300, 50), 45, "ms"));
        l.add (floatParam ("decay", "Decay", skewed (50, 2000, 400), 450, "ms"));
        l.add (floatParam ("click", "Click", Range (0, 100), 40, "%"));
        l.add (floatParam ("clickTone", "Click Tone", skewed (1000, 10000, 3500), 3500, "Hz"));
        l.add (floatParam ("punch", "Punch", Range (0, 100), 40, "%"));
        l.add (floatParam ("drive", "Drive", Range (0, 100), 20, "%"));
        l.add (floatParam ("level", "Level", Range (-24, 6), -3, "dB"));
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "keyTrack", 1 }, "Key Track", false));
        return l;
    }

    KickParams readParams() const
    {
        KickParams p;
        p.tune = param ("tune"); p.pitch = param ("pitch"); p.sweep = param ("sweep"); p.decay = param ("decay");
        p.click = param ("click"); p.clickTone = param ("clickTone"); p.punch = param ("punch"); p.drive = param ("drive");
        p.level = param ("level"); p.keyTrack = param ("keyTrack") > 0.5f;
        return p;
    }

    // the whole kick, for the faceplate's waveform view
    static std::vector<float> renderPreview (const KickParams& p, double sr, double seconds)
    {
        KickVoice v;
        v.start (p, sr, 36, 1.0f);
        std::vector<float> out ((size_t) (sr * seconds));
        for (auto& s : out) s = v.next (p, sr);
        return out;
    }

    void prepareToPlay (double sr, int) override { currentSampleRate = sr; for (auto& v : voices) v.active = false; }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        buffer.clear();
        if (buffer.getNumChannels() == 0) return;
        const auto p = readParams();
        const float gain = juce::Decibels::decibelsToGain (p.level);
        const double sr = currentSampleRate;

        if (audition.exchange (false)) trigger (p, 36, 0.9f);
        auto it = midi.cbegin();
        for (int i = 0; i < n; ++i)
        {
            while (it != midi.cend() && (*it).samplePosition <= i)
            {
                const auto m = (*it).getMessage();
                if (m.isNoteOn()) trigger (p, m.getNoteNumber(), m.getFloatVelocity());
                ++it;
            }
            float y = 0;
            for (auto& v : voices) y += v.next (p, sr);
            y *= gain;
            for (int ch = 0; ch < buffer.getNumChannels(); ++ch) buffer.setSample (ch, i, y);
        }
    }

    std::atomic<bool> audition { false };
    juce::AudioProcessorEditor* createEditor() override;

private:
    std::array<KickVoice, 2> voices;
    int current = 0;

    void trigger (const KickParams& p, int note, float vel)
    {
        // the previous hit fades out over 3 ms instead of clicking off
        auto& old = voices[(size_t) current];
        if (old.active) old.fadeStep = (float) (1.0 / (0.003 * currentSampleRate));
        current = 1 - current;
        voices[(size_t) current].start (p, currentSampleRate, note, vel);
    }
};

class KickEditor : public FaceEditor, private juce::AudioProcessorValueTreeState::Listener
{
public:
    explicit KickEditor (KickSynthFx& p)
        : FaceEditor (p, { KnobStyle::Glossy, juce::Colour (0xff2b2f4a), juce::Colour (0xffffd36b), juce::Colour (0xffff3d7f), juce::Colour (0xff242842),
                           juce::Colour (0xfff3f0ff), juce::Colour (0xff8e92b8), juce::Colour (0xff1b1e33), juce::Colour (0xfff3f0ff) }),
          fx (p)
    {
        for (auto [id, name] : { std::pair { "tune", "TUNE" }, { "pitch", "PITCH" }, { "sweep", "SWEEP" }, { "decay", "DECAY" }, { "punch", "PUNCH" },
                                 { "click", "CLICK" }, { "clickTone", "CLICK TONE" }, { "drive", "DRIVE" }, { "level", "LEVEL" } })
        {
            auto* k = knobs.add (new Knob (p, id, name));
            k->label.setColour (juce::Label::textColourId, look.s.textDim);
            addAndMakeVisible (k);
        }
        play.setButtonText ("PLAY");
        play.onClick = [this] { fx.audition = true; };
        addAndMakeVisible (play);
        keyTrack.setButtonText ("KEY TRACK");
        keyTrack.setClickingTogglesState (true);
        keyTrack.setTooltip ("Follow the note played: C2 plays the Tune frequency");
        keyAttach = std::make_unique<APVTS::ButtonAttachment> (p.state, "keyTrack", keyTrack);
        addAndMakeVisible (keyTrack);
        for (auto* prm : p.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm)) p.state.addParameterListener (r->paramID, this);
        refreshPreview();
        setSize (700, 430);
        startTimerHz (15);
    }

    ~KickEditor() override
    {
        for (auto* prm : fx.getParameters())
            if (auto* r = dynamic_cast<juce::RangedAudioParameter*> (prm)) fx.state.removeParameterListener (r->paramID, this);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        juce::ColourGradient bg (juce::Colour (0xff1c1f36), 0, 0, juce::Colour (0xff0d0e1a), 0, r.getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();
        juce::ColourGradient stripe (look.s.arc, 0, 0, look.s.pointer, r.getWidth(), 0, false);
        g.setGradientFill (stripe);
        g.fillRect (0.0f, 0.0f, r.getWidth(), 4.0f);

        g.setFont (theme::font (24.0f, true));
        g.setGradientFill (stripe);
        g.drawText ("KICK SYNTH", 22, 14, 260, 32, juce::Justification::centredLeft);
        g.setColour (look.s.textDim);
        g.setFont (theme::font (11.0f, true));
        g.drawText ("BIG ROOM  /  808  /  TECHNO", 200, 22, 240, 16, juce::Justification::centredLeft);

        auto a = display.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.4f));
        g.fillRoundedRectangle (a, 8.0f);
        auto plot = a.reduced (10, 10);
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
        if (! preview.empty())
        {
            // waveform as min/max columns
            const int cols = (int) plot.getWidth();
            juce::Path wave;
            std::vector<float> tops, bottoms;
            for (int x = 0; x < cols; ++x)
            {
                const size_t s0 = (size_t) x * preview.size() / (size_t) cols, s1 = std::max (s0 + 1, (size_t) (x + 1) * preview.size() / (size_t) cols);
                float lo = 0, hi = 0;
                for (size_t i = s0; i < s1 && i < preview.size(); ++i) { lo = juce::jmin (lo, preview[i]); hi = juce::jmax (hi, preview[i]); }
                tops.push_back (plot.getCentreY() - hi * plot.getHeight() * 0.48f);
                bottoms.push_back (plot.getCentreY() - lo * plot.getHeight() * 0.48f);
            }
            wave.startNewSubPath (plot.getX(), tops[0]);
            for (int x = 1; x < cols; ++x) wave.lineTo (plot.getX() + (float) x, tops[(size_t) x]);
            for (int x = cols - 1; x >= 0; --x) wave.lineTo (plot.getX() + (float) x, bottoms[(size_t) x]);
            wave.closeSubPath();
            juce::ColourGradient fill (look.s.arc, plot.getX(), 0, look.s.pointer, plot.getRight(), 0, false);
            g.setGradientFill (fill);
            g.fillPath (wave);

            // pitch envelope, log scale 20 Hz .. 2 kHz
            const auto p = fx.readParams();
            juce::Path pitchLine;
            for (int x = 0; x < cols; x += 2)
            {
                const double t = previewSeconds * x / cols;
                const double f = p.tune * std::pow (2.0, p.pitch / 12.0 * std::exp (-t / juce::jmax (0.001, p.sweep / 1000.0 / 3.0)));
                const float y = plot.getBottom() - plot.getHeight() * (float) (std::log (f / 20.0) / std::log (100.0));
                if (x == 0) pitchLine.startNewSubPath (plot.getX(), y); else pitchLine.lineTo (plot.getX() + (float) x, y);
            }
            g.setColour (juce::Colour (0xff6be7ff).withAlpha (0.8f));
            g.strokePath (pitchLine, juce::PathStrokeType (1.5f));
            g.setFont (theme::font (10.0f, true));
            g.drawText ("PITCH", (int) plot.getRight() - 50, (int) plot.getY(), 50, 12, juce::Justification::right);
            g.setColour (look.s.textDim);
            g.drawText (juce::String (p.tune, 0) + " Hz  /  " + juce::String (p.decay, 0) + " ms", (int) plot.getX(), (int) plot.getBottom() - 12, 200, 12, juce::Justification::left);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (22, 14);
        r.removeFromTop (40);
        auto top = r.removeFromTop (150);
        auto side = top.removeFromRight (120);
        display = top.withTrimmedRight (12);
        play.setBounds (side.removeFromTop (70).reduced (0, 4));
        side.removeFromTop (8);
        keyTrack.setBounds (side.removeFromTop (34));
        r.removeFromTop (14);
        auto row1 = r.removeFromTop (r.getHeight() / 2);
        const int w1 = row1.getWidth() / 5;
        for (int i = 0; i < 5; ++i) knobs[i]->setBounds (row1.removeFromLeft (w1).reduced (6, 0));
        const int w2 = r.getWidth() / 5;
        r.removeFromLeft (w2 / 2);
        for (int i = 5; i < 9; ++i) knobs[i]->setBounds (r.removeFromLeft (w2).reduced (6, 0));
    }

private:
    KickSynthFx& fx;
    juce::OwnedArray<Knob> knobs;
    juce::TextButton play, keyTrack;
    std::unique_ptr<APVTS::ButtonAttachment> keyAttach;
    juce::Rectangle<int> display;
    std::vector<float> preview;
    std::atomic<bool> dirty { false };
    double previewSeconds = 0.6;

    void parameterChanged (const juce::String&, float) override { dirty = true; }
    void refreshPreview()
    {
        const auto p = fx.readParams();
        previewSeconds = juce::jlimit (0.25, 2.2, p.decay / 1000.0 * 1.2);
        preview = KickSynthFx::renderPreview (p, 12000.0, previewSeconds);
    }
    void timerCallback() override
    {
        if (dirty.exchange (false)) { refreshPreview(); repaint(); }
    }
};

juce::AudioProcessorEditor* KickSynthFx::createEditor() { return new KickEditor (*this); }

// =============================================================================
//  VOCODER (instrument): the track's MIDI plays a carrier synth; another
//  track's audio (usually a vocal) shapes it through 20 bands.
// =============================================================================
class VocoderFx : public BuiltinFx, public SidechainUser
{
public:
    using BuiltinFx::processBlock;
    static constexpr int numBands = 20;

    VocoderFx() : BuiltinFx ("vocoder", "Vocoder", layout()) { for (auto& b : bandLevel) b = -100.0f; }

    bool acceptsMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 0.5; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (floatParam ("tone", "Saw / Pulse", Range (0, 100), 25, "%"));
        l.add (floatParam ("ensemble", "Ensemble", Range (0, 100), 35, "%"));
        l.add (floatParam ("noise", "Noise", Range (0, 100), 10, "%"));
        l.add (floatParam ("formant", "Formant", Range (-12, 12), 0, "st"));
        l.add (floatParam ("attack", "Attack", skewed (0.5f, 100, 8), 4, "ms"));
        l.add (floatParam ("release", "Release", skewed (10, 1000, 100), 70, "ms"));
        l.add (floatParam ("sibilance", "Sibilance", Range (0, 100), 35, "%"));
        l.add (floatParam ("level", "Level", Range (-24, 12), 0, "dB"));
        return l;
    }

    void getStateInformation (juce::MemoryBlock& mb) override
    {
        state.state.setProperty ("sidechain", sidechainTrackId.load(), nullptr);
        BuiltinFx::getStateInformation (mb);
    }
    void setStateInformation (const void* d, int size) override
    {
        BuiltinFx::setStateInformation (d, size);
        sidechainTrackId = (int) state.state.getProperty ("sidechain", 0);
    }

    static double bandFreq (int i) { return 120.0 * std::pow (8000.0 / 120.0, (double) i / (numBands - 1)); }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        const double ratio = std::pow (8000.0 / 120.0, 1.0 / (numBands - 1));
        bandQ = std::sqrt (ratio) / (ratio - 1.0) * 1.2;
        for (int i = 0; i < numBands; ++i)
        {
            auto& b = bands[(size_t) i];
            b.m1.reset(); b.m2.reset(); b.c1.reset(); b.c2.reset(); b.env = 0;
            b.m1.c = b.m2.c = BiquadCoefs::make (BiquadCoefs::Type::BandPass, sr, bandFreq (i), bandQ, 0);
        }
        hp1.reset(); hp2.reset();
        hp1.c = hp2.c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, 6000, 0.707, 0);
        for (auto& v : voices) v = {};
        lastFormant = 1000.0f;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        const int chans = juce::jmin (2, buffer.getNumChannels());
        buffer.clear();
        if (chans == 0) return;
        const double sr = currentSampleRate;

        const float formant = param ("formant");
        if (! juce::approximatelyEqual (formant, lastFormant))
        {
            const double shift = std::pow (2.0, formant / 12.0);
            for (int i = 0; i < numBands; ++i)
                bands[(size_t) i].c1.c = bands[(size_t) i].c2.c = BiquadCoefs::make (BiquadCoefs::Type::BandPass, sr, juce::jmin (sr * 0.45, bandFreq (i) * shift), bandQ, 0);
            lastFormant = formant;
        }
        const float tone = param ("tone") / 100.0f, noise = param ("noise") / 100.0f, ens = param ("ensemble") / 100.0f;
        const float att = coefFor (param ("attack"), sr), rel = coefFor (param ("release"), sr);
        const float sib = param ("sibilance") / 100.0f;
        const float gain = juce::Decibels::decibelsToGain (param ("level")) * 18.0f;
        const float detune = std::pow (2.0f, ens * 0.12f / 12.0f);   // up to 12 cents
        const float voiceAtt = coefFor (3, sr), voiceRel = coefFor (150, sr);
        const auto* sc = sidechain;

        auto it = midi.cbegin();
        for (int i = 0; i < n; ++i)
        {
            while (it != midi.cend() && (*it).samplePosition <= i)
            {
                handleMidi ((*it).getMessage());
                ++it;
            }

            // carrier: up to 8 notes of saw/pulse, two detuned oscillators each
            float carrier = 0, activity = 0;
            for (auto& v : voices)
            {
                v.env += (v.gate ? voiceAtt : voiceRel) * ((v.gate ? 1.0f : 0.0f) - v.env);
                if (v.env < 1.0e-4f && ! v.gate) { v.note = -1; continue; }
                const double f = 440.0 * std::pow (2.0, (v.note - 69) / 12.0);
                const float o1 = osc (v.p1, f / sr, tone), o2 = osc (v.p2, f * detune / sr, tone);
                carrier += (o1 + ens * o2) / (1.0f + ens) * v.env * v.vel;
                activity += v.env;
            }
            carrier += noise * (rnd.nextFloat() * 2.0f - 1.0f) * juce::jmin (1.0f, activity) * 0.6f;

            const float mod = sc != nullptr ? 0.5f * (sc->getSample (0, i) + sc->getSample (1, i)) : 0.0f;

            float outL = 0, outR = 0;
            for (int b = 0; b < numBands; ++b)
            {
                auto& bd = bands[(size_t) b];
                const float mb = bd.m2.process (bd.m1.process (mod, 0), 0);
                bd.env += (std::abs (mb) > bd.env ? att : rel) * (std::abs (mb) - bd.env);
                const float cb = bd.c2.process (bd.c1.process (carrier, 0), 0) * bd.env;
                // alternate bands lean left / right for width
                if (b % 2 == 0) { outL += cb * 1.15f; outR += cb * 0.85f; } else { outL += cb * 0.85f; outR += cb * 1.15f; }
            }
            const float hiss = sib * hp2.process (hp1.process (mod, 0), 0) * juce::jmin (1.0f, activity);
            outL = outL * gain + hiss; outR = outR * gain + hiss;
            buffer.setSample (0, i, outL);
            if (chans > 1) buffer.setSample (1, i, outR);
        }
        for (int b = 0; b < numBands; ++b) bandLevel[(size_t) b] = dbOf (bands[(size_t) b].env * 2.0f);
    }

    std::array<std::atomic<float>, numBands> bandLevel;
    juce::AudioProcessorEditor* createEditor() override;

private:
    struct Band { Biquad m1, m2, c1, c2; float env = 0; };
    std::array<Band, numBands> bands;
    Biquad hp1, hp2;
    double bandQ = 4.0;
    float lastFormant = 1000.0f;
    juce::Random rnd;

    struct Voice { int note = -1; bool gate = false; float env = 0, vel = 0; double p1 = 0, p2 = 0.37; };
    std::array<Voice, 8> voices;

    static float polyBlep (double t, double dt)
    {
        if (t < dt) { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }
    static float osc (double& phase, double dt, float pulseMix)
    {
        phase += dt;
        if (phase >= 1.0) phase -= 1.0;
        float saw = (float) (2.0 * phase - 1.0) - polyBlep (phase, dt);
        double p2 = phase + 0.5; if (p2 >= 1.0) p2 -= 1.0;
        float sq = (phase < 0.5 ? 1.0f : -1.0f) + polyBlep (phase, dt) - polyBlep (p2, dt);
        return saw + pulseMix * (sq - saw);
    }

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())
        {
            Voice* target = nullptr;
            for (auto& v : voices) if (v.note == m.getNoteNumber()) target = &v;
            if (target == nullptr) for (auto& v : voices) if (v.note < 0) { target = &v; break; }
            if (target == nullptr)
            {
                target = &voices[0];
                for (auto& v : voices) if (v.env < target->env) target = &v;
            }
            target->note = m.getNoteNumber(); target->gate = true; target->vel = m.getFloatVelocity();
        }
        else if (m.isNoteOff())
        {
            for (auto& v : voices) if (v.note == m.getNoteNumber()) v.gate = false;
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices) { v.gate = false; if (m.isAllSoundOff()) { v.env = 0; v.note = -1; } }
        }
    }
};

class VocoderEditor : public FaceEditor
{
public:
    explicit VocoderEditor (VocoderFx& p)
        : FaceEditor (p, { KnobStyle::Vintage, juce::Colour (0xff101113), juce::Colour (0xffffb13b), juce::Colour (0xffffb13b), juce::Colour (0xff3a3d42),
                           juce::Colour (0xffe9e1cf), juce::Colour (0xff8e8a80), juce::Colour (0xff0d0e10), juce::Colour (0xffffc768) }),
          fx (p)
    {
        for (auto [id, name] : { std::pair { "tone", "SAW / PULSE" }, { "ensemble", "ENSEMBLE" }, { "noise", "NOISE" }, { "formant", "FORMANT" },
                                 { "attack", "ATTACK" }, { "release", "RELEASE" }, { "sibilance", "SIBILANCE" }, { "level", "LEVEL" } })
        {
            auto* k = knobs.add (new Knob (p, id, name));
            k->label.setColour (juce::Label::textColourId, look.s.text);
            addAndMakeVisible (k);
        }
        source.setTooltip ("The track whose sound (usually a vocal) shapes the chords you play on this track");
        source.onChange = [this] { fx.sidechainTrackId = source.getSelectedId() > 1 ? source.getSelectedId() - 1 : 0; };
        addAndMakeVisible (source);
        refreshSources();
        setSize (780, 430);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        // wooden cheeks, dark brushed face
        auto drawWood = [&] (juce::Rectangle<int> w)
        {
            juce::ColourGradient wood (juce::Colour (0xff6b3f22), (float) w.getX(), 0, juce::Colour (0xff3a2112), (float) w.getRight(), 0, false);
            g.setGradientFill (wood);
            g.fillRect (w);
            juce::Random rnd (w.getX() + 3);
            for (int i = 0; i < 40; ++i)
            {
                g.setColour (juce::Colour (0xff2a170c).withAlpha (0.25f + rnd.nextFloat() * 0.3f));
                const float x = (float) w.getX() + rnd.nextFloat() * (float) w.getWidth();
                g.drawLine (x, 0, x + rnd.nextFloat() * 3 - 1.5f, (float) w.getHeight(), 0.6f + rnd.nextFloat());
            }
        };
        drawWood (r.removeFromLeft (22));
        drawWood (r.removeFromRight (22));
        juce::ColourGradient face (juce::Colour (0xff2b2e33), 0, 0, juce::Colour (0xff17191c), 0, (float) r.getHeight(), false);
        g.setGradientFill (face);
        g.fillRect (r);
        g.setColour (juce::Colours::white.withAlpha (0.025f));
        for (int y = 0; y < r.getHeight(); y += 2) g.drawHorizontalLine (y, (float) r.getX(), (float) r.getRight());

        g.setColour (look.s.pointer);
        g.setFont (theme::font (20.0f, true));
        g.drawText ("RENASOUND  VOCODER", r.getX() + 20, 14, 320, 28, juce::Justification::centredLeft);
        g.setColour (look.s.textDim);
        g.setFont (theme::font (10.5f, true));
        g.drawText ("20 BAND  /  8 VOICE", r.getX() + 300, 22, 200, 14, juce::Justification::centredLeft);
        g.drawText ("MODULATOR", source.getBounds().translated (0, -14).withHeight (12), juce::Justification::left);

        // band meters: amber LED columns
        auto m = meters.toFloat();
        g.setColour (juce::Colours::black.withAlpha (0.55f));
        g.fillRoundedRectangle (m, 5.0f);
        auto inner = m.reduced (12, 10);
        const float colW = inner.getWidth() / VocoderFx::numBands;
        const int segs = 14;
        const float segH = (inner.getHeight() - 14) / segs;
        for (int b = 0; b < VocoderFx::numBands; ++b)
        {
            const float lvl = fx.bandLevel[(size_t) b].load();
            const int lit = juce::jlimit (0, segs, (int) std::round ((lvl + 60.0f) / 60.0f * segs));
            for (int sgi = 0; sgi < segs; ++sgi)
            {
                const bool on = sgi < lit;
                const auto col = sgi >= segs - 2 ? juce::Colour (0xffff5a36) : look.s.pointer;
                g.setColour (on ? col : col.withAlpha (0.08f));
                g.fillRoundedRectangle (inner.getX() + b * colW + 2, inner.getY() + (segs - 1 - sgi) * segH, colW - 4, segH - 2, 1.5f);
            }
            if (b % 3 == 0 || b == VocoderFx::numBands - 1)
            {
                const double f = VocoderFx::bandFreq (b);
                g.setColour (look.s.textDim);
                g.setFont (theme::font (9.0f, true));
                g.drawText (f >= 1000 ? juce::String (f / 1000.0, 1) + "k" : juce::String ((int) f),
                            juce::Rectangle<float> (inner.getX() + b * colW - 6, inner.getBottom() - 11, colW + 12, 11), juce::Justification::centred);
            }
        }
        if (fx.sidechainTrackId.load() == 0)
        {
            g.setColour (look.s.text.withAlpha (0.85f));
            g.setFont (theme::font (13.0f, true));
            g.drawText ("Pick a modulator track (your vocal), then play chords on this track", m, juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (22, 0).reduced (20, 14);
        r.removeFromTop (40);
        auto row = r.removeFromTop (150);
        source.setBounds (row.removeFromRight (200).withTrimmedTop (60).withHeight (30));
        row.removeFromRight (16);
        meters = row;
        r.removeFromTop (16);
        const int w = r.getWidth() / knobs.size();
        for (auto* k : knobs) k->setBounds (r.removeFromLeft (w).reduced (3, 0));
    }

private:
    VocoderFx& fx;
    juce::OwnedArray<Knob> knobs;
    juce::ComboBox source;
    juce::Rectangle<int> meters;
    int refreshCounter = 0;

    void refreshSources()
    {
        juce::Array<std::pair<int, juce::String>> tracks;
        if (SidechainUser::listTracks) tracks = SidechainUser::listTracks();
        juce::StringArray names;
        tracks.removeIf ([this] (const std::pair<int, juce::String>& t) { return t.first == fx.ownerTrackId.load(); });
        for (auto& t : tracks) names.add (t.second);
        if (names == lastNames && source.getNumItems() > 0)
        {
            if (! source.isPopupActive()) source.setSelectedId (fx.sidechainTrackId.load() + 1, juce::dontSendNotification);
            return;
        }
        lastNames = names;
        source.clear (juce::dontSendNotification);
        source.addItem ("None", 1);
        for (auto& t : tracks) source.addItem (t.second, t.first + 1);
        source.setSelectedId (fx.sidechainTrackId.load() + 1, juce::dontSendNotification);
    }
    juce::StringArray lastNames;

    void timerCallback() override
    {
        if (++refreshCounter % 30 == 0) refreshSources();
        repaint (meters.expanded (2));
    }
};

juce::AudioProcessorEditor* VocoderFx::createEditor() { return new VocoderEditor (*this); }

// =============================================================================
//  SAMPLER (instrument): plays any sound across the keyboard
// =============================================================================
class SamplerFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    struct Sample { juce::AudioBuffer<float> buffer; double rate = 48000.0; juce::String name; juce::File file; };
    using SamplePtr = std::shared_ptr<const Sample>;

    SamplerFx() : BuiltinFx ("sampler", "Sampler", layout())
    {
        formats.registerBasicFormats();
        current = makeDefaultSound();
        for (auto& v : voicePos) v = -1.0f;
    }

    bool acceptsMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 3.0; }

    static juce::String noteName (int n) { return juce::MidiMessage::getMidiNoteName (n, true, true, 4); }   // C4 = middle C, as in the piano roll

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "root", 1 }, "Root Note", 0, 127, 60,
                   juce::AudioParameterIntAttributes().withStringFromValueFunction ([] (int v, int) { return noteName (v); })));
        l.add (choiceParam ("mode", "Play Mode", { "Keys", "One-shot", "Drum" }, 0));
        l.add (floatParam ("tune", "Tune", Range (-24, 24, 1), 0, "st"));
        l.add (floatParam ("fine", "Fine", Range (-100, 100), 0, "ct"));
        l.add (floatParam ("start", "Start", Range (0, 100), 0, "%"));
        l.add (floatParam ("end", "End", Range (0, 100), 100, "%"));
        l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { "loop", 1 }, "Loop", false));
        l.add (floatParam ("attack", "Attack", skewed (0.5f, 3000, 40), 2, "ms"));
        l.add (floatParam ("decay", "Decay", skewed (5, 5000, 300), 400, "ms"));
        l.add (floatParam ("sustain", "Sustain", Range (0, 100), 80, "%"));
        l.add (floatParam ("release", "Release", skewed (5, 8000, 400), 350, "ms"));
        l.add (floatParam ("cutoff", "Cutoff", skewed (60, 20000, 1500), 20000, "Hz"));
        l.add (floatParam ("reso", "Resonance", Range (0, 100), 10, "%"));
        l.add (floatParam ("velocity", "Velocity", Range (0, 100), 70, "%"));
        l.add (floatParam ("level", "Level", Range (-24, 6), -3, "dB"));
        return l;
    }

    // ---- the sound ----
    bool loadSample (const juce::File& f)
    {
        std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
        if (r == nullptr || r->lengthInSamples <= 0) return false;
        auto smp = std::make_shared<Sample>();
        const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 60.0));
        smp->buffer.setSize (2, len);
        r->read (&smp->buffer, 0, len, 0, true, true);
        if (r->numChannels == 1) smp->buffer.copyFrom (1, 0, smp->buffer, 0, 0, len);
        smp->rate = r->sampleRate;
        smp->name = f.getFileNameWithoutExtension();
        smp->file = f;
        // a sound named after a note ("Piano C3", "pad_A#2") sets the root note
        const auto n = f.getFileNameWithoutExtension();
        for (int note = 127; note >= 0; --note)
            if (n.containsWholeWordIgnoreCase (noteName (note)) || n.endsWithIgnoreCase ("_" + noteName (note)))
            {
                if (auto* p = state.getParameter ("root")) p->setValueNotifyingHost (p->convertTo0to1 ((float) note));
                break;
            }
        setSample (smp);
        return true;
    }

    SamplePtr getSample() const { const juce::SpinLock::ScopedLockType sl (sampleLock); return current; }
    void resetToDefault() { setSample (makeDefaultSound()); }

    void getStateInformation (juce::MemoryBlock& mb) override
    {
        auto smp = getSample();
        state.state.setProperty ("sample", smp != nullptr ? smp->file.getFullPathName() : juce::String(), nullptr);
        BuiltinFx::getStateInformation (mb);
    }
    void setStateInformation (const void* d, int size) override
    {
        BuiltinFx::setStateInformation (d, size);
        const auto path = state.state.getProperty ("sample").toString();
        if (path.isNotEmpty() && juce::File (path).existsAsFile())
        {
            const auto rootBefore = param ("root");
            loadSample (juce::File (path));
            if (auto* p = state.getParameter ("root")) p->setValueNotifyingHost (p->convertTo0to1 (rootBefore));   // keep the saved root
        }
    }

    void prepareToPlay (double sr, int) override { currentSampleRate = sr; for (auto& v : voices) v.active = false; }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (buffer.getNumChannels() < 2) return;
        SamplePtr smp;
        {
            const juce::SpinLock::ScopedTryLockType sl (sampleLock);
            if (sl.isLocked()) smp = current;
        }
        if (smp == nullptr) return;

        const double sr = currentSampleRate;
        const float att = coefFor (param ("attack"), sr), dec = coefFor (param ("decay"), sr), rel = coefFor (param ("release"), sr);
        const float sus = param ("sustain") / 100.0f;
        const int mode = (int) std::lround (param ("mode"));
        const float gain = juce::Decibels::decibelsToGain (param ("level"));
        const float velAmt = param ("velocity") / 100.0f;
        const int len = smp->buffer.getNumSamples();
        const double startPos = param ("start") / 100.0 * len;
        const double endPos = juce::jmax (startPos + 32.0, param ("end") / 100.0 * len);
        const bool loop = param ("loop") > 0.5f && mode == 0;
        // filter (Chamberlin state variable, per voice)
        const float cutoff = juce::jmin (param ("cutoff"), (float) (sr * 0.45));
        const float fc = 2.0f * std::sin (juce::MathConstants<float>::pi * cutoff / (float) sr);
        const float q = 1.0f - param ("reso") / 100.0f * 0.92f;
        const bool filterOn = cutoff < 19000.0f;

        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getWritePointer (1);
        const auto* sL = smp->buffer.getReadPointer (0);
        const auto* sR = smp->buffer.getReadPointer (1);

        auto it = midi.cbegin();
        for (int i = 0; i < n; ++i)
        {
            while (it != midi.cend() && (*it).samplePosition <= i)
            {
                const auto m = (*it).getMessage();
                if (m.isNoteOn()) startVoice (m.getNoteNumber(), m.getFloatVelocity(), startPos, smp->rate / sr, mode);
                else if (m.isNoteOff() && mode == 0) { for (auto& v : voices) if (v.active && v.note == m.getNoteNumber()) v.releasing = true; }
                else if (m.isAllNotesOff() || m.isAllSoundOff()) { for (auto& v : voices) { v.releasing = true; if (m.isAllSoundOff()) v.active = false; } }
                ++it;
            }
            float outL = 0, outR = 0;
            for (auto& v : voices)
            {
                if (! v.active) continue;
                // envelope: one-shot and drum ignore note-off and play to the end
                if (v.releasing) v.env += rel * (0.0f - v.env);
                else if (v.attacking) { v.env += att * (1.05f - v.env); if (v.env >= 1.0f) { v.env = 1.0f; v.attacking = false; } }
                else v.env += dec * ((mode == 0 ? sus : 0.0f) - v.env) * (mode == 0 ? 1.0f : 0.0f);
                if (v.releasing && v.env < 1.0e-4f) { v.active = false; continue; }

                if (v.pos >= endPos - 1)
                {
                    if (loop) v.pos = startPos + std::fmod (v.pos - startPos, endPos - startPos);
                    else { v.active = false; continue; }
                }
                const int p0 = (int) v.pos;
                const float fr = (float) (v.pos - p0);
                const int p1 = juce::jmin (p0 + 1, len - 1);
                float a = sL[p0] + fr * (sL[p1] - sL[p0]);
                float b = sR[p0] + fr * (sR[p1] - sR[p0]);
                // short fade at the end of one-shots to avoid a click
                const double left = endPos - v.pos;
                if (! loop && left < 64.0) { a *= (float) (left / 64.0); b *= (float) (left / 64.0); }
                v.pos += v.step;
                if (filterOn)
                {
                    v.lowL += fc * v.bandL; const float hiL = a - v.lowL - q * v.bandL; v.bandL += fc * hiL; a = v.lowL;
                    v.lowR += fc * v.bandR; const float hiR = b - v.lowR - q * v.bandR; v.bandR += fc * hiR; b = v.lowR;
                }
                const float g = v.env * (1.0f - velAmt + velAmt * v.vel);
                outL += a * g; outR += b * g;
            }
            L[i] = outL * gain;
            R[i] = outR * gain;
        }
        playing = std::any_of (voices.begin(), voices.end(), [] (const Voice& v) { return v.active; });
        for (size_t k = 0; k < voices.size(); ++k) voicePos[k] = voices[k].active ? (float) (voices[k].pos / juce::jmax (1, len)) : -1.0f;
    }

    std::atomic<bool> playing { false };
    std::array<std::atomic<float>, 16> voicePos;   // playback positions for the waveform view
    std::atomic<int> auditionNote { -1 };

    juce::AudioProcessorEditor* createEditor() override;

private:
    struct Voice
    {
        bool active = false, releasing = false, attacking = true;
        int note = 60;
        double pos = 0, step = 1;
        float env = 0, vel = 1, lowL = 0, bandL = 0, lowR = 0, bandR = 0;
    };
    std::array<Voice, 16> voices;
    SamplePtr current;
    juce::SpinLock sampleLock;
    juce::AudioFormatManager formats;

    void setSample (SamplePtr smp)
    {
        const juce::SpinLock::ScopedLockType sl (sampleLock);
        current = std::move (smp);
        for (auto& v : voices) v.active = false;
    }

    void startVoice (int note, float vel, double startPos, double rateRatio, int mode)
    {
        Voice* v = nullptr;
        if (mode != 0) for (auto& x : voices) if (x.active && x.note == note) { v = &x; break; }   // drums / one-shots retrigger
        if (v == nullptr) for (auto& x : voices) if (! x.active) { v = &x; break; }
        if (v == nullptr) { v = &voices[0]; for (auto& x : voices) if (x.env < v->env) v = &x; }
        const int root = (int) std::lround (param ("root"));
        const double semis = mode == 2 ? param ("tune") + param ("fine") / 100.0 : (note - root) + param ("tune") + param ("fine") / 100.0;
        *v = Voice();
        v->active = true; v->note = note; v->vel = vel; v->pos = startPos;
        v->step = rateRatio * std::pow (2.0, semis / 12.0);
    }

    // a soft electric-piano-like tone, so the Sampler plays something straight away
    static SamplePtr makeDefaultSound()
    {
        auto smp = std::make_shared<Sample>();
        const double sr = 48000.0, f0 = 261.6256;   // C4
        const int len = (int) (sr * 2.5);
        smp->buffer.setSize (2, len);
        smp->rate = sr;
        smp->name = "Init: Glass Keys";
        for (int i = 0; i < len; ++i)
        {
            const double t = i / sr;
            const double env = std::exp (-t * 1.6) * (1.0 - std::exp (-t * 900.0));
            const double modIdx = 1.8 * std::exp (-t * 6.0);
            double x = std::sin (juce::MathConstants<double>::twoPi * f0 * t + modIdx * std::sin (juce::MathConstants<double>::twoPi * f0 * 2.0 * t));
            x += 0.18 * std::sin (juce::MathConstants<double>::twoPi * f0 * 3.0 * t) * std::exp (-t * 4.0);
            x += 0.08 * std::sin (juce::MathConstants<double>::twoPi * f0 * 7.02 * t) * std::exp (-t * 9.0);
            const float y = (float) (x * env * 0.55);
            smp->buffer.setSample (0, i, y);
            smp->buffer.setSample (1, i, (float) (y * 0.96 + 0.04 * std::sin (juce::MathConstants<double>::twoPi * f0 * 1.003 * t) * env * 0.55));
        }
        return smp;
    }
};

class SamplerEditor : public FaceEditor, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget
{
public:
    explicit SamplerEditor (SamplerFx& p)
        : FaceEditor (p, { KnobStyle::Glossy, juce::Colour (0xff2b2a27), juce::Colour (0xff9dffcf), juce::Colour (0xff3fe0a0), juce::Colour (0xff2c2b28),
                           juce::Colour (0xffe8e4da), juce::Colour (0xff8d887c), juce::Colour (0xff161614), juce::Colour (0xffd9ffe9) }),
          fx (p), mode (p, "mode")
    {
        for (auto [id, name] : { std::pair { "root", "ROOT" }, { "tune", "TUNE" }, { "fine", "FINE" }, { "attack", "ATTACK" }, { "decay", "DECAY" },
                                 { "sustain", "SUSTAIN" }, { "release", "RELEASE" }, { "cutoff", "CUTOFF" }, { "reso", "RESO" }, { "velocity", "VELOCITY" }, { "level", "LEVEL" } })
        {
            auto* k = knobs.add (new Knob (p, id, name, 58));
            k->label.setColour (juce::Label::textColourId, look.s.textDim);
            addAndMakeVisible (k);
        }
        for (auto* b : { &loadButton, &defaultButton, &loopButton })
        {
            addAndMakeVisible (*b);
            b->setColour (juce::TextButton::textColourOffId, look.s.text);
        }
        loopButton.setClickingTogglesState (true);
        loopAttach = std::make_unique<APVTS::ButtonAttachment> (p.state, "loop", loopButton);
        loadButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Load a sound into the Sampler", juce::File::getSpecialLocation (juce::File::userMusicDirectory), "*.wav;*.aif;*.aiff;*.flac;*.mp3;*.ogg");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) load (fc.getResult()); });
        };
        defaultButton.onClick = [this] { fx.resetToDefault(); repaint(); };
        defaultButton.setTooltip ("Back to the built-in Glass Keys sound");
        addAndMakeVisible (mode);
        setSize (780, 440);
        startTimerHz (30);
    }

    // drop a file from Finder / Explorer, or a sound from RENASOUND's Library
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { if (! files.isEmpty()) load (juce::File (files[0])); }
    bool isInterestedInDragSource (const SourceDetails&) override { return true; }
    void itemDropped (const SourceDetails& d) override
    {
        const auto first = d.description.toString().upToFirstOccurrenceOf ("\n", false, false);
        if (juce::File::isAbsolutePath (first)) load (juce::File (first));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        juce::ColourGradient bg (juce::Colour (0xff353430), 0, 0, juce::Colour (0xff1f1e1c), 0, r.getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();
        // brushed lines and rack ears
        g.setColour (juce::Colours::white.withAlpha (0.02f));
        for (int y = 0; y < getHeight(); y += 3) g.drawHorizontalLine (y, 0, r.getWidth());
        for (float x : { 10.0f, r.getWidth() - 22.0f })
            for (float y : { 18.0f, r.getHeight() - 30.0f })
            {
                g.setColour (juce::Colour (0xff141412)); g.fillEllipse (x, y, 12, 12);
                g.setColour (juce::Colour (0xff4a4844)); g.drawEllipse (x, y, 12, 12, 1.0f);
            }

        g.setColour (look.s.text);
        g.setFont (theme::font (21.0f, true));
        g.drawText ("SAMPLER", 40, 12, 200, 30, juce::Justification::centredLeft);
        g.setColour (look.s.arc);
        g.fillEllipse (150, 24, 7, 7);
        g.setColour (look.s.textDim);
        g.setFont (theme::font (11.0f, true));
        g.drawText ("16 VOICE  /  DROP ANY SOUND", 166, 19, 240, 16, juce::Justification::centredLeft);

        // the LCD with the waveform
        auto lcd = display.toFloat();
        g.setColour (juce::Colour (0xff0c1a14));
        g.fillRoundedRectangle (lcd, 6.0f);
        g.setColour (juce::Colour (0xff1d3a2c));
        g.drawRoundedRectangle (lcd, 6.0f, 1.5f);
        auto plot = lcd.reduced (12, 22);
        auto smp = fx.getSample();
        if (smp != nullptr)
        {
            const auto& b = smp->buffer;
            const int len = b.getNumSamples(), cols = (int) plot.getWidth();
            const float s0 = fx.param ("start") / 100.0f, s1 = fx.param ("end") / 100.0f;
            for (int x = 0; x < cols; ++x)
            {
                const int a = (int) ((double) x / cols * len), e = juce::jmax (a + 1, (int) ((double) (x + 1) / cols * len));
                const auto mm = b.findMinMax (0, a, juce::jmin (e, len) - a);
                const float t = (float) x / (float) cols;
                const bool inside = t >= s0 && t <= s1;
                g.setColour (look.s.arc.withAlpha (inside ? 0.9f : 0.25f));
                g.drawVerticalLine ((int) plot.getX() + x, plot.getCentreY() - mm.getEnd() * plot.getHeight() * 0.5f,
                                    plot.getCentreY() - mm.getStart() * plot.getHeight() * 0.5f + 1.0f);
            }
            for (auto [t, label] : { std::pair { s0, "S" }, { s1, "E" } })
            {
                const float x = plot.getX() + t * plot.getWidth();
                g.setColour (look.s.pointer);
                g.drawVerticalLine ((int) x, plot.getY() - 6, plot.getBottom() + 6);
                g.setFont (theme::font (10.0f, true));
                g.drawText (label, juce::Rectangle<float> (x - 6, lcd.getY() + 4, 12, 12), juce::Justification::centred);
            }
            for (auto& vp : fx.voicePos)
                if (const float v = vp.load(); v >= 0)
                {
                    g.setColour (juce::Colours::white.withAlpha (0.8f));
                    g.drawVerticalLine ((int) (plot.getX() + v * plot.getWidth()), plot.getY(), plot.getBottom());
                }
            g.setColour (look.s.pointer);
            g.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::bold)));
            g.drawText (smp->name + "   " + juce::String (len / smp->rate, 2) + " s   root " + SamplerFx::noteName ((int) std::lround (fx.param ("root"))),
                        lcd.reduced (12, 4).removeFromBottom (16), juce::Justification::centredLeft);
        }
        if (dragOver)
        {
            g.setColour (look.s.arc.withAlpha (0.15f));
            g.fillRoundedRectangle (lcd, 6.0f);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (36, 12);
        r.removeFromTop (36);
        auto top = r.removeFromTop (150);
        auto side = top.removeFromRight (150);
        display = top.withTrimmedRight (12);
        loadButton.setBounds (side.removeFromTop (34));
        side.removeFromTop (6);
        defaultButton.setBounds (side.removeFromTop (28));
        side.removeFromTop (6);
        loopButton.setBounds (side.removeFromTop (28));
        r.removeFromTop (12);
        mode.setBounds (r.removeFromTop (28).removeFromLeft (300));
        r.removeFromTop (10);
        auto row1 = r.removeFromTop (r.getHeight() / 2);
        const int w = r.getWidth() / 6;
        for (int i = 0; i < 6; ++i) knobs[i]->setBounds (row1.removeFromLeft (w).reduced (3, 0));
        r.removeFromLeft (w / 2);
        for (int i = 6; i < knobs.size(); ++i) knobs[i]->setBounds (r.removeFromLeft (w).reduced (3, 0));
    }

    // drag the S / E markers
    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! display.contains (e.getPosition())) return;
        const float t = juce::jlimit (0.0f, 1.0f, (float) (e.x - display.getX() - 12) / (float) (display.getWidth() - 24));
        dragId = std::abs (t - fx.param ("start") / 100.0f) < std::abs (t - fx.param ("end") / 100.0f) ? "start" : "end";
        if (auto* p = fx.state.getParameter (dragId)) p->beginChangeGesture();
        mouseDrag (e);
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragId.isEmpty()) return;
        const float t = juce::jlimit (0.0f, 1.0f, (float) (e.x - display.getX() - 12) / (float) (display.getWidth() - 24));
        if (auto* p = fx.state.getParameter (dragId)) p->setValueNotifyingHost (p->convertTo0to1 (t * 100.0f));
        repaint();
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (auto* p = fx.state.getParameter (dragId)) p->endChangeGesture();
        dragId.clear();
    }
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }

private:
    SamplerFx& fx;
    Switch mode;
    juce::OwnedArray<Knob> knobs;
    juce::TextButton loadButton { "Load sound..." }, defaultButton { "Glass Keys" }, loopButton { "Loop" };
    std::unique_ptr<APVTS::ButtonAttachment> loopAttach;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> display;
    juce::String dragId;
    bool dragOver = false;

    void load (const juce::File& f)
    {
        dragOver = false;
        if (! fx.loadSample (f))
            juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Couldn't load that sound", f.getFileName() + " isn't an audio file RENASOUND can read.");
        repaint();
    }
};

juce::AudioProcessorEditor* SamplerFx::createEditor() { return new SamplerEditor (*this); }

// =============================================================================
const std::vector<BuiltinFxInfo>& getBuiltinInstrumentList()
{
    static const std::vector<BuiltinFxInfo> list {
        { "kick",    "Kick Synth", "Synthesised kicks: big room, 808, techno" },
        { "vocoder", "Vocoder",    "Play chords through your vocal (pick the vocal track as modulator)" },
        { "sampler", "Sampler",    "Play any sound across the keyboard: drop a file or a Library sound on it" } };
    return list;
}

std::unique_ptr<BuiltinFx> createWidener()   { return std::make_unique<WidenerFx>(); }
std::unique_ptr<BuiltinFx> createAmp()       { return std::make_unique<AmpFx>(); }
std::unique_ptr<BuiltinFx> createPunch()     { return std::make_unique<PunchFx>(); }
std::unique_ptr<BuiltinFx> createKickSynth() { return std::make_unique<KickSynthFx>(); }
std::unique_ptr<BuiltinFx> createVocoder()   { return std::make_unique<VocoderFx>(); }
std::unique_ptr<BuiltinFx> createSampler()   { return std::make_unique<SamplerFx>(); }

} // namespace rs
