#include "Pitch.h"

namespace rs
{

static float hzToMidi (double hz) { return (float) (69.0 + 12.0 * std::log2 (hz / 440.0)); }

// =============================================================================
//  Analysis
// =============================================================================
PitchAnalysisPtr pitchdsp::analyse (const AudioData& data, const std::atomic<bool>* shouldStop)
{
    auto result = std::make_shared<PitchAnalysis>();
    const double sr = data.sampleRate;
    const int len = data.buffer.getNumSamples();
    result->sampleRate = sr;

    // mono, decimated to ~11 kHz: plenty for pitch and much cheaper
    const int decim = juce::jmax (1, (int) std::round (sr / 11025.0));
    const double asr = sr / decim;
    const int alen = len / decim;
    std::vector<float> x ((size_t) alen + 2048, 0.0f);
    {
        auto* l = data.buffer.getReadPointer (0);
        auto* r = data.buffer.getReadPointer (juce::jmin (1, data.buffer.getNumChannels() - 1));
        for (int i = 0; i < alen; ++i)
        {
            float acc = 0;
            for (int k = 0; k < decim; ++k) acc += l[i * decim + k] + r[i * decim + k];
            x[(size_t) i] = acc / (2.0f * (float) decim);
        }
    }

    const int hopA = 64;
    const int W = (int) std::ceil (asr / 30.0);         // window: two periods of the lowest note
    const int tauMin = (int) std::floor (asr / 1100.0);  // ~1100 Hz top
    const int tauMax = (int) std::ceil (asr / 65.0);     // ~65 Hz bottom
    result->hop = hopA * decim;

    const int numFrames = alen / hopA + 1;
    result->pitch.assign ((size_t) numFrames, std::numeric_limits<float>::quiet_NaN());
    result->levelDb.assign ((size_t) numFrames, -100.0f);
    std::vector<float> aperiodicity ((size_t) numFrames, 1.0f);
    std::vector<float> d ((size_t) tauMax + 2), cmnd ((size_t) tauMax + 2);

    auto sample = [&] (int i) { return (i >= 0 && i < alen) ? x[(size_t) i] : 0.0f; };
    float maxLevel = -100.0f;

    for (int f = 0; f < numFrames; ++f)
    {
        if (shouldStop != nullptr && shouldStop->load()) return nullptr;
        const int t = f * hopA - W / 2;   // frames are centred on f * hop

        double energy = 0;
        for (int j = 0; j < W; ++j) { const float v = sample (t + j); energy += v * v; }
        const float db = juce::Decibels::gainToDecibels ((float) std::sqrt (energy / W), -100.0f);
        result->levelDb[(size_t) f] = db;
        maxLevel = juce::jmax (maxLevel, db);
        if (db < -60.0f) continue;

        // YIN difference function + cumulative mean normalisation
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            double acc = 0;
            for (int j = 0; j < W; ++j)
            {
                const float diff = sample (t + j) - sample (t + j + tau);
                acc += diff * diff;
            }
            d[(size_t) tau] = (float) acc;
        }
        double running = 0;
        cmnd[0] = 1.0f;
        for (int tau = 1; tau <= tauMax; ++tau)
        {
            running += d[(size_t) tau];
            cmnd[(size_t) tau] = running > 0 ? (float) (d[(size_t) tau] * tau / running) : 1.0f;
        }

        int best = -1;
        for (int tau = juce::jmax (2, tauMin); tau < tauMax; ++tau)
            if (cmnd[(size_t) tau] < 0.15f)
            {
                while (tau + 1 < tauMax && cmnd[(size_t) tau + 1] < cmnd[(size_t) tau]) ++tau;
                best = tau;
                break;
            }
        if (best < 0)
        {
            best = juce::jmax (2, tauMin);
            for (int tau = best; tau < tauMax; ++tau)
                if (cmnd[(size_t) tau] < cmnd[(size_t) best]) best = tau;
        }

        aperiodicity[(size_t) f] = cmnd[(size_t) best];
        if (cmnd[(size_t) best] > 0.3f) continue;

        // parabolic interpolation for sub-sample accuracy
        double tauF = best;
        const float a = cmnd[(size_t) best - 1], b = cmnd[(size_t) best], c = cmnd[(size_t) best + 1];
        const float denom = a - 2 * b + c;
        if (std::abs (denom) > 1.0e-9f) tauF += 0.5 * (a - c) / denom;
        result->pitch[(size_t) f] = hzToMidi (asr / tauF);
    }

    // quiet frames relative to the loudest part are treated as unvoiced
    for (int f = 0; f < numFrames; ++f)
        if (result->levelDb[(size_t) f] < maxLevel - 42.0f)
            result->pitch[(size_t) f] = std::numeric_limits<float>::quiet_NaN();

    // median-smooth voiced runs and drop tiny islands
    auto& p = result->pitch;
    std::vector<float> smoothed (p);
    for (int f = 0; f < numFrames; ++f)
    {
        if (std::isnan (p[(size_t) f])) continue;
        std::vector<float> win;
        for (int k = -2; k <= 2; ++k)
            if (f + k >= 0 && f + k < numFrames && ! std::isnan (p[(size_t) (f + k)])) win.push_back (p[(size_t) (f + k)]);
        std::nth_element (win.begin(), win.begin() + (long) win.size() / 2, win.end());
        smoothed[(size_t) f] = win[win.size() / 2];
    }
    p = smoothed;
    for (int f = 0; f < numFrames;)
    {
        if (std::isnan (p[(size_t) f])) { ++f; continue; }
        int e = f;
        while (e < numFrames && ! std::isnan (p[(size_t) e])) ++e;
        if (e - f < 4) for (int k = f; k < e; ++k) p[(size_t) k] = std::numeric_limits<float>::quiet_NaN();
        f = e;
    }

    // ---- group frames into notes ----
    const int minNoteFrames = 5;
    auto addNote = [&] (int f0, int f1)
    {
        if (f1 - f0 < minNoteFrames) return;
        std::vector<float> v;
        float loud = -100.0f;
        for (int k = f0; k < f1; ++k)
        {
            if (! std::isnan (p[(size_t) k])) v.push_back (p[(size_t) k]);
            loud = juce::jmax (loud, result->levelDb[(size_t) k]);
        }
        if (v.size() < (size_t) minNoteFrames) return;
        std::nth_element (v.begin(), v.begin() + (long) v.size() / 2, v.end());
        PitchNote n;
        n.start = std::max<juce::int64> (0, (juce::int64) f0 * result->hop - result->hop / 2);
        n.end = std::min<juce::int64> (len, (juce::int64) f1 * result->hop - result->hop / 2);
        n.detected = v[v.size() / 2];
        n.level = juce::jlimit (0.15f, 1.0f, juce::jmap (loud, maxLevel - 40.0f, maxLevel, 0.15f, 1.0f));
        result->notes.push_back (n);
    };

    for (int f = 0; f < numFrames;)
    {
        // voiced run, bridging gaps of up to 3 frames
        if (std::isnan (p[(size_t) f])) { ++f; continue; }
        int e = f, gap = 0, lastVoiced = f;
        while (e < numFrames)
        {
            if (std::isnan (p[(size_t) e])) { if (++gap > 3) break; }
            else { gap = 0; lastVoiced = e; }
            ++e;
        }
        e = lastVoiced + 1;

        // split the run where the pitch moves to a new note and stays there, or a new onset hits
        int noteStart = f;
        for (int k = f + 1; k < e; ++k)
        {
            if (std::isnan (p[(size_t) k])) continue;
            std::vector<float> ref;
            for (int j = juce::jmax (noteStart, k - 12); j < k; ++j)
                if (! std::isnan (p[(size_t) j])) ref.push_back (p[(size_t) j]);
            if (ref.size() < 3) continue;
            std::nth_element (ref.begin(), ref.begin() + (long) ref.size() / 2, ref.end());
            const float centre = ref[ref.size() / 2];

            bool moved = true;
            for (int j = k; j < juce::jmin (e, k + 5); ++j)
                if (std::isnan (p[(size_t) j]) || std::abs (p[(size_t) j] - centre) < 0.75f) { moved = false; break; }
            const bool onset = k >= 3 && result->levelDb[(size_t) k] - result->levelDb[(size_t) k - 3] > 9.0f
                               && k - noteStart > minNoteFrames;

            if ((moved && k - noteStart >= minNoteFrames) || onset)
            {
                addNote (noteStart, k);
                noteStart = k;
            }
        }
        addNote (noteStart, e);
        f = e;
    }
    return result;
}

float pitchdsp::targetPitch (const PitchAnalysis& an, const PitchNote& n, int frame)
{
    const float centre = n.detected + n.shift;
    if (! an.voiced (frame)) return centre;
    return centre + (an.pitch[(size_t) frame] - n.detected) * (1.0f - n.drift);
}

bool pitchdsp::inScale (int midiNote, int key, int scale)
{
    static const bool major[12] = { 1, 0, 1, 0, 1, 1, 0, 1, 0, 1, 0, 1 };
    static const bool minor[12] = { 1, 0, 1, 1, 0, 1, 0, 1, 1, 0, 1, 0 };
    const int degree = ((midiNote - key) % 12 + 12) % 12;
    if (scale == 2) return true;
    return scale == 1 ? minor[degree] : major[degree];
}

float pitchdsp::snapToScale (float midi, int key, int scale)
{
    const int base = (int) std::round (midi);
    for (int dist = 0; dist < 12; ++dist)
    {
        // prefer the closer of the two candidates
        const int a = base + dist, b = base - dist;
        const bool aIn = inScale (a, key, scale), bIn = inScale (b, key, scale);
        if (aIn && bIn) return (float) (std::abs ((float) a - midi) < std::abs ((float) b - midi) ? a : b);
        if (aIn) return (float) a;
        if (bIn) return (float) b;
    }
    return (float) base;
}

// =============================================================================
//  Rendering (TD-PSOLA)
// =============================================================================
std::shared_ptr<AudioData> pitchdsp::render (const AudioData& src, const PitchAnalysis& an, const std::vector<PitchNote>& notes)
{
    auto out = std::make_shared<AudioData>();
    out->sampleRate = src.sampleRate;
    out->file = src.file;
    out->buffer.makeCopyOf (src.buffer);
    out->peaks = src.peaks;

    const int len = src.buffer.getNumSamples();
    const int nf = an.numFrames();
    if (nf == 0) return out;

    // semitones to move each frame by
    std::vector<float> shift ((size_t) nf, 0.0f);
    for (auto& n : notes)
    {
        const int f0 = an.frameAt (n.start), f1 = an.frameAt (n.end);
        for (int f = f0; f <= f1 && f < nf; ++f)
            shift[(size_t) f] = an.voiced (f) ? pitchdsp::targetPitch (an, n, f) - an.pitch[(size_t) f] : n.shift;
    }
    // glide between notes over ~30 ms instead of jumping
    {
        std::vector<float> s (shift);
        for (int f = 0; f < nf; ++f)
        {
            float acc = 0; int cnt = 0;
            for (int k = -2; k <= 2; ++k)
                if (f + k >= 0 && f + k < nf) { acc += shift[(size_t) (f + k)]; ++cnt; }
            s[(size_t) f] = acc / (float) cnt;
        }
        shift = s;
    }

    auto periodAt = [&] (juce::int64 sampleIndex) -> double
    {
        int f = an.frameAt (sampleIndex);
        for (int k = 0; k < 8 && ! an.voiced (f); ++k) f = juce::jmax (0, f - 1);
        const float midi = an.voiced (f) ? an.pitch[(size_t) f] : 48.0f;
        return src.sampleRate / (440.0 * std::pow (2.0, (midi - 69.0) / 12.0));
    };

    const auto* inL = src.buffer.getReadPointer (0);
    const auto* inR = src.buffer.getReadPointer (1);
    auto mono = [&] (juce::int64 i) { return (i >= 0 && i < len) ? 0.5f * (inL[i] + inR[i]) : 0.0f; };

    // process each voiced stretch that has edits
    for (int f = 0; f < nf;)
    {
        if (! an.voiced (f)) { ++f; continue; }
        int e = f, gap = 0, lastVoiced = f;
        while (e < nf) { if (! an.voiced (e)) { if (++gap > 3) break; } else { gap = 0; lastVoiced = e; } ++e; }
        e = lastVoiced + 1;

        bool edited = false;
        for (int k = f; k < e && ! edited; ++k) edited = std::abs (shift[(size_t) k]) > 0.002f;
        const juce::int64 a = std::max<juce::int64> (0, (juce::int64) f * an.hop - an.hop);
        const juce::int64 b = std::min<juce::int64> (len, (juce::int64) e * an.hop + an.hop);
        f = e;
        if (! edited || b - a < 64) continue;

        // analysis pitch marks, one per period, on waveform peaks
        std::vector<juce::int64> marks;
        {
            const double p0 = periodAt (a);
            juce::int64 m = a;
            for (juce::int64 i = a; i < std::min<juce::int64> (b, a + (juce::int64) p0); ++i)
                if (mono (i) > mono (m)) m = i;
            marks.push_back (m);
            while (marks.back() < b + (juce::int64) periodAt (b))
            {
                const double P = periodAt (marks.back());
                const auto guess = marks.back() + (juce::int64) std::round (P);
                const auto radius = (juce::int64) juce::jmax (1.0, P * 0.2);
                juce::int64 best = guess;
                for (auto i = guess - radius; i <= guess + radius; ++i)
                    if (mono (i) > mono (best)) best = i;
                if (best <= marks.back()) best = marks.back() + 1;
                marks.push_back (best);
                if (marks.size() > 2000000) break;
            }
        }
        if (marks.size() < 3) continue;

        // overlap-add two-period Hann grains at the new spacing
        const juce::int64 margin = (juce::int64) (src.sampleRate * 0.05);
        const juce::int64 base = a - margin;
        const auto bufLen = (size_t) (b - a + 2 * margin);
        std::vector<float> accL (bufLen, 0.0f), accR (bufLen, 0.0f), wsum (bufLen, 0.0f);

        double t = (double) marks.front();
        size_t k = 0;
        while (t < (double) b)
        {
            while (k + 1 < marks.size() && std::abs ((double) marks[k + 1] - t) < std::abs ((double) marks[k] - t)) ++k;
            const auto next = k + 1 < marks.size() ? marks[k + 1] : marks[k] + (marks[k] - marks[k - 1]);
            const int Pa = (int) std::max<juce::int64> (8, next - marks[k]);
            const float ratio = std::pow (2.0f, shift[(size_t) an.frameAt ((juce::int64) t)] / 12.0f);

            const auto centre = (juce::int64) std::round (t);
            for (int j = -Pa; j < Pa; ++j)
            {
                const auto s = marks[k] + j, dst = centre + j - base;
                if (s < 0 || s >= len || dst < 0 || dst >= (juce::int64) bufLen) continue;
                const float w = 0.5f * (1.0f + std::cos (juce::MathConstants<float>::pi * (float) j / (float) Pa));
                accL[(size_t) dst] += inL[s] * w;
                accR[(size_t) dst] += inR[s] * w;
                wsum[(size_t) dst] += w;
            }
            t += (double) Pa / ratio;
        }

        // write back, crossfading into the untouched audio at both ends
        auto* oL = out->buffer.getWritePointer (0);
        auto* oR = out->buffer.getWritePointer (1);
        const auto fade = std::max<juce::int64> (16, std::min<juce::int64> (256, (b - a) / 4));
        for (auto i = a; i < b; ++i)
        {
            const auto bi = (size_t) (i - base);
            if (wsum[bi] < 0.2f) continue;
            const float yl = accL[bi] / wsum[bi], yr = accR[bi] / wsum[bi];
            float g = 1.0f;
            if (i - a < fade) g = (float) (i - a) / (float) fade;
            else if (b - i < fade) g = (float) (b - i) / (float) fade;
            oL[i] = inL[i] + g * (yl - inL[i]);
            oR[i] = inR[i] + g * (yr - inR[i]);
        }
    }
    return out;
}

// =============================================================================
//  Background service
// =============================================================================
PitchService::PitchService (Project& p) : project (p)
{
    project.addChangeListener (this);
}

PitchService::~PitchService()
{
    project.removeChangeListener (this);
    stopping = true;
    pool.removeAllJobs (true, 10000);
    cancelPendingUpdate();
}

PitchAnalysisPtr PitchService::getAnalysis (const ClipPtr& clip)
{
    if (clip == nullptr || clip->audio == nullptr) return nullptr;
    const auto key = clip->audio->file.getFullPathName();
    {
        const juce::ScopedLock sl (mapLock);
        if (auto it = analyses.find (key); it != analyses.end()) return it->second;
        if (analysing.count (key)) return nullptr;
        analysing.insert (key);
    }

    auto data = clip->audio;
    pool.addJob ([this, data, key]
    {
        auto result = pitchdsp::analyse (*data, &stopping);
        if (stopping) return;
        {
            const juce::ScopedLock sl (mapLock);
            analyses[key] = result;
            analysing.erase (key);
        }
        triggerAsyncUpdate();
    });
    sendChangeMessage();
    return nullptr;
}

bool PitchService::isBusy() const
{
    const juce::ScopedLock sl (mapLock);
    return ! analysing.empty() || ! rendering.empty();
}

bool PitchService::isAnalysing (const ClipPtr& clip) const
{
    if (clip == nullptr || clip->audio == nullptr) return false;
    const juce::ScopedLock sl (mapLock);
    return analysing.count (clip->audio->file.getFullPathName()) > 0;
}

bool PitchService::isRendering (int clipId) const
{
    const juce::ScopedLock sl (mapLock);
    return rendering.count (clipId) > 0;
}

void PitchService::timerCallback()
{
    stopTimer();
    for (auto& t : project.tracks)
        for (auto& c : t->clips)
        {
            if (c->type != Clip::Type::Audio || ! c->pitch.active || ! c->pitch.hasChanges()) continue;
            const auto h = c->pitch.hash();
            if (c->pitchRendered != nullptr && c->pitchRenderedHash == h) continue;
            {
                const juce::ScopedLock sl (mapLock);
                if (auto it = rendering.find (c->id); it != rendering.end() && it->second == h) continue;
            }
            if (auto an = getAnalysis (c)) startRender (c, an);
        }
}

void PitchService::startRender (const ClipPtr& clip, PitchAnalysisPtr an)
{
    const auto h = clip->pitch.hash();
    const int id = clip->id;
    {
        const juce::ScopedLock sl (mapLock);
        rendering[id] = h;
    }
    auto data = clip->audio;
    auto notes = clip->pitch.notes;

    pool.addJob ([this, data, an, notes, h, id]
    {
        if (stopping) return;
        auto rendered = pitchdsp::render (*data, *an, notes);
        if (stopping) return;
        {
            const juce::ScopedLock sl (mapLock);
            finished.push_back ({ id, h, rendered });
        }
        triggerAsyncUpdate();
    });
    sendChangeMessage();
}

void PitchService::handleAsyncUpdate()
{
    std::vector<Finished> done;
    {
        const juce::ScopedLock sl (mapLock);
        done.swap (finished);
        for (auto& d : done)
            if (auto it = rendering.find (d.clipId); it != rendering.end() && it->second == d.hash)
                rendering.erase (it);
    }
    for (auto& d : done)
    {
        TrackPtr owner;
        if (auto c = project.findClip (d.clipId, &owner); c != nullptr && c->pitch.hash() == d.hash)
        {
            const juce::ScopedLock sl (project.lock);
            c->pitchRendered = d.audio;
            c->pitchRenderedHash = d.hash;
        }
    }
    sendChangeMessage();
    startTimer (50);   // anything else waiting (e.g. an analysis just finished)?
}

} // namespace rs
