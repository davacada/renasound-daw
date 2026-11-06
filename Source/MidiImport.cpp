#include "MidiImport.h"
#include "SoundMatch.h"
#include <map>
#include <array>

namespace rs
{

bool isMidiFile (const juce::File& f)
{
    if (! f.existsAsFile()) return false;
    juce::FileInputStream in (f);
    if (! in.openedOk()) return false;
    char head[4] {};
    in.read (head, 4);
    if (std::memcmp (head, "MThd", 4) == 0) return true;
    if (std::memcmp (head, "RIFF", 4) == 0 && f.hasFileExtension ("rmi")) return true;
    return false;
}

DawImport importMidiFile (const juce::File& file)
{
    DawImport result;
    result.fromApp = "MIDI file";

    juce::MidiFile mf;
    {
        juce::FileInputStream in (file);
        if (! in.openedOk() || ! mf.readFrom (in, true)) { result.error = "This isn't a MIDI file, or it's damaged."; return result; }
    }
    const short tf = mf.getTimeFormat();
    const bool smpte = tf <= 0;
    const double ppq = smpte ? 1.0 : (double) tf;

    // ---- tempo and time signature: the first of each ----
    double bpm = 120.0;
    int beatsPerBar = 4;
    bool tempoChanges = false;
    {
        juce::MidiMessageSequence tempos, sigs;
        mf.findAllTempoEvents (tempos);
        mf.findAllTimeSigEvents (sigs);
        for (int i = 0; i < tempos.getNumEvents(); ++i)
        {
            const auto& m = tempos.getEventPointer (i)->message;
            const double spq = m.getTempoSecondsPerQuarterNote();
            if (spq <= 0) continue;
            const double b = 60.0 / spq;
            if (i == 0 || tempos.getEventPointer (i)->message.getTimeStamp() <= 0.0) bpm = b;
            else if (std::abs (b - bpm) > 0.5) tempoChanges = true;
        }
        if (sigs.getNumEvents() > 0)
        {
            int n = 4, d = 4;
            sigs.getEventPointer (0)->message.getTimeSignatureInfo (n, d);
            if (n >= 1 && d >= 1) beatsPerBar = juce::jlimit (1, 16, juce::roundToInt (n * 4.0 / d));   // in quarter-note beats
        }
    }
    if (! (bpm >= 20 && bpm <= 400)) bpm = 120.0;
    bpm = std::round (bpm * 1000.0) / 1000.0;
    result.bpm = bpm;

    // SMPTE files are timed in seconds: turn them into beats at the file's tempo
    if (smpte) mf.convertTimestampTicksToSeconds();
    auto toBeats = [&] (double t) { return smpte ? t * bpm / 60.0 : t / ppq; };

    auto root = std::make_unique<juce::XmlElement> ("RENASOUND_PROJECT");
    root->setAttribute ("version", 1);
    root->setAttribute ("bpm", bpm);
    root->setAttribute ("beatsPerBar", beatsPerBar);
    root->setAttribute ("masterVolume", 0.9);
    root->setAttribute ("importedFrom", "MIDI file");

    static const juce::uint32 colours[] = { 0xffff6b4a, 0xff3ec7a5, 0xff6c8cff, 0xffffc247, 0xffd16bff, 0xff4ac1ff, 0xffff6fa8, 0xff9bd65a };
    int nextId = 1;

    // program changes can sit on a different MIDI track than the notes (type 1 files with a "setup" track)
    std::array<int, 16> programOf; programOf.fill (-1);
    for (int ti = 0; ti < mf.getNumTracks(); ++ti)
    {
        const auto* seq = mf.getTrack (ti);
        for (int i = 0; i < seq->getNumEvents(); ++i)
        {
            const auto& m = seq->getEventPointer (i)->message;
            if (m.isProgramChange() && programOf[(size_t) m.getChannel() - 1] < 0) programOf[(size_t) m.getChannel() - 1] = m.getProgramChangeNumber();
        }
    }

    for (int ti = 0; ti < mf.getNumTracks(); ++ti)
    {
        juce::MidiMessageSequence seq (*mf.getTrack (ti));
        seq.updateMatchedPairs();

        juce::String trackName, instrumentName;
        struct Part
        {
            std::vector<soundmatch::NoteInfo> notes;
            int program = -1;
            float volume = -1, pan = 0;
        };
        std::map<int, Part> parts;   // by channel 1..16

        for (int i = 0; i < seq.getNumEvents(); ++i)
        {
            const auto* ev = seq.getEventPointer (i);
            const auto& m = ev->message;
            if (m.isTrackNameEvent() && trackName.isEmpty()) trackName = m.getTextFromTextMetaEvent().trim();
            else if (m.isTextMetaEvent() && m.getMetaEventType() == 4 && instrumentName.isEmpty()) instrumentName = m.getTextFromTextMetaEvent().trim();
            else if (m.isProgramChange() && parts[m.getChannel()].program < 0) parts[m.getChannel()].program = m.getProgramChangeNumber();
            else if (m.isController() && m.getControllerNumber() == 7 && parts[m.getChannel()].volume < 0)
                parts[m.getChannel()].volume = m.getControllerValue() / 127.0f;
            else if (m.isController() && m.getControllerNumber() == 10)
            {
                auto& p = parts[m.getChannel()];
                if (p.notes.empty()) p.pan = juce::jlimit (-1.0f, 1.0f, (m.getControllerValue() - 64) / 63.0f);
            }
            else if (m.isNoteOn())
            {
                const double s = toBeats (m.getTimeStamp());
                double e = s + 0.25;
                if (ev->noteOffObject != nullptr) e = juce::jmax (s + 0.01, toBeats (ev->noteOffObject->message.getTimeStamp()));
                parts[m.getChannel()].notes.push_back ({ m.getNoteNumber(), s, e - s, m.getFloatVelocity() });
            }
        }

        int partsWithNotes = 0;
        for (auto& [ch, p] : parts) if (! p.notes.empty()) ++partsWithNotes;

        for (auto& [ch, p] : parts)
        {
            if (p.notes.empty()) continue;
            const bool drums = ch == 10;
            const int program = p.program >= 0 ? p.program : programOf[(size_t) ch - 1];

            soundmatch::Hints h;
            h.trackName = trackName;
            h.preset = instrumentName;
            h.gmDrums = drums;
            h.gmProgram = drums ? -1 : program;   // no program change: let the names and notes decide
            h.notes = p.notes;
            const auto choice = soundmatch::match (h);

            juce::String name = trackName.isNotEmpty() ? trackName : instrumentName;
            const juce::String what = drums ? juce::String ("Drums") : juce::String (soundmatch::gmName (choice.gmProgram >= 0 ? choice.gmProgram : 0));
            if (name.isEmpty()) name = what;
            else if (partsWithNotes > 1) name << " (" << what << ")";

            auto* te = root->createNewChildElement ("TRACK");
            te->setAttribute ("id", nextId++);
            te->setAttribute ("type", "instrument");
            te->setAttribute ("name", name);
            te->setAttribute ("colour", juce::Colour (colours[(size_t) result.numTracks % std::size (colours)]).toString());
            te->setAttribute ("volume", p.volume >= 0 ? juce::jlimit (0.0, 1.5, (double) p.volume * 0.9) : 0.8);
            te->setAttribute ("pan", (double) p.pan);
            soundmatch::applyToTrackXml (choice, *te);
            result.notes.add ("\"" + name + "\": " + soundmatch::describe (h, choice));
            ++result.numMatchedSounds;
            ++result.numTracks;

            // one clip from the bar the part starts in to the end of the bar it finishes in
            double first = 1.0e12, last = 0;
            for (auto& n : p.notes) { first = juce::jmin (first, n.start); last = juce::jmax (last, n.start + n.length); }
            const double barStart = std::floor (first / beatsPerBar + 1.0e-9) * beatsPerBar;
            const double length = juce::jmax ((double) beatsPerBar, std::ceil ((last - barStart) / beatsPerBar - 1.0e-9) * beatsPerBar);
            auto* ce = te->createNewChildElement ("CLIP");
            ce->setAttribute ("id", nextId++);
            ce->setAttribute ("name", name);
            ce->setAttribute ("start", barStart);
            ce->setAttribute ("lengthBeats", length);
            for (auto& n : p.notes)
            {
                auto* ne = ce->createNewChildElement ("NOTE");
                ne->setAttribute ("p", n.pitch);
                ne->setAttribute ("s", n.start - barStart);
                ne->setAttribute ("l", n.length);
                ne->setAttribute ("v", n.velocity);
                ++result.numNotes;
            }
            ++result.numMidiClips;
        }
    }

    if (result.numTracks == 0) { result.error = "This MIDI file has no notes."; return result; }
    if (tempoChanges) result.notes.add ("The file changes tempo along the way; RENASOUND uses its first tempo (" + juce::String (bpm, 1) + " BPM) throughout.");

    result.project = std::move (root);
    result.ok = true;
    result.summary = juce::String (result.numTracks) + " instrument track" + (result.numTracks == 1 ? "" : "s") + ", "
                   + juce::String (result.numNotes) + " notes at " + (std::abs (bpm - std::round (bpm)) < 0.001 ? juce::String (juce::roundToInt (bpm)) : juce::String (bpm, 2)) + " BPM";
    return result;
}

int appendImportedTracks (juce::XmlElement& projectXml, const juce::XmlElement& imported, double startBeat, int& firstFreeId)
{
    int added = 0;
    for (auto* te : imported.getChildWithTagNameIterator ("TRACK"))
    {
        auto* copy = new juce::XmlElement (*te);
        copy->setAttribute ("id", firstFreeId++);
        for (auto* ce : copy->getChildWithTagNameIterator ("CLIP"))
        {
            ce->setAttribute ("id", firstFreeId++);
            ce->setAttribute ("start", ce->getDoubleAttribute ("start") + startBeat);
        }
        // automation lanes point at the track's own effects by uid; imported MIDI tracks have none
        projectXml.addChildElement (copy);
        ++added;
    }
    return added;
}

} // namespace rs
