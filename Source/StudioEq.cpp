// =============================================================================
//  Two studio EQs:
//   - Dynamic EQ: 6 bands (shelves / bells) where any band can also react to
//     the level in its own frequency range, cutting (or boosting) only when
//     that range gets loud. Low and high cut, output, and a live analyser.
//   - Tube EQ: a passive-style program EQ in the spirit of the classic tube
//     units: stepped frequencies, separate low boost and low attenuation that
//     can be used together (the low-end "boost and cut" trick), a high bell
//     with bandwidth, a high shelf cut, and a tube stage for warmth.
// =============================================================================
#include "Effects.h"
#include "DspHelpers.h"
#include "UI/Theme.h"

namespace rs
{

using APVTS = juce::AudioProcessorValueTreeState;

namespace
{
    constexpr int kBands = 6;
    const juce::Colour bandColours[kBands] { juce::Colour (0xffff6b4a), juce::Colour (0xffffc247), juce::Colour (0xff3ec7a5),
                                             juce::Colour (0xff4ab3ff), juce::Colour (0xff9d7bff), juce::Colour (0xffff5fa8) };
    juce::String pid (const char* base, int band) { return juce::String (base) + juce::String (band + 1); }

    float freqToX (double f, juce::Rectangle<float> a) { return a.getX() + a.getWidth() * (float) (std::log (f / 20.0) / std::log (1000.0)); }
    double xToFreq (float x, juce::Rectangle<float> a) { return 20.0 * std::pow (1000.0, juce::jlimit (0.0, 1.0, (double) ((x - a.getX()) / a.getWidth()))); }
}

// =============================================================================
//  Dynamic EQ
// =============================================================================
class DynamicEqFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;

    struct BandCfg
    {
        bool on = true, dyn = false;
        int type = 1;   // 0 low shelf, 1 bell, 2 high shelf
        float freq = 1000, gain = 0, q = 1, thr = -24, range = -6, att = 5, rel = 120;
        bool isActive() const { return on && (dyn || std::abs (gain) > 0.01f); }
    };

    DynamicEqFx() : BuiltinFx ("dyneq", "Dynamic EQ", layout())
    {
        for (int b = 0; b < kBands; ++b)
        {
            auto& p = ptrs[(size_t) b];
            p.on = state.getRawParameterValue (pid ("on", b));
            p.type = state.getRawParameterValue (pid ("type", b));
            p.freq = state.getRawParameterValue (pid ("freq", b));
            p.gain = state.getRawParameterValue (pid ("gain", b));
            p.q = state.getRawParameterValue (pid ("q", b));
            p.dyn = state.getRawParameterValue (pid ("dyn", b));
            p.thr = state.getRawParameterValue (pid ("thr", b));
            p.range = state.getRawParameterValue (pid ("range", b));
            p.att = state.getRawParameterValue (pid ("att", b));
            p.rel = state.getRawParameterValue (pid ("rel", b));
        }
        for (auto& v : dynDb) v = 0.0f;
        for (auto& v : levelDb) v = -100.0f;
        fifoBuffer.resize ((size_t) fifo.getTotalSize());
    }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        const int   types[kBands] { 0, 1, 1, 1, 1, 2 };
        const float freqs[kBands] { 80, 250, 700, 2000, 5000, 10000 };
        for (int b = 0; b < kBands; ++b)
        {
            const auto n = "Band " + juce::String (b + 1) + " ";
            l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { pid ("on", b), 1 }, n + "On", true));
            l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { pid ("type", b), 1 }, n + "Type",
                                                                 juce::StringArray { "Low Shelf", "Bell", "High Shelf" }, types[b]));
            l.add (floatParam (pid ("freq", b), n + "Freq", skewed (20, 20000, 1000), freqs[b], "Hz"));
            l.add (floatParam (pid ("gain", b), n + "Gain", Range (-18, 18), 0, "dB"));
            l.add (floatParam (pid ("q", b), n + "Q", skewed (0.1f, 10.0f, 1.0f), types[b] == 1 ? 1.0f : 0.7f));
            l.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID { pid ("dyn", b), 1 }, n + "Dynamic", false));
            l.add (floatParam (pid ("thr", b), n + "Threshold", Range (-60, 0), -24, "dB"));
            l.add (floatParam (pid ("range", b), n + "Range", Range (-18, 18), -6, "dB"));
            l.add (floatParam (pid ("att", b), n + "Attack", skewed (0.1f, 200.0f, 10.0f), 5, "ms"));
            l.add (floatParam (pid ("rel", b), n + "Release", skewed (10.0f, 2000.0f, 200.0f), 120, "ms"));
        }
        l.add (floatParam ("hpf", "Low Cut", skewed (20, 1000, 100), 20, "Hz"));
        l.add (floatParam ("lpf", "High Cut", skewed (1000, 20000, 6000), 20000, "Hz"));
        l.add (floatParam ("output", "Output", Range (-18, 18), 0, "dB"));
        return l;
    }

    BandCfg readBand (int b) const
    {
        const auto& p = ptrs[(size_t) b];
        BandCfg c;
        c.on = p.on->load() > 0.5f;
        c.type = juce::jlimit (0, 2, (int) std::lround (p.type->load()));
        c.freq = p.freq->load(); c.gain = p.gain->load(); c.q = p.q->load();
        c.dyn = p.dyn->load() > 0.5f;
        c.thr = p.thr->load(); c.range = p.range->load(); c.att = p.att->load(); c.rel = p.rel->load();
        return c;
    }

    static BiquadCoefs bandCoefs (const BandCfg& c, double gainDb, double sr)
    {
        using T = BiquadCoefs::Type;
        const T t = c.type == 0 ? T::LowShelf : c.type == 2 ? T::HighShelf : T::Peak;
        return BiquadCoefs::make (t, sr, c.freq, c.q, gainDb);
    }

    static BiquadCoefs detectorCoefs (const BandCfg& c, double sr)
    {
        using T = BiquadCoefs::Type;
        if (c.type == 0) return BiquadCoefs::make (T::LowPass, sr, c.freq, 0.707, 0);
        if (c.type == 2) return BiquadCoefs::make (T::HighPass, sr, c.freq, 0.707, 0);
        return BiquadCoefs::make (T::BandPass, sr, c.freq, juce::jmax (0.5f, c.q), 0);
    }

    // How far a dynamic band moves for a level: 1 dB of gain change per 2 dB
    // over the threshold (a 2:1 feel), up to the band's range.
    static float dynamicGain (float levelDbValue, const BandCfg& c)
    {
        const float over = levelDbValue - c.thr;
        if (over <= 0.0f) return 0.0f;
        return std::copysign (juce::jmin (std::abs (c.range), over * 0.5f), c.range);
    }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& b : bands) { b.filter.reset(); b.detector.reset(); b.env = 0; b.appliedGain = 1000.0f; }
        hp.reset(); lp.reset();
        outGain.reset (sr, 0.05);
        outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (param ("output")));
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        const int chans = juce::jmin (2, buffer.getNumChannels());
        if (chans == 0) return;
        const double sr = currentSampleRate;

        std::array<BandCfg, kBands> cfg;
        for (int b = 0; b < kBands; ++b)
        {
            cfg[(size_t) b] = readBand (b);
            auto& st = bands[(size_t) b];
            st.detector.c = detectorCoefs (cfg[(size_t) b], sr);
            st.attCoef = (float) (1.0 - std::exp (-1.0 / (cfg[(size_t) b].att * 0.001 * sr)));
            st.relCoef = (float) (1.0 - std::exp (-1.0 / (cfg[(size_t) b].rel * 0.001 * sr)));
            if (! cfg[(size_t) b].dyn) { st.env = 0; dynDb[(size_t) b] = 0.0f; levelDb[(size_t) b] = -100.0f; }
        }
        const float hpf = param ("hpf"), lpf = param ("lpf");
        const bool hpOn = hpf > 20.5f, lpOn = lpf < 19900.0f;
        if (hpOn) hp.c = BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, hpf, 0.707, 0);
        if (lpOn) lp.c = BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, lpf, 0.707, 0);
        outGain.setTargetValue (juce::Decibels::decibelsToGain (param ("output")));

        auto* L = buffer.getWritePointer (0);
        auto* R = chans > 1 ? buffer.getWritePointer (1) : nullptr;
        constexpr int sub = 16;
        float mono[sub];

        for (int start = 0; start < n; start += sub)
        {
            const int len = juce::jmin (sub, n - start);
            for (int i = 0; i < len; ++i)
                mono[i] = R != nullptr ? 0.5f * (L[start + i] + R[start + i]) : L[start + i];

            for (int b = 0; b < kBands; ++b)
            {
                const auto& c = cfg[(size_t) b];
                auto& st = bands[(size_t) b];
                if (! c.isActive()) continue;

                float gainDb = c.gain;
                if (c.dyn)
                {
                    // the detector listens to the input, in this band's range
                    for (int i = 0; i < len; ++i)
                    {
                        const float y = st.detector.process (mono[i], 0);
                        const float e2 = y * y;
                        st.env += (e2 > st.env ? st.attCoef : st.relCoef) * (e2 - st.env);
                    }
                    const float lvl = 10.0f * std::log10 (st.env + 1.0e-12f) + 3.0f;   // a sine at 0 dBFS reads 0 dB
                    const float d = dynamicGain (lvl, c);
                    gainDb += d;
                    dynDb[(size_t) b] = d;
                    levelDb[(size_t) b] = lvl;
                }

                if (std::abs (gainDb - st.appliedGain) > 0.005f || c.type != st.appliedType
                    || ! juce::approximatelyEqual (c.freq, st.appliedFreq) || ! juce::approximatelyEqual (c.q, st.appliedQ))
                {
                    st.filter.c = bandCoefs (c, gainDb, sr);
                    st.appliedGain = gainDb; st.appliedType = c.type; st.appliedFreq = c.freq; st.appliedQ = c.q;
                }
                for (int i = 0; i < len; ++i) L[start + i] = st.filter.process (L[start + i], 0);
                if (R != nullptr) for (int i = 0; i < len; ++i) R[start + i] = st.filter.process (R[start + i], 1);
            }
        }

        for (int i = 0; i < n; ++i)
        {
            float l = L[i], r = R != nullptr ? R[i] : 0.0f;
            if (hpOn) { l = hp.process (l, 0); if (R) r = hp.process (r, 1); }
            if (lpOn) { l = lp.process (l, 0); if (R) r = lp.process (r, 1); }
            const float g = outGain.getNextValue();
            L[i] = l * g;
            if (R != nullptr) R[i] = r * g;
        }

        // feed the analyser (dropped when nobody is reading)
        int s1, n1, s2, n2;
        fifo.prepareToWrite (n, s1, n1, s2, n2);
        for (int i = 0; i < n1; ++i) fifoBuffer[(size_t) (s1 + i)] = R != nullptr ? 0.5f * (L[i] + R[i]) : L[i];
        for (int i = 0; i < n2; ++i) fifoBuffer[(size_t) (s2 + i)] = R != nullptr ? 0.5f * (L[n1 + i] + R[n1 + i]) : L[n1 + i];
        fifo.finishedWrite (n1 + n2);
    }

    // ---- for the editor ----
    bool hasResponseCurve() const override { return true; }
    double getMagnitudeForFrequency (double f) const override { return magnitudeAt (f, false); }

    double magnitudeAt (double f, bool withDynamics) const
    {
        const double sr = currentSampleRate;
        double m = 1.0;
        for (int b = 0; b < kBands; ++b)
        {
            const auto c = readBand (b);
            if (! c.isActive()) continue;
            m *= bandCoefs (c, c.gain + (withDynamics && c.dyn ? dynDb[(size_t) b].load() : 0.0f), sr).magnitude (f, sr);
        }
        if (param ("hpf") > 20.5f) m *= BiquadCoefs::make (BiquadCoefs::Type::HighPass, sr, param ("hpf"), 0.707, 0).magnitude (f, sr);
        if (param ("lpf") < 19900.0f) m *= BiquadCoefs::make (BiquadCoefs::Type::LowPass, sr, param ("lpf"), 0.707, 0).magnitude (f, sr);
        return m * juce::Decibels::decibelsToGain (param ("output"));
    }

    double getSampleRateForDisplay() const { return currentSampleRate; }

    std::array<std::atomic<float>, kBands> dynDb, levelDb;
    juce::AbstractFifo fifo { 16384 };
    std::vector<float> fifoBuffer;

    juce::AudioProcessorEditor* createEditor() override;

private:
    struct Ptrs { std::atomic<float>* on, * type, * freq, * gain, * q, * dyn, * thr, * range, * att, * rel; };
    std::array<Ptrs, kBands> ptrs {};

    struct BandState
    {
        Biquad filter, detector;
        float env = 0, attCoef = 0.1f, relCoef = 0.01f;
        float appliedGain = 1000.0f, appliedFreq = 0, appliedQ = 0;
        int appliedType = -1;
    };
    std::array<BandState, kBands> bands;
    Biquad hp, lp;
    juce::LinearSmoothedValue<float> outGain { 1.0f };
};

// -----------------------------------------------------------------------------
class DynamicEqEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DynamicEqEditor (DynamicEqFx& p) : AudioProcessorEditor (p), fx (p)
    {
        for (int b = 0; b < kBands; ++b)
        {
            auto& btn = bandButtons[(size_t) b];
            btn.setButtonText (juce::String (b + 1));
            btn.onClick = [this, b] { selectBand (b); };
            addAndMakeVisible (btn);
        }

        type.addItemList ({ "Low Shelf", "Bell", "High Shelf" }, 1);
        addAndMakeVisible (type);
        for (auto* t : { &onButton, &dynButton })
        {
            t->setClickingTogglesState (true);
            t->setColour (juce::TextButton::buttonOnColourId, theme::accent);
            addAndMakeVisible (*t);
        }
        onButton.setTooltip ("Turn this band on or off");
        dynButton.setTooltip ("Make this band react to the level in its frequency range");
        dynButton.onClick = [this] { updateEnablement(); };

        const char* names[] { "FREQ", "GAIN", "Q", "THRESHOLD", "RANGE", "ATTACK", "RELEASE", "LOW CUT", "HIGH CUT", "OUTPUT" };
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            auto& k = knobs[i];
            k.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 18);
            k.label.setText (names[i], juce::dontSendNotification);
            k.label.setJustificationType (juce::Justification::centred);
            k.label.setFont (theme::font (11.0f, true));
            k.label.setColour (juce::Label::textColourId, theme::textDim);
            addAndMakeVisible (k.slider);
            addAndMakeVisible (k.label);
        }
        attach (knobs[7], "hpf", globalAttach[0]);
        attach (knobs[8], "lpf", globalAttach[1]);
        attach (knobs[9], "output", globalAttach[2]);

        levelLabel.setFont (theme::font (11.0f));
        levelLabel.setColour (juce::Label::textColourId, theme::textDim);
        levelLabel.setJustificationType (juce::Justification::centred);
        addAndMakeVisible (levelLabel);

        selectBand (1);
        setSize (900, 560);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        auto title = getLocalBounds().reduced (12, 8).removeFromTop (34);
        g.setColour (theme::accent);
        g.fillRoundedRectangle (title.removeFromLeft (4).toFloat().reduced (0, 6), 2.0f);
        title.removeFromLeft (8);
        g.setColour (theme::text);
        g.setFont (theme::font (17.0f, true));
        g.drawText ("DYNAMIC EQ", title, juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText ("Drag a point for frequency and gain, scroll for Q, double-click to switch a band on or off", title, juce::Justification::centredRight);

        const auto a = graph;
        g.setColour (theme::bg);
        g.fillRoundedRectangle (a, 6.0f);

        // grid
        g.setFont (theme::font (10.0f));
        for (double f : { 30.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            const float x = freqToX (f, a);
            g.setColour (theme::grid);
            g.drawVerticalLine ((int) x, a.getY(), a.getBottom());
            g.setColour (theme::textDim);
            g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f), (int) x + 3, (int) a.getBottom() - 15, 30, 12, juce::Justification::left);
        }
        for (float db : { -18.0f, -12.0f, -6.0f, 0.0f, 6.0f, 12.0f, 18.0f })
        {
            const float y = dbToY (db);
            g.setColour (std::abs (db) < 0.1f ? theme::gridBar : theme::grid.withAlpha (0.6f));
            g.drawHorizontalLine ((int) y, a.getX(), a.getRight());
            g.setColour (theme::textDim.withAlpha (0.7f));
            g.drawText ((db > 0 ? "+" : "") + juce::String ((int) db), (int) a.getX() + 4, (int) y - 7, 28, 12, juce::Justification::left);
        }

        // analyser
        {
            juce::Path spec;
            const int w = (int) a.getWidth();
            spec.startNewSubPath (a.getX(), a.getBottom());
            for (int x = 0; x <= w; x += 2)
            {
                const double f = xToFreq (a.getX() + (float) x, a);
                const float db = spectrumAt (f);
                const float y = juce::jmap (juce::jlimit (-96.0f, 0.0f, db), -96.0f, 0.0f, a.getBottom(), a.getY() + 10);
                spec.lineTo (a.getX() + (float) x, y);
            }
            spec.lineTo (a.getRight(), a.getBottom());
            spec.closeSubPath();
            g.setColour (theme::textDim.withAlpha (0.13f));
            g.fillPath (spec);
        }

        g.saveState();
        g.reduceClipRegion (a.toNearestInt());

        // the range each dynamic band can move through
        const double sr = fx.getSampleRateForDisplay();
        for (int b = 0; b < kBands; ++b)
        {
            const auto c = fx.readBand (b);
            if (! c.on || ! c.dyn) continue;
            const auto c0 = DynamicEqFx::bandCoefs (c, c.gain, sr), c1 = DynamicEqFx::bandCoefs (c, c.gain + c.range, sr);
            juce::Path top, area;
            std::vector<juce::Point<float>> lower;
            for (int x = 0; x <= (int) a.getWidth(); x += 3)
            {
                const double f = xToFreq (a.getX() + (float) x, a);
                const float y0 = dbToY (juce::Decibels::gainToDecibels ((float) c0.magnitude (f, sr), -60.0f));
                const float y1 = dbToY (juce::Decibels::gainToDecibels ((float) c1.magnitude (f, sr), -60.0f));
                if (x == 0) area.startNewSubPath (a.getX(), y0); else area.lineTo (a.getX() + (float) x, y0);
                lower.push_back ({ a.getX() + (float) x, y1 });
            }
            for (auto it = lower.rbegin(); it != lower.rend(); ++it) area.lineTo (*it);
            area.closeSubPath();
            g.setColour (bandColours[b].withAlpha (b == selected ? 0.28f : 0.14f));
            g.fillPath (area);
        }

        // response: as set, and live (with the dynamic bands moving)
        auto curve = [&] (bool live)
        {
            juce::Path p;
            for (int x = 0; x <= (int) a.getWidth(); x += 2)
            {
                const double f = xToFreq (a.getX() + (float) x, a);
                const float y = dbToY (juce::jlimit (-30.0f, 30.0f, juce::Decibels::gainToDecibels ((float) fx.magnitudeAt (f, live), -60.0f)));
                if (x == 0) p.startNewSubPath (a.getX(), y); else p.lineTo (a.getX() + (float) x, y);
            }
            return p;
        };
        bool anyDyn = false;
        for (int b = 0; b < kBands; ++b) { const auto c = fx.readBand (b); anyDyn |= c.on && c.dyn; }
        auto set = curve (false);
        if (anyDyn)
        {
            g.setColour (theme::text.withAlpha (0.35f));
            g.strokePath (set, juce::PathStrokeType (1.2f));
            g.setColour (theme::accent);
            g.strokePath (curve (true), juce::PathStrokeType (2.2f));
        }
        else
        {
            juce::Path fill (set);
            fill.lineTo (a.getRight(), dbToY (0)); fill.lineTo (a.getX(), dbToY (0)); fill.closeSubPath();
            g.setColour (theme::accent.withAlpha (0.12f));
            g.fillPath (fill);
            g.setColour (theme::accent);
            g.strokePath (set, juce::PathStrokeType (2.2f));
        }
        g.restoreState();

        // band points
        for (int b = 0; b < kBands; ++b)
        {
            const auto c = fx.readBand (b);
            const auto pt = nodePos (b);
            const auto col = c.on ? bandColours[b] : theme::gridBar;
            if (c.on && c.dyn)
            {
                const float yLive = dbToY (c.gain + fx.dynDb[(size_t) b].load());
                g.setColour (col.withAlpha (0.8f));
                g.drawLine (pt.x, pt.y, pt.x, yLive, 2.0f);
                g.fillEllipse (pt.x - 3.5f, yLive - 3.5f, 7.0f, 7.0f);
            }
            const float r = b == selected ? 9.0f : 7.5f;
            g.setColour (col.withAlpha (c.on ? 1.0f : 0.6f));
            g.fillEllipse (pt.x - r, pt.y - r, r * 2, r * 2);
            if (b == selected) { g.setColour (juce::Colours::white); g.drawEllipse (pt.x - r - 2, pt.y - r - 2, r * 2 + 4, r * 2 + 4, 1.5f); }
            g.setColour (theme::bg);
            g.setFont (theme::font (11.0f, true));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (pt.x - r, pt.y - r, r * 2, r * 2), juce::Justification::centred);
        }

        // control area panels
        g.setColour (theme::panelHi.withAlpha (0.5f));
        g.fillRoundedRectangle (bandPanel.toFloat(), 6.0f);
        g.fillRoundedRectangle (globalPanel.toFloat(), 6.0f);
        g.setColour (bandColours[selected]);
        g.fillRoundedRectangle (bandPanel.toFloat().removeFromLeft (4).reduced (0, 8), 2.0f);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f, true));
        g.drawText ("BAND " + juce::String (selected + 1), bandPanel.getX() + 14, bandPanel.getY() + 6, 80, 14, juce::Justification::left);
        g.drawText ("DYNAMICS", dynLabelArea, juce::Justification::left);
        g.drawText ("OUTPUT", globalPanel.getX() + 12, globalPanel.getY() + 6, 80, 14, juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (12, 8);
        r.removeFromTop (38);
        graph = r.removeFromTop (290).toFloat();
        r.removeFromTop (8);
        auto bandRow = r.removeFromTop (30);
        for (auto& b : bandButtons) { b.setBounds (bandRow.removeFromLeft (44)); bandRow.removeFromLeft (6); }
        r.removeFromTop (8);

        globalPanel = r.removeFromRight (240);
        r.removeFromRight (8);
        bandPanel = r;

        auto bp = bandPanel.reduced (12, 6);
        bp.removeFromTop (18);
        auto left = bp.removeFromLeft (110);
        type.setBounds (left.removeFromTop (28));
        left.removeFromTop (8);
        onButton.setBounds (left.removeFromTop (26));
        left.removeFromTop (8);
        dynButton.setBounds (left.removeFromTop (26));
        bp.removeFromLeft (8);

        const int kw = (bp.getWidth()) / 7;
        auto place = [&] (Knob& k, juce::Rectangle<int> cell)
        {
            k.label.setBounds (cell.removeFromTop (16));
            k.slider.setBounds (cell.reduced (2, 0));
        };
        for (int i = 0; i < 7; ++i)
        {
            auto cell = bp.removeFromLeft (kw);
            if (i == 3) dynLabelArea = { cell.getX() + 4, bandPanel.getY() + 6, 120, 14 };
            place (knobs[(size_t) i], cell);
        }
        levelLabel.setBounds (dynLabelArea.withX (dynLabelArea.getX() + 70).withWidth (kw * 2));

        auto gp = globalPanel.reduced (8, 6);
        gp.removeFromTop (18);
        const int gw = gp.getWidth() / 3;
        for (int i = 7; i < 10; ++i) place (knobs[(size_t) i], gp.removeFromLeft (gw));
    }

    // ---- point dragging ----
    void mouseDown (const juce::MouseEvent& e) override
    {
        dragBand = hitNode (e.position);
        if (dragBand < 0) return;
        selectBand (dragBand);
        for (auto* p : { param ("freq"), param ("gain") }) p->beginChangeGesture();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragBand < 0) return;
        auto* f = param ("freq"); auto* gn = param ("gain");
        const auto pos = e.position;
        f->setValueNotifyingHost (f->convertTo0to1 ((float) xToFreq (pos.x, graph)));
        gn->setValueNotifyingHost (gn->convertTo0to1 (juce::jlimit (-18.0f, 18.0f, yToDb (pos.y))));
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragBand < 0) return;
        for (auto* p : { param ("freq"), param ("gain") }) p->endChangeGesture();
        dragBand = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        const int b = hitNode (e.position);
        if (b < 0) return;
        selectBand (b);
        auto* on = param ("on");
        on->beginChangeGesture();
        on->setValueNotifyingHost (on->getValue() > 0.5f ? 0.0f : 1.0f);
        on->endChangeGesture();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (! graph.contains (e.position)) return;
        const int b = hitNode (e.position, 40.0f);
        if (b >= 0) selectBand (b);
        auto* q = param ("q");
        const float cur = q->convertFrom0to1 (q->getValue());
        q->beginChangeGesture();
        q->setValueNotifyingHost (q->convertTo0to1 (juce::jlimit (0.1f, 10.0f, cur * std::exp (w.deltaY * 1.2f))));
        q->endChangeGesture();
    }

private:
    struct Knob { juce::Slider slider; juce::Label label; };
    using SA = juce::AudioProcessorValueTreeState::SliderAttachment;

    DynamicEqFx& fx;
    int selected = 0, dragBand = -1;
    std::array<juce::TextButton, kBands> bandButtons;
    juce::ComboBox type;
    juce::TextButton onButton { "On" }, dynButton { "Dynamic" };
    std::array<Knob, 10> knobs;
    juce::Label levelLabel;
    std::array<std::unique_ptr<SA>, 7> bandAttach;
    std::array<std::unique_ptr<SA>, 3> globalAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttach;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> onAttach, dynAttach;
    juce::Rectangle<float> graph;
    juce::Rectangle<int> bandPanel, globalPanel, dynLabelArea;

    // analyser
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    juce::dsp::WindowingFunction<float> window { (size_t) fftSize, juce::dsp::WindowingFunction<float>::hann };
    std::vector<float> ring = std::vector<float> ((size_t) fftSize, 0.0f);
    int ringPos = 0;
    std::vector<float> fftData = std::vector<float> ((size_t) fftSize * 2, 0.0f);
    std::vector<float> spectrum = std::vector<float> ((size_t) fftSize / 2, -120.0f);

    juce::RangedAudioParameter* param (const char* base) const { return fx.state.getParameter (pid (base, selected)); }

    void attach (Knob& k, const juce::String& id, std::unique_ptr<SA>& slot)
    {
        slot.reset();
        slot = std::make_unique<SA> (fx.state, id, k.slider);
        if (auto* p = fx.state.getParameter (id))
            k.slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
    }

    void selectBand (int b)
    {
        selected = juce::jlimit (0, kBands - 1, b);
        const char* ids[] { "freq", "gain", "q", "thr", "range", "att", "rel" };
        for (int i = 0; i < 7; ++i) attach (knobs[(size_t) i], pid (ids[i], selected), bandAttach[(size_t) i]);
        typeAttach.reset(); onAttach.reset(); dynAttach.reset();
        typeAttach = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (fx.state, pid ("type", selected), type);
        onAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (fx.state, pid ("on", selected), onButton);
        dynAttach = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (fx.state, pid ("dyn", selected), dynButton);
        for (auto& k : knobs) k.slider.setColour (juce::Slider::rotarySliderFillColourId, bandColours[selected]);
        for (int i = 7; i < 10; ++i) knobs[(size_t) i].slider.setColour (juce::Slider::rotarySliderFillColourId, theme::accent);
        updateEnablement();
        updateBandButtons();
        repaint();
    }

    void updateEnablement()
    {
        const bool dyn = fx.readBand (selected).dyn;
        for (int i = 3; i < 7; ++i)
        {
            knobs[(size_t) i].slider.setEnabled (dyn);
            knobs[(size_t) i].slider.setAlpha (dyn ? 1.0f : 0.4f);
            knobs[(size_t) i].label.setAlpha (dyn ? 1.0f : 0.4f);
        }
        levelLabel.setVisible (dyn);
    }

    void updateBandButtons()
    {
        for (int b = 0; b < kBands; ++b)
        {
            const auto c = fx.readBand (b);
            auto& btn = bandButtons[(size_t) b];
            const auto col = c.on ? bandColours[b] : theme::gridBar;
            btn.setColour (juce::TextButton::buttonColourId, b == selected ? col : theme::panelHi);
            btn.setColour (juce::TextButton::textColourOffId, b == selected ? theme::bg : col);
        }
    }

    juce::Point<float> nodePos (int b) const
    {
        const auto c = fx.readBand (b);
        return { freqToX (c.freq, graph), dbToY (c.gain) };
    }

    int hitNode (juce::Point<float> p, float radius = 13.0f) const
    {
        int best = -1; float bestD = radius;
        for (int b = 0; b < kBands; ++b)
        {
            const float d = nodePos (b).getDistanceFrom (p);
            if (d < bestD) { bestD = d; best = b; }
        }
        return best;
    }

    float dbToY (float db) const { return graph.getCentreY() - db / 24.0f * (graph.getHeight() * 0.5f - 6.0f); }
    float yToDb (float y) const  { return (graph.getCentreY() - y) / (graph.getHeight() * 0.5f - 6.0f) * 24.0f; }

    float spectrumAt (double f) const
    {
        const double bin = f * fftSize / fx.getSampleRateForDisplay();
        const int i = juce::jlimit (1, fftSize / 2 - 2, (int) bin);
        const float t = (float) juce::jlimit (0.0, 1.0, bin - i);
        return spectrum[(size_t) i] + t * (spectrum[(size_t) i + 1] - spectrum[(size_t) i]);
    }

    void timerCallback() override
    {
        // pull the newest audio from the effect
        int s1, n1, s2, n2;
        const int ready = fx.fifo.getNumReady();
        fx.fifo.prepareToRead (ready, s1, n1, s2, n2);
        auto push = [this] (const float* d, int count) { for (int i = 0; i < count; ++i) { ring[(size_t) ringPos] = d[i]; ringPos = (ringPos + 1) % fftSize; } };
        push (fx.fifoBuffer.data() + s1, n1);
        push (fx.fifoBuffer.data() + s2, n2);
        fx.fifo.finishedRead (n1 + n2);

        for (int i = 0; i < fftSize; ++i) fftData[(size_t) i] = ring[(size_t) ((ringPos + i) % fftSize)];
        std::fill (fftData.begin() + fftSize, fftData.end(), 0.0f);
        window.multiplyWithWindowingTable (fftData.data(), (size_t) fftSize);
        fft.performFrequencyOnlyForwardTransform (fftData.data());
        const float norm = 4.0f / (float) fftSize;   // a full-scale sine reads about 0 dB
        for (int i = 0; i < fftSize / 2; ++i)
        {
            // slope +3 dB/oct so music reads flatter, like most analysers
            const double f = juce::jmax (20.0, (double) i * fx.getSampleRateForDisplay() / fftSize);
            const float db = juce::Decibels::gainToDecibels (fftData[(size_t) i] * norm, -120.0f) + 3.0f * (float) std::log2 (f / 1000.0);
            auto& s = spectrum[(size_t) i];
            s = db > s ? db : s + 0.25f * (db - s);
        }

        if (fx.readBand (selected).dyn)
        {
            const float lvl = fx.levelDb[(size_t) selected].load();
            levelLabel.setText ("level " + (lvl > -90 ? juce::String (lvl, 1) + " dB" : juce::String ("-inf")) + "   moving "
                                + juce::String (fx.dynDb[(size_t) selected].load(), 1) + " dB", juce::dontSendNotification);
        }
        updateBandButtons();
        repaint (graph.toNearestInt().expanded (4));
    }
};

juce::AudioProcessorEditor* DynamicEqFx::createEditor() { return new DynamicEqEditor (*this); }

// =============================================================================
//  Tube EQ
// =============================================================================
class TubeEqFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    TubeEqFx() : BuiltinFx ("tubeeq", "Tube EQ", layout()) {}

    static constexpr float lowFreqs[]   { 20, 30, 60, 100 };
    static constexpr float highFreqs[]  { 3000, 4000, 5000, 8000, 10000, 12000, 16000 };
    static constexpr float attenFreqs[] { 5000, 10000, 20000 };

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        auto knob = [] (const char* id, const char* name) { return floatParam (id, name, Range (0, 10), 0); };
        // laid out like the front panel: low section and tube on the first row, highs on the second
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "lowFreq", 1 }, "Low Freq", juce::StringArray { "20 Hz", "30 Hz", "60 Hz", "100 Hz" }, 2));
        l.add (knob ("lowBoost", "Low Boost"));
        l.add (knob ("lowAtten", "Low Atten"));
        l.add (floatParam ("drive", "Tube Drive", Range (0, 10), 2));
        l.add (floatParam ("output", "Output", Range (-12, 12), 0, "dB"));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "highFreq", 1 }, "High Freq", juce::StringArray { "3k", "4k", "5k", "8k", "10k", "12k", "16k" }, 4));
        l.add (knob ("highBoost", "High Boost"));
        l.add (floatParam ("bandwidth", "Bandwidth", Range (0, 10), 5));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "attenFreq", 1 }, "Atten Freq", juce::StringArray { "5k", "10k", "20k" }, 1));
        l.add (knob ("highAtten", "High Atten"));
        return l;
    }

    struct Values { float lowFreq, lowBoost, lowAtten, highFreq, highBoost, bandwidth, attenFreq, highAtten; };

    Values values() const
    {
        return { lowFreqs[juce::jlimit (0, 3, (int) std::lround (param ("lowFreq")))], param ("lowBoost"), param ("lowAtten"),
                 highFreqs[juce::jlimit (0, 6, (int) std::lround (param ("highFreq")))], param ("highBoost"), param ("bandwidth"),
                 attenFreqs[juce::jlimit (0, 2, (int) std::lround (param ("attenFreq")))], param ("highAtten") };
    }

    // The passive network, modelled as four filters. The boost sits a little
    // below the attenuation and is resonant, so using both gives a bump in the
    // lows and a dip just above it.
    static std::array<BiquadCoefs, 4> coefsFor (const Values& v, double sr)
    {
        using T = BiquadCoefs::Type;
        const double q = 2.5 * std::pow (0.4 / 2.5, v.bandwidth / 10.0);   // sharp (0) to broad (10)
        return { BiquadCoefs::make (T::LowShelf, sr, v.lowFreq * 1.3, 1.3, v.lowBoost * 1.4),
                 BiquadCoefs::make (T::LowShelf, sr, v.lowFreq, 0.45, -v.lowAtten * 1.75),
                 BiquadCoefs::make (T::Peak, sr, v.highFreq, q, v.highBoost * 1.8),
                 BiquadCoefs::make (T::HighShelf, sr, juce::jmin ((double) v.attenFreq, sr * 0.45), 0.5, -v.highAtten * 2.0) };
    }

    void prepareToPlay (double sr, int maxBlock) override
    {
        currentSampleRate = sr;
        preparedBlock = juce::jmax (1, maxBlock);
        oversampling.initProcessing ((size_t) preparedBlock);
        oversampling.reset();
        for (auto& f : filters) f.reset();
        dc[0] = dc[1] = { 0.0f, 0.0f };
        dcCoef = (float) std::exp (-juce::MathConstants<double>::twoPi * 8.0 / sr);
        outGain.reset (sr, 0.05);
        outGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (param ("output")));
        lastSet = false;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        const int chans = juce::jmin (2, buffer.getNumChannels());
        if (chans == 0) return;

        measureIn (buffer);
        const auto v = values();
        if (! lastSet || std::memcmp (&v, &last, sizeof (Values)) != 0)
        {
            const auto c = coefsFor (v, currentSampleRate);
            for (size_t i = 0; i < filters.size(); ++i) filters[i].c = c[i];
            last = v; lastSet = true;
        }
        active = { v.lowBoost > 0.01f, v.lowAtten > 0.01f, v.highBoost > 0.01f, v.highAtten > 0.01f };

        for (int ch = 0; ch < chans; ++ch)
        {
            auto* d = buffer.getWritePointer (ch);
            for (size_t k = 0; k < filters.size(); ++k)
                if (active[k])
                    for (int i = 0; i < n; ++i) d[i] = filters[k].process (d[i], ch);
        }

        // tube stage: asymmetric soft saturation (adds mostly 2nd harmonic), at 2x rate
        const float drive = param ("drive");
        if (drive > 0.01f && chans == 2 && n <= preparedBlock)
        {
            juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), 2, (size_t) n);
            auto up = oversampling.processSamplesUp (block);
            const float k = 1.0f + drive * 0.12f;
            const float bias = 0.1f + drive * 0.03f;
            const float tb = std::tanh (bias);
            const float norm = 1.0f / (k * (1.0f - tb * tb));    // unity gain for quiet signals
            for (size_t ch = 0; ch < up.getNumChannels(); ++ch)
            {
                auto* d = up.getChannelPointer (ch);
                for (size_t i = 0; i < up.getNumSamples(); ++i)
                    d[i] = (std::tanh (k * d[i] + bias) - tb) * norm;
            }
            oversampling.processSamplesDown (block);

            // remove the DC the asymmetry leaves behind
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = buffer.getWritePointer (ch);
                auto& s = dc[ch];
                for (int i = 0; i < n; ++i)
                {
                    const float y = d[i] - s.x1 + dcCoef * s.y1;
                    s.x1 = d[i]; s.y1 = y; d[i] = y;
                }
            }
        }

        outGain.setTargetValue (juce::Decibels::decibelsToGain (param ("output")));
        if (outGain.isSmoothing())
            for (int i = 0; i < n; ++i)
            {
                const float g = outGain.getNextValue();
                for (int ch = 0; ch < chans; ++ch) buffer.getWritePointer (ch)[i] *= g;
            }
        else if (! juce::approximatelyEqual (outGain.getTargetValue(), 1.0f))
            for (int ch = 0; ch < chans; ++ch) buffer.applyGain (ch, 0, n, outGain.getTargetValue());
    }

    bool hasResponseCurve() const override { return true; }
    double getMagnitudeForFrequency (double f) const override
    {
        const auto v = values();
        double m = juce::Decibels::decibelsToGain (param ("output"));
        const auto c = coefsFor (v, currentSampleRate);
        const bool on[4] { v.lowBoost > 0.01f, v.lowAtten > 0.01f, v.highBoost > 0.01f, v.highAtten > 0.01f };
        for (size_t i = 0; i < 4; ++i) if (on[i]) m *= c[i].magnitude (f, currentSampleRate);
        return m;
    }

private:
    std::array<Biquad, 4> filters;
    std::array<bool, 4> active {};
    Values last {};
    bool lastSet = false;
    juce::dsp::Oversampling<float> oversampling { 2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, false };
    struct Dc { float x1, y1; } dc[2] {};
    float dcCoef = 0.999f;
    int preparedBlock = 0;
    juce::LinearSmoothedValue<float> outGain { 1.0f };
};

std::unique_ptr<BuiltinFx> createDynamicEq() { return std::make_unique<DynamicEqFx>(); }
std::unique_ptr<BuiltinFx> createTubeEq()    { return std::make_unique<TubeEqFx>(); }

} // namespace rs
