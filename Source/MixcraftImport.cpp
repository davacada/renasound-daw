#include "MixcraftImport.h"

namespace rs
{

namespace
{
    // Mixcraft positions are in ticks of 1/500 beat; lengths / offsets in milliseconds.
    constexpr double ticksPerBeat = 500.0;

    // Bounds-checked reads: the file comes from outside, so never trust it.
    struct Reader
    {
        const juce::uint8* data;
        size_t size;

        bool has (size_t off, size_t n) const { return off <= size && n <= size - off; }
        double f64 (size_t off) const
        {
            if (! has (off, 8)) return 0.0;
            double v; std::memcpy (&v, data + off, 8);
            return std::isfinite (v) ? v : 0.0;
        }
        juce::int32 i32 (size_t off) const
        {
            if (! has (off, 4)) return 0;
            juce::int32 v; std::memcpy (&v, data + off, 4);
            return v;
        }
        juce::uint32 u32 (size_t off) const { return (juce::uint32) i32 (off); }
        juce::String str (size_t off, size_t maxLen) const
        {
            if (! has (off, 1)) return {};
            maxLen = juce::jmin (maxLen, size - off);
            size_t n = 0;
            while (n < maxLen && data[off + n] != 0) ++n;
            return juce::String::createStringFromData (data + off, (int) n).trim();
        }
        juce::String id (size_t off) const { return has (off, 4) ? juce::String ((const char*) data + off, 4) : juce::String(); }
    };

    struct Chunk { juce::String id; size_t body = 0, size = 0; };

    // Flattens the RIFF tree into the order chunks appear in (that's the order Mixcraft relies on).
    void collectChunks (const Reader& r, size_t off, size_t end, std::vector<Chunk>& out, int depth)
    {
        while (off + 8 <= end && depth < 12)
        {
            const auto cid = r.id (off);
            const size_t size = r.u32 (off + 4);
            if (size > end - off - 8) break;   // corrupt / truncated
            if (cid == "RIFF" || cid == "LIST")
                collectChunks (r, off + 12, off + 8 + size, out, depth + 1);
            else
                out.push_back ({ cid, off + 8, size });
            off += 8 + size + (size & 1);
        }
    }

    struct AudioClipIn  { double startBeat, lengthSec, offsetSec, fileLenSec; juce::String path, relPath, name; };
    struct NoteIn       { int pitch; double start, length; float velocity; };
    struct MidiClipIn   { double startBeat, lengthBeats, loopBeats; std::vector<NoteIn> notes; };
    struct TrackIn
    {
        bool midi = false;
        juce::String name, instrument;
        double volume = 1.0, pan = 0.0;
        bool mute = false;
        std::vector<AudioClipIn> audio;
        std::vector<MidiClipIn> midiClips;
    };

    int presetFor (const juce::String& instrument)
    {
        const auto s = instrument.toLowerCase();
        auto has = [&s] (std::initializer_list<const char*> words) { for (auto* w : words) if (s.contains (w)) return true; return false; };
        if (has ({ "sub" }))                                   return 6;   // Sub Bass
        if (has ({ "bass" }))                                  return 3;   // Bass
        if (has ({ "piano", "rhodes", "keys", "organ", "ep" })) return 4;  // E-Piano
        if (has ({ "pad", "string", "choir", "warm" }))        return 1;   // Soft Pad
        if (has ({ "pluck", "guzheng", "harp", "guitar", "koto" })) return 2;   // Pluck
        if (has ({ "square", "chip", "8bit" }))                return 5;   // Square Keys
        return 0;                                                          // Saw Lead
    }

    std::vector<NoteIn> notesFromEvents (const Reader& r, const Chunk& c)
    {
        // 16-byte records: double time (ticks, from clip start), then a MIDI message
        struct Ev { double beat; int status, d1, d2; };
        std::vector<Ev> evs;
        for (size_t i = 0; i + 16 <= c.size; i += 16)
        {
            const size_t o = c.body + i;
            evs.push_back ({ r.f64 (o) / ticksPerBeat, r.data[o + 8], r.data[o + 9], r.data[o + 10] });
        }
        std::vector<NoteIn> notes;
        std::map<int, std::pair<double, int>> held;   // pitch -> start, velocity
        for (auto& e : evs)
        {
            const int type = e.status & 0xF0;
            const int pitch = e.d1 & 0x7F;
            if (type == 0x90 && e.d2 > 0)
            {
                if (auto it = held.find (pitch); it != held.end())   // retrigger: close the old one
                    notes.push_back ({ pitch, it->second.first, juce::jmax (0.01, e.beat - it->second.first), it->second.second / 127.0f });
                held[pitch] = { e.beat, e.d2 };
            }
            else if (type == 0x80 || (type == 0x90 && e.d2 == 0))
            {
                if (auto it = held.find (pitch); it != held.end())
                {
                    notes.push_back ({ pitch, it->second.first, juce::jmax (0.01, e.beat - it->second.first), it->second.second / 127.0f });
                    held.erase (it);
                }
            }
        }
        for (auto& [pitch, h] : held)
            notes.push_back ({ pitch, h.first, 0.25, h.second / 127.0f });
        return notes;
    }

    // Finds audio files by name across folders, built once per import.
    struct FileFinder
    {
        std::map<juce::String, juce::File> byName;   // lower-case file name -> file

        void scan (const juce::File& folder, int maxFiles = 60000)
        {
            if (! folder.isDirectory()) return;
            int n = 0;
            for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
            {
                const auto f = entry.getFile();
                byName.emplace (f.getFileName().toLowerCase(), f);
                if (++n > maxFiles) break;
            }
        }

        juce::File find (const juce::String& winPath, const juce::String& relPath, const juce::File& projectDir) const
        {
            // 1) the original absolute path (works when opened on the same Windows PC)
            if (juce::File::isAbsolutePath (winPath) && juce::File (winPath).existsAsFile()) return juce::File (winPath);
            // 2) the path relative to the project, as Mixcraft stores it
            if (relPath.isNotEmpty())
            {
                auto rel = relPath.replaceCharacter ('\\', '/');
                while (rel.startsWithChar ('/')) rel = rel.substring (1);
                auto f = projectDir.getChildFile (rel);
                if (f.existsAsFile()) return f;
            }
            // 3) the same file name anywhere in the folders we scanned
            const auto name = winPath.fromLastOccurrenceOf ("\\", false, false).fromLastOccurrenceOf ("/", false, false).toLowerCase();
            if (auto it = byName.find (name); it != byName.end()) return it->second;
            return {};
        }
    };
}

bool isMixcraftProject (const juce::File& f)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) return false;
    char header[12] {};
    in.read (header, 12);
    return std::memcmp (header, "RIFF", 4) == 0 && std::memcmp (header + 8, "MXC", 3) == 0;
}

MixcraftImport importMixcraftProject (const juce::File& projectFile, const juce::Array<juce::File>& extraSearchFolders)
{
    MixcraftImport result;

    juce::MemoryBlock mb;
    if (! projectFile.loadFileAsData (mb) || mb.getSize() < 16)
    {
        result.error = "Couldn't read the file.";
        return result;
    }
    const Reader r { (const juce::uint8*) mb.getData(), mb.getSize() };
    if (r.id (0) != "RIFF" || ! r.id (8).startsWith ("MXC"))
    {
        result.error = "This isn't a Mixcraft project (or it's from a version RENASOUND doesn't know yet).";
        return result;
    }

    std::vector<Chunk> chunks;
    collectChunks (r, 0, mb.getSize(), chunks, 0);

    // ---- walk the chunks in order: a track header is followed by its clips ----
    std::vector<TrackIn> tracks;
    double bpm = 0;
    MidiClipIn* openMidiClip = nullptr;

    for (auto& c : chunks)
    {
        if ((c.id == "TrkA" || c.id == "TrkM") && c.size >= 560)
        {
            TrackIn t;
            t.midi = c.id == "TrkM";
            if (t.midi)
            {
                t.instrument = r.str (c.body + 12, 250);
                t.name = r.str (c.body + 828, 250);
                if (t.name.isEmpty()) t.name = t.instrument;
            }
            else
                t.name = r.str (c.body + 8, 250);
            t.volume = r.f64 (c.body + 528);
            t.pan = r.f64 (c.body + 536);
            t.mute = r.i32 (c.body + 544) == 1;
            if (! (t.volume >= 0.0 && t.volume <= 4.0)) t.volume = 1.0;
            if (! (t.pan >= -1.0 && t.pan <= 1.0)) t.pan = 0.0;
            tracks.push_back (t);
            openMidiClip = nullptr;
        }
        else if (c.id == "ClpA" && ! tracks.empty() && c.size >= 880)
        {
            AudioClipIn a;
            a.path = r.str (c.body + 8, 256);
            a.relPath = r.str (c.body + 268, 256);
            a.name = r.str (c.body + 600, 256);
            a.startBeat = r.f64 (c.body + 536) / ticksPerBeat;
            a.lengthSec = r.f64 (c.body + 544) / 1000.0;
            a.offsetSec = r.f64 (c.body + 552) / 1000.0;
            a.fileLenSec = r.f64 (c.body + 568) / 1000.0;
            if (a.path.isNotEmpty() && a.startBeat >= 0 && a.lengthSec > 0 && a.offsetSec >= 0 && a.lengthSec < 36000)
                tracks.back().audio.push_back (a);
        }
        else if (c.id == "ClhM" && ! tracks.empty() && c.size >= 32)
        {
            MidiClipIn m;
            m.startBeat = r.f64 (c.body + 8) / ticksPerBeat;
            m.lengthBeats = r.f64 (c.body + 16) / ticksPerBeat;
            m.loopBeats = r.f64 (c.body + 24) / ticksPerBeat;
            if (m.startBeat >= 0 && m.lengthBeats > 0 && m.lengthBeats < 100000)
            {
                tracks.back().midiClips.push_back (m);
                openMidiClip = &tracks.back().midiClips.back();
            }
            else openMidiClip = nullptr;
        }
        else if (c.id == "CleM" && openMidiClip != nullptr)
        {
            openMidiClip->notes = notesFromEvents (r, c);
            openMidiClip = nullptr;
        }
        else if (c.id == "TEnv" && c.size >= 96)
        {
            // tempo is stored normalised: 0..1 maps to 40..400 BPM; the last envelope is the tempo track
            const double v = r.f64 (c.body + 88);
            const double candidate = 40.0 + 360.0 * v;
            if (candidate >= 20.0 && candidate <= 400.0) bpm = candidate;
        }
    }

    if (bpm <= 0) bpm = 120.0;
    bpm = std::round (bpm * 1000.0) / 1000.0;
    result.bpm = bpm;

    // ---- find the audio files ----
    const auto projectDir = projectFile.getParentDirectory();
    FileFinder finder;
    finder.scan (projectDir);
    for (auto& f : extraSearchFolders) finder.scan (f);

    // ---- build the RENASOUND project ----
    auto root = std::make_unique<juce::XmlElement> ("RENASOUND_PROJECT");
    root->setAttribute ("version", 1);
    root->setAttribute ("bpm", bpm);
    root->setAttribute ("beatsPerBar", 4);
    root->setAttribute ("masterVolume", 0.9);
    root->setAttribute ("importedFrom", "Mixcraft");

    int nextId = 1;
    std::set<juce::String> missing;
    static const juce::uint32 colours[] = { 0xffff6b4a, 0xff3ec7a5, 0xff6c8cff, 0xffffc247, 0xffd16bff, 0xff4ac1ff, 0xffff6fa8, 0xff9bd65a };

    for (auto& t : tracks)
    {
        if (t.audio.empty() && t.midiClips.empty()) continue;   // Mixcraft keeps lots of empty tracks around

        auto* te = root->createNewChildElement ("TRACK");
        te->setAttribute ("id", nextId++);
        te->setAttribute ("type", t.midi ? "instrument" : "audio");
        auto name = t.name;
        if (name.isEmpty() || name == "Audio Track")
            name = ! t.audio.empty() ? t.audio.front().name : (t.instrument.isNotEmpty() ? t.instrument : juce::String ("Track"));
        te->setAttribute ("name", name.isNotEmpty() ? name : juce::String ("Track"));
        te->setAttribute ("colour", juce::Colour (colours[(size_t) result.numTracks % std::size (colours)]).toString());
        te->setAttribute ("volume", juce::jlimit (0.0, 1.5, t.volume));
        te->setAttribute ("pan", t.pan);
        te->setAttribute ("mute", t.mute);
        if (t.midi) te->setAttribute ("preset", presetFor (t.instrument));
        if (t.midi
            && ((t.instrument + " " + t.name).containsIgnoreCase ("drum") || (t.instrument + " " + t.name).containsIgnoreCase ("kit")
                || (t.instrument + " " + t.name).containsIgnoreCase ("perc") || (t.instrument + " " + t.name).containsIgnoreCase ("beat")))
        {
            // drum kits get RENASOUND Drums (General MIDI notes land on the right pads)
            auto* ie = te->createNewChildElement ("INSTRUMENT");
            ie->setAttribute ("type", "builtin:drums");
            ie->setAttribute ("bypass", false);
            ie->setAttribute ("state", "");
        }
        ++result.numTracks;

        if (! t.midi)
        {
            // crossfade clips that overlap, as Mixcraft does
            auto clips = t.audio;
            std::sort (clips.begin(), clips.end(), [] (auto& a, auto& b) { return a.startBeat < b.startBeat; });
            std::vector<double> fadeIn (clips.size(), 0.0), fadeOut (clips.size(), 0.0);
            for (size_t i = 0; i + 1 < clips.size(); ++i)
            {
                const double endSec = clips[i].startBeat * 60.0 / bpm + clips[i].lengthSec;
                const double nextStartSec = clips[i + 1].startBeat * 60.0 / bpm;
                const double overlap = endSec - nextStartSec;
                if (overlap > 0.005 && overlap < juce::jmin (clips[i].lengthSec, clips[i + 1].lengthSec) * 0.9)
                {
                    fadeOut[i] = overlap;
                    fadeIn[i + 1] = overlap;
                }
            }

            for (size_t i = 0; i < clips.size(); ++i)
            {
                auto& a = clips[i];
                const auto file = finder.find (a.path, a.relPath, projectDir);
                if (! file.existsAsFile())
                {
                    missing.insert (a.path.fromLastOccurrenceOf ("\\", false, false));
                    continue;
                }
                auto* ce = te->createNewChildElement ("CLIP");
                ce->setAttribute ("id", nextId++);
                ce->setAttribute ("name", a.name.isNotEmpty() ? a.name : file.getFileNameWithoutExtension());
                ce->setAttribute ("start", a.startBeat);
                ce->setAttribute ("file", file.getFullPathName());
                ce->setAttribute ("offset", a.offsetSec);
                ce->setAttribute ("length", a.lengthSec);
                if (fadeIn[i] > 0)  ce->setAttribute ("fadeIn", fadeIn[i]);
                if (fadeOut[i] > 0) ce->setAttribute ("fadeOut", fadeOut[i]);
                ++result.numAudioClips;
            }
        }
        else
        {
            for (auto& m : t.midiClips)
            {
                auto* ce = te->createNewChildElement ("CLIP");
                ce->setAttribute ("id", nextId++);
                ce->setAttribute ("name", name);
                ce->setAttribute ("start", m.startBeat);
                ce->setAttribute ("lengthBeats", m.lengthBeats);

                // a clip longer than its loop repeats the loop
                const double loop = (m.loopBeats > 0.01 && m.loopBeats < m.lengthBeats - 0.01) ? m.loopBeats : 1.0e9;
                for (double base = 0; base < m.lengthBeats; base += loop)
                {
                    for (auto& n : m.notes)
                    {
                        const double s = base + n.start;
                        if (n.start >= juce::jmin (loop, m.lengthBeats) || s >= m.lengthBeats) continue;
                        auto* ne = ce->createNewChildElement ("NOTE");
                        ne->setAttribute ("p", n.pitch);
                        ne->setAttribute ("s", s);
                        ne->setAttribute ("l", juce::jmin (n.length, m.lengthBeats - s));
                        ne->setAttribute ("v", n.velocity);
                        ++result.numNotes;
                    }
                    if (loop >= 1.0e8) break;
                }
                ++result.numMidiClips;
            }
        }
    }

    for (auto& m : missing) result.missingFiles.add (m);
    result.project = std::move (root);
    result.ok = true;
    result.summary = juce::String (result.numTracks) + " tracks, " + juce::String (result.numAudioClips) + " audio clips, "
                   + juce::String (result.numMidiClips) + " MIDI clips at " + (std::abs (bpm - std::round (bpm)) < 0.001 ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 2)) + " BPM";
    return result;
}

} // namespace rs
