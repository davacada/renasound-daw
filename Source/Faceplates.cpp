// =============================================================================
//  Faceplates for RENASOUND's built-in effects. Every effect has its own look
//  and a picture of what it's doing:
//    EQ          glass panel, draggable band points on the curve
//    Tube EQ     enamel panel, a glowing tube, the passive curve on a small CRT
//    Compressor  VU needle, the dynamics (transfer) curve with the live level
//    Reverb      the room: size, surfaces (damping), wavefronts bouncing
//    Delay       the echoes as taps in time, left / right for ping-pong
//    Noise Gate  the level against the threshold, open / closed
//    Gater       the 8-step pattern with a moving playhead
//    Phaser      the sweeping notches
//    Chorus      the voices drifting against each other
// =============================================================================
#include "UI/Faceplate.h"

namespace rs
{

using namespace facekit;

// =============================================================================
//  EQ: glass panel with draggable points
// =============================================================================
class EqFace : public EffectFace
{
public:
    explicit EqFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Minimal, juce::Colour (0xff1b2a40), juce::Colour (0xffe8f6ff), juce::Colour (0xff5ad1ff), juce::Colour (0xff22324a),
                           juce::Colour (0xffe8f6ff), juce::Colour (0xff7d93b0), juce::Colour (0xff0d1726), juce::Colour (0xffcfeeff) })
    {
        const auto lc = juce::Colour (0xff8fa8c8);
        for (auto [id, name] : { std::pair { "lowCut", "LOW CUT" }, { "lowFreq", "LOW FREQ" }, { "lowGain", "LOW" }, { "midFreq", "MID FREQ" }, { "midGain", "MID" },
                                 { "midQ", "MID Q" }, { "highFreq", "HIGH FREQ" }, { "highGain", "HIGH" }, { "highCut", "HIGH CUT" } })
            knob (id, name, lc, 60);
        setSize (880, 440);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff13223a), 0, 0, juce::Colour (0xff070d18), 0, (float) getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();
        title (g, { 22, 12, 600, 30 }, "EQ", "FIVE BAND  /  DRAG THE POINTS", juce::Colour (0xffe8f6ff), juce::Colour (0xff5ad1ff));

        auto a = graph.toFloat();
        g.setColour (juce::Colours::white.withAlpha (0.04f));
        g.fillRoundedRectangle (a, 10.0f);
        g.setColour (juce::Colour (0xff5ad1ff).withAlpha (0.25f));
        g.drawRoundedRectangle (a.reduced (0.5f), 10.0f, 1.0f);
        auto plot = a.reduced (14, 12);
        g.setFont (theme::font (10.0f, true));
        for (double fr : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = logX (fr, plot);
            g.setColour (juce::Colours::white.withAlpha (0.06f));
            g.drawVerticalLine ((int) x, plot.getY(), plot.getBottom());
            g.setColour (juce::Colour (0xff7d93b0));
            g.drawText (fr >= 1000 ? juce::String ((int) (fr / 1000)) + "k" : juce::String ((int) fr), (int) x + 3, (int) plot.getBottom() - 13, 30, 12, juce::Justification::left);
        }
        for (float db : { -12.0f, 0.0f, 12.0f })
        {
            g.setColour (juce::Colours::white.withAlpha (db == 0 ? 0.14f : 0.06f));
            g.drawHorizontalLine ((int) dbY (db, plot), plot.getX(), plot.getRight());
        }
        juce::Path c;
        for (int x = 0; x <= (int) plot.getWidth(); x += 2)
        {
            const double fr = xToLogF (plot.getX() + (float) x, plot);
            const float y = dbY (juce::jlimit (-24.0f, 24.0f, juce::Decibels::gainToDecibels ((float) fx.getMagnitudeForFrequency (fr), -60.0f)), plot);
            if (x == 0) c.startNewSubPath (plot.getX(), y); else c.lineTo (plot.getX() + (float) x, y);
        }
        juce::Path fill (c);
        fill.lineTo (plot.getRight(), dbY (0, plot)); fill.lineTo (plot.getX(), dbY (0, plot)); fill.closeSubPath();
        juce::ColourGradient fg (juce::Colour (0xff5ad1ff).withAlpha (0.35f), 0, plot.getY(), juce::Colour (0xff5ad1ff).withAlpha (0.02f), 0, plot.getBottom(), false);
        g.setGradientFill (fg);
        g.fillPath (fill);
        g.setColour (juce::Colour (0xff5ad1ff).withAlpha (0.3f));
        g.strokePath (c, juce::PathStrokeType (6.0f));
        g.setColour (juce::Colour (0xffbff0ff));
        g.strokePath (c, juce::PathStrokeType (2.0f));

        const juce::Colour nodeCols[] { juce::Colour (0xff9db4d6), juce::Colour (0xffff9f68), juce::Colour (0xfff3d36b), juce::Colour (0xff7ee0a2), juce::Colour (0xffc6a6ff) };
        const char* labels[] { "LC", "L", "M", "H", "HC" };
        for (int i = 0; i < 5; ++i)
        {
            const auto pt = node (i, plot);
            g.setColour (nodeCols[i].withAlpha (0.25f));
            g.fillEllipse (pt.x - 12, pt.y - 12, 24, 24);
            g.setColour (nodeCols[i]);
            g.fillEllipse (pt.x - 8, pt.y - 8, 16, 16);
            g.setColour (juce::Colour (0xff0d1726));
            g.setFont (theme::font (8.5f, true));
            g.drawText (labels[i], juce::Rectangle<float> (pt.x - 8, pt.y - 8, 16, 16), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (38);
        graph = r.removeFromTop (230);
        r.removeFromTop (16);
        row (r, 0, 9);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto plot = graph.toFloat().reduced (14, 12);
        dragging = -1;
        for (int i = 0; i < 5; ++i) if (node (i, plot).getDistanceFrom (e.position) < 14) dragging = i;
        if (dragging >= 0) for (auto* prm : params (dragging)) if (prm != nullptr) prm->beginChangeGesture();
    }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        auto plot = graph.toFloat().reduced (14, 12);
        auto ps = params (dragging);
        ps[0]->setValueNotifyingHost (ps[0]->convertTo0to1 ((float) xToLogF (e.position.x, plot)));
        if (ps[1] != nullptr) ps[1]->setValueNotifyingHost (ps[1]->convertTo0to1 (juce::jlimit (-18.0f, 18.0f, (dbY (0, plot) - e.position.y) / (plot.getHeight() * 0.5f - 4) * 24.0f)));
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) for (auto* prm : params (dragging)) if (prm != nullptr) prm->endChangeGesture();
        dragging = -1;
    }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        auto* q = fx.state.getParameter ("midQ");
        q->setValueNotifyingHost (juce::jlimit (0.0f, 1.0f, q->getValue() + w.deltaY * 0.15f));
    }

private:
    juce::Rectangle<int> graph;
    int dragging = -1;
    static float dbY (float db, juce::Rectangle<float> a) { return a.getCentreY() - db / 24.0f * (a.getHeight() * 0.5f - 4); }
    std::array<juce::RangedAudioParameter*, 2> params (int i)
    {
        const char* f[] { "lowCut", "lowFreq", "midFreq", "highFreq", "highCut" };
        const char* gn[] { nullptr, "lowGain", "midGain", "highGain", nullptr };
        return { fx.state.getParameter (f[i]), gn[i] != nullptr ? fx.state.getParameter (gn[i]) : nullptr };
    }
    juce::Point<float> node (int i, juce::Rectangle<float> plot) const
    {
        switch (i)
        {
            case 0:  return { logX (p ("lowCut"), plot), dbY (0, plot) };
            case 1:  return { logX (p ("lowFreq"), plot), dbY (p ("lowGain"), plot) };
            case 2:  return { logX (p ("midFreq"), plot), dbY (p ("midGain"), plot) };
            case 3:  return { logX (p ("highFreq"), plot), dbY (p ("highGain"), plot) };
            default: return { logX (p ("highCut"), plot), dbY (0, plot) };
        }
    }
};

// =============================================================================
//  Tube EQ: green-grey enamel, a glowing tube, a small CRT with the curve
// =============================================================================
class TubeEqFace : public EffectFace
{
public:
    explicit TubeEqFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Vintage, juce::Colour (0xff111111), juce::Colour (0xfff2e6c4), juce::Colour (0xfff2e6c4), juce::Colour (0xff3a403a),
                           juce::Colour (0xfff2e6c4), juce::Colour (0xffc9bc98), juce::Colour (0xff20251f), juce::Colour (0xfff2e6c4) })
    {
        const auto cream = juce::Colour (0xfff2e6c4);
        knob ("lowBoost", "BOOST", cream, 52);
        knob ("lowAtten", "ATTEN", cream, 52);
        knob ("highBoost", "BOOST", cream, 52);
        knob ("bandwidth", "BANDWIDTH", cream, 52);
        knob ("highAtten", "ATTEN", cream, 52);
        knob ("drive", "TUBE DRIVE", cream, 52);
        knob ("output", "OUTPUT", cream, 60);
        lowFreq = choice ("lowFreq");
        highFreq = choice ("highFreq");
        attenFreq = choice ("attenFreq");
        setSize (900, 480);
        startTimerHz (30);
        enamel = juce::Image (juce::Image::RGB, 900, 480, false);
        juce::Graphics eg (enamel);
        juce::ColourGradient bg (juce::Colour (0xff57665b), 0, 0, juce::Colour (0xff3b473f), 0, 480, false);
        eg.setGradientFill (bg);
        eg.fillAll();
        noiseTexture (eg, { 0, 0, 900, 480 }, 11, 0.12f);
    }

    void paint (juce::Graphics& g) override
    {
        g.drawImageAt (enamel, 0, 0);
        const auto cream = juce::Colour (0xfff2e6c4);
        // screws
        for (auto pt : { juce::Point<float> (14, 14), { (float) getWidth() - 26, 14 }, { 14, (float) getHeight() - 26 }, { (float) getWidth() - 26, (float) getHeight() - 26 } })
        {
            g.setColour (juce::Colour (0xffb9b6aa)); g.fillEllipse (pt.x, pt.y, 12, 12);
            g.setColour (juce::Colour (0xff6c6a62)); g.drawLine (pt.x + 2, pt.y + 6, pt.x + 10, pt.y + 6, 1.5f);
        }
        g.setColour (cream);
        g.setFont (juce::Font (juce::FontOptions ("Georgia", 24.0f, juce::Font::bold)));
        g.drawText ("TUBE  EQUALIZER", 40, 14, 400, 34, juce::Justification::centredLeft);
        g.setFont (theme::font (10.0f, true));
        g.setColour (cream.withAlpha (0.7f));
        g.drawText ("PROGRAM EQUALIZER  /  RENASOUND", 42, 44, 300, 14, juce::Justification::centredLeft);

        // CRT with the curve
        auto crt = screen.toFloat();
        g.setColour (juce::Colour (0xff1c211c)); g.fillRoundedRectangle (crt.expanded (5), 12.0f);
        g.setColour (juce::Colour (0xff031008)); g.fillRoundedRectangle (crt, 10.0f);
        auto plot = crt.reduced (10, 8);
        g.setColour (juce::Colour (0xff1e5c33).withAlpha (0.5f));
        for (double fr : { 100.0, 1000.0, 10000.0 }) g.drawVerticalLine ((int) logX (fr, plot), plot.getY(), plot.getBottom());
        g.drawHorizontalLine ((int) plot.getCentreY(), plot.getX(), plot.getRight());
        juce::Path c;
        for (int x = 0; x <= (int) plot.getWidth(); x += 2)
        {
            const double fr = xToLogF (plot.getX() + (float) x, plot);
            const float db = juce::jlimit (-20.0f, 20.0f, juce::Decibels::gainToDecibels ((float) fx.getMagnitudeForFrequency (fr), -60.0f));
            const float y = plot.getCentreY() - db / 20.0f * plot.getHeight() * 0.48f;
            if (x == 0) c.startNewSubPath (plot.getX(), y); else c.lineTo (plot.getX() + (float) x, y);
        }
        g.setColour (juce::Colour (0xff59ff8e).withAlpha (0.25f)); g.strokePath (c, juce::PathStrokeType (5.0f));
        g.setColour (juce::Colour (0xffb6ffcb)); g.strokePath (c, juce::PathStrokeType (1.6f));

        // the tube: glows with drive and with the signal
        auto t = tube.toFloat();
        const float lvl = juce::jlimit (0.0f, 1.0f, (fx.inputDb.load() + 50.0f) / 50.0f);
        const float glow = 0.15f + 0.45f * p ("drive") / 10.0f + 0.4f * lvl;
        g.setColour (juce::Colour (0xffff8a2a).withAlpha (0.22f * glow));
        g.fillEllipse (t.expanded (22, 14));
        juce::Path glass;
        glass.addRoundedRectangle (t.getX(), t.getY() + 10, t.getWidth(), t.getHeight() - 10, t.getWidth() * 0.45f);
        g.setColour (juce::Colour (0x55d7e8ee)); g.fillPath (glass);
        g.setColour (juce::Colour (0xaae9f2f5)); g.strokePath (glass, juce::PathStrokeType (1.4f));
        auto plate = t.reduced (t.getWidth() * 0.26f, t.getHeight() * 0.28f).withTrimmedTop (6);
        g.setColour (juce::Colour (0xff5a5d60)); g.fillRect (plate);
        juce::ColourGradient fil (juce::Colour (0xffffd27a).withAlpha (glow), plate.getCentreX(), plate.getCentreY(),
                                  juce::Colour (0x00ff6a00), plate.getCentreX(), plate.getY() - 6, true);
        g.setGradientFill (fil); g.fillRect (plate.expanded (6, 4));
        g.setColour (juce::Colour (0xff2b2b2b)); g.fillRoundedRectangle (t.getX() + 6, t.getBottom() - 8, t.getWidth() - 12, 14, 3.0f);
        g.setColour (cream.withAlpha (0.8f)); g.setFont (theme::font (9.5f, true));
        g.drawText ("12AX7", t.withY (t.getBottom() + 10).withHeight (12), juce::Justification::centred);

        // engraved section labels
        g.setColour (cream);
        g.setFont (theme::font (11.0f, true));
        g.drawText ("LOW FREQUENCY  CPS", lowFreq->getBounds().translated (0, -16).withHeight (14), juce::Justification::left);
        g.drawText ("HIGH FREQUENCY  KCS", highFreq->getBounds().translated (0, -16).withHeight (14), juce::Justification::left);
        g.drawText ("ATTEN SEL  KCS", attenFreq->getBounds().translated (0, -16).withHeight (14), juce::Justification::left);
        g.setColour (cream.withAlpha (0.35f));
        g.drawHorizontalLine (divider, 30, (float) getWidth() - 30);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (30, 16);
        r.removeFromTop (52);
        auto top = r.removeFromTop (130);
        tube = top.removeFromRight (70).withSizeKeepingCentre (54, 110);
        top.removeFromRight (20);
        screen = top.removeFromRight (360).reduced (0, 6);
        auto left = top.reduced (0, 10);
        left.removeFromTop (18);
        lowFreq->setBounds (left.removeFromTop (28).withWidth (300));
        left.removeFromTop (22);
        attenFreq->setBounds (left.removeFromTop (28).withWidth (220));
        r.removeFromTop (14);
        divider = r.getY();
        r.removeFromTop (24);
        highFreq->setBounds (r.removeFromTop (28).withWidth (520).withX (r.getX()));
        r.removeFromTop (8);
        row (r, 0, 7);
    }

private:
    Switch* lowFreq = nullptr; Switch* highFreq = nullptr; Switch* attenFreq = nullptr;
    juce::Rectangle<int> screen, tube;
    int divider = 0;
    juce::Image enamel;
};

// =============================================================================
//  Compressor: VU needle, transfer curve with the live level, GR history
// =============================================================================
class CompressorFace : public EffectFace
{
public:
    explicit CompressorFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Glossy, juce::Colour (0xff2b2b2e), juce::Colour (0xffffc46b), juce::Colour (0xffff8f3a), juce::Colour (0xff2a2a2d),
                           juce::Colour (0xffeeeeee), juce::Colour (0xff8a8a90), juce::Colour (0xff141416), juce::Colour (0xffffe0b0) })
    {
        const auto lc = juce::Colour (0xffa9a9b0);
        for (auto [id, name] : { std::pair { "threshold", "THRESHOLD" }, { "ratio", "RATIO" }, { "attack", "ATTACK" }, { "release", "RELEASE" }, { "makeup", "MAKEUP" }, { "mix", "MIX" } })
            knob (id, name, lc);
        setSize (880, 450);
        startTimerHz (40);
    }

    // the static curve: output level for an input level
    float curve (float in) const
    {
        const float thr = p ("threshold"), ratio = p ("ratio");
        const float out = in <= thr ? in : thr + (in - thr) / ratio;
        return out + p ("makeup");
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff222225), 0, 0, juce::Colour (0xff0e0e10), 0, (float) getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();
        g.setColour (juce::Colours::white.withAlpha (0.018f));
        for (int x = 0; x < getWidth(); x += 2) g.drawVerticalLine (x, 0, (float) getHeight());
        title (g, { 22, 12, 600, 30 }, "COMPRESSOR", "FEED-FORWARD  /  RMS", juce::Colour (0xffeeeeee), juce::Colour (0xffff8f3a));

        // ---- VU meter (gain reduction) ----
        auto vu = vuArea.toFloat();
        juce::ColourGradient face (juce::Colour (0xfff6e7c4), vu.getCentreX(), vu.getY(), juce::Colour (0xffd9c08c), vu.getCentreX(), vu.getBottom(), false);
        g.setColour (juce::Colour (0xff111111)); g.fillRoundedRectangle (vu.expanded (6), 10.0f);
        g.setGradientFill (face); g.fillRoundedRectangle (vu, 6.0f);
        g.saveState();
        g.reduceClipRegion (vu.toNearestInt());
        const auto pivot = juce::Point<float> (vu.getCentreX(), vu.getBottom() - 10);
        const float rad = juce::jmin (vu.getHeight() - 34, vu.getWidth() * 0.5f);
        const float a0 = -0.68f, a1 = 0.68f;   // radians from vertical
        g.setColour (juce::Colour (0xff2b2116));
        g.setFont (theme::font (10.0f, true));
        for (int db : { 0, -1, -2, -3, -5, -7, -10, -15, -20 })
        {
            const float t = std::sqrt ((float) -db / 20.0f);
            const float ang = a1 - t * (a1 - a0);
            const auto p1 = pivot + juce::Point<float> (std::sin (ang), -std::cos (ang)) * rad;
            const auto p2 = pivot + juce::Point<float> (std::sin (ang), -std::cos (ang)) * (rad - 10);
            g.setColour (db >= -3 ? juce::Colour (0xffb3261e) : juce::Colour (0xff2b2116));
            g.drawLine ({ p1, p2 }, 1.6f);
            const auto lp = pivot + juce::Point<float> (std::sin (ang), -std::cos (ang)) * (rad - 22);
            g.drawText (juce::String (db), juce::Rectangle<float> (lp.x - 12, lp.y - 7, 24, 14), juce::Justification::centred);
        }
        g.setColour (juce::Colour (0xff2b2116));
        g.drawText ("GAIN REDUCTION  dB", vu.withTrimmedBottom (vu.getHeight() * 0.22f).withTrimmedTop (vu.getHeight() * 0.58f), juce::Justification::centred);
        const float gr = juce::jlimit (0.0f, 20.0f, -fx.meterValue.load());
        needle += 0.25f * (gr - needle);
        const float ang = a1 - std::sqrt (needle / 20.0f) * (a1 - a0);
        g.setColour (juce::Colour (0xff1a1a1a));
        g.drawLine ({ pivot, pivot + juce::Point<float> (std::sin (ang), -std::cos (ang)) * (rad - 4) }, 2.0f);
        g.fillEllipse (pivot.x - 7, pivot.y - 7, 14, 14);
        g.restoreState();

        // ---- transfer curve ----
        auto tc = curveArea.toFloat();
        g.setColour (juce::Colour (0xff0b0b0d)); g.fillRoundedRectangle (tc, 8.0f);
        auto plot = tc.reduced (16, 14);
        auto toX = [&] (float db) { return plot.getX() + (db + 60.0f) / 60.0f * plot.getWidth(); };
        auto toY = [&] (float db) { return plot.getBottom() - (juce::jlimit (-60.0f, 12.0f, db) + 60.0f) / 72.0f * plot.getHeight(); };
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        for (float db : { -48.0f, -36.0f, -24.0f, -12.0f, 0.0f }) { g.drawVerticalLine ((int) toX (db), plot.getY(), plot.getBottom()); g.drawHorizontalLine ((int) toY (db), plot.getX(), plot.getRight()); }
        g.setColour (juce::Colours::white.withAlpha (0.15f));
        g.drawLine (toX (-60), toY (-60), toX (0), toY (0), 1.0f);   // 1:1
        juce::Path pc;
        for (float in = -60.0f; in <= 0.01f; in += 0.5f)
            if (in < -59.9f) pc.startNewSubPath (toX (in), toY (curve (in))); else pc.lineTo (toX (in), toY (curve (in)));
        g.setColour (juce::Colour (0xffff8f3a).withAlpha (0.3f)); g.strokePath (pc, juce::PathStrokeType (6.0f));
        g.setColour (juce::Colour (0xffffc46b)); g.strokePath (pc, juce::PathStrokeType (2.2f));
        // threshold marker
        g.setColour (juce::Colour (0xffff8f3a).withAlpha (0.5f));
        g.drawVerticalLine ((int) toX (p ("threshold")), plot.getY(), plot.getBottom());
        // the live level riding the curve
        const float in = juce::jlimit (-60.0f, 0.0f, fx.inputDb.load());
        dot += 0.35f * (in - dot);
        if (fx.inputDb.load() > -59.0f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.25f)); g.fillEllipse (toX (dot) - 9, toY (curve (dot)) - 9, 18, 18);
            g.setColour (juce::Colours::white); g.fillEllipse (toX (dot) - 4.5f, toY (curve (dot)) - 4.5f, 9, 9);
        }
        g.setColour (juce::Colour (0xff8a8a90));
        g.setFont (theme::font (10.0f, true));
        g.drawText ("IN", plot.removeFromBottom (12).removeFromRight (20), juce::Justification::right);
        g.drawText ("OUT", tc.reduced (6).removeFromTop (12).removeFromLeft (30), juce::Justification::left);
        g.drawText (juce::String (p ("ratio"), 1) + ":1", tc.reduced (10, 6).removeFromTop (14), juce::Justification::right);

        // ---- gain reduction history ----
        auto hs = historyArea.toFloat();
        g.setColour (juce::Colour (0xff0b0b0d)); g.fillRoundedRectangle (hs, 8.0f);
        auto hp = hs.reduced (10, 8);
        juce::Path h;
        const int w0 = fx.historyPos.load();
        for (int i = 0; i < BuiltinFx::historySize; ++i)
        {
            const float v = juce::jlimit (0.0f, 20.0f, -fx.history[(size_t) ((w0 + i) % BuiltinFx::historySize)].load());
            const float x = hp.getX() + hp.getWidth() * (float) i / (BuiltinFx::historySize - 1);
            const float y = hp.getY() + v / 20.0f * hp.getHeight();
            if (i == 0) h.startNewSubPath (x, hp.getY()), h.lineTo (x, y); else h.lineTo (x, y);
        }
        h.lineTo (hp.getRight(), hp.getY()); h.closeSubPath();
        g.setColour (juce::Colour (0xffff8f3a).withAlpha (0.55f)); g.fillPath (h);
        g.setColour (juce::Colour (0xff8a8a90)); g.setFont (theme::font (9.5f, true));
        g.drawText ("GAIN REDUCTION OVER TIME", hp, juce::Justification::bottomLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (38);
        auto top = r.removeFromTop (220);
        vuArea = top.removeFromLeft (250).reduced (8, 10);
        top.removeFromLeft (14);
        curveArea = top.removeFromLeft (top.getHeight() + 40);
        top.removeFromLeft (14);
        historyArea = top;
        r.removeFromTop (18);
        row (r, 0, 6);
    }

private:
    juce::Rectangle<int> vuArea, curveArea, historyArea;
    float needle = 0.0f, dot = -60.0f;
};

// =============================================================================
//  Reverb: the room, its surfaces and the wavefronts
// =============================================================================
class ReverbFace : public EffectFace
{
public:
    explicit ReverbFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Minimal, juce::Colour (0xff20193a), juce::Colour (0xffe9e3ff), juce::Colour (0xffb39dff), juce::Colour (0xff2c2550),
                           juce::Colour (0xffe9e3ff), juce::Colour (0xff8f86b8), juce::Colour (0xff14102a), juce::Colour (0xffe9e3ff) })
    {
        const auto lc = juce::Colour (0xffa59cd0);
        for (auto [id, name] : { std::pair { "size", "SIZE" }, { "damping", "DAMPING" }, { "width", "WIDTH" }, { "predelay", "PRE-DELAY" }, { "lowcut", "LOW CUT" }, { "mix", "MIX" } })
            knob (id, name, lc);
        setSize (880, 470);
        startTimerHz (40);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff1a1433), getWidth() * 0.5f, 120, juce::Colour (0xff07050f), 0, (float) getHeight(), true);
        g.setGradientFill (bg);
        g.fillAll();
        title (g, { 22, 12, 600, 30 }, "REVERB", "THE SPACE AROUND YOUR SOUND", juce::Colour (0xffe9e3ff), juce::Colour (0xffb39dff));

        auto a = stage.toFloat();
        const float size = p ("size") / 100.0f, damp = p ("damping") / 100.0f, width = p ("width") / 100.0f;
        const double life = 0.6 + 4.5 * size * size;

        // readouts
        g.setFont (theme::font (10.5f, true));
        g.setColour (juce::Colour (0xff8f86b8));
        const auto info = a.removeFromTop (16);
        g.drawText ("DECAY ~" + juce::String (life * 0.8, 1) + " s", info, juce::Justification::left);
        g.drawText (damp > 0.66f ? "SOFT SURFACES" : damp > 0.33f ? "MIXED SURFACES" : "HARD SURFACES", info, juce::Justification::centred);
        g.drawText ("PRE-DELAY " + juce::String (juce::roundToInt (p ("predelay"))) + " ms", info, juce::Justification::right);
        a.removeFromTop (6);

        // the room as a box seen from the front: Width sets how wide it is, Size how deep
        // (a bigger room's back wall sits further away, so it looks smaller)
        const float frontW = a.getWidth() * (0.5f + 0.48f * width);
        const auto front = a.withSizeKeepingCentre (frontW, a.getHeight());
        const juce::Point<float> vp (front.getCentreX(), front.getY() + front.getHeight() * 0.42f);
        const float k = 0.62f - 0.42f * size;   // back wall scale
        auto toBack = [&] (juce::Point<float> q) { return vp + (q - vp) * k; };
        const auto fl = front.getBottomLeft(), fr = front.getBottomRight(), tl = front.getTopLeft(), tr = front.getTopRight();
        const auto bl = toBack (fl), br = toBack (fr), btl = toBack (tl), btr = toBack (tr);
        juce::Path floor, ceiling, leftW, rightW, backW;
        floor.addQuadrilateral (fl.x, fl.y, fr.x, fr.y, br.x, br.y, bl.x, bl.y);
        ceiling.addQuadrilateral (tl.x, tl.y, tr.x, tr.y, btr.x, btr.y, btl.x, btl.y);
        leftW.addQuadrilateral (tl.x, tl.y, btl.x, btl.y, bl.x, bl.y, fl.x, fl.y);
        rightW.addQuadrilateral (tr.x, tr.y, btr.x, btr.y, br.x, br.y, fr.x, fr.y);
        backW.addQuadrilateral (btl.x, btl.y, btr.x, btr.y, br.x, br.y, bl.x, bl.y);

        // surfaces: hard = bright, crisp lines; soft (high damping) = dim, padded
        const auto edge = juce::Colour (0xffb39dff).withAlpha (0.75f - 0.45f * damp);
        g.setColour (juce::Colour (0xff1c1638)); g.fillPath (ceiling);
        g.setColour (juce::Colour (0xff221a44)); g.fillPath (leftW); g.fillPath (rightW);
        g.setColour (juce::Colour (0xff2b2256).withAlpha (0.9f)); g.fillPath (backW);
        juce::ColourGradient fg (juce::Colour (0xff3a2d72), vp.x, fl.y, juce::Colour (0xff171230), vp.x, bl.y, false);
        g.setGradientFill (fg); g.fillPath (floor);
        if (damp > 0.33f)   // soft panels on the walls
        {
            g.setColour (juce::Colour (0xff120e24).withAlpha (damp * 0.7f));
            for (int i = 0; i < 3; ++i)
            {
                const float t0 = 0.12f + 0.28f * (float) i, t1 = t0 + 0.2f;
                for (auto [a0, b0] : { std::pair { tl, fl }, { tr, fr } })
                {
                    auto at = [&] (juce::Point<float> f, float t) { return f + (toBack (f) - f) * t; };
                    juce::Path pnl;
                    const auto top0 = at (a0, t0), top1 = at (a0, t1), bot0 = at (b0, t0), bot1 = at (b0, t1);
                    const auto m = [] (juce::Point<float> u, juce::Point<float> v, float t) { return u + (v - u) * t; };
                    const auto p1 = m (top0, bot0, 0.2f), p2 = m (top1, bot1, 0.2f), p3 = m (top1, bot1, 0.7f), p4 = m (top0, bot0, 0.7f);
                    pnl.addQuadrilateral (p1.x, p1.y, p2.x, p2.y, p3.x, p3.y, p4.x, p4.y);
                    g.fillPath (pnl);
                }
            }
        }
        for (int i = 1; i < 10; ++i)   // floor boards
        {
            const float t = (float) i / 10.0f;
            g.setColour (edge.withAlpha (0.1f));
            g.drawLine ({ fl + (fr - fl) * t, bl + (br - bl) * t }, 0.8f);
        }
        const float lineW = 0.8f + 1.6f * (1.0f - damp);
        g.setColour (edge);
        for (auto [u, v] : { std::pair { tl, btl }, { tr, btr }, { fl, bl }, { fr, br } }) g.drawLine ({ u, v }, lineW);
        g.strokePath (backW, juce::PathStrokeType (lineW));

        // the source on the floor and wavefronts spreading from it; they start late by the pre-delay
        const juce::Point<float> src = fl + (fr - fl) * 0.5f + (bl - fl) * 0.45f;
        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (fx.inputDb.load() > -45.0f && now - lastWave > 0.12) { waves.push_back (now + p ("predelay") * 0.001); lastWave = now; }
        waves.erase (std::remove_if (waves.begin(), waves.end(), [&] (double t) { return now - t > life; }), waves.end());
        const float squash = (fl.y - bl.y) / (fr.x - fl.x) * 1.6f;   // circles on the floor look flattened
        g.saveState();
        g.reduceClipRegion (floor);
        for (double t0 : waves)
        {
            const double age = now - t0;
            if (age < 0) continue;
            const float kk = (float) (age / life);
            const float rx = frontW * 0.9f * kk, ry = rx * squash;
            const float alpha = (1.0f - kk) * (0.5f + 0.45f * p ("mix") / 100.0f) * (1.0f - 0.45f * damp);
            g.setColour (juce::Colour (0xffd8ccff).withAlpha (alpha));
            g.drawEllipse (src.x - rx, src.y - ry, rx * 2, ry * 2, 1.0f + 2.0f * (1.0f - kk));
            // reflections off the side walls
            g.setColour (juce::Colour (0xffb39dff).withAlpha (alpha * (0.6f - 0.4f * damp)));
            const float lx = juce::jmin (fl.x, bl.x), rxW = juce::jmax (fr.x, br.x);
            g.drawEllipse (lx - (src.x - lx) - rx, src.y - ry, rx * 2, ry * 2, 1.0f);
            g.drawEllipse (rxW + (rxW - src.x) - rx, src.y - ry, rx * 2, ry * 2, 1.0f);
        }
        g.restoreState();
        const float lvl = juce::jlimit (0.0f, 1.0f, (fx.inputDb.load() + 50.0f) / 50.0f);
        g.setColour (juce::Colours::white.withAlpha (0.15f + 0.3f * lvl));
        g.fillEllipse (src.x - 12, src.y - 12, 24, 24);
        g.setColour (juce::Colours::white);
        g.fillEllipse (src.x - 5, src.y - 5, 10, 10);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (38);
        stage = r.removeFromTop (260);
        r.removeFromTop (16);
        row (r, 0, 6);
    }

private:
    juce::Rectangle<int> stage;
    std::vector<double> waves;
    double lastWave = 0;
};

// =============================================================================
//  Delay: the echoes in time
// =============================================================================
class DelayFace : public EffectFace
{
public:
    explicit DelayFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Vintage, juce::Colour (0xff111111), juce::Colour (0xffffb36b), juce::Colour (0xffffb36b), juce::Colour (0xff4a3a2c),
                           juce::Colour (0xfff3e2c8), juce::Colour (0xffb79e7e), juce::Colour (0xff22180f), juce::Colour (0xfff3e2c8) })
    {
        const auto lc = juce::Colour (0xffe7d2b0);
        knob ("time", "TIME", lc);
        knob ("feedback", "FEEDBACK", lc);
        knob ("tone", "TONE", lc);
        knob ("mix", "MIX", lc);
        sync = choice ("sync");
        pingPong = toggle ("pingpong", "PING-PONG");
        setSize (880, 450);
        startTimerHz (40);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff3a2a1d), 0, 0, juce::Colour (0xff1a120b), 0, (float) getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();
        noiseTexture (g, getLocalBounds(), 5, 0.05f);
        title (g, { 22, 12, 600, 30 }, "DELAY", "ECHOES IN TIME", juce::Colour (0xfff3e2c8), juce::Colour (0xffffb36b));

        auto a = stage.toFloat();
        g.setColour (juce::Colour (0xff120c07)); g.fillRoundedRectangle (a, 10.0f);
        auto plot = a.reduced (18, 16);
        const double delay = juce::jmax (0.01, (double) fx.phase.load() > 0 ? fx.phase.load() : p ("time") * 0.001);
        const float fb = p ("feedback") / 100.0f;
        const bool pp = p ("pingpong") > 0.5f;
        const float toneT = (float) (std::log (p ("tone") / 500.0) / std::log (40.0));   // 0 dark .. 1 bright
        const int taps = 8;
        const double span = delay * (taps + 0.5);
        const float mid = plot.getCentreY();
        g.setColour (juce::Colour (0xffb79e7e).withAlpha (0.2f));
        g.drawHorizontalLine ((int) mid, plot.getX(), plot.getRight());
        g.setFont (theme::font (10.0f, true));
        g.setColour (juce::Colour (0xffb79e7e));
        if (pp) { g.drawText ("L", plot.withWidth (14).withHeight (14), juce::Justification::centred); g.drawText ("R", plot.withWidth (14).withY (plot.getBottom() - 14).withHeight (14), juce::Justification::centred); }

        const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;
        if (fx.inputDb.load() > -40.0f && now - lastHit > delay * 0.9) lastHit = now;
        for (int k = 0; k <= taps; ++k)
        {
            const float amp = k == 0 ? 1.0f : std::pow (fb, (float) (k - 1)) * (p ("mix") / 100.0f + 0.25f);
            const float x = plot.getX() + (float) (k * delay / span) * plot.getWidth();
            const float h = amp * plot.getHeight() * (pp ? 0.46f : 0.9f);
            const bool right = pp && k > 0 && k % 2 == 0;
            // later echoes get duller when Tone is low
            const auto col = juce::Colour (0xffffb36b).interpolatedWith (juce::Colour (0xff6b5440), juce::jlimit (0.0f, 1.0f, (1.0f - toneT) * (float) k / taps * 1.6f));
            const double since = now - lastHit - k * delay;
            const float flash = since >= 0 && since < 0.18 ? (float) (1.0 - since / 0.18) : 0.0f;
            juce::Rectangle<float> bar (x - 7, pp ? (right ? mid : mid - h) : plot.getBottom() - h, 14, h);
            g.setColour ((k == 0 ? juce::Colour (0xfff3e2c8) : col).withAlpha (0.25f + 0.75f * flash));
            g.fillRoundedRectangle (bar.expanded (4 * flash), 3.0f);
            g.setColour (k == 0 ? juce::Colour (0xfff3e2c8) : col);
            g.fillRoundedRectangle (bar, 3.0f);
            if (k > 0 && k <= 4)
            {
                g.setColour (juce::Colour (0xffb79e7e));
                g.drawText (juce::String (juce::roundToInt (k * delay * 1000)) + " ms", juce::Rectangle<float> (x - 30, plot.getBottom() + 1, 60, 12), juce::Justification::centred);
            }
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (38);
        stage = r.removeFromTop (220);
        r.removeFromTop (16);
        auto controls = r.removeFromTop (30);
        sync->setBounds (controls.removeFromLeft (560));
        controls.removeFromLeft (16);
        pingPong->setBounds (controls.removeFromLeft (130));
        r.removeFromTop (8);
        row (r.withTrimmedRight (r.getWidth() / 3), 0, 4);
    }

    void timerCallback() override
    {
        knobs[0]->slider.setEnabled (p ("sync") < 0.5f);   // Time is used in Free mode
        knobs[0]->setAlpha (p ("sync") < 0.5f ? 1.0f : 0.45f);
        repaint();
    }

private:
    Switch* sync = nullptr;
    juce::TextButton* pingPong = nullptr;
    juce::Rectangle<int> stage;
    double lastHit = 0;
};

// =============================================================================
//  Noise Gate: level against the threshold
// =============================================================================
class GateFace : public EffectFace
{
public:
    explicit GateFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Neon, juce::Colour (0xff1a1a1a), juce::Colour (0xffffd400), juce::Colour (0xffffb000), juce::Colour (0xff2c2c2c),
                           juce::Colour (0xfff0f0f0), juce::Colour (0xff9a9a9a), juce::Colour (0xff121212), juce::Colour (0xfff0f0f0) })
    {
        const auto lc = juce::Colour (0xffbdbdbd);
        for (auto [id, name] : { std::pair { "threshold", "THRESHOLD" }, { "ratio", "RANGE" }, { "attack", "ATTACK" }, { "release", "RELEASE" } })
            knob (id, name, lc);
        if (fx.historyPos.load() == 0) for (auto& h : fx.history) h = -100.0f;
        setSize (800, 430);
        startTimerHz (40);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff202020));
        // hazard stripe header
        g.saveState();
        g.reduceClipRegion (0, 0, getWidth(), 8);
        for (int x = -20; x < getWidth(); x += 24)
        {
            juce::Path s; s.addQuadrilateral ((float) x, 8, (float) x + 12, 0, (float) x + 24, 0, (float) x + 12, 8);
            g.setColour (juce::Colour (0xffffd400)); g.fillPath (s);
        }
        g.restoreState();
        title (g, { 22, 16, 600, 30 }, "NOISE GATE", "SILENCE BETWEEN THE NOTES", juce::Colour (0xfff0f0f0), juce::Colour (0xffffd400));

        auto a = stage.toFloat();
        g.setColour (juce::Colour (0xff0e0e0e)); g.fillRoundedRectangle (a, 8.0f);
        auto plot = a.reduced (12, 10);
        auto toY = [&] (float db) { return plot.getBottom() - (juce::jlimit (-80.0f, 0.0f, db) + 80.0f) / 80.0f * plot.getHeight(); };
        const float thr = p ("threshold");
        const int w0 = fx.historyPos.load();
        const float colW = plot.getWidth() / BuiltinFx::historySize;
        for (int i = 0; i < BuiltinFx::historySize; ++i)
        {
            const float v = fx.history[(size_t) ((w0 + i) % BuiltinFx::historySize)].load();
            const float x = plot.getX() + colW * (float) i;
            const bool open = v > thr;
            if (! open) { g.setColour (juce::Colour (0xffff3b3b).withAlpha (0.08f)); g.fillRect (x, plot.getY(), colW + 0.5f, plot.getHeight()); }
            g.setColour (open ? juce::Colour (0xffe0e0e0).withAlpha (0.75f) : juce::Colour (0xff6e6e6e).withAlpha (0.6f));
            g.fillRect (x, toY (v), juce::jmax (1.0f, colW - 0.5f), plot.getBottom() - toY (v));
        }
        g.setColour (juce::Colour (0xffffd400));
        g.drawLine (plot.getX(), toY (thr), plot.getRight(), toY (thr), 2.0f);
        g.setFont (theme::font (10.5f, true));
        g.drawText ("THRESHOLD " + juce::String (juce::roundToInt (thr)) + " dB", juce::Rectangle<float> (plot.getX() + 6, toY (thr) - 16, 200, 14), juce::Justification::left);

        // the big status light
        const bool open = fx.inputDb.load() > thr;
        auto led = ledArea.toFloat();
        const auto col = open ? juce::Colour (0xff40e070) : juce::Colour (0xffff3b3b);
        g.setColour (col.withAlpha (0.2f)); g.fillEllipse (led.expanded (8));
        g.setColour (col); g.fillEllipse (led);
        g.setColour (juce::Colours::white.withAlpha (0.4f)); g.fillEllipse (led.reduced (led.getWidth() * 0.3f).translated (-4, -4));
        g.setColour (juce::Colour (0xfff0f0f0)); g.setFont (theme::font (13.0f, true));
        g.drawText (open ? "OPEN" : "CLOSED", led.translated (0, led.getHeight() + 6).withHeight (18).expanded (30, 0), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 14);
        r.removeFromTop (40);
        auto top = r.removeFromTop (220);
        ledArea = top.removeFromRight (110).withSizeKeepingCentre (54, 54).translated (0, -14);
        top.removeFromRight (10);
        stage = top;
        r.removeFromTop (18);
        row (r, 0, 4);
    }

private:
    juce::Rectangle<int> stage, ledArea;
};

// =============================================================================
//  Gater: the step pattern and the playhead
// =============================================================================
class GaterFace : public EffectFace
{
public:
    explicit GaterFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Neon, juce::Colour (0xff050a06), juce::Colour (0xffb6ff3b), juce::Colour (0xff39ff88), juce::Colour (0xff15301e),
                           juce::Colour (0xffd8ffe4), juce::Colour (0xff6fa883), juce::Colour (0xff07120b), juce::Colour (0xffd8ffe4) })
    {
        const auto lc = juce::Colour (0xff8fd6a6);
        knob ("depth", "DEPTH", lc);
        knob ("length", "GATE LENGTH", lc);
        knob ("smooth", "SMOOTH", lc);
        rate = choice ("rate");
        pattern = choice ("pattern");
        setSize (840, 460);
        startTimerHz (40);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff050806));
        g.setColour (juce::Colour (0xff39ff88).withAlpha (0.04f));
        for (int y = 0; y < getHeight(); y += 4) g.drawHorizontalLine (y, 0, (float) getWidth());
        title (g, { 22, 12, 600, 30 }, "GATER", "TEMPO-SYNCED RHYTHM", juce::Colour (0xffd8ffe4), juce::Colour (0xff39ff88));

        static const std::array<std::array<bool, 8>, 5> patterns {{
            {{ 1, 1, 1, 1, 1, 1, 1, 1 }}, {{ 0, 1, 0, 1, 0, 1, 0, 1 }}, {{ 1, 0, 1, 1, 1, 0, 1, 1 }},
            {{ 1, 1, 1, 0, 1, 1, 0, 1 }}, {{ 1, 1, 1, 1, 0, 0, 0, 0 }} }};
        const auto& pat = patterns[(size_t) juce::jlimit (0, 4, (int) std::lround (p ("pattern")))];
        const float len = p ("length") / 100.0f, depth = p ("depth") / 100.0f;
        const float pos = fx.phase.load();
        const int current = (int) pos % 8;

        auto a = stage.toFloat();
        const float cw = a.getWidth() / 8.0f;
        for (int s = 0; s < 8; ++s)
        {
            auto cell = juce::Rectangle<float> (a.getX() + cw * (float) s, a.getY(), cw, a.getHeight() * 0.62f).reduced (5);
            const bool on = pat[(size_t) s];
            const bool now = s == current;
            g.setColour (on ? juce::Colour (0xff39ff88).withAlpha (now ? 0.95f : 0.35f) : juce::Colour (0xff15301e).withAlpha (now ? 1.0f : 0.6f));
            g.fillRoundedRectangle (cell, 6.0f);
            if (now) { g.setColour (juce::Colour (0xffb6ff3b).withAlpha (0.35f)); g.drawRoundedRectangle (cell.expanded (3), 8.0f, 2.0f); }
            if (on)
            {
                g.setColour (juce::Colour (0xff07120b).withAlpha (0.6f));
                g.fillRect (cell.getX() + cell.getWidth() * len, cell.getBottom() - 6, cell.getWidth() * (1.0f - len), 4.0f);
            }
            g.setColour (on && now ? juce::Colour (0xff07120b) : on ? juce::Colour (0xffd8ffe4) : juce::Colour (0xff6fa883));
            g.setFont (theme::font (12.0f, true));
            g.drawText (juce::String (s + 1), cell, juce::Justification::centred);
        }
        // the volume envelope the pattern makes
        auto env = a.withTrimmedTop (a.getHeight() * 0.68f);
        juce::Path e;
        for (int x = 0; x <= (int) env.getWidth(); ++x)
        {
            const float sp = 8.0f * (float) x / env.getWidth();
            const int si = juce::jmin (7, (int) sp);
            const bool open = pat[(size_t) si] && (sp - (float) si) < len;
            const float v = open ? 1.0f : 1.0f - depth;
            const float y = env.getBottom() - v * env.getHeight();
            if (x == 0) e.startNewSubPath (env.getX(), y); else e.lineTo (env.getX() + (float) x, y);
        }
        g.setColour (juce::Colour (0xff39ff88).withAlpha (0.25f)); g.strokePath (e, juce::PathStrokeType (5.0f));
        g.setColour (juce::Colour (0xffb6ff3b)); g.strokePath (e, juce::PathStrokeType (1.6f));
        const float px = env.getX() + env.getWidth() * std::fmod (pos, 8.0f) / 8.0f;
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawVerticalLine ((int) px, a.getY(), env.getBottom());
        g.setColour (juce::Colour (0xff6fa883)); g.setFont (theme::font (10.0f, true));
        g.drawText ("RATE", rate->getBounds().translated (0, -14).withHeight (12), juce::Justification::left);
        g.drawText ("PATTERN", pattern->getBounds().translated (0, -14).withHeight (12), juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (38);
        stage = r.removeFromTop (190);
        r.removeFromTop (24);
        auto sw = r.removeFromTop (28);
        rate->setBounds (sw.removeFromLeft (380));
        sw.removeFromLeft (16);
        pattern->setBounds (sw);
        r.removeFromTop (12);
        row (r.withTrimmedRight (r.getWidth() / 3), 0, 3);
    }

private:
    Switch* rate = nullptr; Switch* pattern = nullptr;
    juce::Rectangle<int> stage;
};

// =============================================================================
//  Phaser: sweeping notches
// =============================================================================
class PhaserFace : public EffectFace
{
public:
    explicit PhaserFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Glossy, juce::Colour (0xff3a1636), juce::Colour (0xffffd166), juce::Colour (0xffff4fa3), juce::Colour (0xff3a1a33),
                           juce::Colour (0xffffeef7), juce::Colour (0xffd59cc0), juce::Colour (0xff22091f), juce::Colour (0xffffeef7) })
    {
        const auto lc = juce::Colour (0xffe8b6d4);
        for (auto [id, name] : { std::pair { "rate", "RATE" }, { "depth", "DEPTH" }, { "centre", "CENTRE" }, { "feedback", "FEEDBACK" }, { "mix", "MIX" } })
            knob (id, name, lc);
        setSize (840, 440);
        startTimerHz (40);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff5a1640), 0, 0, juce::Colour (0xff1c0b2b), (float) getWidth(), (float) getHeight(), false);
        bg.addColour (0.5, juce::Colour (0xff3a0f3c));
        g.setGradientFill (bg);
        g.fillAll();
        // swirl
        const float ph = fx.phase.load();
        for (int k = 0; k < 6; ++k)
        {
            juce::Path s;
            const float rr = 60.0f + k * 34.0f;
            s.addCentredArc ((float) getWidth() - 120, 90, rr, rr * 0.6f, ph * juce::MathConstants<float>::twoPi + k * 0.5f, 0.0f, 2.2f, true);
            g.setColour (juce::Colour (0xffff9f43).withAlpha (0.06f));
            g.strokePath (s, juce::PathStrokeType (10.0f));
        }
        title (g, { 22, 12, 600, 30 }, "PHASER", "SWEEPING NOTCHES", juce::Colour (0xffffeef7), juce::Colour (0xffffd166));

        auto a = stage.toFloat();
        g.setColour (juce::Colour (0xff15061a).withAlpha (0.8f)); g.fillRoundedRectangle (a, 10.0f);
        auto plot = a.reduced (14, 14);
        const float lfo = std::sin (ph * juce::MathConstants<float>::twoPi);
        const double sweep = p ("centre") * std::pow (2.0, p ("depth") / 100.0 * 1.5 * lfo);
        const float notchDepth = 0.55f + 0.4f * std::abs (p ("feedback")) / 90.0f;
        const float mix = p ("mix") / 100.0f;
        juce::Path c;
        for (int x = 0; x <= (int) plot.getWidth(); x += 2)
        {
            const double fr = xToLogF (plot.getX() + (float) x, plot);
            float v = 1.0f;
            for (double mult : { 0.5, 1.0, 2.1, 4.4 })
            {
                const double d = std::log2 (fr / (sweep * mult));
                v -= mix * notchDepth * (float) std::exp (-d * d * 18.0) * (mult == 1.0 ? 1.0f : 0.75f);
            }
            const float y = plot.getY() + (1.0f - juce::jlimit (0.0f, 1.0f, v)) * plot.getHeight() * 0.85f + plot.getHeight() * 0.05f;
            if (x == 0) c.startNewSubPath (plot.getX(), y); else c.lineTo (plot.getX() + (float) x, y);
        }
        juce::Path fill (c);
        fill.lineTo (plot.getRight(), plot.getBottom()); fill.lineTo (plot.getX(), plot.getBottom()); fill.closeSubPath();
        juce::ColourGradient fg (juce::Colour (0xffff4fa3).withAlpha (0.45f), plot.getX(), 0, juce::Colour (0xffffd166).withAlpha (0.45f), plot.getRight(), 0, false);
        g.setGradientFill (fg); g.fillPath (fill);
        g.setColour (juce::Colour (0xffffeef7)); g.strokePath (c, juce::PathStrokeType (2.0f));
        g.setColour (juce::Colour (0xffd59cc0)); g.setFont (theme::font (10.0f, true));
        for (double fr : { 100.0, 1000.0, 10000.0 })
            g.drawText (fr >= 1000 ? juce::String ((int) (fr / 1000)) + "k" : juce::String ((int) fr), (int) logX (fr, plot) + 3, (int) plot.getBottom() - 12, 30, 12, juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (38);
        stage = r.removeFromTop (220);
        r.removeFromTop (16);
        row (r, 0, 5);
    }

private:
    juce::Rectangle<int> stage;
};

// =============================================================================
//  Chorus: voices drifting against each other
// =============================================================================
class ChorusFace : public EffectFace
{
public:
    explicit ChorusFace (BuiltinFx& f)
        : EffectFace (f, { KnobStyle::Neon, juce::Colour (0xff062a33), juce::Colour (0xffeafffb), juce::Colour (0xff3fe6d1), juce::Colour (0xff0d3a44),
                           juce::Colour (0xffeafffb), juce::Colour (0xff7fbfc0), juce::Colour (0xff04191f), juce::Colour (0xffeafffb) })
    {
        const auto lc = juce::Colour (0xff9fd8d6);
        for (auto [id, name] : { std::pair { "rate", "RATE" }, { "depth", "DEPTH" }, { "delay", "DELAY" }, { "feedback", "FEEDBACK" }, { "mix", "MIX" } })
            knob (id, name, lc);
        setSize (820, 430);
        startTimerHz (40);
    }

    void paint (juce::Graphics& g) override
    {
        juce::ColourGradient bg (juce::Colour (0xff0a4651), 0, 0, juce::Colour (0xff031418), 0, (float) getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();
        title (g, { 22, 12, 600, 30 }, "CHORUS", "MANY VOICES FROM ONE", juce::Colour (0xffeafffb), juce::Colour (0xff3fe6d1));

        auto a = stage.toFloat();
        g.setColour (juce::Colour (0xff02101a).withAlpha (0.6f)); g.fillRoundedRectangle (a, 10.0f);
        auto plot = a.reduced (16, 18);
        const float ph = fx.phase.load();
        const float depth = p ("depth") / 100.0f, mix = p ("mix") / 100.0f, fb = std::abs (p ("feedback")) / 90.0f;
        const float delayMs = p ("delay");
        const juce::Colour cols[] { juce::Colour (0xffeafffb), juce::Colour (0xff3fe6d1), juce::Colour (0xff6fb7ff) };
        for (int v = 0; v < 3; ++v)
        {
            // the dry voice, then two copies pushed in time by the delay, swaying with the LFO
            const float sway = v == 0 ? 0.0f : std::sin ((ph + (float) v / 3.0f) * juce::MathConstants<float>::twoPi) * depth;
            const float shift = v == 0 ? 0.0f : (delayMs / 30.0f * 0.25f + sway * 0.18f) * plot.getWidth();
            const float alpha = v == 0 ? 0.9f : 0.25f + 0.65f * mix;
            for (int trail = 0; trail < (v == 0 ? 1 : 1 + (int) (fb * 3)); ++trail)
            {
                juce::Path w;
                for (int x = 0; x <= (int) plot.getWidth(); x += 2)
                {
                    const float t = ((float) x - shift - trail * 14.0f) / plot.getWidth();
                    const float y = plot.getCentreY() + std::sin (t * 18.0f) * std::sin (t * 3.0f + 0.5f) * plot.getHeight() * 0.38f;
                    if (x == 0) w.startNewSubPath (plot.getX(), y); else w.lineTo (plot.getX() + (float) x, y);
                }
                g.setColour (cols[v].withAlpha (alpha * (1.0f - 0.3f * trail)));
                g.strokePath (w, juce::PathStrokeType (v == 0 ? 2.2f : 1.6f));
            }
        }
        g.setColour (juce::Colour (0xff7fbfc0)); g.setFont (theme::font (10.0f, true));
        g.drawText ("DRY", plot.removeFromTop (12), juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 12);
        r.removeFromTop (38);
        stage = r.removeFromTop (210);
        r.removeFromTop (16);
        row (r, 0, 5);
    }

private:
    juce::Rectangle<int> stage;
};

// =============================================================================
juce::AudioProcessorEditor* createFaceplate (BuiltinFx& fx)
{
    if (auto* e = createFaceplate2 (fx)) return e;
    if (auto* e = createFaceplate3 (fx)) return e;
    const auto id = fx.getFxId();
    if (id == "eq")         return new EqFace (fx);
    if (id == "tubeeq")     return new TubeEqFace (fx);
    if (id == "compressor") return new CompressorFace (fx);
    if (id == "reverb")     return new ReverbFace (fx);
    if (id == "delay")      return new DelayFace (fx);
    if (id == "noisegate")  return new GateFace (fx);
    if (id == "gater")      return new GaterFace (fx);
    if (id == "phaser")     return new PhaserFace (fx);
    if (id == "chorus")     return new ChorusFace (fx);
    return nullptr;
}

} // namespace rs
