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
        const size_t fade = juce::jmin<size_t> (96, x.size());
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

    std::vector<float> snare (double tone1, double tone2, double toneDecay, double noiseDecay, float noiseLevel, double noiseHp, int seed)
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
            x[i] = (float) tone * 0.7f + (i < noise.size() ? noise[i] * noiseLevel : 0.0f);
        }
        normalise (x);
        return x;
    }

    std::vector<float> clap (double centre, double tail, int seed)
    {
        Gen g (seed);
        auto x = makeBuffer (tail * 7.0 + 0.05);
        const double bursts[] = { 0.0, 0.011, 0.021, 0.031 };
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

    std::vector<float> tom (double freq, double decay, float noiseAmt, int seed)
    {
        Gen g (seed);
        auto x = makeBuffer (decay * 7.0 + 0.02);
        double phase = 0;
        for (size_t i = 0; i < x.size(); ++i)
        {
            const double t = i / synthRate;
            const double f = freq * (1.0 + 0.6 * std::exp (-t / 0.04));
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

    // kit index, pad index -> sound
    std::vector<float> synthesise (int kit, int pad)
    {
        const int seed = 1000 + kit * 37 + pad;
        const std::vector<double> hat808 { 205.3, 304.4, 369.6, 522.7, 540.0, 800.0 };
        std::vector<double> hatFreqs = hat808;
        const double hatScale = kit == 1 || kit == 4 ? 1.45 : kit == 2 ? 1.3 : 1.0;
        for (auto& f : hatFreqs) f *= hatScale;

        std::vector<float> x;
        switch (pad)
        {
            case 0: // kick
                if (kit == 0)      x = kick (150, 47, 0.045, 0.42, 0.10f, 1.3f, seed);
                else if (kit == 1) x = kick (270, 52, 0.022, 0.20, 0.55f, 2.8f, seed);
                else if (kit == 2) x = kick (125, 41, 0.070, 0.85, 0.15f, 2.0f, seed);
                else if (kit == 3) x = kick (140, 50, 0.040, 0.30, 0.20f, 1.8f, seed);
                else               x = kick (230, 50, 0.028, 0.26, 0.40f, 2.2f, seed);
                break;
            case 1: // snare
                if (kit == 0)      x = snare (238, 476, 0.09, 0.14, 0.55f, 1800, seed);
                else if (kit == 1) x = snare (185, 330, 0.07, 0.19, 0.95f, 1200, seed);
                else if (kit == 2) x = snare (215, 430, 0.05, 0.11, 1.10f, 2500, seed);
                else if (kit == 3) x = snare (200, 380, 0.08, 0.16, 0.80f, 1400, seed);
                else               x = snare (190, 350, 0.06, 0.17, 0.90f, 1500, seed);
                break;
            case 2:  x = clap (kit == 2 ? 1400 : 1100, kit == 1 || kit == 4 ? 0.16 : 0.12, seed); break;
            case 3:  x = blip (1700, 820, 0.012, 0.6f, 500, seed); break;
            case 4:  x = metal (hatFreqs, kit == 2 ? 0.032 : 0.045, 7000, kit == 1 || kit == 4 ? 0.45f : 0.15f, seed); break;
            case 5:  x = metal (hatFreqs, kit == 2 ? 0.25 : 0.33, 6500, kit == 1 || kit == 4 ? 0.45f : 0.15f, seed); break;
            case 6:  x = tom (kit == 0 || kit == 2 ? 80 : 95, kit == 0 ? 0.35 : 0.25, 0.08f, seed); break;
            case 7:  x = tom (kit == 0 || kit == 2 ? 115 : 135, kit == 0 ? 0.30 : 0.22, 0.08f, seed); break;
            case 8:  x = tom (kit == 0 || kit == 2 ? 160 : 185, kit == 0 ? 0.26 : 0.19, 0.08f, seed); break;
            case 9:  x = metal ({ 296, 413, 517, 641, 782, 931, 1103, 1320 }, 1.3, 4500, 0.55f, seed); break;   // crash
            case 10: x = metal ({ 330, 505, 612, 791, 1012, 1324 }, 0.9, 3800, 0.30f, seed); break;             // ride
            case 11: x = cowbell (kit == 1 || kit == 4 ? 1.1 : 1.0, seed); break;
            case 12: x = shaker (0.06, 7000, 3, seed); break;                                                   // tambourine
            case 13: x = shaker (0.035, 5000, 2, seed); break;                                                  // shaker
            case 14: x = blip (2500, 5000, 0.035, 0.1f, 800, seed); break;                                      // clave
            case 15: x = snare (kit == 2 ? 260 : 220, kit == 2 ? 520 : 440, 0.04, 0.08, 1.0f, 3000, seed); break;   // snare 2
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
    }
    loadKit (0);
}

DrumMachine::~DrumMachine() = default;

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
        const auto x = synthesise (kit, p);
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
    const auto x = synthesise (kitIndex.load(), pad);
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
    addIf (juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("RENASOUND Kits"));
    addIf (juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("RENASOUND Kits"));
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
                const float sl = l[i0] + frac * (l[i0 + 1] - l[i0]);
                const float sr = r[i0] + frac * (r[i0 + 1] - r[i0]);
                buffer.addSample (0, i, sl * v.gainL * v.fade);
                if (buffer.getNumChannels() > 1) buffer.addSample (1, i, sr * v.gainR * v.fade);
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
}

// =============================================================================
//  Editor: 4 x 4 pads + the selected pad's controls
// =============================================================================
class DrumMachineEditor : public juce::AudioProcessorEditor, private juce::Timer
{
public:
    explicit DrumMachineEditor (DrumMachine& d) : AudioProcessorEditor (d), drums (d)
    {
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

        for (auto* k : { &level, &tune, &pan, &master })
        {
            addAndMakeVisible (k);
            k->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            k->setTextBoxStyle (juce::Slider::TextBoxBelow, false, 64, 18);
        }
        for (auto* l : { &levelLabel, &tuneLabel, &panLabel, &masterLabel, &padTitle })
        {
            addAndMakeVisible (l);
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::textColourId, theme::textDim);
            l->setFont (theme::font (11.5f, true));
        }
        levelLabel.setText ("LEVEL", juce::dontSendNotification);
        tuneLabel.setText ("TUNE", juce::dontSendNotification);
        panLabel.setText ("PAN", juce::dontSendNotification);
        masterLabel.setText ("KIT VOLUME", juce::dontSendNotification);
        padTitle.setFont (theme::font (15.0f, true));
        padTitle.setColour (juce::Label::textColourId, theme::text);
        padTitle.setJustificationType (juce::Justification::centredLeft);
        masterAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (drums.state, "master", master);

        drums.onPadsChanged = [sp = juce::Component::SafePointer<DrumMachineEditor> (this)]
        {
            juce::MessageManager::callAsync ([sp] { if (sp != nullptr) sp->refresh(); });
        };
        select (0);
        refresh();
        setSize (640, 400);
        startTimerHz (30);
    }

    ~DrumMachineEditor() override { drums.onPadsChanged = nullptr; }

    void select (int pad)
    {
        selected = pad;
        levelAttach.reset (); tuneAttach.reset(); panAttach.reset();
        levelAttach = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (drums.state, "lvl" + juce::String (pad), level);
        tuneAttach  = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (drums.state, "tune" + juce::String (pad), tune);
        panAttach   = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (drums.state, "pan" + juce::String (pad), pan);
        refresh();
    }

    void refresh()
    {
        kitBox.setText (drums.getKitLabel(), juce::dontSendNotification);
        if (drums.getKitLabel() == DrumMachine::getKitNames()[drums.getKitIndex()]) kitBox.setSelectedId (drums.getKitIndex() + 1, juce::dontSendNotification);
        padTitle.setText (drums.getPadName (selected) + "   (note " + juce::MidiMessage::getMidiNoteName (drums.getPadNote (selected), true, true, 4) + ")",
                          juce::dontSendNotification);
        resetButton.setEnabled (drums.padHasCustomSample (selected));
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        g.setColour (theme::accent);
        g.fillRoundedRectangle (16.0f, 16.0f, 4.0f, 22.0f, 2.0f);
        g.setColour (theme::text);
        g.setFont (theme::font (17.0f, true));
        g.drawText ("RENASOUND DRUMS", 28, 12, 220, 30, juce::Justification::centredLeft);
        g.setColour (theme::panelHi);
        g.fillRoundedRectangle (getLocalBounds().toFloat().withTrimmedLeft (380).reduced (8, 52).withTrimmedBottom (-40), 8.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16);
        auto top = r.removeFromTop (30);
        folderButton.setBounds (top.removeFromRight (150));
        top.removeFromRight (8);
        kitBox.setBounds (top.removeFromRight (140));
        r.removeFromTop (12);

        auto grid = r.removeFromLeft (356);
        const int cell = grid.getWidth() / 4;
        for (int p = 0; p < DrumMachine::numPads; ++p)
        {
            const int row = 3 - p / 4, col = p % 4;   // like hardware: first pads at the bottom
            padButtons[p]->setBounds (grid.getX() + col * cell, grid.getY() + row * cell * 76 / 100 + 0, cell - 6, cell * 76 / 100 - 6);
        }

        auto side = r.withTrimmedLeft (24).reduced (8, 4);
        padTitle.setBounds (side.removeFromTop (24));
        side.removeFromTop (8);
        auto knobs = side.removeFromTop (104);
        const int kw = knobs.getWidth() / 3;
        for (auto [k, l] : { std::pair<juce::Slider*, juce::Label*> { &level, &levelLabel }, { &tune, &tuneLabel }, { &pan, &panLabel } })
        {
            auto c = knobs.removeFromLeft (kw);
            l->setBounds (c.removeFromTop (16));
            k->setBounds (c);
        }
        side.removeFromTop (10);
        auto buttons = side.removeFromTop (28);
        sampleButton.setBounds (buttons.removeFromLeft (buttons.getWidth() / 2 + 20).reduced (2, 0));
        resetButton.setBounds (buttons.reduced (2, 0));
        side.removeFromTop (14);
        auto m = side.removeFromTop (100).withSizeKeepingCentre (110, 100);
        masterLabel.setBounds (m.removeFromTop (16));
        master.setBounds (m);
    }

private:
    class PadButton : public juce::Component, public juce::FileDragAndDropTarget
    {
    public:
        PadButton (DrumMachineEditor& o, int p) : owner (o), pad (p) {}

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const double since = juce::Time::getMillisecondCounterHiRes() - owner.drums.getLastHitTime (pad);
            const float flash = since < 180.0 ? (float) (1.0 - since / 180.0) : 0.0f;
            const bool sel = owner.selected == pad;
            g.setColour (theme::bg.brighter (0.06f).interpolatedWith (theme::accent, 0.15f + 0.65f * flash));
            g.fillRoundedRectangle (r, 7.0f);
            if (sel) { g.setColour (theme::accent); g.drawRoundedRectangle (r.reduced (1), 7.0f, 2.0f); }
            if (dragOver) { g.setColour (theme::accent2); g.drawRoundedRectangle (r.reduced (1), 7.0f, 2.5f); }
            g.setColour (theme::text);
            g.setFont (theme::font (12.0f, true));
            g.drawFittedText (owner.drums.getPadName (pad), getLocalBounds().reduced (6, 6), juce::Justification::centredTop, 2);
            g.setColour (owner.drums.padHasCustomSample (pad) ? theme::accent2 : theme::textDim);
            g.setFont (theme::font (10.0f));
            g.drawText (juce::MidiMessage::getMidiNoteName (owner.drums.getPadNote (pad), true, true, 4), getLocalBounds().reduced (6, 4),
                        juce::Justification::bottomRight);
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            owner.select (pad);
            // harder near the top of the pad
            owner.drums.triggerPad (pad, juce::jlimit (0.3f, 1.0f, 1.1f - (float) e.y / (float) getHeight() * 0.6f));
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

    void timerCallback() override { for (auto* b : padButtons) b->repaint(); }

    DrumMachine& drums;
    juce::OwnedArray<PadButton> padButtons;
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
    juce::TextButton folderButton { "Load sample folder..." }, sampleButton { "Load sample..." }, resetButton { "Use kit sound" };
    juce::Slider level, tune, pan, master;
    juce::Label levelLabel, tuneLabel, panLabel, masterLabel, padTitle;
    std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> levelAttach, tuneAttach, panAttach, masterAttach;
    std::unique_ptr<juce::FileChooser> chooser;
    int selected = 0;
};

juce::AudioProcessorEditor* DrumMachine::createEditor() { return new DrumMachineEditor (*this); }

} // namespace rs
