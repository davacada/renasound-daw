#include "AudioFit.h"
#include <array>
#include <cmath>

namespace rs::audiofit
{

namespace
{
    const char* const pcNames[12] = { "C", "C#", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B" };

    // Krumhansl-Kessler key profiles
    const double majorProfile[12] = { 6.35, 2.23, 3.48, 2.33, 4.38, 4.09, 2.52, 5.19, 2.39, 3.66, 2.29, 2.88 };
    const double minorProfile[12] = { 6.33, 2.68, 3.52, 5.38, 2.60, 3.53, 2.54, 4.75, 3.98, 2.69, 3.34, 3.17 };

    double correlate (const std::array<double, 12>& chroma, const double* profile, int key)
    {
        double mx = 0, my = 0;
        for (int i = 0; i < 12; ++i) { mx += chroma[(size_t) i]; my += profile[i]; }
        mx /= 12; my /= 12;
        double sxy = 0, sxx = 0, syy = 0;
        for (int i = 0; i < 12; ++i)
        {
            const double x = chroma[(size_t) ((i + key) % 12)] - mx, y = profile[i] - my;
            sxy += x * y; sxx += x * x; syy += y * y;
        }
        return sxx > 0 && syy > 0 ? sxy / std::sqrt (sxx * syy) : 0.0;
    }

    struct KeyResult { int key = -1; bool minor = false; float confidence = 0; };
    KeyResult keyFromChroma (const std::array<double, 12>& chroma)
    {
        double total = 0, peak = 0;
        for (auto v : chroma) { total += v; peak = std::max (peak, v); }
        if (total <= 0) return {};
        double best = -2, second = -2;
        KeyResult r;
        for (int k = 0; k < 12; ++k)
            for (int m = 0; m < 2; ++m)
            {
                const double c = correlate (chroma, m ? minorProfile : majorProfile, k);
                if (c > best) { second = best; best = c; r.key = k; r.minor = m == 1; }
                else if (c > second) second = c;
            }
        // flat chroma (drums, noise) has no key; a clear key correlates well and beats the runner-up
        const double flatness = total / 12.0 / peak;
        r.confidence = (float) juce::jlimit (0.0, 1.0, (best - 0.45) * 2.0 + (best - second) * 4.0);
        if (best < 0.55 || flatness > 0.8) r.key = -1;
        return r;
    }

    // mono, low-passed and decimated to about 11 kHz (all the analysis needs)
    std::vector<float> monoDecimated (const AudioData& a, double& rateOut)
    {
        const int factor = juce::jmax (1, (int) std::round (a.sampleRate / 11025.0));
        rateOut = a.sampleRate / factor;
        const int n = a.buffer.getNumSamples() / factor;
        std::vector<float> out ((size_t) juce::jmax (0, n));
        const auto* l = a.buffer.getReadPointer (0);
        const auto* r = a.buffer.getReadPointer (juce::jmin (1, a.buffer.getNumChannels() - 1));
        for (int i = 0; i < n; ++i)
        {
            float s = 0;
            for (int j = 0; j < factor; ++j) s += l[i * factor + j] + r[i * factor + j];
            out[(size_t) i] = s / (2.0f * factor);
        }
        return out;
    }

    float cubic (const float* x, int len, double pos)
    {
        const int i = (int) std::floor (pos);
        const float t = (float) (pos - i);
        auto at = [&] (int k) { return x[juce::jlimit (0, len - 1, k)]; };
        const float y0 = at (i - 1), y1 = at (i), y2 = at (i + 1), y3 = at (i + 2);
        return y1 + 0.5f * t * (y2 - y0 + t * (2.0f * y0 - 5.0f * y1 + 4.0f * y2 - y3 + t * (3.0f * (y1 - y2) + y3 - y0)));
    }
}

juce::String keyName (int key, bool minor)
{
    if (key < 0) return {};
    return juce::String (pcNames[key % 12]) + (minor ? " minor" : " major");
}

// ---------------------------------------------------------------------------
double bpmFromName (const juce::String& name)
{
    // "120bpm", "120 BPM", "bpm120", "120-bpm"
    const auto s = name.toLowerCase();
    for (int i = s.indexOf ("bpm"); i >= 0; i = s.indexOf (i + 1, "bpm"))
    {
        int e = i;
        while (e > 0 && (s[e - 1] == ' ' || s[e - 1] == '_' || s[e - 1] == '-')) --e;
        int b = e;
        while (b > 0 && (juce::CharacterFunctions::isDigit (s[b - 1]) || s[b - 1] == '.')) --b;
        double v = s.substring (b, e).getDoubleValue();
        if (v < 40 || v > 300)
        {
            int a = i + 3;
            while (a < s.length() && (s[a] == ' ' || s[a] == '_' || s[a] == '-')) ++a;
            int z = a;
            while (z < s.length() && (juce::CharacterFunctions::isDigit (s[z]) || s[z] == '.')) ++z;
            v = s.substring (a, z).getDoubleValue();
        }
        if (v >= 40 && v <= 300) return v;
    }
    return 0;
}

int keyFromName (const juce::String& name, bool& minor)
{
    // tokens like "Am", "C#min", "Gbmaj", "F#m", "A minor", "Cmaj7" (the 7 is ignored)
    juce::StringArray tokens;
    juce::String cur;
    for (auto c : name + " ")
    {
        if (juce::CharacterFunctions::isLetterOrDigit (c) || c == '#') cur << juce::String::charToString (c);
        else { if (cur.isNotEmpty()) tokens.add (cur); cur.clear(); }
    }
    for (int t = 0; t < tokens.size(); ++t)
    {
        auto tok = tokens[t];
        if (tok.isEmpty()) continue;
        const auto letter = juce::CharacterFunctions::toUpperCase (tok[0]);
        if (letter < 'A' || letter > 'G') continue;
        static const int base[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        int key = base[letter - 'A'];
        int p = 1;
        if (p < tok.length() && tok[p] == '#') { ++key; ++p; }
        else if (p < tok.length() && tok[p] == 'b' && tok.length() != 2) { --key; ++p; }   // "Bb", "Ebm" (but "Ab" alone is A-flat too)
        else if (p < tok.length() && tok[p] == 'b' && tok.length() == 2) { --key; ++p; }
        auto rest = tok.substring (p);
        while (rest.isNotEmpty() && juce::CharacterFunctions::isDigit (rest.getLastCharacter())) rest = rest.dropLastCharacters (1);
        bool isMinor = false, explicitMode = false;
        if (rest == "m" || rest == "min" || rest == "minor" || rest == "mi") { isMinor = true; explicitMode = true; }
        else if (rest == "maj" || rest == "major" || rest == "M" || rest == "ma") explicitMode = true;
        else if (rest.isNotEmpty()) continue;   // a word that happens to start with A-G ("Bass", "Drums")
        if (! explicitMode)
        {
            // "A minor" written as two words, or a bare note name next to a tempo ("120 Am"): bare letters only count with a mode word after them
            const auto next = tokens[t + 1].toLowerCase();
            if (next == "minor" || next == "min") { isMinor = true; explicitMode = true; }
            else if (next == "major" || next == "maj") explicitMode = true;
            else if (tok.length() < 2) continue;   // a lone "A" or "E" is too often something else
        }
        minor = isMinor;
        return ((key % 12) + 12) % 12;
    }
    return -1;
}

// ---------------------------------------------------------------------------
Analysis analyse (const AudioData& audio, const juce::String& fileName, int beatsPerBar)
{
    Analysis a;
    double rate = 11025;
    const auto x = monoDecimated (audio, rate);
    const double duration = audio.getLengthSeconds();
    if (x.size() < 4096) return a;

    // ---- tempo: spectral flux onsets, then autocorrelation ----
    {
        constexpr int order = 9, N = 1 << order, hop = 128;
        juce::dsp::FFT fft (order);
        std::vector<float> win (N), buf (2 * N), prev (N / 2 + 1, 0.0f), onset;
        for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N);
        for (size_t s = 0; s + N <= x.size(); s += hop)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < N; ++i) buf[(size_t) i] = x[s + (size_t) i] * win[(size_t) i];
            fft.performFrequencyOnlyForwardTransform (buf.data());
            float flux = 0;
            for (int k = 1; k <= N / 2; ++k)
            {
                const float m = std::log1p (100.0f * buf[(size_t) k]);
                flux += std::max (0.0f, m - prev[(size_t) k]);
                prev[(size_t) k] = m;
            }
            onset.push_back (flux);
        }
        const double envRate = rate / hop;
        // remove the slow trend, keep the peaks
        std::vector<float> o (onset.size());
        const int w = (int) (envRate * 0.25);
        for (size_t i = 0; i < onset.size(); ++i)
        {
            double m = 0; int cnt = 0;
            for (int j = -w; j <= w; ++j) if ((int) i + j >= 0 && (int) i + j < (int) onset.size()) { m += onset[(size_t) ((int) i + j)]; ++cnt; }
            o[i] = std::max (0.0f, onset[i] - (float) (m / cnt));
        }
        const int minLag = (int) (envRate * 60.0 / 200.0), maxLag = (int) (envRate * 60.0 / 55.0);
        if ((int) o.size() > maxLag * 2)
        {
            std::vector<double> ac ((size_t) maxLag * 2 + 2, 0.0);
            double ac0 = 0;
            for (size_t i = 0; i < o.size(); ++i) ac0 += (double) o[i] * o[i];
            for (int lag = minLag / 2; lag <= maxLag * 2 && lag < (int) o.size(); ++lag)
            {
                double s = 0;
                for (size_t i = (size_t) lag; i < o.size(); ++i) s += (double) o[i] * o[i - (size_t) lag];
                ac[(size_t) lag] = s / (double) (o.size() - (size_t) lag) * (double) o.size();
            }
            double best = 0; int bestLag = 0;
            for (int lag = minLag; lag <= maxLag; ++lag)
            {
                // a beat repeats: its double should line up too; prefer tempos near 120
                const double bpm = 60.0 * envRate / lag;
                const double prior = std::exp (-0.5 * std::pow (std::log2 (bpm / 120.0) / 0.9, 2.0));
                const double score = (ac[(size_t) lag] + 0.5 * ac[(size_t) std::min (lag * 2, maxLag * 2)]) * prior;
                if (score > best) { best = score; bestLag = lag; }
            }
            if (bestLag > minLag && bestLag < maxLag && ac0 > 0)
            {
                const double y0 = ac[(size_t) bestLag - 1], y1 = ac[(size_t) bestLag], y2 = ac[(size_t) bestLag + 1];
                const double den = y0 - 2 * y1 + y2;
                const double lag = bestLag + (std::abs (den) > 1e-12 ? 0.5 * (y0 - y2) / den : 0.0);
                a.bpm = 60.0 * envRate / lag;
                a.tempoConfidence = (float) juce::jlimit (0.0, 1.0, ac[(size_t) bestLag] / ac0 * 2.5);
            }
        }
    }

    // ---- the file name knows best ----
    if (const double named = bpmFromName (fileName); named > 0) { a.bpm = named; a.tempoConfidence = 1.0f; a.tempoFromName = true; }

    // ---- a loop: a whole number of bars (lock the tempo to the exact length) ----
    if (a.bpm > 0 && duration > 0.5 && duration < 60.0)
    {
        double bestErr = 1e9; int bestBeats = 0;
        for (int bars : { 1, 2, 4, 8, 16, 32 })
            for (int mi = 0; mi < 3; ++mi)
            {
                const double mult = mi == 0 ? 0.5 : mi == 1 ? 1.0 : 2.0;
                const int beats = bars * beatsPerBar;
                const double impliedBpm = beats * 60.0 / duration;
                const double err = std::abs (std::log2 (impliedBpm / (a.bpm * mult)));
                const double score = err + (mi == 1 ? 0.0 : 0.02);   // the beat we heard, before its half / double
                if (score < bestErr && impliedBpm >= 55 && impliedBpm <= 200 && (mi == 1 || ! a.tempoFromName)) { bestErr = score; bestBeats = beats; }
            }
        if (bestBeats > 0 && bestErr < (a.tempoFromName ? 0.02 : 0.065))
        {
            a.isLoop = true;
            a.bars = bestBeats / beatsPerBar;
            a.bpm = bestBeats * 60.0 / duration;
            a.tempoConfidence = std::max (a.tempoConfidence, 0.8f);
        }
    }
    if (a.bpm > 0 && ! a.tempoFromName) a.bpm = std::round (a.bpm * 100.0) / 100.0;

    // ---- key: chroma against the major / minor profiles ----
    {
        constexpr int order = 13, N = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> buf (2 * N), win (N);
        for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N);
        std::array<double, 12> chroma {};
        std::vector<int> pcOfBin (N / 2 + 1, -1);
        for (int k = 1; k <= N / 2; ++k)
        {
            const double f = k * rate / N;
            if (f >= 55.0 && f <= 2000.0) pcOfBin[(size_t) k] = (int) std::lround (69.0 + 12.0 * std::log2 (f / 440.0)) % 12;
        }
        int frames = 0;
        for (size_t s = 0; s + N <= x.size() && frames < 400; s += N / 2, ++frames)
        {
            std::fill (buf.begin(), buf.end(), 0.0f);
            for (int i = 0; i < N; ++i) buf[(size_t) i] = x[s + (size_t) i] * win[(size_t) i];
            fft.performFrequencyOnlyForwardTransform (buf.data());
            // only the frame's clear spectral peaks count: tones, not the noise of hats and snares
            float frameMax = 0;
            for (int k = 1; k <= N / 2; ++k) if (pcOfBin[(size_t) k] >= 0) frameMax = std::max (frameMax, buf[(size_t) k]);
            for (int k = 2; k < N / 2; ++k)
            {
                const float m = buf[(size_t) k];
                if (pcOfBin[(size_t) k] >= 0 && m > 0.1f * frameMax && m > buf[(size_t) k - 1] && m >= buf[(size_t) k + 1]
                    && m > 2.0f * std::max (buf[(size_t) k - 3], buf[(size_t) k + 3]))
                    chroma[(size_t) pcOfBin[(size_t) k]] += m;
            }
        }
        const auto kr = keyFromChroma (chroma);
        a.key = kr.key; a.minor = kr.minor; a.keyConfidence = kr.confidence;
    }
    bool nameMinor = false;
    if (const int nk = keyFromName (fileName, nameMinor); nk >= 0) { a.key = nk; a.minor = nameMinor; a.keyConfidence = 1.0f; a.keyFromName = true; }
    return a;
}

int songKey (const Project& p, bool& minor)
{
    std::array<double, 12> chroma {};
    int notes = 0;
    for (auto& t : p.tracks)
    {
        if (t->type != Track::Type::Instrument) continue;
        if (t->instrument != nullptr && t->instrument->type == "builtin:drums") continue;
        for (auto& c : t->clips)
            for (auto& n : c->notes)
            {
                if (n.start < 0 || n.start >= c->lengthBeats) continue;
                chroma[(size_t) (n.pitch % 12)] += std::min (n.length, 4.0);
                ++notes;
            }
    }
    if (notes < 12) return -1;
    const auto kr = keyFromChroma (chroma);
    minor = kr.minor;
    return kr.key;
}

int semitonesBetween (int key, bool minor, int targetKey, bool targetMinor)
{
    if (key < 0 || targetKey < 0) return 0;
    int target = targetKey;
    if (minor && ! targetMinor) target = (targetKey + 9) % 12;   // the song's relative minor
    if (! minor && targetMinor) target = (targetKey + 3) % 12;   // the song's relative major
    int d = ((target - key) % 12 + 12) % 12;
    if (d > 5) d -= 12;
    return d;
}

// ---------------------------------------------------------------------------
std::shared_ptr<AudioData> render (const AudioData& src, double stretch, double semitones, const std::atomic<bool>* cancel)
{
    auto out = std::make_shared<AudioData>();
    out->sampleRate = src.sampleRate;
    const int len = src.buffer.getNumSamples();
    const int chans = juce::jmin (2, src.buffer.getNumChannels());
    if (len < 16 || stretch <= 0.05 || stretch > 20) return nullptr;

    const double pitch = std::pow (2.0, semitones / 12.0);
    const double a = stretch * pitch;   // time-stretch first, then resample by `pitch`

    // ---- WSOLA time-stretch by `a` ----
    juce::AudioBuffer<float> st;
    if (std::abs (a - 1.0) < 1.0e-4)
        st.makeCopyOf (src.buffer);
    else
    {
        int N = (int) std::round (2048.0 * src.sampleRate / 44100.0);
        N += N & 1;
        const int Hs = N / 2;
        const double Ha = Hs / a;
        const int tol = N / 4;
        const int outLen = (int) std::round (len * a);
        st.setSize (chans, outLen + N);
        st.clear();
        std::vector<float> norm ((size_t) (outLen + N), 0.0f), win ((size_t) N), mono ((size_t) len);
        for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * i / N);
        for (int i = 0; i < len; ++i)
        {
            float s = 0;
            for (int c = 0; c < chans; ++c) s += src.buffer.getSample (c, i);
            mono[(size_t) i] = s;
        }
        auto clampPos = [&] (long p) { return (int) juce::jlimit (0L, (long) juce::jmax (0, len - N), p); };
        int prev = 0;
        for (int k = 0; (long) k * Hs < outLen; ++k)
        {
            if (cancel != nullptr && cancel->load()) return nullptr;
            const long nominal = (long) std::llround (k * Ha);
            int pos = clampPos (nominal);
            if (k > 0)
            {
                // the input position whose start best continues what the last frame was heading into
                const int natural = prev + Hs;
                double best = -1e30;
                for (int d = -tol; d <= tol; d += 2)
                {
                    const int cand = clampPos (nominal + d);
                    if (natural + Hs > len || cand + Hs > len) break;
                    double s = 0;
                    for (int i = 0; i < Hs; i += 2) s += (double) mono[(size_t) (natural + i)] * mono[(size_t) (cand + i)];
                    if (s > best) { best = s; pos = cand; }
                }
            }
            prev = pos;
            const int o = k * Hs;
            for (int c = 0; c < chans; ++c)
            {
                const float* in = src.buffer.getReadPointer (c);
                float* dst = st.getWritePointer (c);
                for (int i = 0; i < N && pos + i < len && o + i < st.getNumSamples(); ++i) dst[o + i] += in[pos + i] * win[(size_t) i];
            }
            for (int i = 0; i < N && o + i < (int) norm.size(); ++i) norm[(size_t) (o + i)] += win[(size_t) i];
        }
        for (int c = 0; c < chans; ++c)
        {
            float* dst = st.getWritePointer (c);
            for (int i = 0; i < outLen; ++i) dst[i] = norm[(size_t) i] > 1.0e-3f ? dst[i] / norm[(size_t) i] : 0.0f;
        }
        st.setSize (chans, outLen, true);
    }

    // ---- resample by `pitch` (faster playback = higher) ----
    const int finalLen = juce::jmax (1, (int) std::round (len * stretch));
    out->buffer.setSize (2, finalLen);
    for (int c = 0; c < 2; ++c)
    {
        const float* in = st.getReadPointer (juce::jmin (c, chans - 1));
        float* dst = out->buffer.getWritePointer (c);
        if (std::abs (pitch - 1.0) < 1.0e-6)
            for (int i = 0; i < finalLen; ++i) dst[i] = i < st.getNumSamples() ? in[i] : 0.0f;
        else
            for (int i = 0; i < finalLen; ++i) dst[i] = cubic (in, st.getNumSamples(), i * pitch);
    }
    // peaks for drawing
    const int blocks = (finalLen + AudioData::peakBlock - 1) / AudioData::peakBlock;
    out->peaks.assign ((size_t) blocks * 2, 0.0f);
    for (int b = 0; b < blocks; ++b)
    {
        float mn = 0, mx = 0;
        for (int i = b * AudioData::peakBlock; i < juce::jmin (finalLen, (b + 1) * AudioData::peakBlock); ++i)
            for (int c = 0; c < 2; ++c) { const float v = out->buffer.getSample (c, i); mn = std::min (mn, v); mx = std::max (mx, v); }
        out->peaks[(size_t) b * 2] = mn; out->peaks[(size_t) b * 2 + 1] = mx;
    }
    return out;
}

// ---------------------------------------------------------------------------
Fit plan (const Analysis& a, const Project& p, bool& adoptTempo, bool onlyIfLoop)
{
    Fit f;
    adoptTempo = false;
    juce::StringArray what;

    bool songHasMusic = false;
    for (auto& t : p.tracks) if (! t->clips.empty()) songHasMusic = true;

    if (a.bpm > 0 && a.tempoConfidence >= 0.3f && (a.isLoop || ! onlyIfLoop))
    {
        if (! songHasMusic && a.isLoop)
        {
            adoptTempo = true;   // the first loop in an empty song sets the tempo; nothing to stretch
            f.sourceBpm = a.bpm;
            what.add ("the song now runs at the loop's " + juce::String (a.bpm, 1) + " BPM");
        }
        else
        {
            // half / double time count as the same groove: stretch by the smallest amount
            double src = a.bpm;
            for (double m : { 0.5, 2.0 })
                if (std::abs (std::log2 (a.bpm * m / p.bpm)) < std::abs (std::log2 (src / p.bpm))) src = a.bpm * m;
            const double stretch = src / p.bpm;
            if (std::abs (stretch - 1.0) > 0.003 && stretch > 0.5 && stretch < 2.0)
            {
                f.tempo = true; f.stretch = stretch; f.sourceBpm = src;
                what.add (juce::String (a.bpm, std::abs (a.bpm - std::round (a.bpm)) < 1.0e-9 ? 0 : 1) + " -> " + juce::String (p.bpm, std::abs (p.bpm - std::round (p.bpm)) < 1.0e-9 ? 0 : 1) + " BPM");
            }
        }
    }

    bool songMinor = false;
    const int sk = songKey (p, songMinor);
    if (a.key >= 0 && sk >= 0 && (a.keyConfidence >= 0.35f || a.keyFromName))
    {
        const int semis = semitonesBetween (a.key, a.minor, sk, songMinor);
        if (semis != 0)
        {
            f.key = true; f.semitones = semis;
            const int newKey = ((a.key + semis) % 12 + 12) % 12;
            what.add (keyName (a.key, a.minor) + " -> " + keyName (newKey, a.minor) + " (" + (semis > 0 ? "+" : "") + juce::String (semis) + ")");
        }
    }
    f.description = what.joinIntoString (", ");
    return f;
}

juce::File renderToFile (const AudioData& src, const Fit& f, const juce::String& baseName, juce::String& error)
{
    auto out = render (src, f.stretch, f.semitones);
    if (out == nullptr) { error = "Couldn't stretch this audio."; return {}; }
    auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Renasound").getChildFile ("Fitted Audio");
    dir.createDirectory();
    juce::String tag;
    if (f.tempo) tag << juce::String (f.sourceBpm / f.stretch, 1).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") << " BPM";
    if (f.key) tag << (tag.isEmpty() ? "" : ", ") << (f.semitones > 0 ? "+" : "") << juce::String ((int) f.semitones) << " st";
    auto file = dir.getChildFile (juce::File::createLegalFileName (baseName + (tag.isNotEmpty() ? " (" + tag + ")" : juce::String())) + ".wav").getNonexistentSibling();
    std::unique_ptr<juce::OutputStream> os (file.createOutputStream());
    if (os == nullptr) { error = "Couldn't write " + file.getFullPathName(); return {}; }
    juce::WavAudioFormat wav;
    auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (src.sampleRate).withNumChannels (2).withBitsPerSample (24));
    if (w == nullptr) { error = "Couldn't write " + file.getFullPathName(); return {}; }
    w->writeFromAudioSampleBuffer (out->buffer, 0, out->buffer.getNumSamples());
    w.reset();
    return file;
}

} // namespace rs::audiofit
