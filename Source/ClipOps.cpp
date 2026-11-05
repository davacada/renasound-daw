#include "ClipOps.h"
#include "AudioEngine.h"

namespace rs::clipops
{

namespace
{
    juce::File writeWav (const juce::AudioBuffer<float>& b, double sampleRate, const juce::String& name)
    {
        auto f = AudioEngine::getRecordingsFolder().getNonexistentChildFile (juce::File::createLegalFileName (name), ".wav");
        f.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (f.createOutputStream());
        if (stream == nullptr) return {};
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}.withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (24));
        if (writer == nullptr) return {};
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        writer.reset();
        return f;
    }

    juce::String withoutSuffix (juce::String n, const juce::String& suffix)
    {
        return n.endsWith (suffix) ? n.dropLastCharacters (suffix.length()) : n + suffix;
    }
}

int reverse (Project& p, const std::vector<int>& ids)
{
    int done = 0;
    for (int id : ids)
    {
        TrackPtr owner;
        auto c = p.findClip (id, &owner);
        if (c == nullptr || c->locked) continue;

        if (c->type == Clip::Type::Midi)
        {
            const juce::ScopedLock sl (p.lock);
            const double len = c->lengthBeats;
            // a note [s, e) becomes [len - e, len - s)
            std::vector<MidiNote> out;
            for (auto n : c->notes)
            {
                if (n.start < 0 || n.start >= len) continue;
                const double s = n.start, e = juce::jmin (len, n.start + n.length);
                n.start = len - e;
                n.length = e - s;
                out.push_back (n);
            }
            std::sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.start < b.start; });
            c->notes = out;
            c->name = withoutSuffix (c->name, " (reversed)");
            ++done;
            continue;
        }

        if (c->audio == nullptr) continue;
        const auto& src = c->audio->buffer;
        const double sr = c->audio->sampleRate;
        const int s0 = juce::jlimit (0, src.getNumSamples(), (int) std::lround (c->offsetSeconds * sr));
        const int n = juce::jlimit (0, src.getNumSamples() - s0, (int) std::lround (c->lengthSeconds * sr));
        if (n <= 0) continue;
        juce::AudioBuffer<float> rev (2, n);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* in = src.getReadPointer (ch, s0);
            auto* out = rev.getWritePointer (ch);
            for (int i = 0; i < n; ++i) out[i] = in[n - 1 - i];
        }
        const auto newName = withoutSuffix (c->name, " (reversed)");
        const auto file = writeWav (rev, sr, newName);
        auto data = file.existsAsFile() ? p.audioCache.load (file) : nullptr;
        if (data == nullptr) continue;

        const juce::ScopedLock sl (p.lock);
        c->audio = data;
        c->offsetSeconds = 0;
        c->lengthSeconds = n / sr;
        std::swap (c->fadeInSeconds, c->fadeOutSeconds);
        c->pitch = {};
        c->pitchRendered = nullptr;
        c->name = newName;
        ++done;
    }
    if (done > 0) p.changed();
    return done;
}

std::vector<int> join (Project& p, const std::vector<int>& ids)
{
    // group the clips by track
    std::map<int, std::vector<ClipPtr>> byTrack;
    for (int id : ids)
    {
        TrackPtr owner;
        if (auto c = p.findClip (id, &owner); c != nullptr && owner != nullptr && ! c->locked)
            byTrack[owner->id].push_back (c);
    }

    std::vector<int> made;
    const double spb = 60.0 / p.bpm;
    for (auto& [trackId, clips] : byTrack)
    {
        if (clips.size() < 2) continue;
        std::sort (clips.begin(), clips.end(), [] (auto& a, auto& b) { return a->startBeat < b->startBeat; });
        double start = 1.0e9, end = 0;
        for (auto& c : clips) { start = juce::jmin (start, c->startBeat); end = juce::jmax (end, c->getEndBeat (p.bpm)); }

        auto joined = std::make_shared<Clip>();
        joined->type = clips.front()->type;
        joined->name = clips.front()->name;
        joined->startBeat = start;

        if (joined->type == Clip::Type::Midi)
        {
            joined->lengthBeats = end - start;
            for (auto& c : clips)
                for (auto n : c->notes)
                {
                    if (n.start < 0 || n.start >= c->lengthBeats) continue;   // trimmed away
                    n.length = juce::jmin (n.length, c->lengthBeats - n.start);
                    n.start += c->startBeat - start;
                    joined->notes.push_back (n);
                }
            std::sort (joined->notes.begin(), joined->notes.end(), [] (auto& a, auto& b) { return a.start < b.start; });
        }
        else
        {
            // mix every clip (with its fades and pitch edits) into one file
            const double sr = clips.front()->audio != nullptr ? clips.front()->audio->sampleRate : 48000.0;
            const int total = (int) std::ceil ((end - start) * spb * sr) + 1;
            juce::AudioBuffer<float> mix (2, total);
            mix.clear();
            for (auto& c : clips)
            {
                const AudioData* data = c->getPlaybackAudio();
                if (data == nullptr) continue;
                const auto& src = data->buffer;
                const int at = (int) std::lround ((c->startBeat - start) * spb * sr);
                const int n = (int) std::lround (c->lengthSeconds * sr);
                for (int i = 0; i < n && at + i < total; ++i)
                {
                    const double t = i / sr;
                    const double sp = (c->offsetSeconds + t) * data->sampleRate;
                    const int k = (int) sp;
                    if (k < 0 || k + 1 >= src.getNumSamples()) continue;
                    const float frac = (float) (sp - k);
                    float g = 1.0f;
                    if (c->fadeInSeconds > 0 && t < c->fadeInSeconds) g *= (float) std::sin (juce::MathConstants<double>::halfPi * t / c->fadeInSeconds);
                    if (c->fadeOutSeconds > 0 && c->lengthSeconds - t < c->fadeOutSeconds)
                        g *= (float) std::sin (juce::MathConstants<double>::halfPi * juce::jmax (0.0, c->lengthSeconds - t) / c->fadeOutSeconds);
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto* s = src.getReadPointer (ch);
                        mix.addSample (ch, at + i, g * (s[k] + frac * (s[k + 1] - s[k])));
                    }
                }
            }
            const auto file = writeWav (mix, sr, joined->name + " (joined)");
            joined->audio = file.existsAsFile() ? p.audioCache.load (file) : nullptr;
            if (joined->audio == nullptr) continue;
            joined->name = joined->name + " (joined)";
            joined->offsetSeconds = 0;
            joined->lengthSeconds = (end - start) * spb;
        }

        {
            const juce::ScopedLock sl (p.lock);
            auto t = p.findTrack (trackId);
            if (t == nullptr) continue;
            for (auto& c : clips)
                t->clips.erase (std::remove (t->clips.begin(), t->clips.end(), c), t->clips.end());
            joined->id = p.newId();
            t->clips.push_back (joined);
        }
        made.push_back (joined->id);
    }
    if (! made.empty()) p.changed();
    return made;
}

Clipboard copy (const Project& p, const std::vector<int>& ids)
{
    Clipboard cb;
    double first = 1.0e9, last = 0;
    for (int id : ids)
    {
        TrackPtr owner;
        if (auto c = p.findClip (id, &owner); c != nullptr && owner != nullptr)
        {
            int index = 0;
            for (int i = 0; i < (int) p.tracks.size(); ++i) if (p.tracks[(size_t) i] == owner) index = i;
            cb.items.push_back ({ owner->id, index, *c });
            first = juce::jmin (first, c->startBeat);
            last = juce::jmax (last, c->getEndBeat (p.bpm));
        }
    }
    for (auto& it : cb.items) it.clip.startBeat -= first;   // relative to the earliest clip
    cb.span = cb.items.empty() ? 0 : last - first;
    return cb;
}

std::vector<int> paste (Project& p, const Clipboard& cb, double atBeat, int selectedTrackId)
{
    std::vector<int> made;
    if (cb.empty()) return made;

    bool oneTrack = true;
    for (auto& it : cb.items) oneTrack = oneTrack && it.trackId == cb.items.front().trackId;
    auto selected = p.findTrack (selectedTrackId);

    for (auto& it : cb.items)
    {
        int target = it.trackId;
        if (oneTrack && selected != nullptr && (selected->type == Track::Type::Audio) == (it.clip.type == Clip::Type::Audio))
            target = selected->id;
        if (p.findTrack (target) == nullptr && it.trackIndex < (int) p.tracks.size())
            target = p.tracks[(size_t) it.trackIndex]->id;   // the track was deleted: use the one in its place
        if (auto c = p.addClipCopy (target, it.clip, atBeat + it.clip.startBeat))
            made.push_back (c->id);
    }
    return made;
}

} // namespace rs::clipops
