// =============================================================================
//  RENASOUND keys and bass:
//    Organ          91 phase-locked tonewheels (or divide-down combo organ voices),
//                   9 drawbars, percussion, key click, vibrato / chorus scanner,
//                   drive and the built-in Rotary
//    Electric Piano tine or reed, built from decaying partials through a pickup
//                   that barks when you play hard; tremolo / autopan
//    Acid Bass      saw / square into a squelchy ladder filter, accent and slide,
//                   and a 16-step sequencer that follows the song
// =============================================================================
#include "Effects.h"
#include "DspHelpers.h"
#include "UI/Faceplate.h"
#include <complex>

namespace rs
{

using namespace facekit;

namespace
{
    std::unique_ptr<juce::AudioParameterChoice> choiceP (const char* id, const char* name, const juce::StringArray& items, int def)
    {
        return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { id, 1 }, name, items, def);
    }
    std::unique_ptr<juce::AudioParameterInt> intP (const char* id, const char* name, int lo, int hi, int def)
    {
        return std::make_unique<juce::AudioParameterInt> (juce::ParameterID { id, 1 }, name, lo, hi, def);
    }
    std::unique_ptr<juce::AudioParameterBool> boolP (const char* id, const char* name, bool def)
    {
        return std::make_unique<juce::AudioParameterBool> (juce::ParameterID { id, 1 }, name, def);
    }
    inline float polyBlep (double t, double dt)
    {
        if (t < dt) { t /= dt; return (float) (t + t - t * t - 1.0); }
        if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
        return 0.0f;
    }
    inline float fastTanh (float x) { x = juce::jlimit (-3.0f, 3.0f, x); return x * (27.0f + x * x) / (27.0f + 9.0f * x * x); }
    void setParam (BuiltinFx& fx, const char* id, float v)
    {
        if (auto* p = fx.state.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (v));
    }
    bool isBlackKey (int n) { const int pc = ((n % 12) + 12) % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }
}

// =============================================================================
//  ORGAN
// =============================================================================
class OrganFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static constexpr int numWheels = 96;          // MIDI 24 .. 119
    static constexpr int firstWheel = 24;
    static constexpr const char* drawbarIds[9] { "db1", "db2", "db3", "db4", "db5", "db6", "db7", "db8", "db9" };
    static constexpr int footageSemis[9] { -12, 7, 0, 12, 19, 24, 28, 31, 36 };   // 16' 5 1/3' 8' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1'

    OrganFx() : BuiltinFx ("organ", "Organ", layout())
    {
        rotary = createRotary();
    }
    bool acceptsMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 1.0; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        const int defaults[9] { 8, 8, 8, 0, 0, 0, 0, 0, 0 };
        const char* names[9] { "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'" };
        for (int i = 0; i < 9; ++i) l.add (intP (drawbarIds[i], names[i], 0, 8, defaults[i]));
        l.add (choiceP ("model", "Organ", { "Tonewheel", "Combo F", "Combo V" }, 0));
        l.add (boolP ("perc", "Percussion", true));
        l.add (choiceP ("percharm", "Percussion Harmonic", { "Second", "Third" }, 1));
        l.add (choiceP ("percdecay", "Percussion Decay", { "Fast", "Slow" }, 0));
        l.add (choiceP ("percvol", "Percussion Volume", { "Normal", "Soft" }, 0));
        l.add (choiceP ("vibrato", "Vibrato / Chorus", { "Off", "V1", "V2", "V3", "C1", "C2", "C3" }, 6));
        l.add (floatParam ("click", "Key Click", Range (0, 100), 40, "%"));
        l.add (floatParam ("drive", "Drive", Range (0, 100), 25, "%"));
        l.add (floatParam ("tone", "Tone", Range (0, 100), 60, "%"));
        l.add (choiceP ("rotary", "Rotary", { "Off", "Slow", "Fast" }, 1));
        l.add (floatParam ("volume", "Volume", Range (-24, 6), -6, "dB"));
        return l;
    }

    // live readings for the panel
    std::array<std::atomic<float>, numWheels> wheelLevel {};
    std::array<std::atomic<bool>, 128> keyDown {};
    std::atomic<float> percNow { 0 }, vibNow { 0 };

    void prepareToPlay (double sr, int block) override
    {
        currentSampleRate = sr;
        for (int w = 0; w < numWheels; ++w) { phase[(size_t) w] = (w * 0.6180339887) - std::floor (w * 0.6180339887); amp[(size_t) w] = target[(size_t) w] = 0; }
        held.fill (false);
        percEnv = 0;
        scanner.assign (4096, 0.0f); scanPos = 0; scanPh = 0;
        clickEnv = 0; clickLp = clickHp = 0;
        toneL = 0;
        rotary->setPlayConfigDetails (2, 2, sr, block);
        rotary->prepareToPlay (sr, block);
        dirty = true;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        buffer.clear();
        const double sr = currentSampleRate;

        // drawbar levels: each step is 3 dB
        float bar[9];
        for (int i = 0; i < 9; ++i)
        {
            const int v = (int) param (drawbarIds[i]);
            bar[i] = v <= 0 ? 0.0f : std::pow (10.0f, -3.0f * (8 - v) / 20.0f);
        }
        const bool perc = param ("perc") > 0.5f;
        if (perc) bar[8] = 0.0f;   // as on the classic organs: percussion takes the 1' drawbar's place
        for (int i = 0; i < 9; ++i) if (! juce::approximatelyEqual (bar[i], lastBar[i])) { lastBar[i] = bar[i]; dirty = true; }
        const int model = (int) param ("model");
        const int percHarm = param ("percharm") > 0.5f ? 19 : 12;
        const float percDecay = (float) std::exp (-1.0 / ((param ("percdecay") > 0.5f ? 1.0 : 0.25) * sr / 4.6));
        const float percGain = (param ("percvol") > 0.5f ? 0.5f : 1.0f) * 0.9f;
        const float bodyGain = perc && param ("percvol") < 0.5f ? 0.7f : 1.0f;   // normal percussion pulls the rest down a little
        const float clickAmt = param ("click") * 0.01f;
        const float drive = param ("drive") * 0.01f;
        const float toneK = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * (1500.0f + 9000.0f * param ("tone") * 0.01f) / (float) sr);
        const float vol = juce::Decibels::decibelsToGain (param ("volume")) * expression;
        const float smooth = 1.0f - std::exp (-1.0f / (0.004f * (float) sr));   // keys reach full level in a few ms

        auto it = midi.cbegin();
        float peakPerc = 0;
        for (int i = 0; i < n; ++i)
        {
            while (it != midi.cend() && (*it).samplePosition <= i)
            {
                const auto m = (*it).getMessage();
                if (m.isNoteOn()) keyOn (m.getNoteNumber());
                else if (m.isNoteOff()) keyOff (m.getNoteNumber());
                else if (m.isControllerOfType (1)) modWheelFast = m.getControllerValue() >= 64;
                else if (m.isControllerOfType (11)) expression = 0.15f + 0.85f * m.getControllerValue() / 127.0f;
                else if (m.isAllNotesOff() || m.isAllSoundOff()) { held.fill (false); dirty = true; }
                ++it;
            }
            if (dirty) { retarget (bar); dirty = false; }

            // the wheels
            float out = 0, percOut = 0;
            for (int w = 0; w < numWheels; ++w)
            {
                auto& a = amp[(size_t) w];
                const float t = target[(size_t) w], pt = percTarget[(size_t) w] * percEnv;
                a += smooth * (t - a);
                if (a < 1.0e-5f && pt < 1.0e-5f) { advance (w, sr); continue; }
                const float s = wave (w, model, sr);
                out += a * s;
                percOut += pt * s;
            }
            percEnv *= percDecay;
            peakPerc = juce::jmax (peakPerc, percEnv);
            float x = out * bodyGain + percOut * percGain;

            // key click: a short, bright burst when contacts close or open
            if (clickEnv > 1.0e-4f)
            {
                const float noise = rnd.nextFloat() * 2.0f - 1.0f;
                clickHp = 0.6f * clickHp + 0.4f * noise;
                const float bright = noise - clickHp;
                x += bright * clickEnv * clickAmt * 0.35f;
                clickEnv *= clickDecay;
            }

            // combo organs: brighter, so the tone control works harder
            toneL += toneK * (x - toneL);
            x = model == 0 ? toneL : toneL * 0.8f;

            // vibrato / chorus scanner
            const int vib = (int) param ("vibrato");
            scanner[(size_t) scanPos] = x;
            if (vib > 0)
            {
                const double depthMs = (vib == 1 || vib == 4) ? 0.25 : (vib == 2 || vib == 5) ? 0.5 : 0.9;
                scanPh += 6.9 / sr; if (scanPh >= 1.0) scanPh -= 1.0;
                const double d = (1.2 + depthMs * (0.5 + 0.5 * std::sin (scanPh * juce::MathConstants<double>::twoPi))) * 0.001 * sr;
                double p = scanPos - d; while (p < 0) p += 4096;
                const int i0 = (int) p; const float f = (float) (p - i0);
                const float wet = scanner[(size_t) (i0 & 4095)] * (1 - f) + scanner[(size_t) ((i0 + 1) & 4095)] * f;
                x = vib >= 4 ? 0.5f * (x + wet) : wet;
            }
            scanPos = (scanPos + 1) & 4095;

            // drive: a warm, slightly uneven preamp
            if (drive > 0.001f)
            {
                const float g = 1.0f + drive * 9.0f;
                x = fastTanh (x * g + 0.08f * drive) - fastTanh (0.08f * drive);
                x /= std::sqrt (g) * 0.85f;
            }
            buffer.setSample (0, i, x * vol);
            if (buffer.getNumChannels() > 1) buffer.setSample (1, i, x * vol);
        }

        // the rotary speaker
        const int rot = (int) param ("rotary");
        if (rot > 0 && buffer.getNumChannels() >= 2)
        {
            const float speed = (rot == 2 || modWheelFast) ? 1.0f : 0.0f;
            if (speed != lastRotarySpeed) { setParam (*rotary, "speed", speed); lastRotarySpeed = speed; }   // only on a change
            juce::MidiBuffer none;
            rotary->processBlock (buffer, none);
        }

        for (int w = 0; w < numWheels; ++w) wheelLevel[(size_t) w] = amp[(size_t) w] + percTarget[(size_t) w] * percEnv;
        for (int k = 0; k < 128; ++k) keyDown[(size_t) k] = held[(size_t) k];
        percNow = peakPerc;
        vibNow = (float) scanPh;
        measureOut (buffer);
    }

    void loadRegistration (const juce::String& digits)
    {
        for (int i = 0; i < 9 && i < digits.length(); ++i) setParam (*this, drawbarIds[i], (float) (digits[i] - '0'));
    }
    BuiltinFx& rotaryFx() { return *rotary; }

    juce::AudioProcessorEditor* createEditor() override;

private:
    std::unique_ptr<BuiltinFx> rotary;
    float lastRotarySpeed = -1.0f;
    std::array<double, numWheels> phase {};
    std::array<float, numWheels> amp {}, target {}, percTarget {};
    std::array<bool, 128> held {};
    float lastBar[9] { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
    bool dirty = true, modWheelFast = false;
    float percEnv = 0, expression = 1.0f;
    std::vector<float> scanner;
    int scanPos = 0;
    double scanPh = 0;
    float clickEnv = 0, clickDecay = 0.99f, clickLp = 0, clickHp = 0, toneL = 0;
    juce::Random rnd;

    static int wheelFor (int note, int footage)
    {
        int p = note + footageSemis[footage];
        while (p < firstWheel) p += 12;                         // the lowest 16' notes fold back up
        while (p >= firstWheel + numWheels) p -= 12;           // the top wheels fold down
        return p - firstWheel;
    }
    void retarget (const float (&bar)[9])
    {
        target.fill (0.0f);
        percTarget.fill (0.0f);
        int keys = 0;
        for (int k = 0; k < 128; ++k)
        {
            if (! held[(size_t) k]) continue;
            ++keys;
            for (int f = 0; f < 9; ++f) if (bar[f] > 0) target[(size_t) wheelFor (k, f)] += bar[f] * 0.11f;
            int p = k + (param ("percharm") > 0.5f ? 19 : 12);
            while (p >= firstWheel + numWheels) p -= 12;
            while (p < firstWheel) p += 12;
            if (param ("perc") > 0.5f) percTarget[(size_t) (p - firstWheel)] += 0.25f;
        }
        juce::ignoreUnused (keys);
    }
    void keyOn (int note)
    {
        bool any = false;
        for (auto h : held) any = any || h;
        held[(size_t) note] = true;
        if (! any) percEnv = 1.0f;   // single-trigger: percussion only restarts when you play detached
        clickEnv = 1.0f;
        clickDecay = (float) std::exp (-1.0 / (0.004 * currentSampleRate));
        dirty = true;
    }
    void keyOff (int note)
    {
        held[(size_t) note] = false;
        clickEnv = juce::jmax (clickEnv, 0.4f);
        dirty = true;
    }
    void advance (int w, double sr)
    {
        auto& ph = phase[(size_t) w];
        ph += 440.0 * std::pow (2.0, (firstWheel + w - 69) / 12.0) / sr;
        if (ph >= 1.0) ph -= 1.0;
    }
    float wave (int w, int model, double sr)
    {
        auto& ph = phase[(size_t) w];
        const double inc = 440.0 * std::pow (2.0, (firstWheel + w - 69) / 12.0) / sr;
        float s;
        if (model == 0)
            s = (float) std::sin (ph * juce::MathConstants<double>::twoPi) * (1.0f - 0.02f * (float) std::cos (ph * juce::MathConstants<double>::twoPi * 3.0));   // a hint of wheel shape
        else if (model == 1)
        {
            // combo F: a reedy square from divide-down dividers
            s = (ph < 0.5 ? 1.0f : -1.0f) + polyBlep (ph, inc) - polyBlep (std::fmod (ph + 0.5, 1.0), inc);
            s *= 0.55f;
        }
        else
        {
            // combo V: a softer, sawtooth-ish voice
            s = (float) (2.0 * ph - 1.0) - polyBlep (ph, inc);
            s *= 0.6f;
        }
        ph += inc;
        if (ph >= 1.0) ph -= 1.0;
        return s;
    }
};

// ---- drawbars you pull out ----
class Drawbar : public juce::Component
{
public:
    Drawbar (BuiltinFx& fx, const char* id, juce::Colour handle, juce::String footage)
        : colour (handle), label (std::move (footage)),
          attach (*fx.state.getParameter (id), [this] (float v) { value = juce::roundToInt (v); repaint(); })
    {
        attach.sendInitialUpdate();
    }
    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        auto slot = r.withTrimmedTop (18).withTrimmedBottom (2);
        const float travel = slot.getHeight() - 34;
        // the slot
        g.setColour (juce::Colour (0xff0b0907)); g.fillRoundedRectangle (slot.withSizeKeepingCentre (14, slot.getHeight()), 4);
        // the shaft with its numbers, pulled out by the value
        const float out = travel * (float) value / 8.0f;
        auto shaft = juce::Rectangle<float> (slot.getCentreX() - 6, slot.getY(), 12, out + 4);
        g.setColour (juce::Colour (0xffe9e1cf)); g.fillRect (shaft);
        g.setFont (theme::font (8.5f, true));
        for (int k = 1; k <= value; ++k)
        {
            const float y = slot.getY() + travel * (float) k / 8.0f;
            g.setColour (juce::Colour (0xff3a2d22));
            g.drawText (juce::String (k), juce::Rectangle<float> (shaft.getX(), y - 10, 12, 10), juce::Justification::centred);
        }
        // the handle
        auto handle = juce::Rectangle<float> (slot.getCentreX() - 15, slot.getY() + out, 30, 30);
        juce::ColourGradient grad (colour.brighter (0.4f), handle.getX(), handle.getY(), colour.darker (0.5f), handle.getRight(), handle.getBottom(), false);
        g.setGradientFill (grad); g.fillRoundedRectangle (handle, 7);
        g.setColour (juce::Colours::black.withAlpha (0.4f)); g.drawRoundedRectangle (handle, 7, 1.2f);
        g.setColour (colour.getPerceivedBrightness() > 0.5f ? juce::Colour (0xff221a14) : juce::Colour (0xfff0e6d8));
        g.drawText (juce::String (value), handle, juce::Justification::centred);
        g.setColour (juce::Colour (0xffcbb89b)); g.setFont (theme::font (9.5f, true));
        g.drawText (label, r.withHeight (16), juce::Justification::centred);
    }
    void mouseDown (const juce::MouseEvent& e) override { attach.beginGesture(); dragStart = value; startY = e.position.y; }
    void mouseDrag (const juce::MouseEvent& e) override
    {
        const float travel = (float) getHeight() - 54;
        const int v = juce::jlimit (0, 8, dragStart + juce::roundToInt ((e.position.y - startY) / (travel / 8.0f)));
        if (v != value) attach.setValueAsPartOfGesture ((float) v);
    }
    void mouseUp (const juce::MouseEvent&) override { attach.endGesture(); }
    void mouseDoubleClick (const juce::MouseEvent&) override { attach.setValueAsCompleteGesture (value == 0 ? 8.0f : 0.0f); }
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& w) override
    {
        attach.setValueAsCompleteGesture ((float) juce::jlimit (0, 8, value + (w.deltaY < 0 ? 1 : -1)));
    }
private:
    juce::Colour colour;
    juce::String label;
    int value = 0, dragStart = 0;
    float startY = 0;
    juce::ParameterAttachment attach;
};

class OrganFace : public EffectFace
{
public:
    explicit OrganFace (OrganFx& f)
        : EffectFace (f, { KnobStyle::ChickenHead, juce::Colour (0xff1a1410), juce::Colour (0xfff0e6d8), juce::Colour (0xffe0a25a), juce::Colour (0xff3a2a1c),
                           juce::Colour (0xfff0e6d8), juce::Colour (0xffbfa98a), juce::Colour (0xff1c140e), juce::Colour (0xfff0e6d8) }), org (f)
    {
        const juce::Colour brown (0xff6b3c22), white (0xffefe9dc), black (0xff1d1a18);
        const juce::Colour cols[9] { brown, brown, white, white, black, white, black, black, white };
        const char* feet[9] { "16'", "5 1/3'", "8'", "4'", "2 2/3'", "2'", "1 3/5'", "1 1/3'", "1'" };
        for (int i = 0; i < 9; ++i) addAndMakeVisible (bars.add (new Drawbar (f, OrganFx::drawbarIds[i], cols[i], feet[i])));
        const auto lc = juce::Colour (0xffd9c6a5);
        knob ("click", "KEY CLICK", lc); knob ("drive", "DRIVE", lc); knob ("tone", "TONE", lc); knob ("volume", "VOLUME", lc);
        model = choice ("model"); vib = choice ("vibrato"); rot = choice ("rotary");
        percHarm = choice ("percharm"); percDecay = choice ("percdecay"); percVol = choice ("percvol");
        percBtn = toggle ("perc", "PERCUSSION");
        const std::pair<const char*, const char*> regs[] { { "Jazz", "888000000" }, { "Gospel", "888800000" }, { "Full", "888888888" },
                                                           { "Rock", "888000008" }, { "Ballad", "008600000" }, { "Flutes", "806000000" }, { "Bright", "008888800" } };
        for (auto [name, digits] : regs)
        {
            auto* b = presets.add (new juce::TextButton (name));
            const juce::String d (digits);
            b->setTooltip ("Drawbars " + d.substring (0, 2) + " " + d.substring (2, 6) + " " + d.substring (6));
            b->onClick = [this, d] { org.loadRegistration (d); };
            addAndMakeVisible (b);
            b->sendLookAndFeelChange();
        }
        setSize (980, 600);
        startTimerHz (30);
    }
    void paint (juce::Graphics& g) override
    {
        // dark walnut with a satin finish
        juce::ColourGradient wood (juce::Colour (0xff4a2a17), 0, 0, juce::Colour (0xff1e110a), 0, (float) getHeight(), false);
        g.setGradientFill (wood); g.fillAll();
        juce::Random rnd (11);
        for (int i = 0; i < 80; ++i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.04f + rnd.nextFloat() * 0.05f));
            const float y = rnd.nextFloat() * getHeight();
            g.drawLine (0, y, (float) getWidth(), y + rnd.nextFloat() * 24 - 12, 1.0f + rnd.nextFloat() * 2);
        }
        title (g, { 22, 10, 600, 30 }, "ORGAN", juce::String (org.param ("model") < 0.5f ? "91 TONEWHEELS" : "COMBO VOICES") + "  /  DRAWBARS  /  PERCUSSION  /  ROTARY",
               juce::Colour (0xfff0e6d8), juce::Colour (0xffe0a25a));
        // the drawbar panel
        g.setColour (juce::Colour (0xff120c08)); g.fillRoundedRectangle (barsArea.toFloat().expanded (8, 6), 8);

        // what you hear: the wheels lit up by how loud each one is
        auto a = wheelsArea.toFloat();
        g.setColour (juce::Colour (0xff0d0a07)); g.fillRoundedRectangle (a, 8);
        auto plot = a.reduced (10, 14);
        const float w = plot.getWidth() / OrganFx::numWheels;
        for (int i = 0; i < OrganFx::numWheels; ++i)
        {
            const float lvl = juce::jlimit (0.0f, 1.0f, std::sqrt (org.wheelLevel[(size_t) i].load() * 2.5f));
            auto bar = juce::Rectangle<float> (plot.getX() + i * w + 0.5f, plot.getBottom() - plot.getHeight() * lvl, w - 1, plot.getHeight() * lvl);
            g.setColour (juce::Colour (0xffe0a25a).withAlpha (0.25f + 0.75f * lvl));
            g.fillRect (bar);
            g.setColour (juce::Colour (0xff2a1d12)); g.fillRect (plot.getX() + i * w + 0.5f, plot.getBottom(), w - 1, 3.0f);
        }
        g.setColour (juce::Colour (0xffbfa98a)); g.setFont (theme::font (9.5f, true));
        g.drawText ("TONEWHEELS", a.reduced (10, 2), juce::Justification::topLeft);
        // percussion lamp and the scanner
        auto lamps = lampArea.toFloat();
        const float pc = org.percNow.load();
        g.setColour (juce::Colour (0xffff6a2a).withAlpha (0.15f + 0.85f * pc)); g.fillEllipse (lamps.removeFromLeft (14).withSizeKeepingCentre (12, 12));
        lamps.removeFromLeft (6);
        g.setColour (juce::Colour (0xffbfa98a)); g.drawText ("PERC", lamps.removeFromLeft (40), juce::Justification::centredLeft);
        const float sc = org.vibNow.load();
        g.setColour (juce::Colour (0xffe0a25a).withAlpha (org.param ("vibrato") > 0.5f ? 0.9f : 0.2f));
        g.fillEllipse (lamps.getX() + 4 + 30 * (0.5f + 0.5f * std::sin (sc * juce::MathConstants<float>::twoPi)), lamps.getCentreY() - 5, 10, 10);
        g.setColour (juce::Colour (0xffbfa98a)); g.drawText ("SCANNER", lamps.withTrimmedLeft (48), juce::Justification::centredLeft);
        g.setColour (juce::Colour (0xffbfa98a)); g.setFont (theme::font (10.0f, true));
        g.drawText ("ORGAN", modelLabel, juce::Justification::bottomLeft);
        g.drawText ("VIBRATO / CHORUS", vibLabel, juce::Justification::bottomLeft);
        g.drawText ("ROTARY (MOD WHEEL = FAST)", rotLabel, juce::Justification::bottomLeft);
        g.drawText ("REGISTRATIONS", presetLabel, juce::Justification::bottomLeft);
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 10);
        r.removeFromTop (40);
        auto top = r.removeFromTop (250);
        auto left = top.removeFromLeft (440);
        barsArea = left.reduced (8, 6);
        const int bw = barsArea.getWidth() / 9;
        auto br = barsArea;
        for (auto* b : bars) b->setBounds (br.removeFromLeft (bw));
        top.removeFromLeft (20);
        modelLabel = top.removeFromTop (16); model->setBounds (top.removeFromTop (28)); top.removeFromTop (6);
        auto percRow = top.removeFromTop (28);
        percBtn->setBounds (percRow.removeFromLeft (120)); percRow.removeFromLeft (6);
        percHarm->setBounds (percRow);
        top.removeFromTop (6);
        auto percRow2 = top.removeFromTop (28);
        percDecay->setBounds (percRow2.removeFromLeft (percRow2.getWidth() / 2 - 3)); percRow2.removeFromLeft (6);
        percVol->setBounds (percRow2);
        vibLabel = top.removeFromTop (18); vib->setBounds (top.removeFromTop (28));
        rotLabel = top.removeFromTop (18); rot->setBounds (top.removeFromTop (28));
        top.removeFromTop (8);
        lampArea = top.removeFromTop (20);
        r.removeFromTop (12);
        wheelsArea = r.removeFromTop (90);
        r.removeFromTop (8);
        presetLabel = r.removeFromTop (16);
        auto pr = r.removeFromTop (30);
        const int pw = pr.getWidth() / presets.size();
        for (auto* b : presets) b->setBounds (pr.removeFromLeft (pw).reduced (3, 0));
        r.removeFromTop (8);
        row (r, 0, 4);
    }
private:
    OrganFx& org;
    juce::OwnedArray<Drawbar> bars;
    juce::OwnedArray<juce::TextButton> presets;
    Switch* model = nullptr; Switch* vib = nullptr; Switch* rot = nullptr;
    Switch* percHarm = nullptr; Switch* percDecay = nullptr; Switch* percVol = nullptr;
    juce::TextButton* percBtn = nullptr;
    juce::Rectangle<int> barsArea, wheelsArea, lampArea, modelLabel, vibLabel, rotLabel, presetLabel;
};

juce::AudioProcessorEditor* OrganFx::createEditor() { return new OrganFace (*this); }

// =============================================================================
//  ELECTRIC PIANO
// =============================================================================
class EPianoFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static constexpr int maxVoices = 32, maxPartials = 7;
    EPianoFx() : BuiltinFx ("epiano", "Electric Piano", layout()) {}
    bool acceptsMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 6.0; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choiceP ("model", "Model", { "Tine", "Reed" }, 0));
        l.add (floatParam ("bell", "Bell", Range (0, 100), 45, "%"));
        l.add (floatParam ("bark", "Bark", Range (0, 100), 40, "%"));
        l.add (floatParam ("decay", "Decay", Range (20, 200), 100, "%"));
        l.add (floatParam ("release", "Release", skewed (20, 1500, 150), 160, "ms"));
        l.add (floatParam ("tone", "Tone", Range (0, 100), 55, "%"));
        l.add (floatParam ("tremrate", "Tremolo Rate", skewed (0.5f, 12, 4), 4.5f, "Hz"));
        l.add (floatParam ("tremdepth", "Tremolo Depth", Range (0, 100), 35, "%"));
        l.add (floatParam ("drive", "Drive", Range (0, 100), 15, "%"));
        l.add (floatParam ("velocity", "Velocity", Range (0, 100), 75, "%"));
        l.add (floatParam ("volume", "Volume", Range (-24, 6), -4, "dB"));
        return l;
    }

    std::array<std::atomic<float>, 128> keyLevel {};
    std::atomic<float> tremNow { 0 }, barkNow { 0 };

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        for (auto& v : voices) v.active = false;
        tremPh = 0; lpL = lpR = 0; lp2L = lp2R = 0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        buffer.clear();
        if (buffer.getNumChannels() < 2) return;
        const double sr = currentSampleRate;
        const bool reed = param ("model") > 0.5f;
        const float bark = param ("bark") * 0.01f;
        const float tone = 900.0f * std::pow (2.0f, param ("tone") * 0.01f * 4.5f);
        const float lk = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * juce::jmin (tone, (float) sr * 0.45f) / (float) sr);
        const double tremInc = param ("tremrate") / sr;
        const float tremDepth = param ("tremdepth") * 0.01f;
        const float drive = param ("drive") * 0.01f;
        const float vol = juce::Decibels::decibelsToGain (param ("volume"));
        auto it = midi.cbegin();
        float barkPeak = 0;
        for (int i = 0; i < n; ++i)
        {
            while (it != midi.cend() && (*it).samplePosition <= i)
            {
                const auto m = (*it).getMessage();
                if (m.isNoteOn()) noteOn (m.getNoteNumber(), m.getFloatVelocity(), reed);
                else if (m.isNoteOff()) noteOff (m.getNoteNumber());
                else if (m.isControllerOfType (64)) { pedal = m.getControllerValue() >= 64; if (! pedal) for (auto& v : voices) if (v.active && v.pedalled) damp (v); }
                else if (m.isAllNotesOff() || m.isAllSoundOff()) for (auto& v : voices) if (v.active) damp (v);
                ++it;
            }
            float sum = 0;
            for (auto& v : voices)
            {
                if (! v.active) continue;
                float x = 0;
                for (int p = 0; p < v.numPartials; ++p)
                {
                    auto& q = v.part[p];
                    const float re = q.re * q.cr - q.im * q.ci, im = q.re * q.ci + q.im * q.cr;
                    q.re = re; q.im = im;
                    x += im;
                }
                // the hammer: a tiny knock at the start
                if (v.knock > 1.0e-4f)
                {
                    const float noise = rnd.nextFloat() * 2.0f - 1.0f;
                    v.knockLp += 0.25f * (noise - v.knockLp);
                    x += v.knockLp * v.knock;
                    v.knock *= v.knockDecay;
                }
                // the pickup: harder playing pushes the tine or reed into its uneven, barking zone
                const float b = bark * v.drive;
                float y = reed ? fastTanh (x * (1.0f + 3.0f * b)) / (1.0f + 1.2f * b)
                               : x + b * 0.9f * (x * x - 0.5f * v.ampNow * v.ampNow) - b * 0.25f * x * x * x;
                v.ampNow = juce::jmax (std::abs (x), v.ampNow * 0.9995f);
                barkPeak = juce::jmax (barkPeak, b * v.ampNow);
                sum += y * v.gain;
                if (v.released)
                {
                    v.relCount++;
                    if (v.relCount > (int) (sr * 2.0) || v.ampNow < 1.0e-4f) v.active = false;
                }
                else if (v.ampNow < 1.0e-5f && v.age > (int) sr) v.active = false;
                ++v.age;
            }
            // tone, then drive
            lpL += lk * (sum - lpL);
            float x = lpL;
            if (drive > 0.001f) { const float g = 1.0f + drive * 6.0f; x = fastTanh (x * g) / std::sqrt (g) * 1.3f; }
            // tremolo: autopan for tines, amplitude for reeds
            tremPh += tremInc; if (tremPh >= 1.0) tremPh -= 1.0;
            const float s = (float) std::sin (tremPh * juce::MathConstants<double>::twoPi);
            float l, r;
            if (reed) { const float a = 1.0f - tremDepth * 0.5f * (1.0f + s); l = r = x * a; }
            else { const float pan = s * tremDepth; l = x * (1.0f - juce::jmax (0.0f, pan)); r = x * (1.0f + juce::jmin (0.0f, pan)); }
            buffer.setSample (0, i, l * vol);
            buffer.setSample (1, i, r * vol);
        }
        std::array<float, 128> lv {};
        for (auto& v : voices) if (v.active) lv[(size_t) v.note] = juce::jmax (lv[(size_t) v.note], v.ampNow * v.gain);
        for (int k = 0; k < 128; ++k) keyLevel[(size_t) k] = lv[(size_t) k];
        tremNow = (float) tremPh;
        barkNow = barkPeak;
        measureOut (buffer);
    }

    juce::AudioProcessorEditor* createEditor() override;

private:
    struct Partial { float re = 0, im = 0, cr = 1, ci = 0; };
    struct Voice
    {
        bool active = false, released = false, pedalled = false;
        int note = 60, numPartials = 0, age = 0, relCount = 0;
        Partial part[maxPartials];
        float gain = 1, drive = 0, ampNow = 0, knock = 0, knockDecay = 0.99f, knockLp = 0;
        juce::int64 order = 0;
    };
    std::array<Voice, maxVoices> voices;
    juce::int64 counter = 0;
    bool pedal = false;
    double tremPh = 0;
    float lpL = 0, lpR = 0, lp2L = 0, lp2R = 0;
    juce::Random rnd;

    static void setPartial (Partial& p, double freq, double tau, float amp, double sr, float phase = 0)
    {
        const double w = juce::MathConstants<double>::twoPi * juce::jmin (freq, sr * 0.45) / sr;
        const double d = std::exp (-1.0 / (juce::jmax (0.005, tau) * sr));
        p.cr = (float) (d * std::cos (w)); p.ci = (float) (d * std::sin (w));
        p.re = amp * std::cos (phase); p.im = amp * std::sin (phase);
        if (freq >= sr * 0.45) p.re = p.im = 0;
    }

    void noteOn (int note, float vel, bool reed)
    {
        // the same key again: the old strike is damped
        for (auto& v : voices) if (v.active && v.note == note && ! v.released) damp (v);
        Voice* v = nullptr;
        for (auto& x : voices) if (! x.active) { v = &x; break; }
        if (v == nullptr) { v = &voices[0]; for (auto& x : voices) if (x.order < v->order) v = &x; }
        *v = Voice();
        v->active = true; v->note = note; v->order = ++counter;
        const double sr = currentSampleRate;
        const double f0 = 440.0 * std::pow (2.0, (note - 69) / 12.0) * (1.0 + (rnd.nextFloat() - 0.5f) * 0.0008);   // each key a hair different
        const float velAmt = param ("velocity") * 0.01f;
        const float v1 = (1.0f - velAmt) * 0.6f + velAmt * vel;
        const double decayScale = param ("decay") * 0.01;
        const double tau = juce::jlimit (0.35, 9.0, 6.5 * std::pow (2.0, -(note - 36) / 22.0)) * decayScale * (reed ? 0.55 : 1.0);
        const float bell = param ("bell") * 0.01f;
        int k = 0;
        if (! reed)
        {
            setPartial (v->part[k++], f0, tau, 1.0f, sr);
            setPartial (v->part[k++], f0 * 2.0, tau * 0.45, 0.10f + 0.12f * vel, sr);
            setPartial (v->part[k++], f0 * 3.0, tau * 0.25, 0.03f + 0.05f * vel, sr);
            // the bell: the tine's own high overtone, louder when struck hard, gone quickly
            const double bellTau = juce::jlimit (0.04, 0.6, 0.45 * std::pow (2.0, -(note - 48) / 18.0));
            setPartial (v->part[k++], f0 * 7.03, bellTau, bell * (0.15f + 0.55f * vel * vel), sr);
            setPartial (v->part[k++], f0 * 10.1, bellTau * 0.5, bell * 0.18f * vel * vel, sr);
        }
        else
        {
            const float amps[6] { 1.0f, 0.42f, 0.30f, 0.17f, 0.10f, 0.06f };
            for (int h = 0; h < 6; ++h) setPartial (v->part[k++], f0 * (h + 1) * (1.0 + 0.0004 * h * h), tau / std::pow (h + 1.0, 0.8), amps[h] * (h == 0 ? 1.0f : (0.6f + 0.5f * vel)), sr);
            setPartial (v->part[k++], f0 * 4.4, 0.06, bell * 0.25f * vel * vel, sr);   // a little buzz on the attack
        }
        v->numPartials = k;
        v->gain = 0.22f * v1 * (reed ? 0.8f : 1.0f);
        v->drive = 0.35f + 0.65f * vel * vel;
        v->knock = 0.08f * vel;
        v->knockDecay = (float) std::exp (-1.0 / (0.003 * sr));
    }
    void noteOff (int note)
    {
        for (auto& v : voices)
            if (v.active && v.note == note && ! v.released)
            {
                if (pedal) { v.pedalled = true; continue; }
                damp (v);
            }
    }
    void damp (Voice& v)
    {
        // the damper: every partial dies away over the release time
        const double sr = currentSampleRate;
        const double rel = param ("release") * 0.001;
        for (int p = 0; p < v.numPartials; ++p)
        {
            auto& q = v.part[p];
            const double mag = std::sqrt ((double) q.cr * q.cr + (double) q.ci * q.ci);
            if (mag <= 0) continue;
            const double target = std::exp (-6.9 / (rel * sr * (p == 0 ? 1.0 : 0.6)));   // the release time is the time to fade 60 dB
            const double k = juce::jmin (1.0, target / mag);
            q.cr = (float) (q.cr * k); q.ci = (float) (q.ci * k);
        }
        v.released = true; v.relCount = 0;
        v.knock = juce::jmax (v.knock, 0.02f);   // the damper touching down
    }
};

class EPianoFace : public EffectFace
{
public:
    explicit EPianoFace (EPianoFx& f)
        : EffectFace (f, { KnobStyle::Vintage, juce::Colour (0xff2a1512), juce::Colour (0xfff4e9d8), juce::Colour (0xffd8524a), juce::Colour (0xff3b2420),
                           juce::Colour (0xfff4e9d8), juce::Colour (0xffc7a99a), juce::Colour (0xff1c0e0c), juce::Colour (0xfff4e9d8) }), ep (f)
    {
        const auto lc = juce::Colour (0xffe9d2c2);
        for (auto [id, name] : { std::pair { "bell", "BELL" }, { "bark", "BARK" }, { "decay", "DECAY" }, { "release", "RELEASE" }, { "tone", "TONE" },
                                 { "tremrate", "TREM RATE" }, { "tremdepth", "TREM DEPTH" }, { "drive", "DRIVE" }, { "velocity", "VELOCITY" }, { "volume", "VOLUME" } })
            knob (id, name, lc);
        model = choice ("model");
        setSize (920, 570);
        startTimerHz (40);
    }
    void paint (juce::Graphics& g) override
    {
        const bool reed = ep.param ("model") > 0.5f;
        // a lacquered lid: deep red for tines, cream for reeds
        const auto body = reed ? juce::Colour (0xff3a2f27) : juce::Colour (0xff3a1714);
        juce::ColourGradient bg (body.brighter (0.15f), 0, 0, body.darker (0.6f), 0, (float) getHeight(), false);
        g.setGradientFill (bg); g.fillAll();
        title (g, { 22, 10, 600, 30 }, "ELECTRIC PIANO", reed ? "REEDS  /  ELECTROSTATIC PICKUP  /  TREMOLO" : "TINES  /  MAGNETIC PICKUPS  /  AUTOPAN",
               juce::Colour (0xfff4e9d8), juce::Colour (0xffd8524a));
        // the harp: a tine (or reed) per key over three octaves, vibrating when it sounds
        auto a = harp.toFloat();
        g.setColour (juce::Colour (0xff120807)); g.fillRoundedRectangle (a, 8);
        auto plot = a.reduced (12, 16);
        const int lo = 36, hi = 96;
        const float w = plot.getWidth() / (float) (hi - lo + 1);
        const float t = (float) juce::Time::getMillisecondCounterHiRes() * 0.001f;
        for (int k = lo; k <= hi; ++k)
        {
            const float lvl = juce::jlimit (0.0f, 1.0f, std::sqrt (ep.keyLevel[(size_t) k].load() * 6.0f));
            const float len = plot.getHeight() * (0.95f - 0.6f * (float) (k - lo) / (float) (hi - lo));   // low tines are long
            const float x = plot.getX() + (k - lo + 0.5f) * w;
            juce::Path p;
            p.startNewSubPath (x, plot.getBottom());
            const int segs = 10;
            for (int s = 1; s <= segs; ++s)
            {
                const float fy = (float) s / segs;
                const float wob = lvl * 3.5f * fy * fy * std::sin (t * (8.0f + (k - lo) * 0.6f) + k);
                p.lineTo (x + wob, plot.getBottom() - len * fy);
            }
            g.setColour ((reed ? juce::Colour (0xffd9b26f) : juce::Colour (0xffc9ccd2)).withAlpha (0.35f + 0.65f * lvl));
            g.strokePath (p, juce::PathStrokeType (reed ? 2.6f : 1.6f));
            if (lvl > 0.02f) { g.setColour (juce::Colour (0xffd8524a).withAlpha (lvl)); g.fillEllipse (x - 3, plot.getBottom() - len - 3, 6, 6); }
        }
        // the pickup rail and the tremolo / bark lamps
        g.setColour (juce::Colour (0xff4a3a34)); g.fillRect (plot.getX(), plot.getBottom() + 2, plot.getWidth(), 6.0f);
        const float trem = ep.tremNow.load();
        const float depth = ep.param ("tremdepth") * 0.01f;
        auto lamp = [&] (juce::Rectangle<float> r, float v, const juce::String& txt)
        {
            g.setColour (juce::Colour (0xffd8524a).withAlpha (0.15f + 0.85f * juce::jlimit (0.0f, 1.0f, v)));
            g.fillEllipse (r.removeFromLeft (12).withSizeKeepingCentre (11, 11));
            g.setColour (juce::Colour (0xffc7a99a)); g.setFont (theme::font (10.0f, true));
            g.drawText (txt, r.withTrimmedLeft (6), juce::Justification::centredLeft);
        };
        auto lr = lamps.toFloat();
        lamp (lr.removeFromLeft (120), depth * (0.5f + 0.5f * std::sin (trem * juce::MathConstants<float>::twoPi)), reed ? "TREMOLO" : "AUTOPAN");
        lamp (lr.removeFromLeft (120), ep.barkNow.load() * 8.0f, "BARK");
    }
    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 10);
        r.removeFromTop (40);
        auto head = r.removeFromTop (30);
        model->setBounds (head.removeFromLeft (220));
        lamps = head.withTrimmedLeft (20);
        r.removeFromTop (8);
        harp = r.removeFromTop (200);
        r.removeFromTop (12);
        auto r1 = r.removeFromTop (r.getHeight() / 2 - 6);
        r.removeFromTop (12);
        row (r1, 0, 5);
        row (r, 5, 10);
    }
private:
    EPianoFx& ep;
    Switch* model = nullptr;
    juce::Rectangle<int> harp, lamps;
};

juce::AudioProcessorEditor* EPianoFx::createEditor() { return new EPianoFace (*this); }

// =============================================================================
//  ACID BASS
// =============================================================================
class AcidBassFx : public BuiltinFx
{
public:
    using BuiltinFx::processBlock;
    static constexpr int steps = 16;
    AcidBassFx() : BuiltinFx ("acid", "Acid Bass", layout())
    {
        // a pattern to start with
        const int notes[steps] { 0, 0, 12, 0, 3, 0, 10, 0, 0, 7, 0, 12, 3, 0, 5, 0 };
        const int acc[steps]   { 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0 };
        const int sld[steps]   { 0, 0, 1, 0, 0, 0, 1, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        for (int i = 0; i < steps; ++i) { stepNote[(size_t) i] = notes[i] % 12; stepOct[(size_t) i] = notes[i] >= 12 ? 1 : 0; stepAccent[(size_t) i] = acc[i]; stepSlide[(size_t) i] = sld[i]; stepGate[(size_t) i] = 1; }
        stepGate[5] = 0; stepGate[13] = 0;
    }
    bool acceptsMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 0.5; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (choiceP ("wave", "Wave", { "Saw", "Square" }, 0));
        l.add (floatParam ("tune", "Tune", Range (-12, 12, 1), 0, "st"));
        l.add (floatParam ("cutoff", "Cutoff", Range (0, 100), 30, "%"));
        l.add (floatParam ("reso", "Resonance", Range (0, 100), 70, "%"));
        l.add (floatParam ("envmod", "Env Mod", Range (0, 100), 55, "%"));
        l.add (floatParam ("decay", "Decay", skewed (100, 2500, 400), 450, "ms"));
        l.add (floatParam ("accent", "Accent", Range (0, 100), 60, "%"));
        l.add (floatParam ("drive", "Drive", Range (0, 100), 25, "%"));
        l.add (floatParam ("volume", "Volume", Range (-24, 6), -6, "dB"));
        l.add (choiceP ("seq", "Sequencer", { "Off (play keys)", "On (with the song)" }, 1));
        l.add (intP ("length", "Pattern Length", 1, steps, steps));
        l.add (intP ("root", "Root", 24, 60, 36));
        return l;
    }

    // the pattern (edited on the panel, saved with the project)
    std::array<std::atomic<int>, steps> stepNote {}, stepOct {}, stepAccent {}, stepSlide {}, stepGate {};
    std::atomic<int> nowStep { -1 };
    std::atomic<bool> preview { false };
    std::atomic<float> cutoffNow { 0 }, envNow { 0 };

    void getStateInformation (juce::MemoryBlock& mb) override
    {
        juce::String s;
        for (int i = 0; i < steps; ++i) s << stepNote[(size_t) i].load() << ":" << stepOct[(size_t) i].load() << ":" << stepAccent[(size_t) i].load() << ":" << stepSlide[(size_t) i].load() << ":" << stepGate[(size_t) i].load() << ";";
        state.state.setProperty ("pattern", s, nullptr);
        BuiltinFx::getStateInformation (mb);
    }
    void setStateInformation (const void* d, int size) override
    {
        BuiltinFx::setStateInformation (d, size);
        juce::StringArray st; st.addTokens (state.state.getProperty ("pattern").toString(), ";", "");
        for (int i = 0; i < steps && i < st.size(); ++i)
        {
            juce::StringArray f; f.addTokens (st[i], ":", "");
            if (f.size() < 5) continue;
            stepNote[(size_t) i] = juce::jlimit (0, 12, f[0].getIntValue()); stepOct[(size_t) i] = juce::jlimit (-1, 1, f[1].getIntValue());
            stepAccent[(size_t) i] = f[2].getIntValue() != 0; stepSlide[(size_t) i] = f[3].getIntValue() != 0; stepGate[(size_t) i] = f[4].getIntValue() != 0;
        }
    }

    void prepareToPlay (double sr, int) override
    {
        currentSampleRate = sr;
        ph = 0; freq = target = 55; gate = false; ampEnv = 0; filtEnv = 0; accentCharge = 0;
        std::memset (ld, 0, sizeof (ld)); lastOut = 0;
        lastStep = -1; heldNotes.clear(); heldNotes.ensureStorageAllocated (128); freeClock = 0;
    }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        const int n = buffer.getNumSamples();
        buffer.clear();
        const double sr = currentSampleRate;
        const bool seqOn = param ("seq") > 0.5f;
        const int len = (int) param ("length");
        double bpm = 120, ppq = 0; bool playing = false;
        if (auto* ph2 = getPlayHead())
            if (auto pos = ph2->getPosition())
            {
                if (auto b = pos->getBpm()) bpm = *b;
                if (auto p = pos->getPpqPosition()) ppq = *p;
                playing = pos->getIsPlaying();
            }
        const bool running = seqOn && (playing || preview.load());
        if (! playing && preview.load()) ppq = freeClock;
        const double ppqPerSample = bpm / 60.0 / sr;

        const float decayS = param ("decay") * 0.001f;
        const float accentAmt = param ("accent") * 0.01f;
        const float envmod = param ("envmod") * 0.01f;
        const float cut = 40.0f * std::pow (2.0f, param ("cutoff") * 0.01f * 7.0f);   // ~40 Hz .. 5 kHz
        const float reso = param ("reso") * 0.01f;
        const float drive = param ("drive") * 0.01f;
        const float vol = juce::Decibels::decibelsToGain (param ("volume"));
        const bool square = param ("wave") > 0.5f;
        const double tune = param ("tune");
        const float filtDecay = (float) std::exp (-1.0 / (decayS * sr / 4.6));
        const float accDecay = (float) std::exp (-1.0 / (0.2 * sr / 4.6));
        const float ampDecay = (float) std::exp (-1.0 / (3.0 * sr / 4.6));
        const float relK = (float) std::exp (-1.0 / (0.008 * sr));
        const float slideK = (float) (1.0 - std::exp (-1.0 / (0.06 * sr / 4.6)));   // ~60 ms glide

        auto it = midi.cbegin();
        float envPeak = 0, cutPeak = 0;
        for (int i = 0; i < n; ++i)
        {
            while (it != midi.cend() && (*it).samplePosition <= i)
            {
                const auto m = (*it).getMessage();
                if (m.isNoteOn())
                {
                    heldNotes.addIfNotAlreadyThere (m.getNoteNumber());
                    if (! seqOn)
                    {
                        // legato (another key still down) slides; hard velocity accents
                        const bool legato = heldNotes.size() > 1 && gate;
                        trigger (m.getNoteNumber() + tune, m.getVelocity() >= 100, legato);
                    }
                }
                else if (m.isNoteOff())
                {
                    heldNotes.removeFirstMatchingValue (m.getNoteNumber());
                    if (! seqOn)
                    {
                        if (heldNotes.isEmpty()) gate = false;
                        else trigger (heldNotes.getLast() + tune, false, true);
                    }
                }
                else if (m.isAllNotesOff() || m.isAllSoundOff()) { heldNotes.clearQuick(); gate = false; }
                ++it;
            }

            if (running)
            {
                const double pos = ppq + i * ppqPerSample;
                const double stepPos = pos * 4.0;   // sixteenths
                const int step = (int) std::floor (stepPos) % len;
                const double within = stepPos - std::floor (stepPos);
                if (step != lastStep)
                {
                    lastStep = step;
                    nowStep = step;
                    const int prev = (step + len - 1) % len;
                    if (stepGate[(size_t) step].load())
                    {
                        // a held key transposes the pattern (from the root)
                        const int base = (int) param ("root") + (heldNotes.isEmpty() ? 0 : heldNotes.getLast() - 48);
                        const int note = base + stepNote[(size_t) step].load() + 12 * stepOct[(size_t) step].load();
                        const bool slideIn = stepSlide[(size_t) prev].load() && stepGate[(size_t) prev].load() && gate;
                        trigger (note + tune, stepAccent[(size_t) step].load() != 0, slideIn);
                    }
                    else gate = false;
                }
                // the gate closes halfway through a step unless this step slides into the next
                if (within > 0.5 && ! stepSlide[(size_t) step].load()) gate = false;
            }
            else if (seqOn && lastStep >= 0) { gate = false; lastStep = -1; nowStep = -1; }

            // pitch: glide when sliding
            freq += (sliding ? slideK : 1.0f) * (target - freq);
            const double inc = freq / sr;
            float osc;
            if (! square) osc = (float) (2.0 * ph - 1.0) - polyBlep (ph, inc);
            else osc = (ph < 0.5 ? 1.0f : -1.0f) + polyBlep (ph, inc) - polyBlep (std::fmod (ph + 0.5, 1.0), inc);
            ph += inc; if (ph >= 1.0) ph -= 1.0;

            // envelopes
            if (gate) ampEnv = 1.0f - (1.0f - ampEnv) * 0.995f, ampEnv *= ampDecay; else ampEnv *= relK;
            filtEnv *= accentNow ? accDecay : filtDecay;
            accentCharge *= 0.99995f;
            envPeak = juce::jmax (envPeak, filtEnv);

            // the filter: four poles, a saturating feedback loop that squelches
            const float accBoost = accentNow ? accentAmt * (0.6f * filtEnv + 0.5f * accentCharge) : 0.0f;
            const float fc = juce::jlimit (30.0f, (float) sr * 0.42f, cut * std::pow (2.0f, (envmod * 4.0f) * filtEnv + accBoost * 2.0f));
            cutPeak = juce::jmax (cutPeak, fc);
            const float g = std::tan (juce::MathConstants<float>::pi * fc / (float) sr);
            const float G = g / (1.0f + g);
            const float k = reso * 4.1f;
            float u = fastTanh ((osc * 0.6f - k * lastOut) * 1.2f);
            for (int s = 0; s < 4; ++s)
            {
                const float v = (u - ld[s]) * G;
                const float y = v + ld[s];
                ld[s] = y + v;
                u = s == 0 ? y : fastTanh (y * 1.05f) / 1.05f;   // each stage saturates a little: the diode-ladder bite
            }
            lastOut = u;
            float y = u * (1.0f + k * 0.35f);
            float amp = ampEnv * (accentNow ? 1.0f + accentAmt * 0.8f : 0.75f);
            y *= amp;
            if (drive > 0.001f) { const float dg = 1.0f + drive * 12.0f; y = fastTanh (y * dg) / std::pow (dg, 0.6f); }
            const float out = y * vol;
            buffer.setSample (0, i, out);
            if (buffer.getNumChannels() > 1) buffer.setSample (1, i, out);
        }
        if (! playing && preview.load()) freeClock += n * ppqPerSample;
        envNow = envPeak;
        cutoffNow = cutPeak;
        measureOut (buffer);
    }

    juce::AudioProcessorEditor* createEditor() override;

private:
    double ph = 0, freqD = 0;
    float freq = 55, target = 55;
    bool gate = false, sliding = false, accentNow = false;
    float ampEnv = 0, filtEnv = 0, accentCharge = 0;
    float ld[4] {}, lastOut = 0;
    int lastStep = -1;
    double freeClock = 0;
    juce::Array<int> heldNotes;

    void trigger (double note, bool accent, bool slide)
    {
        target = (float) (440.0 * std::pow (2.0, (note - 69.0) / 12.0));
        sliding = slide;
        if (! slide)
        {
            freq = target;
            filtEnv = 1.0f;
            accentNow = accent;
            if (accent) accentCharge = juce::jmin (1.0f, accentCharge + 0.5f);   // accents close together build up
            if (! gate) ampEnv = juce::jmax (ampEnv, 0.0f);
        }
        gate = true;
    }
};

class AcidBassFace : public EffectFace
{
public:
    explicit AcidBassFace (AcidBassFx& f)
        : EffectFace (f, { KnobStyle::Neon, juce::Colour (0xff0d0f0b), juce::Colour (0xffc6ff3d), juce::Colour (0xffc6ff3d), juce::Colour (0xff22281a),
                           juce::Colour (0xffeaffc8), juce::Colour (0xff8fa67a), juce::Colour (0xff080906), juce::Colour (0xffeaffc8) }), ab (f)
    {
        const auto lc = juce::Colour (0xffb8d39a);
        for (auto [id, name] : { std::pair { "tune", "TUNE" }, { "cutoff", "CUTOFF" }, { "reso", "RESONANCE" }, { "envmod", "ENV MOD" },
                                 { "decay", "DECAY" }, { "accent", "ACCENT" }, { "drive", "DRIVE" }, { "volume", "VOLUME" } })
            knob (id, name, lc, 58);
        wave = choice ("wave"); seq = choice ("seq");
        playBtn.setButtonText ("PLAY PATTERN");
        playBtn.setClickingTogglesState (true);
        playBtn.setTooltip ("Hear the pattern without playing the song");
        playBtn.onClick = [this] { ab.preview = playBtn.getToggleState(); };
        addAndMakeVisible (playBtn);
        playBtn.sendLookAndFeelChange();
        for (auto [txt, fn] : { std::pair<const char*, int> { "CLEAR", 0 }, { "RANDOM", 1 }, { "SHIFT >", 2 } })
        {
            auto* b = tools.add (new juce::TextButton (txt));
            b->onClick = [this, fn] { patternTool (fn); };
            addAndMakeVisible (b);
            b->sendLookAndFeelChange();
        }
        lengthKnob = knob ("length", "LENGTH", lc, 50);
        rootKnob = knob ("root", "ROOT", lc, 50);
        setSize (1000, 590);
        startTimerHz (30);
    }
    ~AcidBassFace() override { ab.preview = false; }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (juce::Colour (0xff0b0d09));
        // a faint acid-green grid
        g.setColour (juce::Colour (0xffc6ff3d).withAlpha (0.035f));
        for (int x = 0; x < getWidth(); x += 16) g.drawVerticalLine (x, 0, (float) getHeight());
        title (g, { 22, 10, 600, 30 }, "ACID BASS", "SAW / SQUARE  /  4-POLE SQUELCH  /  ACCENT & SLIDE  /  16 STEPS",
               juce::Colour (0xffeaffc8), juce::Colour (0xffc6ff3d));
        // the filter, live: its cutoff and the envelope
        auto a = scope.toFloat();
        g.setColour (juce::Colour (0xff050604)); g.fillRoundedRectangle (a, 8);
        auto plot = a.reduced (10, 10);
        const float fc = ab.cutoffNow.load();
        const float reso = ab.param ("reso") * 0.01f;
        juce::Path curve;
        for (int x = 0; x <= (int) plot.getWidth(); x += 2)
        {
            const double f = 20.0 * std::pow (1000.0, x / plot.getWidth());
            const double w = f / juce::jmax (30.0f, fc);
            const std::complex<double> s1 (1.0, w);
            const double k = reso * 4.1;
            const double mag = (1.0 + k * 0.35) / std::abs (s1 * s1 * s1 * s1 + k);
            const float db = juce::Decibels::gainToDecibels ((float) mag, -60.0f);
            const float y = plot.getCentreY() - db / 30.0f * plot.getHeight() * 0.5f;
            if (x == 0) curve.startNewSubPath (plot.getX(), y); else curve.lineTo (plot.getX() + x, juce::jlimit (plot.getY(), plot.getBottom(), y));
        }
        g.setColour (juce::Colour (0xffc6ff3d).withAlpha (0.25f)); g.strokePath (curve, juce::PathStrokeType (6.0f));
        g.setColour (juce::Colour (0xffc6ff3d)); g.strokePath (curve, juce::PathStrokeType (2.0f));
        g.setColour (juce::Colour (0xff8fa67a)); g.setFont (theme::font (10.0f, true));
        g.drawText ("FILTER  " + juce::String (juce::roundToInt (fc)) + " Hz", plot, juce::Justification::topRight);

        // the step grid
        const int len = (int) ab.param ("length");
        const int now = ab.nowStep.load();
        auto gr = grid.toFloat();
        g.setColour (juce::Colour (0xff050604)); g.fillRoundedRectangle (gr, 8);
        const float cw = colW(), keyW = 34;
        auto notesArea = noteArea();
        // the note rows: C at the bottom to C an octave up
        for (int r = 0; r <= 12; ++r)
        {
            const float y = notesArea.getBottom() - (r + 1) * rowH();
            const bool blk = isBlackKey (r);
            g.setColour (blk ? juce::Colour (0xff141811) : juce::Colour (0xff1b2016));
            g.fillRect (notesArea.getX(), y, notesArea.getWidth(), rowH() - 1);
            g.setColour (juce::Colour (0xff8fa67a)); g.setFont (theme::font (9.0f, true));
            static const char* nm[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B", "C" };
            g.drawText (nm[r], juce::Rectangle<float> (gr.getX() + 6, y, keyW - 8, rowH()), juce::Justification::centredLeft);
        }
        for (int s = 0; s < AcidBassFx::steps; ++s)
        {
            const float x = notesArea.getX() + s * cw;
            const bool inLen = s < len;
            if (s % 4 == 0) { g.setColour (juce::Colour (0xffc6ff3d).withAlpha (0.12f)); g.drawVerticalLine ((int) x, gr.getY() + 6, gr.getBottom() - 6); }
            if (s == now) { g.setColour (juce::Colour (0xffc6ff3d).withAlpha (0.12f)); g.fillRect (x, gr.getY() + 4, cw, gr.getHeight() - 8); }
            const bool on = ab.stepGate[(size_t) s].load() != 0;
            const int note = ab.stepNote[(size_t) s].load();
            const auto c = juce::Colour (0xffc6ff3d).withMultipliedAlpha (inLen ? 1.0f : 0.25f);
            if (on)
            {
                const float y = notesArea.getBottom() - (note + 1) * rowH();
                g.setColour (c.withMultipliedAlpha (s == now ? 1.0f : 0.8f));
                g.fillRoundedRectangle (x + 2, y + 1, cw - 4, rowH() - 3, 3);
                if (ab.stepSlide[(size_t) s].load())   // a slide: a line into the next step
                {
                    g.setColour (c.withMultipliedAlpha (0.6f));
                    g.fillRect (x + cw - 4, y + rowH() * 0.5f - 1.5f, 8.0f, 3.0f);
                }
            }
            // the rows under the notes: octave, accent, slide, rest
            auto toggleCell = [&] (int rowIdx, bool lit, const juce::String& txt)
            {
                auto cell = juce::Rectangle<float> (x + 2, rowY (rowIdx) + 2, cw - 4, rowH2() - 4);
                g.setColour (lit ? c : juce::Colour (0xff1b2016));
                g.fillRoundedRectangle (cell, 3);
                g.setColour (lit ? juce::Colour (0xff0b0d09) : juce::Colour (0xff55644a));
                g.setFont (theme::font (9.0f, true));
                g.drawText (txt, cell, juce::Justification::centred);
            };
            const int oct = ab.stepOct[(size_t) s].load();
            toggleCell (0, oct != 0, oct > 0 ? "UP" : oct < 0 ? "DN" : "-");
            toggleCell (1, ab.stepAccent[(size_t) s].load() != 0, "ACC");
            toggleCell (2, ab.stepSlide[(size_t) s].load() != 0, "SLD");
            toggleCell (3, ! on, on ? juce::String (s + 1) : "REST");
        }
        g.setColour (juce::Colour (0xff8fa67a)); g.setFont (theme::font (9.0f, true));
        const char* rowNames[] { "OCT", "ACC", "SLIDE", "STEP" };
        for (int r = 0; r < 4; ++r) g.drawText (rowNames[r], juce::Rectangle<float> (gr.getX() + 4, rowY (r), keyW - 4, rowH2()), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 10);
        r.removeFromTop (40);
        auto head = r.removeFromTop (28);
        wave->setBounds (head.removeFromLeft (180)); head.removeFromLeft (12);
        seq->setBounds (head.removeFromLeft (330)); head.removeFromLeft (12);
        playBtn.setBounds (head.removeFromLeft (130));
        head.removeFromLeft (12);
        for (auto* b : tools) { b->setBounds (head.removeFromLeft (76)); head.removeFromLeft (6); }
        r.removeFromTop (10);
        auto mid = r.removeFromTop (310);
        auto side = mid.removeFromRight (190);
        scope = side.removeFromTop (150);
        side.removeFromTop (10);
        lengthKnob->setBounds (side.removeFromLeft (95).withHeight (110));
        rootKnob->setBounds (side.withHeight (110));
        mid.removeFromRight (10);
        grid = mid;
        r.removeFromTop (12);
        row (r, 0, 8);
    }

    void mouseDown (const juce::MouseEvent& e) override { edit (e.position, true); }
    void mouseDrag (const juce::MouseEvent& e) override { edit (e.position, false); }

private:
    AcidBassFx& ab;
    Switch* wave = nullptr; Switch* seq = nullptr;
    Knob* lengthKnob = nullptr; Knob* rootKnob = nullptr;
    juce::TextButton playBtn;
    juce::OwnedArray<juce::TextButton> tools;
    juce::Rectangle<int> grid, scope;
    int dragRow = -1;
    bool dragValue = false;

    float colW() const { return (grid.getWidth() - 34 - 10) / (float) AcidBassFx::steps; }
    juce::Rectangle<float> noteArea() const
    {
        auto gr = grid.toFloat().reduced (5, 6);
        gr.removeFromLeft (34 - 5);
        return gr.withTrimmedBottom (4 * rowH2() + 6);
    }
    float rowH() const { return noteArea().getHeight() / 13.0f; }
    float rowH2() const { return 24.0f; }
    float rowY (int r) const { return noteArea().getBottom() + 6 + r * rowH2(); }

    void edit (juce::Point<float> p, bool first)
    {
        auto na = noteArea();
        if (p.x < na.getX() || p.x >= na.getRight()) return;
        const int s = juce::jlimit (0, AcidBassFx::steps - 1, (int) ((p.x - na.getX()) / colW()));
        if (p.y >= na.getY() && p.y < na.getBottom())
        {
            if (first) dragRow = -2;
            if (dragRow != -2) return;
            const int note = juce::jlimit (0, 12, (int) ((na.getBottom() - p.y) / rowH()));
            ab.stepNote[(size_t) s] = note;
            ab.stepGate[(size_t) s] = 1;
        }
        else
        {
            for (int r = 0; r < 4; ++r)
                if (p.y >= rowY (r) && p.y < rowY (r) + rowH2())
                {
                    if (first)
                    {
                        dragRow = r;
                        if (r == 0) { auto& o = ab.stepOct[(size_t) s]; o = o.load() == 0 ? 1 : o.load() == 1 ? -1 : 0; dragRow = -1; }
                        else
                        {
                            auto& v = r == 1 ? ab.stepAccent[(size_t) s] : r == 2 ? ab.stepSlide[(size_t) s] : ab.stepGate[(size_t) s];
                            v = v.load() ? 0 : 1;
                            dragValue = v.load() != 0;
                        }
                    }
                    else if (r == dragRow && r > 0)
                    {
                        auto& v = r == 1 ? ab.stepAccent[(size_t) s] : r == 2 ? ab.stepSlide[(size_t) s] : ab.stepGate[(size_t) s];
                        v = dragValue ? 1 : 0;
                    }
                }
        }
        repaint();
    }
    void patternTool (int fn)
    {
        juce::Random rnd;
        for (int s = 0; s < AcidBassFx::steps; ++s)
        {
            if (fn == 0) { ab.stepNote[(size_t) s] = 0; ab.stepOct[(size_t) s] = 0; ab.stepAccent[(size_t) s] = 0; ab.stepSlide[(size_t) s] = 0; ab.stepGate[(size_t) s] = 1; }
            if (fn == 1)
            {
                static const int scale[] { 0, 0, 0, 3, 5, 7, 10, 12 };   // minor-ish, heavy on the root
                ab.stepNote[(size_t) s] = scale[rnd.nextInt (8)] % 13;
                ab.stepOct[(size_t) s] = rnd.nextFloat() < 0.15f ? 1 : 0;
                ab.stepAccent[(size_t) s] = rnd.nextFloat() < 0.3f;
                ab.stepSlide[(size_t) s] = rnd.nextFloat() < 0.25f;
                ab.stepGate[(size_t) s] = rnd.nextFloat() < 0.82f;
            }
        }
        if (fn == 2)
        {
            auto rot = [] (std::array<std::atomic<int>, AcidBassFx::steps>& a) { const int last = a[AcidBassFx::steps - 1].load(); for (int s = AcidBassFx::steps - 1; s > 0; --s) a[(size_t) s] = a[(size_t) s - 1].load(); a[0] = last; };
            rot (ab.stepNote); rot (ab.stepOct); rot (ab.stepAccent); rot (ab.stepSlide); rot (ab.stepGate);
        }
        repaint();
    }
};

juce::AudioProcessorEditor* AcidBassFx::createEditor() { return new AcidBassFace (*this); }

// =============================================================================
std::unique_ptr<BuiltinFx> createOrgan()    { return std::make_unique<OrganFx>(); }
std::unique_ptr<BuiltinFx> createEPiano()   { return std::make_unique<EPianoFx>(); }
std::unique_ptr<BuiltinFx> createAcidBass() { return std::make_unique<AcidBassFx>(); }

} // namespace rs
