#include "AbletonImport.h"
#include "SoundMatch.h"
#include "Model.h"

namespace rs
{

namespace
{
    const juce::XmlElement* child (const juce::XmlElement* e, const juce::String& path)
    {
        for (auto& part : juce::StringArray::fromTokens (path, "/", {}))
        {
            if (e == nullptr) return nullptr;
            e = e->getChildByName (part);
        }
        return e;
    }

    juce::String value (const juce::XmlElement* e, const juce::String& path, const juce::String& fallback = {}, const char* attribute = "Value")
    {
        auto* c = child (e, path);
        return c != nullptr ? c->getStringAttribute (attribute, fallback) : fallback;
    }

    double number (const juce::XmlElement* e, const juce::String& path, double fallback = 0.0)
    {
        auto* c = child (e, path);
        if (c == nullptr || ! c->hasAttribute ("Value")) return fallback;
        const double v = c->getDoubleAttribute ("Value", fallback);
        return std::isfinite (v) ? v : fallback;
    }

    bool flag (const juce::XmlElement* e, const juce::String& path, bool fallback = false)
    {
        auto* c = child (e, path);
        if (c == nullptr) return fallback;
        return c->getStringAttribute ("Value").equalsIgnoreCase ("true");
    }

    struct FileFinder
    {
        std::map<juce::String, juce::File> byName;
        void scan (const juce::File& folder)
        {
            if (! folder.isDirectory()) return;
            int n = 0;
            for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
            {
                byName.emplace (entry.getFile().getFileName().toLowerCase(), entry.getFile());
                if (++n > 60000) break;
            }
        }
    };

    // Where a clip's audio file is, trying every way Live versions store it.
    juce::File resolveSample (const juce::XmlElement* fileRef, const juce::File& projectDir, const FileFinder& finder, juce::String& nameOut)
    {
        if (fileRef == nullptr) return {};
        auto tryFile = [] (const juce::File& f) { return f.existsAsFile() ? f : juce::File(); };

        // Live 11+: absolute Path + RelativePath strings
        const auto path = value (fileRef, "Path");
        const auto relString = child (fileRef, "RelativePath") != nullptr ? child (fileRef, "RelativePath")->getStringAttribute ("Value") : juce::String();
        // Live 9 / 10: RelativePath made of directory elements + Name
        juce::String relDirs;
        if (auto* rp = child (fileRef, "RelativePath"))
            for (auto* el : rp->getChildWithTagNameIterator ("RelativePathElement"))
                relDirs << (el->getStringAttribute ("Dir").isEmpty() ? juce::String ("..") : el->getStringAttribute ("Dir")) << "/";
        juce::String absDirs;
        if (auto* ph = child (fileRef, "SearchHint/PathHint"))
            for (auto* el : ph->getChildWithTagNameIterator ("RelativePathElement"))
                absDirs << "/" << el->getStringAttribute ("Dir");
        auto name = value (fileRef, "Name");
        if (name.isEmpty()) name = path.isNotEmpty() ? path.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false)
                                                     : relString.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);
        nameOut = name;

        if (path.isNotEmpty() && juce::File::isAbsolutePath (path))
            if (auto f = tryFile (juce::File (path)); f != juce::File()) return f;
        if (relString.isNotEmpty())
            if (auto f = tryFile (projectDir.getChildFile (relString.replaceCharacter ('\\', '/'))); f != juce::File()) return f;
        if (relDirs.isNotEmpty() && name.isNotEmpty())
            if (auto f = tryFile (projectDir.getChildFile (relDirs + name)); f != juce::File()) return f;
        if (absDirs.isNotEmpty() && name.isNotEmpty() && juce::File::isAbsolutePath (absDirs))
            if (auto f = tryFile (juce::File (absDirs).getChildFile (name)); f != juce::File()) return f;
        if (name.isNotEmpty())
            if (auto it = finder.byName.find (name.toLowerCase()); it != finder.byName.end()) return it->second;
        return {};
    }

    struct Warp
    {
        std::vector<std::pair<double, double>> marks;   // beat -> seconds
        double fallbackSecPerBeat = 0.5;

        double secAt (double beat) const
        {
            if (marks.size() < 2)
                return (marks.empty() ? 0.0 : marks[0].second + (beat - marks[0].first) * fallbackSecPerBeat);
            size_t i = 1;
            while (i + 1 < marks.size() && marks[i].first < beat) ++i;
            const auto& a = marks[i - 1];
            const auto& b = marks[i];
            const double slope = (b.second - a.second) / juce::jmax (1.0e-9, b.first - a.first);
            return a.second + (beat - a.first) * slope;
        }
    };

    // One stretch of clip content placed on the timeline.
    struct Segment { double timelineOffset, contentStart, contentEnd; };

    // Unrolls a clip's loop into straight segments.
    std::vector<Segment> segmentsFor (const juce::XmlElement* clip, double lengthBeats, bool contentInSeconds, double secPerBeat)
    {
        const double loopStart = number (clip, "Loop/LoopStart");
        const double loopEnd = number (clip, "Loop/LoopEnd", loopStart + 4);
        const double startRel = number (clip, "Loop/StartRelative");
        const bool loopOn = flag (clip, "Loop/LoopOn");
        // content units per timeline beat
        const double rate = contentInSeconds ? secPerBeat : 1.0;

        std::vector<Segment> out;
        double pos = 0;
        double c = loopStart + startRel;
        if (! loopOn || loopEnd - loopStart < 1.0e-3)
        {
            out.push_back ({ 0.0, c, c + lengthBeats * rate });
            return out;
        }
        int guard = 0;
        while (pos < lengthBeats - 1.0e-6 && guard++ < 4096)
        {
            if (c >= loopEnd - 1.0e-9) c = loopStart;
            const double segBeats = juce::jmin ((loopEnd - c) / rate, lengthBeats - pos);
            out.push_back ({ pos, c, c + segBeats * rate });
            pos += segBeats;
            c = loopStart;
        }
        return out;
    }

    struct ClipRef { const juce::XmlElement* clip; double position; };

    std::vector<ClipRef> arrangementClips (const juce::XmlElement* track, bool audio)
    {
        std::vector<ClipRef> out;
        auto* events = child (track, audio ? "DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events"
                                           : "DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events");
        if (events == nullptr) return out;
        for (auto* c : events->getChildWithTagNameIterator (audio ? "AudioClip" : "MidiClip"))
            out.push_back ({ c, c->getDoubleAttribute ("Time", number (c, "CurrentStart")) });
        return out;
    }

    std::vector<const juce::XmlElement*> sessionClips (const juce::XmlElement* track, bool audio)
    {
        std::vector<const juce::XmlElement*> out;   // one entry per scene (nullptr if empty)
        auto* slots = child (track, "DeviceChain/MainSequencer/ClipSlotList");
        if (slots == nullptr) return out;
        for (auto* slot : slots->getChildWithTagNameIterator ("ClipSlot"))
            out.push_back (child (slot, juce::String ("ClipSlot/Value/") + (audio ? "AudioClip" : "MidiClip")));
        return out;
    }

    bool hasDevice (const juce::XmlElement* track, const char* tag)
    {
        if (auto* devices = child (track, "DeviceChain/DeviceChain/Devices"))
            for (auto* d : devices->getChildIterator())
                if (d->getTagName() == tag) return true;
        return false;
    }

    bool nameSaysDrums (const juce::String& name)
    {
        const auto n = " " + name.toLowerCase().replaceCharacters ("-_.()", "     ") + " ";
        for (auto* w : { "drum", "beat", "kick", " bd ", "snare", "hat", "perc", "clap", "impulse", "808 kit", "909" })
            if (n.contains (w)) return true;
        return false;
    }

    bool nameSaysKickOnly (const juce::String& name)
    {
        const auto n = " " + name.toLowerCase().replaceCharacters ("-_.()", "     ") + " ";
        return (n.contains ("kick") || n.contains (" bd ")) && ! n.contains ("snare") && ! n.contains ("hat") && ! n.contains ("drum");
    }

    // Track volume / pan automation. Envelopes point at a parameter's
    // AutomationTarget Id; events are (beat, value) pairs, and the first one
    // usually sits at a huge negative time to hold the starting value.
    int addMixerAutomation (const juce::XmlElement* track, juce::XmlElement& te)
    {
        const auto volId = value (track, "DeviceChain/Mixer/Volume/AutomationTarget", {}, "Id");
        const auto panId = value (track, "DeviceChain/Mixer/Pan/AutomationTarget", {}, "Id");
        auto* envelopes = child (track, "AutomationEnvelopes/Envelopes");
        if (envelopes == nullptr) return 0;

        int added = 0;
        for (auto* env : envelopes->getChildWithTagNameIterator ("AutomatedEnvelope"))
        {
            const auto target = value (env, "EnvelopeTarget/PointeeId");
            const bool isVol = target.isNotEmpty() && target == volId;
            const bool isPan = target.isNotEmpty() && target == panId;
            if (! isVol && ! isPan) continue;

            auto* events = child (env, "Automation/Events");
            if (events == nullptr) continue;
            std::vector<std::pair<double, double>> pts;
            for (auto* e : events->getChildWithTagNameIterator ("FloatEvent"))
                pts.push_back ({ e->getDoubleAttribute ("Time"), e->getDoubleAttribute ("Value") });
            if (pts.empty()) continue;
            std::sort (pts.begin(), pts.end());

            auto* ae = te.createNewChildElement ("AUTOMATION");
            ae->setAttribute ("param", isVol ? "volume" : "pan");
            ae->setAttribute ("visible", true);
            double lastBeat = -1.0;
            for (size_t i = 0; i < pts.size(); ++i)
            {
                double beat = pts[i].first;
                if (beat < 0.0)
                {
                    // keep only the last pre-roll value, placed at the start
                    if (i + 1 < pts.size() && pts[i + 1].first <= 0.0) continue;
                    beat = 0.0;
                }
                if (beat <= lastBeat - 1.0e-9) continue;
                const float v = isVol ? AutomationLane::volumeToValue ((float) pts[i].second)
                                      : AutomationLane::panToValue ((float) juce::jlimit (-1.0, 1.0, pts[i].second));
                auto* pe = ae->createNewChildElement ("P");
                pe->setAttribute ("b", beat);
                pe->setAttribute ("v", v);
                lastBeat = beat;
            }
            ++added;
        }
        return added;
    }

    // Live's Impulse plays its 8 slots from C3 (60) upwards; move them onto General MIDI drum notes
    int impulseToGm (int note)
    {
        switch (note)
        {
            case 60: return 36; case 62: return 38; case 64: return 39; case 65: return 42;
            case 67: return 46; case 69: return 45; case 71: return 49; case 72: return 37;
            default: return note;
        }
    }

    juce::String instrumentName (const juce::XmlElement* track)
    {
        if (auto* devices = child (track, "DeviceChain/DeviceChain/Devices"))
            for (auto* d : devices->getChildIterator())
            {
                if (d->getTagName() == "PluginDevice")
                {
                    for (auto* p : { "PluginDesc/VstPluginInfo/PlugName", "PluginDesc/Vst3PluginInfo/Name", "PluginDesc/AuPluginInfo/Name" })
                        if (auto v = value (d, p); v.isNotEmpty()) return v;
                    return "Plugin";
                }
                return d->getTagName();   // OriginalSimpler, Operator, InstrumentVector, UltraAnalog...
            }
        return {};
    }

    // Live's Simpler / Sampler / Drum Rack samples, as an SFZ instrument the Multi Sampler plays.
    // Returns the number of regions written (0 = no samples found, nothing written).
    struct SfzBuild { juce::String text; int regions = 0; bool drums = false; };
    void collectSampleParts (const juce::XmlElement* e, int drumNote, const juce::File& projectDir, const FileFinder& finder, SfzBuild& out, int depth = 0)
    {
        if (e == nullptr || depth > 40) return;
        if (e->getTagName() == "DrumBranch")
        {
            const int rn = (int) number (e, "BranchInfo/ReceivingNote", -1);
            if (rn >= 0 && rn <= 128) { drumNote = 128 - rn; out.drums = true; }   // Live stores pads counted down from 128
        }
        if (e->getTagName() == "MultiSamplePart")
        {
            juce::String name;
            const auto f = resolveSample (child (e, "SampleRef/FileRef"), projectDir, finder, name);
            if (! f.existsAsFile()) return;
            auto& t = out.text;
            t << "<region> sample=" << f.getFullPathName();
            const double vol = number (e, "Volume", 1.0), pan = number (e, "Panorama", 0.0);
            if (drumNote >= 0)
                t << " key=" << drumNote << " pitch_keycenter=" << drumNote << " loop_mode=one_shot";
            else
            {
                t << " lokey=" << (int) number (e, "KeyRange/Min", 0) << " hikey=" << (int) number (e, "KeyRange/Max", 127)
                  << " pitch_keycenter=" << (int) number (e, "RootKey", 60);
                const int loopMode = (int) number (e, "SustainLoop/Mode", 0);
                if (loopMode == 1 || loopMode == 2)
                    t << " loop_mode=loop_continuous loop_start=" << (juce::int64) number (e, "SustainLoop/Start", 0)
                      << " loop_end=" << (juce::int64) number (e, "SustainLoop/End", 0);
            }
            t << " lovel=" << (int) number (e, "VelocityRange/Min", 1) << " hivel=" << (int) number (e, "VelocityRange/Max", 127);
            if (const double det = number (e, "Detune", 0); std::abs (det) > 0.01) t << " tune=" << juce::roundToInt (det);
            if (vol > 0 && std::abs (vol - 1.0) > 0.001) t << " volume=" << juce::String (juce::Decibels::gainToDecibels (vol), 2);
            if (std::abs (pan) > 0.001) t << " pan=" << juce::roundToInt (pan * 100.0);
            if (const auto st = (juce::int64) number (e, "SampleStart", 0); st > 0) t << " offset=" << st;
            if (const auto en = (juce::int64) number (e, "SampleEnd", 0); en > 0) t << " end=" << en;
            t << "\n";
            ++out.regions;
            return;
        }
        for (auto* c : e->getChildIterator()) collectSampleParts (c, drumNote, projectDir, finder, out, depth + 1);
    }

    juce::File writeSamplerSfz (const juce::XmlElement* track, const juce::File& projectDir, const FileFinder& finder,
                                const juce::String& setName, const juce::String& trackName, int index, bool& isDrums)
    {
        SfzBuild b;
        collectSampleParts (child (track, "DeviceChain/DeviceChain/Devices"), -1, projectDir, finder, b);
        isDrums = b.drums;
        if (b.regions == 0) return {};
        auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("RENASOUND").getChildFile ("Imported Instruments").getChildFile (juce::File::createLegalFileName (setName));
        dir.createDirectory();
        auto f = dir.getChildFile (juce::String (index).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (trackName) + ".sfz");
        if (! f.replaceWithText ("// made by RENASOUND from the Ableton Live set \"" + setName + "\"\n" + b.text)) return {};
        return f;
    }

}

bool isAbletonProject (const juce::File& f)
{
    if (! f.hasFileExtension ("als")) return false;
    juce::FileInputStream in (f);
    if (! in.openedOk()) return false;
    const auto b0 = (juce::uint8) in.readByte(), b1 = (juce::uint8) in.readByte();
    return (b0 == 0x1f && b1 == 0x8b) || b0 == '<';
}

DawImport importAbletonProject (const juce::File& file, const juce::Array<juce::File>& extraSearchFolders)
{
    DawImport result;
    result.fromApp = "Ableton Live";

    // ---- unzip + parse ----
    std::unique_ptr<juce::XmlElement> doc;
    {
        juce::FileInputStream raw (file);
        if (! raw.openedOk()) { result.error = "Couldn't read the file."; return result; }
        juce::MemoryBlock mb;
        const auto b0 = (juce::uint8) raw.readByte();
        raw.setPosition (0);
        if (b0 == 0x1f)
        {
            juce::GZIPDecompressorInputStream gz (&raw, false, juce::GZIPDecompressorInputStream::gzipFormat);
            juce::MemoryOutputStream mo (mb, false);
            mo.writeFromInputStream (gz, 512 * 1024 * 1024);
        }
        else
        {
            juce::MemoryOutputStream mo (mb, false);
            mo.writeFromInputStream (raw, 512 * 1024 * 1024);
        }
        doc = juce::XmlDocument::parse (mb.toString());
    }
    if (doc == nullptr || ! doc->hasTagName ("Ableton"))
    {
        result.error = "This isn't an Ableton Live set, or the file is damaged.";
        return result;
    }
    auto* live = doc->getChildByName ("LiveSet");
    auto* tracksXml = child (live, "Tracks");
    if (live == nullptr || tracksXml == nullptr) { result.error = "The Live set has no tracks."; return result; }

    double bpm = number (live, "MainTrack/DeviceChain/Mixer/Tempo/Manual", 0);
    if (bpm <= 0) bpm = number (live, "MasterTrack/DeviceChain/Mixer/Tempo/Manual", 120);
    if (bpm < 20 || bpm > 999) bpm = 120;
    result.bpm = bpm;
    const double secPerBeat = 60.0 / bpm;

    const auto projectDir = file.getParentDirectory();
    FileFinder finder;
    finder.scan (projectDir);
    for (auto& f : extraSearchFolders) finder.scan (f);

    // Is anything in the arrangement? If not, lay out the Session view's scenes.
    bool arrangementEmpty = true;
    for (auto* t : tracksXml->getChildIterator())
        if (t->getTagName() == "AudioTrack" || t->getTagName() == "MidiTrack")
            if (! arrangementClips (t, t->getTagName() == "AudioTrack").empty()) { arrangementEmpty = false; break; }

    std::vector<double> sceneStart;
    if (arrangementEmpty)
    {
        std::vector<double> sceneLen;
        for (auto* t : tracksXml->getChildIterator())
        {
            if (t->getTagName() != "AudioTrack" && t->getTagName() != "MidiTrack") continue;
            auto clips = sessionClips (t, t->getTagName() == "AudioTrack");
            if (sceneLen.size() < clips.size()) sceneLen.resize (clips.size(), 0.0);
            for (size_t s = 0; s < clips.size(); ++s)
                if (clips[s] != nullptr)
                {
                    double len = number (clips[s], "Loop/LoopEnd") - number (clips[s], "Loop/LoopStart");
                    if (! flag (clips[s], "IsWarped", true)) len /= secPerBeat;
                    sceneLen[s] = juce::jmax (sceneLen[s], juce::jlimit (1.0, 256.0, len));
                }
        }
        double pos = 0;
        for (auto len : sceneLen) { sceneStart.push_back (pos); if (len > 0) pos += std::ceil (len / 4.0) * 4.0; }
        result.notes.add ("The arrangement was empty, so the Session view's scenes were placed one after another.");
    }

    // ---- build the RENASOUND project ----
    auto root = std::make_unique<juce::XmlElement> ("RENASOUND_PROJECT");
    root->setAttribute ("version", 1);
    root->setAttribute ("bpm", bpm);
    root->setAttribute ("beatsPerBar", 4);
    root->setAttribute ("masterVolume", 0.9);
    root->setAttribute ("importedFrom", "Ableton Live");

    static const juce::uint32 colours[] = { 0xffff6b4a, 0xff3ec7a5, 0xff6c8cff, 0xffffc247, 0xffd16bff, 0xff4ac1ff, 0xffff6fa8, 0xff9bd65a };
    int nextId = 1;
    std::set<juce::String> missing;
    bool anyWarpedTempoMismatch = false;
    struct InstrumentTrack { juce::XmlElement* te; const juce::XmlElement* track; juce::String name; bool drums; };
    std::vector<InstrumentTrack> instruments;

    for (auto* t : tracksXml->getChildIterator())
    {
        const bool isAudio = t->getTagName() == "AudioTrack";
        if (! isAudio && t->getTagName() != "MidiTrack") continue;

        // gather the clips (arrangement, or session laid out by scene)
        std::vector<ClipRef> clips;
        if (! arrangementEmpty) clips = arrangementClips (t, isAudio);
        else
        {
            auto sc = sessionClips (t, isAudio);
            for (size_t s = 0; s < sc.size(); ++s)
                if (sc[s] != nullptr && s < sceneStart.size()) clips.push_back ({ sc[s], sceneStart[s] });
        }
        if (clips.empty()) continue;

        auto name = value (t, "Name/UserName");
        if (name.isEmpty()) name = value (t, "Name/EffectiveName", isAudio ? "Audio" : "MIDI");

        auto* te = root->createNewChildElement ("TRACK");
        te->setAttribute ("id", nextId++);
        te->setAttribute ("type", isAudio ? "audio" : "instrument");
        te->setAttribute ("name", name);
        te->setAttribute ("colour", juce::Colour (colours[(size_t) result.numTracks % std::size (colours)]).toString());
        te->setAttribute ("volume", juce::jlimit (0.0, 1.5, number (t, "DeviceChain/Mixer/Volume/Manual", 1.0)));
        te->setAttribute ("pan", juce::jlimit (-1.0, 1.0, number (t, "DeviceChain/Mixer/Pan/Manual", 0.0)));
        te->setAttribute ("mute", ! flag (t, "DeviceChain/Mixer/Speaker/Manual", true));
        ++result.numTracks;
        result.numAutomationLanes += addMixerAutomation (t, *te);

        const bool impulse = ! isAudio && hasDevice (t, "InstrumentImpulse");
        const bool drums = ! isAudio && (hasDevice (t, "DrumGroupDevice") || impulse || nameSaysDrums (name));
        const bool kickOnly = drums && ! impulse && ! hasDevice (t, "DrumGroupDevice") && nameSaysKickOnly (name);
        if (! isAudio)
            instruments.push_back ({ te, t, name, drums });

        for (auto& ref : clips)
        {
            auto* c = ref.clip;
            if (flag (c, "Disabled")) continue;   // deactivated clip
            const double length = number (c, "CurrentEnd") - number (c, "CurrentStart");
            if (length <= 0 || length > 100000) continue;
            const auto clipName = value (c, "Name");

            if (! isAudio)
            {
                auto* ce = te->createNewChildElement ("CLIP");
                ce->setAttribute ("id", nextId++);
                ce->setAttribute ("name", clipName.isNotEmpty() ? clipName : name);
                ce->setAttribute ("start", ref.position);
                ce->setAttribute ("lengthBeats", length);

                struct N { int pitch; double t, d; float v; };
                std::vector<N> notes;
                if (auto* keyTracks = child (c, "Notes/KeyTracks"))
                    for (auto* kt : keyTracks->getChildWithTagNameIterator ("KeyTrack"))
                    {
                        const int pitch = (int) number (kt, "MidiKey", 60);
                        if (auto* ns = kt->getChildByName ("Notes"))
                            for (auto* n : ns->getChildWithTagNameIterator ("MidiNoteEvent"))
                                if (n->getStringAttribute ("IsEnabled", "true") != "false")
                                    notes.push_back ({ pitch, n->getDoubleAttribute ("Time"), n->getDoubleAttribute ("Duration", 0.25),
                                                       (float) juce::jlimit (0.0, 1.0, n->getDoubleAttribute ("Velocity", 100) / 127.0) });
                    }

                for (auto& seg : segmentsFor (c, length, false, secPerBeat))
                    for (auto& n : notes)
                    {
                        if (n.t < seg.contentStart - 1.0e-6 || n.t >= seg.contentEnd - 1.0e-6) continue;
                        const double s = seg.timelineOffset + (n.t - seg.contentStart);
                        auto* ne = ce->createNewChildElement ("NOTE");
                        const int pitch = kickOnly ? 36 : impulse ? impulseToGm (n.pitch) : n.pitch;
                        ne->setAttribute ("p", juce::jlimit (0, 127, pitch));
                        ne->setAttribute ("s", s);
                        ne->setAttribute ("l", juce::jmax (0.01, juce::jmin (n.d, length - s)));
                        ne->setAttribute ("v", n.v);
                        ++result.numNotes;
                    }
                ++result.numMidiClips;
                continue;
            }

            // ---- audio ----
            juce::String sampleName;
            const auto file2 = resolveSample (child (c, "SampleRef/FileRef"), projectDir, finder, sampleName);
            if (! file2.existsAsFile())
            {
                if (sampleName.isNotEmpty()) missing.insert (sampleName);
                continue;
            }

            const bool warped = flag (c, "IsWarped", true);
            Warp warp;
            warp.fallbackSecPerBeat = secPerBeat;
            if (auto* wm = child (c, "WarpMarkers"))
                for (auto* m : wm->getChildWithTagNameIterator ("WarpMarker"))
                    warp.marks.push_back ({ m->getDoubleAttribute ("BeatTime"), m->getDoubleAttribute ("SecTime") });
            std::sort (warp.marks.begin(), warp.marks.end());

            if (warped && warp.marks.size() >= 2)
            {
                const auto& a = warp.marks.front();
                const auto& b = warp.marks.back();
                const double sampleSecPerBeat = (b.second - a.second) / juce::jmax (1.0e-9, b.first - a.first);
                if (std::abs (sampleSecPerBeat - secPerBeat) / secPerBeat > 0.02) anyWarpedTempoMismatch = true;
            }

            for (auto& seg : segmentsFor (c, length, ! warped, secPerBeat))
            {
                const double offsetSec = warped ? warp.secAt (seg.contentStart) : seg.contentStart;
                const double segBeats = warped ? seg.contentEnd - seg.contentStart : (seg.contentEnd - seg.contentStart) / secPerBeat;
                auto* ce = te->createNewChildElement ("CLIP");
                ce->setAttribute ("id", nextId++);
                ce->setAttribute ("name", clipName.isNotEmpty() ? clipName : file2.getFileNameWithoutExtension());
                ce->setAttribute ("start", ref.position + seg.timelineOffset);
                ce->setAttribute ("file", file2.getFullPathName());
                ce->setAttribute ("offset", juce::jmax (0.0, offsetSec));
                ce->setAttribute ("length", segBeats * secPerBeat);
                ++result.numAudioClips;
            }
        }
    }

    // ---- give every instrument track its closest sound, now that its notes are known ----
    int instIndex = 0;
    for (auto& inst : instruments)
    {
        soundmatch::Hints h;
        h.trackName = inst.name;
        h.device = instrumentName (inst.track);
        for (auto* ce : inst.te->getChildWithTagNameIterator ("CLIP"))
            for (auto* ne : ce->getChildWithTagNameIterator ("NOTE"))
                h.notes.push_back ({ ne->getIntAttribute ("p"), ce->getDoubleAttribute ("start") + ne->getDoubleAttribute ("s"),
                                     ne->getDoubleAttribute ("l"), (float) ne->getDoubleAttribute ("v") });

        // Simpler, Sampler and Drum Rack samples: the very same sounds, played by the Multi Sampler
        bool sampledDrums = false;
        const auto sfz = writeSamplerSfz (inst.track, projectDir, finder, file.getFileNameWithoutExtension(), inst.name, ++instIndex, sampledDrums);
        soundmatch::Choice choice;
        if (sfz.existsAsFile())
            choice = soundmatch::multiSamplerFrom (sfz, 0, sampledDrums ? "its Drum Rack samples" : "its own samples");
        else
        {
            h.gmDrums = inst.drums;
            choice = soundmatch::match (h);
        }
        soundmatch::applyToTrackXml (choice, *inst.te);
        result.notes.add ("\"" + inst.name + "\": " + soundmatch::describe (h, choice));
        ++result.numMatchedSounds;
    }

    if (anyWarpedTempoMismatch)
        result.notes.add ("Some audio was warped to a different tempo in Live. Those clips come in at their original speed: right-click one and choose \"Fit to the song's tempo and key\" to stretch it.");

    for (auto& m : missing) result.missingFiles.add (m);
    result.project = std::move (root);
    result.ok = true;
    result.summary = juce::String (result.numTracks) + " tracks, " + juce::String (result.numAudioClips) + " audio clips, "
                   + juce::String (result.numMidiClips) + " MIDI clips at "
                   + (std::abs (bpm - std::round (bpm)) < 0.001 ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 2)) + " BPM";
    return result;
}

} // namespace rs
