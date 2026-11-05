#include "Drums.h"
#include "UI/Theme.h"

namespace rs
{

// =============================================================================
//  Pad layout: General MIDI drum notes
// =============================================================================
namespace
{
    struct PadDef { const char* name; int note; int choke; float pan; };
    const PadDef padDefs[DrumMachine::numPads] = {
        { "Kick",       36, 0,  0.00f }, { "Snare",      38, 0,  0.00f }, { "Clap",       39, 0,  0.05f }, { "Rim",        37, 0, -0.10f },
        { "Closed Hat", 42, 1,  0.25f }, { "Open Hat",   46, 1,  0.25f }, { "Low Tom",    41, 0, -0.35f }, { "Mid Tom",    45, 0,  0.00f },
        { "High Tom",   48, 0,  0.35f }, { "Crash",      49, 0, -0.30f }, { "Ride",       51, 0,  0.30f }, { "Cowbell",    56, 0,  0.15f },
        { "Tambourine", 54, 0, -0.20f }, { "Shaker",     70, 0,  0.30f }, { "Clave",      75, 0, -0.25f }, { "Snare 2",    40, 0,  0.00f } };

    // other GM notes go to the closest pad
    int fallbackPad (int note)
    {
        switch (note)
        {
            case 35: return 0;                       // acoustic kick
            case 44: return 4;                       // pedal hat
            case 43: return 6;  case 47: return 7;   // toms
            case 50: return 8;
            case 52: case 55: case 57: return 9;     // china / splash / crash 2
            case 53: case 59: return 10;             // ride bell / ride 2
            case 69: return 13;                      // cabasa
            case 76: case 77: return 14;             // woodblocks
            case 60: case 61: case 62: case 63: case 64: return 3;   // bongos / congas -> rim-ish
            default: return -1;
        }
    }

    // ---------------------------------------------------------------------
    //  Synthesis toolkit
    // ---------------------------------------------------------------------
    constexpr double synthRate = 48000.0;

    struct Gen
    {
        juce::Random rnd;
        explicit Gen (int seed) : rnd (seed) {}
        float noise() { return rnd.nextFloat() * 2.0f - 1.0f; }
    };

    std::vector<float> makeBuffer (double seconds) { return std::vector<float> ((size_t) (seconds * synthRate), 0.0f); }

    void filter (std::vector<float>& x, const juce::IIRCoefficients& c)
    {
        juce::IIRFilter f;
        f.setCoefficients (c);
        f.processSamples (x.data(), (int) x.size());
    }
    juce::IIRCoefficients hp (double f, double q = 0.707)  { return juce::IIRCoefficients::makeHighPass (synthRate, f, q); }
    juce::IIRCoefficients lp (double f, double q = 0.707)  { return juce::IIRCoefficients::makeLowPass (synthRate, f, q); }
    juce::IIRCoefficients bp (double f, double q)          { return juce::IIRCoefficients::makeBandPass (synthRate, f, q); }

    void normalise (std::vector<float>& x, float peak = 0.9f)
    {
        float m = 0;
        for (auto v : x) m = juce::jmax (m, std::abs (v));
        if (m > 0) for (auto& v : x) v *= peak / m;
        // trim the silent tail
        size_t end = x.size();
        while (end > 64 && std::abs (x[end - 1]) < 0.0005f) --end;
        x.resize (end);
        // 2 ms fade at the very end
        const size_t fade = std::min<size_t> (96, x.size());
        for (size_t i = 0; i < fade; ++i) x[x.size() - 1 - i] *= (float) i / (float) fade;
    }

    void saturate (std::vector<float>& x, float drive)
    {
        if (drive <= 1.0f) return;
        const float norm = std::tanh (drive);
        for (auto& v : x) v = std::tanh (v * drive) / norm;
    }

    // ---- individual instruments ----
    std::vector<float> kick (double f0, double f1, double pitchDecay, double ampDecay, float click, float drive, int seed)
    {
        Gen g (seed);
        auto x = makeBuffer (ampDecay * 7.0 + 0.05);
        double phase = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            const double f = f1 + (f0 - f1) * std::exp (-t / pitchDecay);
            phase += juce::MathConstants<double>::twoPi * f / synthRate;
            const double amp = std::exp (-t / ampDecay) * juce::jmin (1.0, t / 0.0015);
            x[i] = (float) (std::sin (phase) * amp) + click * g.noise() * (float) std::exp (-t / 0.0025);
        }
        saturate (x, drive);
        filter (x, lp (9000));
        normalise (x);
        return x;
    }

    std::vector<float> snare (double tone1, double tone2, double toneDecay, double noiseDecay, float noiseLevel, double noiseHp, int seed, float body = 1.0f)
    {
        Gen g (seed);
        auto noise = makeBuffer (noiseDecay * 7.0 + 0.02);
        for (size_t i = 0; i < noise.size(); ++i)
            noise[i] = g.noise() * (float) std::exp (-(double) i / synthRate / noiseDecay);
        filter (noise, hp (noiseHp));
        filter (noise, lp (11000));
        auto x = makeBuffer (juce::jmax (toneDecay * 7.0, noiseDecay * 7.0) + 0.02);
        double p1 = 0, p2 = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            const double bend = 1.0 + 0.3 * std::exp (-t / 0.01);
            p1 += juce::MathConstants<double>::twoPi * tone1 * bend / synthRate;
            p2 += juce::MathConstants<double>::twoPi * tone2 * bend / synthRate;
            const double tone = (std::sin (p1) + 0.6 * std::sin (p2)) * std::exp (-t / toneDecay);
            x[i] = (float) tone * 0.7f * body + (i < noise.size() ? noise[i] * noiseLevel : 0.0f);
        }
        normalise (x);
        return x;
    }

    std::vector<float> clap (double centre, double tail, int seed, double spread = 1.0)
    {
        Gen g (seed);
        auto x = makeBuffer (tail * 7.0 + 0.05 + 0.03 * spread);
        const double bursts[] = { 0.0, 0.011 * spread, 0.021 * spread, 0.031 * spread };
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            double env = 0;
            for (int b = 0; b < 3; ++b)
                if (t >= bursts[b]) env = juce::jmax (env, std::exp (-(t - bursts[b]) / 0.0045));
            if (t >= bursts[3]) env = juce::jmax (env, std::exp (-(t - bursts[3]) / tail));
            x[i] = g.noise() * (float) env;
        }
        filter (x, bp (centre, 1.2));
        filter (x, hp (600));
        normalise (x);
        return x;
    }

    std::vector<float> metal (const std::vector<double>& freqs, double decay, double hpFreq, float noiseMix, int seed, double attack = 0.0005)
    {
        Gen g (seed);
        auto x = makeBuffer (decay * 6.0 + 0.02);
        std::vector<double> ph (freqs.size(), 0.0);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            double s = 0;
            for (size_t k = 0; k < freqs.size(); ++k)
            {
                ph[k] += freqs[k] / synthRate;
                if (ph[k] >= 1.0) ph[k] -= 1.0;
                s += ph[k] < 0.5 ? 1.0 : -1.0;   // square waves give the classic metallic clang
            }
            const double env = std::exp (-t / decay) * juce::jmin (1.0, t / attack);
            x[i] = (float) ((s / (double) freqs.size()) * (1.0 - noiseMix) + g.noise() * noiseMix) * (float) env;
        }
        filter (x, hp (hpFreq));
        filter (x, hp (hpFreq));
        filter (x, lp (16000));
        normalise (x);
        return x;
    }

    std::vector<float> tom (double freq, double decay, float noiseAmt, int seed, double sweep = 0.6)
    {
        Gen g (seed);
        auto x = makeBuffer (decay * 7.0 + 0.02);
        double phase = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            const double f = freq * (1.0 + sweep * std::exp (-t / 0.04));
            phase += juce::MathConstants<double>::twoPi * f / synthRate;
            x[i] = (float) (std::sin (phase) * std::exp (-t / decay)) + noiseAmt * g.noise() * (float) std::exp (-t / 0.02);
        }
        filter (x, lp (5000));
        normalise (x);
        return x;
    }

    std::vector<float> blip (double freq, double freq2, double decay, float clickAmt, double hpFreq, int seed)
    {
        Gen g (seed);
        auto x = makeBuffer (decay * 8.0 + 0.01);
        double p1 = 0, p2 = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            p1 += juce::MathConstants<double>::twoPi * freq / synthRate;
            p2 += juce::MathConstants<double>::twoPi * freq2 / synthRate;
            x[i] = (float) ((std::sin (p1) + 0.5 * std::sin (p2)) * std::exp (-t / decay))
                 + clickAmt * g.noise() * (float) std::exp (-t / 0.0015);
        }
        filter (x, hp (hpFreq));
        normalise (x);
        return x;
    }

    std::vector<float> cowbell (double scale, int seed)
    {
        auto x = metal ({ 540.0 * scale, 800.0 * scale }, 0.12, 400, 0.0f, seed);
        filter (x, bp (2600 * scale, 1.5));
        // two-stage decay: a sharp hit then a ring
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            x[i] *= (float) (0.6 * std::exp (-t / 0.02) + 0.4);
        }
        normalise (x);
        return x;
    }

    std::vector<float> shaker (double decay, double hpFreq, int bursts, int seed)
    {
        Gen g (seed);
        auto x = makeBuffer (decay * 6.0 + bursts * 0.02 + 0.02);
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            double env = 0;
            for (int b = 0; b < bursts; ++b)
            {
                const double start = b * 0.018;
                if (t >= start) env = juce::jmax (env, std::exp (-(t - start) / decay) * juce::jmin (1.0, (t - start) / 0.004) * (1.0 - 0.25 * b));
            }
            x[i] = g.noise() * (float) env;
        }
        filter (x, hp (hpFreq));
        filter (x, hp (hpFreq));
        normalise (x, 0.8f);
        return x;
    }

    void lofi (std::vector<float>& x, int seed)
    {
        juce::ignoreUnused (seed);
        // sample-and-hold to ~16 kHz, 10-bit, soft tape-ish saturation and a dull top
        float held = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            if (i % 3 == 0) held = x[i];
            x[i] = std::round (held * 512.0f) / 512.0f;
        }
        saturate (x, 1.6f);
        filter (x, lp (6500));
        normalise (x);
    }

    // A pad's two "character" controls (0..100, 50 = as designed), for the synthesised kits.
    // Kick / toms: Sweep, Click. Snares: Snappy, Body. Clap: Snappy, Spread. Hats / cymbals: Ring, Metal.
    juce::StringArray characterNames (int pad)
    {
        switch (pad)
        {
            case 0: case 6: case 7: case 8:  return { "Sweep", "Click" };
            case 1: case 15:                 return { "Snappy", "Body" };
            case 2:                          return { "Snappy", "Spread" };
            case 4: case 5: case 9: case 10: return { "Ring", "Metal" };
            default:                         return {};
        }
    }

    // kit index, pad index -> sound
    std::vector<float> synthesise (int kit, int pad, float c1 = 50.0f, float c2 = 50.0f)
    {
        const double m1 = std::pow (2.0, (c1 - 50.0) / 25.0), m2 = std::pow (2.0, (c2 - 50.0) / 25.0);   // x0.25 .. x4
        const float clickAmt = (float) std::pow (c2 / 50.0, 1.5);
        auto kickC = [&] (double f0, double f1, double pd, double ad, float click, float drv, int sd)
        {
            return kick (f1 + (f0 - f1) * m1, f1, pd * std::sqrt (m1), ad, click * clickAmt, drv, sd);
        };
        auto tomC = [&] (double f, double d, int sd) { return tom (f, d, 0.08f * clickAmt, sd, 0.6 * m1); };
        auto snareC = [&] (double t1, double t2, double td, double nd, float nl, double nhp, int sd)
        {
            return snare (t1, t2, td * std::sqrt (m2), nd * std::sqrt (m1), nl * (float) m1, nhp, sd, (float) std::sqrt (m2));
        };
        const bool hatOrCymbal = pad == 4 || pad == 5 || pad == 9 || pad == 10;
        auto metalC = [&] (const std::vector<double>& fr, double d, double hpF, float nm, int sd)
        {
            return metal (fr, d * (hatOrCymbal ? m1 : 1.0), hpF, juce::jlimit (0.0f, 0.95f, nm / (float) m2), sd);
        };
        const int seed = 1000 + kit * 37 + pad;
        const std::vector<double> hat808 { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
        std::vector<double> hatFreqs = hat808;
        const double hatScale = kit == 1 || kit == 4 ? 1.45 : kit == 2 ? 1.3 : 1.0;
        for (auto& f : hatFreqs) f *= hatScale;

        std::vector<float> x;
        switch (pad)
        {
            case 0: // kick
                if (kit == 0)      x = kickC (150, 47, 0.045, 0.42, 0.10f, 1.3f, seed);
                else if (kit == 1) x = kickC (270, 52, 0.022, 0.20, 0.55f, 2.8f, seed);
                else if (kit == 2) x = kickC (125, 41, 0.070, 0.85, 0.15f, 2.0f, seed);
                else if (kit == 3) x = kickC (140, 50, 0.040, 0.30, 0.20f, 1.8f, seed);
                else               x = kickC (230, 50, 0.028, 0.26, 0.40f, 2.2f, seed);
                break;
            case 1: // snare
                if (kit == 0)      x = snareC (238, 476, 0.09, 0.14, 0.55f, 1800, seed);
                else if (kit == 1) x = snareC (185, 330, 0.07, 0.19, 0.95f, 1200, seed);
                else if (kit == 2) x = snareC (215, 430, 0.05, 0.11, 1.10f, 2500, seed);
                else if (kit == 3) x = snareC (200, 380, 0.08, 0.16, 0.80f, 1400, seed);
                else               x = snareC (190, 350, 0.06, 0.17, 0.90f, 1500, seed);
                break;
            case 2:  x = clap ((kit == 2 ? 1400 : 1100) * std::sqrt (m1), kit == 1 || kit == 4 ? 0.16 : 0.12, seed, m2); break;
            case 3:  x = blip (1700, 820, 0.012, 0.6f, 500, seed); break;
            case 4:  x = metalC (hatFreqs, kit == 2 ? 0.032 : 0.045, 7000, kit == 1 || kit == 4 ? 0.45f : 0.15f, seed); break;
            case 5:  x = metalC (hatFreqs, kit == 2 ? 0.25 : 0.33, 6500, kit == 1 || kit == 4 ? 0.45f : 0.15f, seed); break;
            case 6:  x = tomC (kit == 0 || kit == 2 ? 80 : 95, kit == 0 ? 0.35 : 0.25, seed); break;
            case 7:  x = tomC (kit == 0 || kit == 2 ? 115 : 135, kit == 0 ? 0.30 : 0.22, seed); break;
            case 8:  x = tomC (kit == 0 || kit == 2 ? 160 : 185, kit == 0 ? 0.26 : 0.19, seed); break;
            case 9:  x = metalC ({ 296, 413, 517, 641, 782, 931, 1103, 1320 }, 1.3, 4500, 0.55f, seed); break;   // crash
            case 10: x = metalC ({ 330, 505, 612, 791, 1012, 1324 }, 0.9, 3800, 0.30f, seed); break;             // ride
            case 11: x = cowbell (kit == 1 || kit == 4 ? 1.1 : 1.0, seed); break;
            case 12: x = shaker (0.06, 7000, 3, seed); break;                                                   // tambourine
            case 13: x = shaker (0.035, 5000, 2, seed); break;                                                  // shaker
            case 14: x = blip (2500, 5000, 0.035, 0.1f, 800, seed); break;                                      // clave
            case 15: x = snareC (kit == 2 ? 260 : 220, kit == 2 ? 520 : 440, 0.04, 0.08, 1.0f, 3000, seed); break;   // snare 2
            default: x = makeBuffer (0.1); break;
        }
        if (kit == 3) lofi (x, seed);
        return x;
    }

    juce::AudioFormatManager& formats()
    {
        static juce::AudioFormatManager fm;
        static bool registered = [] { fm.registerBasicFormats(); return true; }();
        juce::ignoreUnused (registered);
        return fm;
    }
}

// =============================================================================
DrumMachine::DrumMachine()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo())),
      state (*this, nullptr, "DRUMS", [] {
          auto dbText = [] (float v, int) { return v <= -29.9f ? juce::String ("-inf") : (v > 0 ? "+" : "") + juce::String (v, 1) + " dB"; };
          auto stText = [] (float v, int) { return (v > 0 ? "+" : "") + juce::String (v, 1) + " st"; };
          auto panText = [] (float v, int)
          {
              const int p = juce::roundToInt (std::abs (v) * 100.0f);
              return p == 0 ? juce::String ("Centre") : juce::String (p) + (v < 0 ? " L" : " R");
          };
          juce::AudioProcessorValueTreeState::ParameterLayout l;
          l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "master", 1 }, "Kit Volume", juce::NormalisableRange<float> (-30.0f, 6.0f), 0.0f,
                                                              juce::AudioParameterFloatAttributes().withStringFromValueFunction (dbText)));
          for (int p = 0; p < numPads; ++p)
          {
              const juce::String n (padDefs[p].name);
              l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "lvl" + juce::String (p), 1 }, n + " Level",
                                                                  juce::NormalisableRange<float> (-30.0f, 6.0f), 0.0f,
                                                                  juce::AudioParameterFloatAttributes().withStringFromValueFunction (dbText)));
              l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "tune" + juce::String (p), 1 }, n + " Tune",
                                                                  juce::NormalisableRange<float> (-12.0f, 12.0f), 0.0f,
                                                                  juce::AudioParameterFloatAttributes().withStringFromValueFunction (stText)));
              l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { "pan" + juce::String (p), 1 }, n + " Pan",
                                                                  juce::NormalisableRange<float> (-1.0f, 1.0f), padDefs[p].pan,
                                                                  juce::AudioParameterFloatAttributes().withStringFromValueFunction (panText)));
              auto pct = [] (float v, int) { return juce::String (juce::roundToInt (v)) + "%"; };
              auto signedPct = [] (float v, int) { const int i = juce::roundToInt (v); return i == 0 ? juce::String ("0") : (i > 0 ? "+" : "") + juce::String (i); };
              auto addP = [&] (const char* id, const juce::String& name, float lo, float hi, float def, std::function<juce::String (float, int)> txt)
              {
                  l.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID { id + juce::String (p), 1 }, n + " " + name,
                                                                      juce::NormalisableRange<float> (lo, hi), def,
                                                                      juce::AudioParameterFloatAttributes().withStringFromValueFunction (txt)));
              };
              addP ("dec", "Decay", 0, 100, 100, pct);
              addP ("tone", "Tone", -100, 100, 0, signedPct);
              addP ("att", "Attack", -100, 100, 0, signedPct);
              addP ("drive", "Drive", 0, 100, 0, pct);
              const auto cn = characterNames (p);
              addP ("c1", cn.isEmpty() ? juce::String ("Character 1") : cn[0], 0, 100, 50, pct);
              addP ("c2", cn.isEmpty() ? juce::String ("Character 2") : cn[1], 0, 100, 50, pct);
          }
          return l;
      }())
{
    for (int p = 0; p < numPads; ++p)
    {
        pads[(size_t) p].name = padDefs[p].name;
        pads[(size_t) p].note = padDefs[p].note;
        pads[(size_t) p].chokeGroup = padDefs[p].choke;
        pendingHits[(size_t) p] = 0.0f;
        lastHit[(size_t) p] = 0.0;
        needsResynth[(size_t) p] = false;
        state.addParameterListener ("c1" + juce::String (p), this);
        state.addParameterListener ("c2" + juce::String (p), this);
    }
    loadKit (0);
    startTimer (40);
}

juce::StringArray DrumMachine::getCharacterNames (int pad) { return characterNames (pad); }

void DrumMachine::parameterChanged (const juce::String& id, float)
{
    // may come from the audio thread (automation): the timer does the work
    const int pad = id.substring (2).getIntValue();
    if (pad >= 0 && pad < numPads) needsResynth[(size_t) pad] = true;
}

void DrumMachine::timerCallback()
{
    for (int p = 0; p < numPads; ++p)
        if (needsResynth[(size_t) p].exchange (false))
            resynthesise (p);
}

void DrumMachine::resynthesise (int pad)
{
    if (padHasCustomSample (pad) || kitFolderName.isNotEmpty()) return;
    const auto x = synthesise (kitIndex.load(), pad, state.getRawParameterValue ("c1" + juce::String (pad))->load(),
                               state.getRawParameterValue ("c2" + juce::String (pad))->load());
    auto smp = std::make_shared<Sample>();
    smp->sampleRate = synthRate;
    smp->buffer.setSize (2, (int) x.size());
    smp->buffer.copyFrom (0, 0, x.data(), (int) x.size());
    smp->buffer.copyFrom (1, 0, x.data(), (int) x.size());
    {
        const juce::SpinLock::ScopedLockType sl (padLock);
        pads[(size_t) pad].sample = smp;
    }
    if (onPadsChanged) onPadsChanged();
}

std::vector<float> DrumMachine::getPadPeaks (int pad, int columns) const
{
    SamplePtr smp;
    {
        const juce::SpinLock::ScopedLockType sl (padLock);
        smp = pads[(size_t) juce::jlimit (0, numPads - 1, pad)].sample;
    }
    std::vector<float> out ((size_t) juce::jmax (1, columns), 0.0f);
    if (smp == nullptr) return out;
    const int len = smp->buffer.getNumSamples();
    float peak = 1.0e-6f;
    for (int c = 0; c < columns; ++c)
    {
        const int a = (int) ((juce::int64) c * len / columns), b = juce::jmax (a + 1, (int) ((juce::int64) (c + 1) * len / columns));
        out[(size_t) c] = smp->buffer.getMagnitude (0, a, juce::jmin (b, len) - a);
        peak = juce::jmax (peak, out[(size_t) c]);
    }
    for (auto& v : out) v /= peak;
    return out;
}

double DrumMachine::getPadSeconds (int pad) const
{
    const juce::SpinLock::ScopedLockType sl (padLock);
    const auto& smp = pads[(size_t) juce::jlimit (0, numPads - 1, pad)].sample;
    return smp != nullptr ? smp->buffer.getNumSamples() / smp->sampleRate : 0.0;
}

DrumMachine::~DrumMachine()
{
    stopTimer();
    for (int p = 0; p < numPads; ++p)
    {
        state.removeParameterListener ("c1" + juce::String (p), this);
        state.removeParameterListener ("c2" + juce::String (p), this);
    }
}

juce::StringArray DrumMachine::getKitNames() { return { "808", "909", "Trap", "Lo-Fi", "House" }; }

juce::String DrumMachine::getKitLabel() const
{
    if (kitFolderName.isNotEmpty()) return kitFolderName;
    const auto names = getKitNames();
    return names[juce::jlimit (0, names.size() - 1, kitIndex.load())];
}

void DrumMachine::loadKit (int kit)
{
    kit = juce::jlimit (0, getKitNames().size() - 1, kit);
    std::array<SamplePtr, numPads> fresh;
    for (int p = 0; p < numPads; ++p)
    {
        const auto x = synthesise (kit, p, state.getRawParameterValue ("c1" + juce::String (p))->load(),
                                   state.getRawParameterValue ("c2" + juce::String (p))->load());
        auto s = std::make_shared<Sample>();
        s->sampleRate = synthRate;
        s->buffer.setSize (2, (int) x.size());
        s->buffer.copyFrom (0, 0, x.data(), (int) x.size());
        s->buffer.copyFrom (1, 0, x.data(), (int) x.size());
        fresh[(size_t) p] = s;
    }
    {
        const juce::SpinLock::ScopedLockType sl (padLock);
        for (int p = 0; p < numPads; ++p)
        {
            pads[(size_t) p].sample = fresh[(size_t) p];
            pads[(size_t) p].customFile = juce::File();
        }
        kitIndex = kit;
        kitFolderName.clear();
    }
    updateHostDisplay();
    if (onPadsChanged) onPadsChanged();
}

DrumMachine::SamplePtr DrumMachine::loadSampleFile (const juce::File& f)
{
    std::unique_ptr<juce::AudioFormatReader> r (formats().createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0 || r->lengthInSamples > (juce::int64) r->sampleRate * 30) return nullptr;
    auto s = std::make_shared<Sample>();
    s->sampleRate = r->sampleRate;
    s->buffer.setSize (2, (int) r->lengthInSamples);
    r->read (&s->buffer, 0, (int) r->lengthInSamples, 0, true, true);
    if (r->numChannels == 1) s->buffer.copyFrom (1, 0, s->buffer, 0, 0, s->buffer.getNumSamples());
    return s;
}

bool DrumMachine::loadPadSample (int pad, const juce::File& f)
{
    if (pad < 0 || pad >= numPads) return false;
    auto s = loadSampleFile (f);
    if (s == nullptr) return false;
    {
        const juce::SpinLock::ScopedLockType sl (padLock);
        pads[(size_t) pad].sample = s;
        pads[(size_t) pad].customFile = f;
    }
    if (onPadsChanged) onPadsChanged();
    return true;
}

void DrumMachine::resetPad (int pad)
{
    if (pad < 0 || pad >= numPads) return;
    const auto x = synthesise (kitIndex.load(), pad, state.getRawParameterValue ("c1" + juce::String (pad))->load(),
                               state.getRawParameterValue ("c2" + juce::String (pad))->load());
    auto s = std::make_shared<Sample>();
    s->sampleRate = synthRate;
    s->buffer.setSize (2, (int) x.size());
    s->buffer.copyFrom (0, 0, x.data(), (int) x.size());
    s->buffer.copyFrom (1, 0, x.data(), (int) x.size());
    {
        const juce::SpinLock::ScopedLockType sl (padLock);
        pads[(size_t) pad].sample = s;
        pads[(size_t) pad].customFile = juce::File();
    }
    if (onPadsChanged) onPadsChanged();
}

bool DrumMachine::loadKitFolder (const juce::File& folder)
{
    // match sample names to pads by keyword
    static const std::vector<std::pair<int, juce::StringArray>> keywords {
        { 4, { "closed", "chh", "hhc", "hat_c", "hh_c", "c hat" } }, { 5, { "open", "ohh", "hho", "hat_o", "hh_o", "o hat" } },
        { 2, { "clap", "clp" } }, { 3, { "rim", "stick", "side" } }, { 15, { "snare2", "snare 2", "snr2" } },
        { 1, { "snare", "snr", "sd" } }, { 0, { "kick", "bd", "bass drum", "kik" } },
        { 6, { "low tom", "tom low", "tom1", "lt", "floor" } }, { 7, { "mid tom", "tom mid", "tom2", "mt" } }, { 8, { "high tom", "hi tom", "tom3", "ht" } },
        { 9, { "crash", "cym" } }, { 10, { "ride" } }, { 11, { "cowbell", "bell" } }, { 12, { "tamb" } }, { 13, { "shak", "maraca" } },
        { 14, { "clave", "wood", "perc" } }, { 4, { "hat", "hh", "hihat" } } };

    juce::Array<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator (folder, true, formats().getWildcardForAllFormats(), juce::File::findFiles))
        files.add (e.getFile());
    if (files.isEmpty()) return false;
    files.sort();

    std::array<juce::File, numPads> chosen;
    for (auto& f : files)
    {
        const auto n = f.getFileNameWithoutExtension().toLowerCase();
        for (auto& [pad, words] : keywords)
        {
            bool match = false;
            for (auto& w : words) if (n.contains (w)) { match = true; break; }
            if (match)
            {
                if (chosen[(size_t) pad] == juce::File()) chosen[(size_t) pad] = f;
                break;
            }
        }
    }
    int loaded = 0;
    for (int p = 0; p < numPads; ++p)
        if (chosen[(size_t) p] != juce::File())
        {
            if (auto s = loadSampleFile (chosen[(size_t) p]))
            {
                const juce::SpinLock::ScopedLockType sl (padLock);
                pads[(size_t) p].sample = s;
                pads[(size_t) p].customFile = chosen[(size_t) p];
                ++loaded;
            }
        }
    if (loaded == 0) return false;
    kitFolderName = folder.getFileName();
    updateHostDisplay();
    if (onPadsChanged) onPadsChanged();
    return true;
}

// ---- kit files ----
static juce::File kitFolderList()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("RENASOUND").getChildFile ("kit-folders.txt");
}

juce::Array<juce::File> DrumMachine::getKitFolders()
{
    juce::Array<juce::File> out;
    auto addIf = [&out] (const juce::File& f) { if (f.isDirectory() && ! out.contains (f)) out.add (f); };
    // the usual places, matched loosely ("Renasound kits", "RENASOUND Kits", a "Music " folder inside Documents...)
    auto childNamed = [] (const juce::File& parent, std::initializer_list<const char*> names)
    {
        juce::Array<juce::File> found;
        if (parent.isDirectory())
            for (const auto& e : juce::RangedDirectoryIterator (parent, false, "*", juce::File::findDirectories))
                for (auto* n : names)
                    if (e.getFile().getFileName().trim().equalsIgnoreCase (n)) found.add (e.getFile());
        return found;
    };
    const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);
    const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    juce::Array<juce::File> parents { music, docs };
    for (auto& f : childNamed (docs, { "Music" })) parents.add (f);
    for (auto& f : childNamed (music, { "Renasound" })) parents.add (f);
    for (auto& p : parents)
        for (auto& f : childNamed (p, { "Renasound kits", "Renasound kit", "Kits" }))
            if (p != music && p != docs ? true : ! f.getFileName().equalsIgnoreCase ("Kits")) addIf (f);
    juce::StringArray lines;
    lines.addLines (kitFolderList().loadFileAsString());
    for (auto& l : lines)
        if (l.trim().isNotEmpty() && juce::File::isAbsolutePath (l)) addIf (juce::File (l));   // paths may end in spaces, so not trimmed
    return out;
}

void DrumMachine::addKitFolder (const juce::File& folder)
{
    if (! folder.isDirectory() || getKitFolders().contains (folder)) return;
    auto list = kitFolderList();
    list.getParentDirectory().createDirectory();
    list.appendText (folder.getFullPathName() + "\n");
}

juce::Array<juce::File> DrumMachine::findKitFiles()
{
    juce::Array<juce::File> kits;
    for (auto& folder : getKitFolders())
        for (const auto& e : juce::RangedDirectoryIterator (folder, true, "*.rskit", juce::File::findFiles))
            if (! kits.contains (e.getFile())) kits.add (e.getFile());
    struct ByName { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileNameWithoutExtension().compareNatural (b.getFileNameWithoutExtension()); } } cmp;
    kits.sort (cmp);
    return kits;
}

bool DrumMachine::loadKitFile (const juce::File& kitFile)
{
    auto xml = juce::XmlDocument::parse (kitFile);
    if (xml == nullptr || ! xml->hasTagName ("RENASOUND_KIT")) return false;
    const auto base = kitFile.getParentDirectory();
    int loaded = 0;
    for (auto* pe : xml->getChildWithTagNameIterator ("PAD"))
    {
        const int note = pe->getIntAttribute ("note", -1);
        int pad = -1;
        for (int p = 0; p < numPads; ++p) if (pads[(size_t) p].note == note) pad = p;
        if (pad < 0) continue;
        const auto path = pe->getStringAttribute ("file");
        const auto file = juce::File::isAbsolutePath (path) ? juce::File (path) : base.getChildFile (path);
        if (! loadPadSample (pad, file)) continue;
        ++loaded;
        auto setParam = [this] (const juce::String& id, double v)
        {
            if (auto* prm = state.getParameter (id)) prm->setValueNotifyingHost (prm->convertTo0to1 ((float) v));
        };
        setParam ("lvl" + juce::String (pad), pe->getDoubleAttribute ("level", 0.0));
        setParam ("tune" + juce::String (pad), pe->getDoubleAttribute ("tune", 0.0));
        setParam ("pan" + juce::String (pad), pe->getDoubleAttribute ("pan", 0.0));
    }
    if (loaded == 0) return false;
    kitFolderName = xml->getStringAttribute ("name", kitFile.getFileNameWithoutExtension());
    updateHostDisplay();
    if (onPadsChanged) onPadsChanged();
    return true;
}

juce::String DrumMachine::getPadName (int pad) const
{
    if (pad < 0 || pad >= numPads) return {};
    const auto& p = pads[(size_t) pad];
    return p.customFile != juce::File() ? p.customFile.getFileNameWithoutExtension() : p.name;
}

int DrumMachine::getPadNote (int pad) const { return pad >= 0 && pad < numPads ? pads[(size_t) pad].note : -1; }
bool DrumMachine::padHasCustomSample (int pad) const { return pad >= 0 && pad < numPads && pads[(size_t) pad].customFile != juce::File(); }

int DrumMachine::padForNote (int note) const
{
    for (int p = 0; p < numPads; ++p)
        if (pads[(size_t) p].note == note) return p;
    return fallbackPad (note);
}

void DrumMachine::triggerPad (int pad, float velocity)
{
    if (pad >= 0 && pad < numPads) pendingHits[(size_t) pad] = velocity;
}

// =============================================================================
//  Playback
// =============================================================================
void DrumMachine::prepareToPlay (double sampleRate, int)
{
    hostRate = sampleRate;
    for (auto& v : voices) v.active = false;
}

void DrumMachine::startVoice (int pad, float velocity)
{
    SamplePtr sample;
    {
        const juce::SpinLock::ScopedTryLockType sl (padLock);
        if (! sl.isLocked()) return;
        sample = pads[(size_t) pad].sample;
    }
    if (sample == nullptr) return;

    // each pad is monophonic, and hi-hats cut each other
    const int group = pads[(size_t) pad].chokeGroup;
    for (auto& v : voices)
        if (v.active && (v.pad == pad || (group != 0 && pads[(size_t) v.pad].chokeGroup == group)))
            v.fadeStep = -1.0f / (float) (0.004 * hostRate);

    Voice* slot = nullptr;
    for (auto& v : voices) if (! v.active) { slot = &v; break; }
    if (slot == nullptr)   // steal the oldest
    {
        slot = &voices[0];
        for (auto& v : voices) if (v.pos > slot->pos) slot = &v;
    }

    const float lvl = juce::Decibels::decibelsToGain (state.getRawParameterValue ("lvl" + juce::String (pad))->load()
                                                      + state.getRawParameterValue ("master")->load());
    const float tune = state.getRawParameterValue ("tune" + juce::String (pad))->load();
    const float pan = state.getRawParameterValue ("pan" + juce::String (pad))->load();
    const float vel = (0.25f + 0.75f * velocity) * 0.5f;   // softer hits are quieter, but never silent; -6 dB headroom
    const float angle = (pan + 1.0f) * juce::MathConstants<float>::pi * 0.25f;

    // shaping for this hit
    {
        auto pv = [this, pad] (const char* id) { return state.getRawParameterValue (id + juce::String (pad))->load(); };
        const double sr = hostRate;
        const float dec = pv ("dec"), tone = pv ("tone"), att = pv ("att"), drive = pv ("drive");
        slot->age = 0;
        slot->env = 1.0f;
        slot->hold = (int) (0.004 * sr);
        slot->envCoef = dec >= 99.5f ? 1.0f : (float) std::exp (-1.0 / ((0.012 + std::pow (dec / 100.0, 2.2) * 2.5) * sr));
        slot->fadeIn = att < 0 ? juce::jmax (1, (int) (-att / 100.0 * 0.035 * sr)) : 0;
        slot->boost = att > 0 ? att / 100.0f * 1.2f : 0.0f;
        slot->boostCoef = (float) std::exp (-1.0 / (0.009 * sr));
        // tone: darker = two one-pole low-passes, brighter = two one-pole high-passes
        const double lpHz = tone < 0 ? 20000.0 * std::pow (350.0 / 20000.0, -tone / 100.0) : sr * 0.49;
        const double hpHz = tone > 0 ? 20.0 * std::pow (1800.0 / 20.0, tone / 100.0) : 0.0;
        slot->lpA = tone < 0 ? (float) (1.0 - std::exp (-juce::MathConstants<double>::twoPi * lpHz / sr)) : 1.0f;
        slot->hpA = tone > 0 ? (float) std::exp (-juce::MathConstants<double>::twoPi * hpHz / sr) : 0.0f;
        slot->toneGain = tone > 0 ? juce::Decibels::decibelsToGain (tone / 100.0f * 4.0f) : 1.0f;
        slot->drive = 1.0f + drive / 100.0f * 6.0f;
        slot->driveNorm = 1.0f / std::tanh (slot->drive);
        for (int ch = 0; ch < 2; ++ch) slot->lp1[ch] = slot->lp2[ch] = slot->hpIn[ch] = slot->hpOut[ch] = 0.0f;
    }
    slot->sample = sample;
    slot->pad = pad;
    slot->pos = 0;
    slot->step = sample->sampleRate / hostRate * std::pow (2.0, tune / 12.0);
    slot->gainL = lvl * vel * std::cos (angle) * 1.414f;
    slot->gainR = lvl * vel * std::sin (angle) * 1.414f;
    slot->fade = 1.0f;
    slot->fadeStep = 0.0f;
    slot->active = true;
    lastHit[(size_t) pad] = juce::Time::getMillisecondCounterHiRes();
}

void DrumMachine::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    juce::ScopedNoDenormals nd;
    buffer.clear();
    const int n = buffer.getNumSamples();

    for (int p = 0; p < numPads; ++p)
        if (const float v = pendingHits[(size_t) p].exchange (0.0f); v > 0.0f)
            startVoice (p, v);

    auto render = [&] (int from, int to)
    {
        for (auto& v : voices)
        {
            if (! v.active) continue;
            const auto& src = v.sample->buffer;
            const int len = src.getNumSamples();
            auto* l = src.getReadPointer (0);
            auto* r = src.getReadPointer (1);
            for (int i = from; i < to; ++i)
            {
                const int i0 = (int) v.pos;
                if (i0 + 1 >= len || v.fade <= 0.0f) { v.active = false; break; }
                const float frac = (float) (v.pos - i0);
                float sl = l[i0] + frac * (l[i0 + 1] - l[i0]);
                float sr = r[i0] + frac * (r[i0 + 1] - r[i0]);

                // tone
                if (v.lpA < 1.0f)
                {
                    v.lp1[0] += v.lpA * (sl - v.lp1[0]); v.lp2[0] += v.lpA * (v.lp1[0] - v.lp2[0]); sl = v.lp2[0];
                    v.lp1[1] += v.lpA * (sr - v.lp1[1]); v.lp2[1] += v.lpA * (v.lp1[1] - v.lp2[1]); sr = v.lp2[1];
                }
                if (v.hpA > 0.0f)
                {
                    const float yl = v.hpA * (v.hpOut[0] + sl - v.hpIn[0]); v.hpIn[0] = sl; v.hpOut[0] = yl;
                    const float yr = v.hpA * (v.hpOut[1] + sr - v.hpIn[1]); v.hpIn[1] = sr; v.hpOut[1] = yr;
                    sl = yl * v.toneGain; sr = yr * v.toneGain;
                }
                // drive
                if (v.drive > 1.001f) { sl = std::tanh (sl * v.drive) * v.driveNorm; sr = std::tanh (sr * v.drive) * v.driveNorm; }
                // attack and decay
                float shape = v.env * (1.0f + v.boost);
                if (v.fadeIn > 0 && v.age < v.fadeIn) shape *= (float) v.age / (float) v.fadeIn;
                if (v.age >= v.hold) v.env *= v.envCoef;
                v.boost *= v.boostCoef;
                ++v.age;
                if (v.env < 1.0e-4f) { v.active = false; break; }

                buffer.addSample (0, i, sl * shape * v.gainL * v.fade);
                if (buffer.getNumChannels() > 1) buffer.addSample (1, i, sr * shape * v.gainR * v.fade);
                v.pos += v.step;
                v.fade += v.fadeStep;
            }
        }
    };

    int done = 0;
    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        const int at = juce::jlimit (0, n, meta.samplePosition);
        if (m.isNoteOn())
        {
            render (done, at);
            done = at;
            if (const int pad = padForNote (m.getNoteNumber()); pad >= 0)
                startVoice (pad, m.getFloatVelocity());
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
        {
            for (auto& v : voices) if (v.active) v.fadeStep = -1.0f / (float) (0.01 * hostRate);
        }
    }
    render (done, n);
    samplePosition += n;
}

// =============================================================================
//  State
// =============================================================================
void DrumMachine::getStateInformation (juce::MemoryBlock& mb)
{
    juce::XmlElement xml ("RENASOUND_DRUMS");
    xml.setAttribute ("kit", kitIndex.load());
    xml.setAttribute ("kitFolder", kitFolderName);
    for (int p = 0; p < numPads; ++p)
        if (pads[(size_t) p].customFile != juce::File())
        {
            auto* pe = xml.createNewChildElement ("PAD");
            pe->setAttribute ("index", p);
            pe->setAttribute ("file", pads[(size_t) p].customFile.getFullPathName());
        }
    if (auto params = state.copyState().createXml()) xml.addChildElement (params.release());
    copyXmlToBinary (xml, mb);
}

void DrumMachine::setStateInformation (const void* data, int size)
{
    auto xml = getXmlFromBinary (data, size);
    if (xml == nullptr || ! xml->hasTagName ("RENASOUND_DRUMS")) return;
    loadKit (xml->getIntAttribute ("kit", 0));
    for (auto* pe : xml->getChildWithTagNameIterator ("PAD"))
        loadPadSample (pe->getIntAttribute ("index", -1), juce::File (pe->getStringAttribute ("file")));
    kitFolderName = xml->getStringAttribute ("kitFolder");
    if (auto* params = xml->getChildByName (state.state.getType()))
        state.replaceState (juce::ValueTree::fromXml (*params));
    for (int p = 0; p < numPads; ++p) { needsResynth[(size_t) p] = false; resynthesise (p); }
}

// =============================================================================
//  Editor: 4 x 4 pads + the selected pad's controls
// =============================================================================
// Pad colours by family
static juce::Colour padColour (int pad)
{
    switch (pad)
    {
        case 0:                          return juce::Colour (0xffff7a45);   // kick
        case 1: case 2: case 3: case 15: return juce::Colour (0xffe9c46a);   // snares, clap, rim
        case 4: case 5: case 12: case 13:return juce::Colour (0xff4fd1c5);   // hats, tambourine, shaker
        case 6: case 7: case 8:          return juce::Colour (0xffb388ff);   // toms
        case 9: case 10:                 return juce::Colour (0xff64b5f6);   // cymbals
        default:                         return juce::Colour (0xff9ccc65);   // cowbell, clave
    }
}

// Ring knobs tinted with the selected pad's colour
class DrumLook : public juce::LookAndFeel_V4
{
public:
    juce::Colour tint { 0xffff7a45 };
    DrumLook()
    {
        setColour (juce::Slider::textBoxTextColourId, theme::text);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, theme::textDim);
        setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff1b1916));
        setColour (juce::ComboBox::textColourId, theme::text);
        setColour (juce::ComboBox::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::ComboBox::arrowColourId, theme::textDim);
        setColour (juce::PopupMenu::backgroundColourId, theme::panel);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, theme::accent);
        setColour (juce::TextButton::buttonColourId, juce::Colours::transparentBlack);
        setColour (juce::TextButton::textColourOffId, theme::text);
    }
    juce::Font getComboBoxFont (juce::ComboBox&) override { return theme::font (14.0f, true); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return theme::font (12.5f, true); }
    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool over, bool down) override
    {
        if (over || down) { g.setColour (theme::text.withAlpha (down ? 0.12f : 0.06f)); g.fillRoundedRectangle (b.getLocalBounds().toFloat(), 5.0f); }
    }
    void drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, juce::Slider& sl) override
    {
        const float size = (float) juce::jmin (w, h) - 4.0f;
        const auto c = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h).getCentre();
        const float r = size * 0.5f;
        const float angle = a0 + pos * (a1 - a0);
        const bool bipolar = juce::approximatelyEqual (sl.getMinimum(), -sl.getMaximum());
        const float from = bipolar ? (a0 + a1) * 0.5f : a0;
        const float alpha = sl.isEnabled() ? 1.0f : 0.3f;
        juce::Path track, value;
        track.addCentredArc (c.x, c.y, r * 0.86f, r * 0.86f, 0, a0, a1, true);
        g.setColour (juce::Colour (0xff2a2622).withMultipliedAlpha (alpha));
        g.strokePath (track, juce::PathStrokeType (r * 0.14f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        if (std::abs (angle - from) > 0.01f)
        {
            value.addCentredArc (c.x, c.y, r * 0.86f, r * 0.86f, 0, juce::jmin (from, angle), juce::jmax (from, angle), true);
            g.setColour (tint.withAlpha (0.22f * alpha));
            g.strokePath (value, juce::PathStrokeType (r * 0.32f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (tint.withMultipliedAlpha (alpha));
            g.strokePath (value, juce::PathStrokeType (r * 0.14f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        const float body = r * 0.6f;
        juce::ColourGradient gr (juce::Colour (0xff3a3530), c.x, c.y - body, juce::Colour (0xff15130f), c.x, c.y + body, false);
        g.setGradientFill (gr);
        g.fillEllipse (c.x - body, c.y - body, body * 2, body * 2);
        const auto tip = c.getPointOnCircumference (body * 0.8f, angle);
        g.setColour (juce::Colours::white.withAlpha (0.9f * alpha));
        g.fillEllipse (tip.x - 2.5f, tip.y - 2.5f, 5, 5);
    }
};

// =============================================================================
//  Editor: glowing pads on the left, the selected pad's screen and controls on the right
// =============================================================================
class DrumMachineEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DrumMachineEditor (DrumMachine& d) : AudioProcessorEditor (d), drums (d)
    {
        setLookAndFeel (&look);
        for (int p = 0; p < DrumMachine::numPads; ++p)
            addAndMakeVisible (padButtons.add (new PadButton (*this, p)));

        addAndMakeVisible (kitBox);
        fillKitBox();
        kitBox.onChange = [this]
        {
            const int id = kitBox.getSelectedId();
            if (id <= 0) return;
            if (id < 100) { drums.loadKit (id - 1); return; }
            if (id == 999)
            {
                chooser = std::make_unique<juce::FileChooser> ("Choose a folder with RENASOUND kit files (.rskit)", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
                {
                    if (fc.getResult().isDirectory()) DrumMachine::addKitFolder (fc.getResult());
                    fillKitBox();
                    if (userKits.isEmpty())
                        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "No kits found", "That folder has no .rskit kit files yet.");
                });
                return;
            }
            if (id - 100 < userKits.size() && ! drums.loadKitFile (userKits[id - 100]))
                juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Kit not loaded",
                                                        "The samples for " + userKits[id - 100].getFileNameWithoutExtension() + " weren't found next to the kit file.");
        };

        addAndMakeVisible (folderButton);
        folderButton.setTooltip ("Use a folder of your own drum samples (matched to pads by name: kick, snare, hat...)");
        folderButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a folder of drum samples", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
            {
                if (fc.getResult().isDirectory() && ! drums.loadKitFolder (fc.getResult()))
                    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "No drum samples found",
                                                            "Name the files with words like kick, snare, clap, hat, tom, crash or ride.");
            });
        };
        addAndMakeVisible (sampleButton);
        sampleButton.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Load a sample onto " + drums.getPadName (selected),
                                                           juce::File::getSpecialLocation (juce::File::userMusicDirectory), formats().getWildcardForAllFormats());
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
            {
                if (fc.getResult().existsAsFile()) drums.loadPadSample (selected, fc.getResult());
            });
        };
        addAndMakeVisible (resetButton);
        resetButton.onClick = [this] { drums.resetPad (selected); };
        addAndMakeVisible (resetShapeButton);
        resetShapeButton.setTooltip ("Put this pad's shaping back to how it was designed");
        resetShapeButton.onClick = [this]
        {
            for (auto id : { "lvl", "tune", "pan", "dec", "tone", "att", "drive", "c1", "c2" })
                if (auto* p = drums.state.getParameter (id + juce::String (selected)))
                    p->setValueNotifyingHost (p->getDefaultValue());
        };

        const char* names[] { "LEVEL", "TUNE", "PAN", "DECAY", "TONE", "ATTACK", "DRIVE", "", "" };
        for (int i = 0; i < 9; ++i)
        {
            auto* k = knobs.add (new KnobCell (names[i]));
            addAndMakeVisible (k);
        }
        master = std::make_unique<KnobCell> ("KIT");
        addAndMakeVisible (*master);
        masterAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (drums.state, "master", master->slider);

        drums.onPadsChanged = [sp = juce::Component::SafePointer<DrumMachineEditor> (this)]
        {
            juce::MessageManager::callAsync ([sp] { if (sp != nullptr) sp->refresh(); });
        };
        select (0);
        setSize (900, 520);
        startTimerHz (30);
    }

    ~DrumMachineEditor() override
    {
        drums.onPadsChanged = nullptr;
        setLookAndFeel (nullptr);
    }

    void select (int pad)
    {
        selected = pad;
        const char* ids[] { "lvl", "tune", "pan", "dec", "tone", "att", "drive", "c1", "c2" };
        for (int i = 0; i < 9; ++i)
        {
            knobs[i]->attach.reset();
            knobs[i]->attach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (drums.state, ids[i] + juce::String (pad), knobs[i]->slider);
            if (auto* p = drums.state.getParameter (ids[i] + juce::String (pad)))
                knobs[i]->slider.setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));
        }
        look.tint = padColour (pad);
        refresh();
    }

    void refresh()
    {
        kitBox.setText (drums.getKitLabel(), juce::dontSendNotification);
        const auto cn = DrumMachine::getCharacterNames (selected);
        const bool synth = drums.padIsSynthesised (selected) && ! cn.isEmpty();
        for (int i = 0; i < 2; ++i)
        {
            knobs[7 + i]->label.setText (cn.isEmpty() ? juce::String() : cn[i].toUpperCase(), juce::dontSendNotification);
            knobs[7 + i]->setVisible (! cn.isEmpty());
            knobs[7 + i]->slider.setEnabled (synth);
            knobs[7 + i]->slider.setTooltip (synth ? juce::String() : "Only for RENASOUND's own synthesised sounds");
        }
        resetButton.setEnabled (drums.padHasCustomSample (selected));
        for (int p = 0; p < DrumMachine::numPads; ++p) padButtons[p]->peaks = drums.getPadPeaks (p, 40);
        screenPeaks = drums.getPadPeaks (selected, 300);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        juce::ColourGradient body (juce::Colour (0xff1c1a17), 0, 0, juce::Colour (0xff0d0c0a), 0, r.getHeight(), false);
        g.setGradientFill (body);
        g.fillAll();
        g.setTiledImageFill (theme::grain(), 0, 0, 1.0f);
        g.fillAll();

        // header
        const auto logoFont = juce::Font (juce::FontOptions (20.0f, juce::Font::bold)).withExtraKerningFactor (0.12f);
        g.setFont (logoFont);
        g.setColour (theme::text);
        g.drawText ("RENASOUND", 22, 14, 200, 30, juce::Justification::centredLeft);
        g.setColour (look.tint);
        g.drawText ("DRUMS", 22 + juce::roundToInt (juce::GlyphArrangement::getStringWidth (logoFont, "RENASOUND ")) + 4, 14, 100, 30, juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        g.drawText ("KIT", kitBox.getBounds().translated (-34, 0).withWidth (30), juce::Justification::centredRight);

        // pad well
        g.setColour (juce::Colours::black.withAlpha (0.35f));
        g.fillRoundedRectangle (padArea.toFloat().expanded (8), 12.0f);

        // the screen
        auto sc = screen.toFloat();
        g.setColour (juce::Colour (0xff0a0908));
        g.fillRoundedRectangle (sc, 10.0f);
        g.setColour (look.tint.withAlpha (0.25f));
        g.drawRoundedRectangle (sc.reduced (0.5f), 10.0f, 1.0f);
        auto inner = sc.reduced (16, 12);
        auto head = inner.removeFromTop (26);
        g.setColour (look.tint);
        g.setFont (theme::font (19.0f, true));
        g.drawText (drums.getPadName (selected), head, juce::Justification::centredLeft, true);
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.0f, true));
        const auto note = juce::MidiMessage::getMidiNoteName (drums.getPadNote (selected), true, true, 4);
        g.drawText ((drums.padHasCustomSample (selected) ? juce::String ("SAMPLE") : "SYNTH  " + DrumMachine::getKitNames()[drums.getKitIndex()].toUpperCase())
                        + "   " + note + "   " + juce::String (drums.getPadSeconds (selected), 2) + " s",
                    head, juce::Justification::centredRight);
        inner.removeFromTop (6);

        // waveform with the Decay / Attack shape over it
        const auto plot = inner;
        const auto pv = [this] (const char* id) { return drums.state.getRawParameterValue (id + juce::String (selected))->load(); };
        const double secs = juce::jmax (0.05, drums.getPadSeconds (selected));
        const float dec = pv ("dec"), att = pv ("att");
        const double tau = dec >= 99.5f ? 1.0e9 : 0.012 + std::pow (dec / 100.0, 2.2) * 2.5;
        auto shapeAt = [&] (double t)
        {
            double sh = t < 0.004 ? 1.0 : std::exp (-(t - 0.004) / tau);
            if (att < 0) sh *= juce::jmin (1.0, t / (-att / 100.0 * 0.035));
            if (att > 0) sh *= 1.0 + att / 100.0 * 1.2 * std::exp (-t / 0.009);
            return sh;
        };
        const int n = (int) screenPeaks.size();
        juce::Path wave, env;
        for (int i = 0; i < n; ++i)
        {
            const double t = secs * i / n;
            const float a = screenPeaks[(size_t) i] * (float) juce::jmin (1.6, shapeAt (t));
            const float x = plot.getX() + plot.getWidth() * (float) i / (float) n;
            const float hgt = juce::jmin (1.0f, a) * plot.getHeight() * 0.48f;
            if (i == 0) { wave.startNewSubPath (x, plot.getCentreY() - hgt); } else wave.lineTo (x, plot.getCentreY() - hgt);
            const float ey = plot.getCentreY() - (float) juce::jmin (1.3, shapeAt (t) / 1.3) * plot.getHeight() * 0.48f;
            if (i == 0) env.startNewSubPath (x, ey); else env.lineTo (x, ey);
        }
        for (int i = n - 1; i >= 0; --i)
        {
            const double t = secs * i / n;
            const float a = screenPeaks[(size_t) i] * (float) juce::jmin (1.6, shapeAt (t));
            wave.lineTo (plot.getX() + plot.getWidth() * (float) i / (float) n, plot.getCentreY() + juce::jmin (1.0f, a) * plot.getHeight() * 0.48f);
        }
        wave.closeSubPath();
        juce::ColourGradient wg (look.tint.withAlpha (0.95f), plot.getX(), 0, look.tint.withAlpha (0.35f), plot.getRight(), 0, false);
        g.setGradientFill (wg);
        g.fillPath (wave);
        g.setColour (juce::Colours::white.withAlpha (0.5f));
        g.strokePath (env, juce::PathStrokeType (1.2f));

        // flash on hits
        const double since = juce::Time::getMillisecondCounterHiRes() - drums.getLastHitTime (selected);
        if (since < 200.0)
        {
            g.setColour (look.tint.withAlpha ((float) (0.12 * (1.0 - since / 200.0))));
            g.fillRoundedRectangle (sc, 10.0f);
        }

        // control panel
        g.setColour (juce::Colours::white.withAlpha (0.03f));
        g.fillRoundedRectangle (controls.toFloat(), 10.0f);
        g.setColour (theme::textDim.withAlpha (0.7f));
        g.setFont (theme::font (10.0f, true));
        g.drawText ("PAD", controls.getX() + 14, controls.getY() + 6, 60, 12, juce::Justification::left);
        g.drawText ("SHAPE", knobs[3]->getX(), controls.getY() + 6, 60, 12, juce::Justification::left);
        if (knobs[7]->isVisible()) g.drawText ("CHARACTER", knobs[7]->getX(), knobs[7]->getY() - 14, 90, 12, juce::Justification::left);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 14);
        auto top = r.removeFromTop (32);
        folderButton.setBounds (top.removeFromRight (160));
        top.removeFromRight (8);
        kitBox.setBounds (top.removeFromRight (190));
        r.removeFromTop (16);

        padArea = r.removeFromLeft (r.getHeight());
        const int cell = padArea.getWidth() / 4;
        for (int p = 0; p < DrumMachine::numPads; ++p)
        {
            const int row = 3 - p / 4, col = p % 4;   // like hardware: first pads at the bottom
            padButtons[p]->setBounds (padArea.getX() + col * cell, padArea.getY() + row * cell, cell - 8, cell - 8);
        }
        r.removeFromLeft (24);
        screen = r.removeFromTop (160);
        r.removeFromTop (12);
        controls = r;

        auto c = r.reduced (10, 20);
        auto rowA = c.removeFromTop (100);
        const int w = rowA.getWidth() / 7;
        for (int i = 0; i < 7; ++i) knobs[i]->setBounds (rowA.removeFromLeft (w).reduced (2, 0));
        c.removeFromTop (22);
        auto rowB = c.removeFromTop (100);
        for (int i = 7; i < 9; ++i) knobs[i]->setBounds (rowB.removeFromLeft (w).reduced (2, 0));
        master->setBounds (rowB.removeFromRight (w).reduced (2, 0));
        auto buttons = rowB.reduced (8, 18);
        sampleButton.setBounds (buttons.removeFromTop (22));
        resetButton.setBounds (buttons.removeFromTop (22));
        resetShapeButton.setBounds (buttons.removeFromTop (22));
    }

private:
    struct KnobCell : juce::Component
    {
        explicit KnobCell (const juce::String& name)
        {
            slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 70, 16);
            label.setText (name, juce::dontSendNotification);
            label.setJustificationType (juce::Justification::centred);
            label.setFont (theme::font (10.5f, true));
            addAndMakeVisible (slider);
            addAndMakeVisible (label);
        }
        void resized() override { auto b = getLocalBounds(); label.setBounds (b.removeFromTop (14)); slider.setBounds (b); }
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attach;
    };

    class PadButton : public juce::Component, public juce::FileDragAndDropTarget
    {
    public:
        PadButton (DrumMachineEditor& o, int p) : owner (o), pad (p) {}
        std::vector<float> peaks;

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const double since = juce::Time::getMillisecondCounterHiRes() - owner.drums.getLastHitTime (pad);
            const float flash = since < 260.0 ? (float) (1.0 - since / 260.0) : 0.0f;
            const bool sel = owner.selected == pad;
            const auto col = padColour (pad);

            // glow
            if (flash > 0.0f)
            {
                g.setColour (col.withAlpha (0.35f * flash));
                g.fillRoundedRectangle (r, 12.0f);
            }
            auto face = r.reduced (3.0f);
            juce::ColourGradient pg (juce::Colour (0xff2a2622).interpolatedWith (col, 0.10f + 0.55f * flash), face.getX(), face.getY(),
                                     juce::Colour (0xff141210).interpolatedWith (col, 0.04f + 0.3f * flash), face.getX(), face.getBottom(), false);
            g.setGradientFill (pg);
            g.fillRoundedRectangle (face, 9.0f);
            g.setColour (col.withAlpha (sel ? 0.95f : 0.18f + 0.5f * flash));
            g.drawRoundedRectangle (face, 9.0f, sel ? 2.0f : 1.0f);
            // colour strip on top
            g.setColour (col.withAlpha (0.8f));
            g.fillRoundedRectangle (face.getX() + 10, face.getY() + 6, face.getWidth() * 0.28f, 3.0f, 1.5f);
            if (dragOver) { g.setColour (theme::accent2); g.drawRoundedRectangle (face, 9.0f, 2.5f); }

            // waveform thumbnail
            auto wave = face.reduced (10, 0).withTrimmedTop (face.getHeight() * 0.42f).withTrimmedBottom (18);
            if (! peaks.empty())
            {
                g.setColour (col.withAlpha (0.45f + 0.4f * flash));
                const float bw = wave.getWidth() / (float) peaks.size();
                for (size_t i = 0; i < peaks.size(); ++i)
                {
                    const float hgt = juce::jmax (1.0f, peaks[i] * wave.getHeight());
                    g.fillRect (wave.getX() + bw * (float) i, wave.getCentreY() - hgt * 0.5f, juce::jmax (1.0f, bw - 1.0f), hgt);
                }
            }
            g.setColour (theme::text.withAlpha (0.92f));
            g.setFont (theme::font (11.5f, true));
            g.drawFittedText (owner.drums.getPadName (pad), face.reduced (10, 0).withTrimmedTop (12).withHeight (30).toNearestInt(), juce::Justification::topLeft, 2);
            g.setColour (owner.drums.padHasCustomSample (pad) ? theme::accent2 : theme::textDim);
            g.setFont (theme::font (9.5f, true));
            g.drawText (juce::MidiMessage::getMidiNoteName (owner.drums.getPadNote (pad), true, true, 4), face.reduced (9, 5).toNearestInt(),
                        juce::Justification::bottomRight);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.select (pad);
            owner.drums.triggerPad (pad, juce::jlimit (0.3f, 1.0f, 1.1f - (float) e.y / (float) getHeight() * 0.6f));   // harder near the top
        }

        bool isInterestedInFileDrag (const juce::StringArray& files) override { return files.size() == 1; }
        void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
        void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }
        void filesDropped (const juce::StringArray& files, int, int) override
        {
            dragOver = false;
            owner.drums.loadPadSample (pad, juce::File (files[0]));
            owner.select (pad);
        }

    private:
        DrumMachineEditor& owner;
        const int pad;
        bool dragOver = false;
    };

    void timerCallback() override
    {
        for (auto* b : padButtons) b->repaint();
        repaint (screen.expanded (4));
    }

    DrumLook look;
    DrumMachine& drums;
    juce::OwnedArray<PadButton> padButtons;
    juce::OwnedArray<KnobCell> knobs;
    std::unique_ptr<KnobCell> master;
    std::vector<float> screenPeaks;
    juce::Rectangle<int> padArea, screen, controls;
    juce::ComboBox kitBox;
    juce::Array<juce::File> userKits;

    void fillKitBox()
    {
        kitBox.clear (juce::dontSendNotification);
        kitBox.addSectionHeading ("RENASOUND");
        kitBox.addItemList (DrumMachine::getKitNames(), 1);
        userKits = DrumMachine::findKitFiles();
        if (! userKits.isEmpty())
        {
            kitBox.addSectionHeading ("YOUR KITS");
            for (int i = 0; i < userKits.size(); ++i) kitBox.addItem (userKits[i].getFileNameWithoutExtension(), 100 + i);
        }
        kitBox.addSeparator();
        kitBox.addItem ("Add kits folder...", 999);
        kitBox.setText (drums.getKitLabel(), juce::dontSendNotification);
    }
    juce::TextButton folderButton { "Load sample folder..." }, sampleButton { "Load sample..." }, resetButton { "Use kit sound" },
                     resetShapeButton { "Reset pad" };
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> masterAttach;
    std::unique_ptr<juce::FileChooser> chooser;
    int selected = 0;
};

juce::AudioProcessorEditor* DrumMachine::createEditor() { return new DrumMachineEditor (*this); }

} // namespace rs
