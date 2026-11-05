#include "Model.h"
#include "SynthFx.h"

namespace rs
{

juce::Colour trackColourFor (int index)
{
    static const juce::uint32 colours[] = { 0xffff6b4a, 0xff3ec7a5, 0xff6c8cff, 0xffffc247,
                                            0xffd16bff, 0xff4ac1ff, 0xffff6fa8, 0xff9bd65a };
    return juce::Colour (colours[(size_t) index % std::size (colours)]);
}

// =============================================================================
AudioCache::AudioCache()
{
    formats.registerBasicFormats();
}

AudioDataPtr AudioCache::load (const juce::File& f)
{
    const juce::ScopedLock sl (cacheLock);
    const auto key = f.getFullPathName();

    if (auto it = cache.find (key); it != cache.end())
        return it->second;

    std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));
    if (reader == nullptr || reader->lengthInSamples <= 0)
        return nullptr;

    auto data = std::make_shared<AudioData>();
    data->file = f;
    data->sampleRate = reader->sampleRate;
    const auto len = (int) std::min<juce::int64> (reader->lengthInSamples, (juce::int64) (std::numeric_limits<int>::max() / 2));
    data->buffer.setSize (2, len);
    reader->read (&data->buffer, 0, len, 0, true, true);
    if (reader->numChannels == 1)
        data->buffer.copyFrom (1, 0, data->buffer, 0, 0, len);

    // waveform overview
    const int numPeaks = len / AudioData::peakBlock + 1;
    data->peaks.resize ((size_t) numPeaks * 2);
    for (int p = 0; p < numPeaks; ++p)
    {
        const int s0 = p * AudioData::peakBlock;
        const int n  = juce::jmin (AudioData::peakBlock, len - s0);
        float lo = 0, hi = 0;
        for (int ch = 0; ch < 2 && n > 0; ++ch)
        {
            auto r = data->buffer.findMinMax (ch, s0, n);
            lo = juce::jmin (lo, r.getStart());
            hi = juce::jmax (hi, r.getEnd());
        }
        data->peaks[(size_t) p * 2]     = lo;
        data->peaks[(size_t) p * 2 + 1] = hi;
    }

    cache[key] = data;
    return data;
}

// =============================================================================
static std::atomic<int> nextFxUid { 1 };
FxSlot::FxSlot() : uid (nextFxUid++) {}

juce::String AutomationLane::getId() const
{
    switch (kind)
    {
        case Kind::Volume:     return "volume";
        case Kind::Pan:        return "pan";
        case Kind::Effect:     return "fx:" + juce::String (fxUid) + ":" + juce::String (paramIndex);
        case Kind::Instrument: return "inst:" + juce::String (paramIndex);
    }
    return "volume";
}

AutomationLane AutomationLane::fromId (const juce::String& id)
{
    AutomationLane l;
    if (id == "pan") l.kind = Kind::Pan;
    else if (id.startsWith ("fx:"))
    {
        l.kind = Kind::Effect;
        l.fxUid = id.fromFirstOccurrenceOf (":", false, false).upToFirstOccurrenceOf (":", false, false).getIntValue();
        l.paramIndex = id.fromLastOccurrenceOf (":", false, false).getIntValue();
    }
    else if (id.startsWith ("inst:"))
    {
        l.kind = Kind::Instrument;
        l.paramIndex = id.fromLastOccurrenceOf (":", false, false).getIntValue();
    }
    return l;
}

juce::AudioProcessorParameter* Track::getAutomationParameter (const AutomationLane& lane) const
{
    juce::AudioProcessor* proc = nullptr;
    if (lane.kind == AutomationLane::Kind::Effect)
        if (auto* f = findFx (lane.fxUid)) proc = f->processor.get();
    if (lane.kind == AutomationLane::Kind::Instrument && instrument != nullptr)
        proc = instrument->processor.get();
    if (proc == nullptr) return nullptr;
    const auto& params = proc->getParameters();
    return lane.paramIndex >= 0 && lane.paramIndex < params.size() ? params[lane.paramIndex] : nullptr;
}

Track::Track() = default;

Project::Project (AudioCache& c) : audioCache (c)
{
    master = std::make_shared<Track>();
    master->isMaster = true;
    master->id = -100;   // never a real track id (and -1 means "no selection")
    master->name = "Master";
    master->colour = juce::Colour (0xffd8a96e);
    master->externalFx = &masterFx;
}

TrackPtr Project::addTrack (Track::Type type)
{
    auto t = std::make_shared<Track>();
    t->type = type;

    {
        const juce::ScopedLock sl (lock);
        t->id = nextId++;
        int countOfType = 1;
        for (auto& existing : tracks)
            if (existing->type == type) ++countOfType;
        t->name = (type == Track::Type::Audio ? "Audio " : "Instrument ") + juce::String (countOfType);
        t->colour = trackColourFor ((int) tracks.size());
        if (type == Track::Type::Instrument)
        {
            t->autoName = true;   // takes the instrument's name
            t->synth = std::make_unique<RenaSynth>();
            if (onTrackCreated) onTrackCreated (*t);
            if (createFx)   // every new instrument track starts with RENASYNTH
            {
                juce::String err;
                t->instrument = createFx ("builtin:synth", nullptr, err);
            }
        }
        tracks.push_back (t);
    }
    changed();
    return t;
}

void Project::removeTrack (int trackId)
{
    {
        const juce::ScopedLock sl (lock);
        tracks.erase (std::remove_if (tracks.begin(), tracks.end(), [=] (auto& t) { return t->id == trackId; }), tracks.end());
    }
    changed();
}

ClipPtr Project::addAudioClip (int trackId, const juce::File& f, double startBeat)
{
    auto data = audioCache.load (f);
    if (data == nullptr) return nullptr;

    auto c = std::make_shared<Clip>();
    c->type = Clip::Type::Audio;
    c->name = f.getFileNameWithoutExtension();
    c->audio = data;
    c->lengthSeconds = data->getLengthSeconds();
    c->startBeat = juce::jmax (0.0, startBeat);

    {
        const juce::ScopedLock sl (lock);
        auto t = findTrack (trackId);
        if (t == nullptr || t->type != Track::Type::Audio) return nullptr;
        c->id = nextId++;
        t->clips.push_back (c);
    }
    changed();
    return c;
}

ClipPtr Project::addMidiClip (int trackId, double startBeat, double lengthBeats)
{
    auto c = std::make_shared<Clip>();
    c->type = Clip::Type::Midi;
    c->name = "MIDI";
    c->startBeat = juce::jmax (0.0, startBeat);
    c->lengthBeats = lengthBeats;

    {
        const juce::ScopedLock sl (lock);
        auto t = findTrack (trackId);
        if (t == nullptr || t->type != Track::Type::Instrument) return nullptr;
        c->id = nextId++;
        c->name = t->name;
        t->clips.push_back (c);
    }
    changed();
    return c;
}

void Project::removeClip (int clipId)
{
    {
        const juce::ScopedLock sl (lock);
        for (auto& t : tracks)
            t->clips.erase (std::remove_if (t->clips.begin(), t->clips.end(), [=] (auto& c) { return c->id == clipId; }), t->clips.end());
    }
    changed();
}

ClipPtr Project::duplicateClip (int clipId)
{
    ClipPtr copy;
    {
        const juce::ScopedLock sl (lock);
        TrackPtr owner;
        auto c = findClip (clipId, &owner);
        if (c == nullptr) return nullptr;
        copy = std::make_shared<Clip> (*c);
        copy->id = nextId++;
        copy->startBeat = c->getEndBeat (bpm);
        owner->clips.push_back (copy);
    }
    changed();
    return copy;
}

ClipPtr Project::addClipCopy (int trackId, const Clip& src, double startBeat)
{
    ClipPtr c;
    {
        const juce::ScopedLock sl (lock);
        auto t = findTrack (trackId);
        if (t == nullptr || (t->type == Track::Type::Audio) != (src.type == Clip::Type::Audio)) return nullptr;
        c = std::make_shared<Clip> (src);
        c->id = nextId++;
        c->startBeat = juce::jmax (0.0, startBeat);
        c->locked = false;
        t->clips.push_back (c);
    }
    changed();
    return c;
}

ClipPtr Project::splitClip (int clipId, double atBeat)
{
    ClipPtr right;
    {
        const juce::ScopedLock sl (lock);
        TrackPtr owner;
        auto c = findClip (clipId, &owner);
        if (c == nullptr || atBeat <= c->startBeat + 0.01 || atBeat >= c->getEndBeat (bpm) - 0.01)
            return nullptr;

        right = std::make_shared<Clip> (*c);
        right->id = nextId++;
        right->startBeat = atBeat;
        const double leftBeats = atBeat - c->startBeat;

        if (c->type == Clip::Type::Audio)
        {
            const double leftSec = leftBeats * 60.0 / bpm;
            right->offsetSeconds = c->offsetSeconds + leftSec;
            right->lengthSeconds = c->lengthSeconds - leftSec;
            c->lengthSeconds = leftSec;
        }
        else
        {
            right->lengthBeats = c->lengthBeats - leftBeats;
            right->notes.clear();
            std::vector<MidiNote> keep;
            for (auto n : c->notes)
            {
                if (n.start < leftBeats) keep.push_back (n);
                else { n.start -= leftBeats; right->notes.push_back (n); }
            }
            c->notes = keep;
            c->lengthBeats = leftBeats;
        }
        owner->clips.push_back (right);
    }
    changed();
    return right;
}

TrackPtr Project::findTrack (int trackId) const
{
    if (trackId == master->id) return master;
    for (auto& t : tracks)
        if (t->id == trackId) return t;
    return nullptr;
}

ClipPtr Project::findClip (int clipId, TrackPtr* ownerOut) const
{
    for (auto& t : tracks)
        for (auto& c : t->clips)
            if (c->id == clipId)
            {
                if (ownerOut) *ownerOut = t;
                return c;
            }
    return nullptr;
}

double Project::getEndBeat() const
{
    double end = 0;
    for (auto& t : tracks)
        for (auto& c : t->clips)
            end = juce::jmax (end, c->getEndBeat (bpm));
    return end;
}

// =============================================================================
void Project::beginEdit()
{
    if (auto xml = toXml())
    {
        undoStack.push_back (xml->toString());
        if (undoStack.size() > 100) undoStack.erase (undoStack.begin());
        redoStack.clear();
    }
    dirty = true;
}

void Project::undo()
{
    if (undoStack.empty()) return;
    if (auto xml = toXml()) redoStack.push_back (xml->toString());
    auto state = undoStack.back();
    undoStack.pop_back();
    restoreFrom (state);
}

void Project::redo()
{
    if (redoStack.empty()) return;
    if (auto xml = toXml()) undoStack.push_back (xml->toString());
    auto state = redoStack.back();
    redoStack.pop_back();
    restoreFrom (state);
}

void Project::restoreFrom (const juce::String& xmlText)
{
    if (auto xml = juce::parseXML (xmlText))
        fromXml (*xml);
}

// =============================================================================
// Only the message thread changes the project, so reading it here needs no lock
// (and plugins' getStateInformation never stalls the audio thread).
std::unique_ptr<juce::XmlElement> Project::toXml() const
{
    auto root = std::make_unique<juce::XmlElement> ("RENASOUND_PROJECT");
    root->setAttribute ("version", 1);
    root->setAttribute ("bpm", bpm);
    if (countIn != 0) root->setAttribute ("countIn", countIn);
    {
        auto* se = root->createNewChildElement ("SONG");
        se->setAttribute ("title", song.title);   se->setAttribute ("artist", song.artist);
        se->setAttribute ("album", song.album);   se->setAttribute ("writers", song.writers);
        se->setAttribute ("producers", song.producers); se->setAttribute ("cover", song.coverPath);
        se->setAttribute ("format", song.format); se->setAttribute ("bits", song.bitDepth);
        se->setAttribute ("kbps", song.oggKbps);  se->setAttribute ("rate", song.sampleRate);
        se->setAttribute ("stems", song.stems);   se->setAttribute ("stemsWithMix", song.stemsWithMix);
    }
    root->setAttribute ("beatsPerBar", beatsPerBar);
    root->setAttribute ("loopEnabled", loopEnabled);
    root->setAttribute ("loopStart", loopStart);
    root->setAttribute ("loopEnd", loopEnd);
    root->setAttribute ("masterVolume", masterVolume);

    for (auto& t : tracks)
    {
        auto* te = root->createNewChildElement ("TRACK");
        te->setAttribute ("id", t->id);
        te->setAttribute ("type", t->type == Track::Type::Audio ? "audio" : "instrument");
        te->setAttribute ("name", t->name);
        te->setAttribute ("colour", t->colour.toString());
        te->setAttribute ("volume", t->volume);
        te->setAttribute ("pan", t->pan);
        te->setAttribute ("mute", t->mute);
        te->setAttribute ("solo", t->solo);
        te->setAttribute ("armed", t->armed);
        if (t->autoName) te->setAttribute ("autoName", true);
        te->setAttribute ("audioInput", t->audioInput);
        te->setAttribute ("audioInputStereo", t->audioInputStereo);
        if (t->midiInput.isNotEmpty()) te->setAttribute ("midiInput", t->midiInput);
        if (t->synth) te->setAttribute ("preset", t->synth->presetIndex.load());
        if (t->instrument) fxToXml (*t->instrument, *te, "INSTRUMENT");
        for (auto& f : t->fx) fxToXml (*f, *te, "FX");
        for (auto& lane : t->automation)
        {
            auto* ae = te->createNewChildElement ("AUTOMATION");
            ae->setAttribute ("param", lane.getId());
            ae->setAttribute ("visible", lane.visible);
            for (auto& p : lane.points)
            {
                auto* pe = ae->createNewChildElement ("P");
                pe->setAttribute ("b", p.beat);
                pe->setAttribute ("v", p.value);
            }
        }

        for (auto& c : t->clips)
        {
            auto* ce = te->createNewChildElement ("CLIP");
            ce->setAttribute ("id", c->id);
            ce->setAttribute ("name", c->name);
            ce->setAttribute ("start", c->startBeat);
            if (c->locked) ce->setAttribute ("locked", true);
            if (c->type == Clip::Type::Audio)
            {
                ce->setAttribute ("file", c->audio != nullptr ? c->audio->file.getFullPathName() : juce::String());
                ce->setAttribute ("offset", c->offsetSeconds);
                ce->setAttribute ("length", c->lengthSeconds);
                if (c->fadeInSeconds > 0)  ce->setAttribute ("fadeIn", c->fadeInSeconds);
                if (c->fadeOutSeconds > 0) ce->setAttribute ("fadeOut", c->fadeOutSeconds);
                if (c->pitch.active)
                {
                    auto* pe = ce->createNewChildElement ("PITCH");
                    pe->setAttribute ("bypass", c->pitch.bypass);
                    pe->setAttribute ("key", c->pitch.key);
                    pe->setAttribute ("scale", c->pitch.scale);
                    for (auto& n : c->pitch.notes)
                    {
                        auto* ne = pe->createNewChildElement ("PN");
                        ne->setAttribute ("s", juce::String (n.start));
                        ne->setAttribute ("e", juce::String (n.end));
                        ne->setAttribute ("d", n.detected);
                        ne->setAttribute ("sh", n.shift);
                        ne->setAttribute ("dr", n.drift);
                        ne->setAttribute ("l", n.level);
                    }
                }
            }
            else
            {
                ce->setAttribute ("lengthBeats", c->lengthBeats);
                for (auto& n : c->notes)
                {
                    auto* ne = ce->createNewChildElement ("NOTE");
                    ne->setAttribute ("p", n.pitch);
                    ne->setAttribute ("s", n.start);
                    ne->setAttribute ("l", n.length);
                    ne->setAttribute ("v", n.velocity);
                }
            }
        }
    }
    auto* masterXml = root->createNewChildElement ("MASTERFX");
    masterXml->setAttribute ("shown", showMaster);
    for (auto& f : masterFx) fxToXml (*f, *masterXml, "FX");
    for (auto& lane : master->automation)
    {
        auto* ae = masterXml->createNewChildElement ("AUTOMATION");
        ae->setAttribute ("param", lane.getId());
        ae->setAttribute ("visible", lane.visible);
        for (auto& pt : lane.points)
        {
            auto* pe = ae->createNewChildElement ("P");
            pe->setAttribute ("b", pt.beat);
            pe->setAttribute ("v", pt.value);
        }
    }
    return root;
}

void Project::fxToXml (const FxSlot& slot, juce::XmlElement& parent, const char* tag)
{
    auto* e = parent.createNewChildElement (tag);
    e->setAttribute ("uid", slot.uid);
    e->setAttribute ("type", slot.type);
    e->setAttribute ("bypass", slot.bypassed.load());
    if (slot.type == "plugin")
        if (auto d = slot.plugin.createXml()) e->addChildElement (d.release());

    juce::MemoryBlock mb;
    if (slot.processor != nullptr) slot.processor->getStateInformation (mb);
    else mb = slot.missingState;
    e->setAttribute ("state", mb.toBase64Encoding());
}

FxPtr Project::fxFromXml (const juce::XmlElement& e, FxPtr reuse)
{
    const auto type = e.getStringAttribute ("type");
    juce::PluginDescription desc;
    if (auto* d = e.getChildByName ("PLUGIN")) desc.loadFromXml (*d);
    const auto identity = type == "plugin" ? "plugin:" + desc.createIdentifierString() : type;

    juce::MemoryBlock state;
    state.fromBase64Encoding (e.getStringAttribute ("state"));

    FxPtr slot;
    if (reuse != nullptr && reuse->identity() == identity && reuse->processor != nullptr)
    {
        slot = reuse;   // same effect as before (undo / redo): just restore its settings
    }
    else
    {
        juce::String err;
        if (createFx) slot = createFx (type, type == "plugin" ? &desc : nullptr, err);
        if (slot == nullptr)
        {
            slot = std::make_shared<FxSlot>();
            slot->type = type;
            slot->plugin = desc;
        }
    }

    if (slot->processor != nullptr && state.getSize() > 0)
        slot->processor->setStateInformation (state.getData(), (int) state.getSize());
    else if (slot->processor == nullptr)
        slot->missingState = state;
    slot->bypassed = e.getBoolAttribute ("bypass");
    if (const int uid = e.getIntAttribute ("uid", 0); uid > 0)
    {
        slot->uid = uid;
        if (nextFxUid.load() <= uid) nextFxUid = uid + 1;
    }
    return slot;
}

std::vector<FxPtr>* Project::getFxChain (int trackId)
{
    if (trackId < 0) return &masterFx;
    if (auto t = findTrack (trackId)) return &t->fx;
    return nullptr;
}

void Project::addFx (int trackId, FxPtr slot, int index)
{
    {
        const juce::ScopedLock sl (lock);
        auto* chain = getFxChain (trackId);
        if (chain == nullptr || slot == nullptr) return;
        if (index < 0 || index > (int) chain->size()) chain->push_back (slot);
        else chain->insert (chain->begin() + index, slot);
    }
    changed();
}

void Project::removeFx (int trackId, const FxPtr& slot)
{
    {
        const juce::ScopedLock sl (lock);
        if (auto* chain = getFxChain (trackId))
            chain->erase (std::remove (chain->begin(), chain->end(), slot), chain->end());
    }
    changed();
}

void Project::moveFx (int trackId, const FxPtr& slot, int newIndex)
{
    {
        const juce::ScopedLock sl (lock);
        auto* chain = getFxChain (trackId);
        if (chain == nullptr) return;
        auto it = std::find (chain->begin(), chain->end(), slot);
        if (it == chain->end()) return;
        auto keep = *it;
        chain->erase (it);
        const int j = juce::jlimit (0, (int) chain->size(), newIndex);
        chain->insert (chain->begin() + j, keep);
    }
    changed();
}

void Project::setInstrument (int trackId, FxPtr slot)
{
    {
        const juce::ScopedLock sl (lock);
        auto t = findTrack (trackId);
        if (t == nullptr || t->type != Track::Type::Instrument) return;
        if (t->synth) t->synth->allNotesOff();
        t->instrument = std::move (slot);
    }
    changed();
}

void Project::fromXml (const juce::XmlElement& root)
{
    std::vector<TrackPtr> newTracks;
    int id = 1;

    // effects of tracks that survive (undo / redo) are reused instead of reloaded
    std::map<int, TrackPtr> oldTracks;
    for (auto& t : tracks) oldTracks[t->id] = t;

    for (auto* te : root.getChildWithTagNameIterator ("TRACK"))
    {
        auto t = std::make_shared<Track>();
        t->id = te->getIntAttribute ("id", 0);
        if (t->id <= 0) t->id = id + 100000;
        id = juce::jmax (id, t->id + 1);
        t->type = te->getStringAttribute ("type") == "instrument" ? Track::Type::Instrument : Track::Type::Audio;
        t->name = te->getStringAttribute ("name", "Track");
        t->colour = juce::Colour::fromString (te->getStringAttribute ("colour", "ffff6b4a"));
        t->volume = (float) te->getDoubleAttribute ("volume", 0.8);
        t->pan = (float) te->getDoubleAttribute ("pan", 0.0);
        t->mute = te->getBoolAttribute ("mute");
        t->solo = te->getBoolAttribute ("solo");
        t->armed = te->getBoolAttribute ("armed");
        t->autoName = te->getBoolAttribute ("autoName");
        t->audioInput = te->getIntAttribute ("audioInput", 0);
        t->audioInputStereo = te->getBoolAttribute ("audioInputStereo", true);
        t->midiInput = te->getStringAttribute ("midiInput");
        if (t->type == Track::Type::Instrument)
        {
            t->synth = std::make_unique<RenaSynth>();
            t->synth->presetIndex = te->getIntAttribute ("preset", 0);
            if (onTrackCreated) onTrackCreated (*t);
        }

        TrackPtr old = oldTracks.count (t->id) ? oldTracks[t->id] : nullptr;
        if (auto* ie = te->getChildByName ("INSTRUMENT"); ie != nullptr && t->type == Track::Type::Instrument)
            t->instrument = fxFromXml (*ie, old != nullptr ? old->instrument : nullptr);
        else if (t->type == Track::Type::Instrument && createFx)
        {
            // projects from before RENASYNTH: the old synth's sound becomes the same RENASYNTH sound
            juce::String err;
            if ((t->instrument = createFx ("builtin:synth", nullptr, err)) != nullptr)
                loadSynthPatch (t->instrument->processor.get(), te->getIntAttribute ("preset", 0));
        }
        int fxIndex = 0;
        for (auto* fe : te->getChildWithTagNameIterator ("FX"))
        {
            FxPtr reuse = (old != nullptr && fxIndex < (int) old->fx.size()) ? old->fx[(size_t) fxIndex] : nullptr;
            t->fx.push_back (fxFromXml (*fe, reuse));
            ++fxIndex;
        }

        for (auto* ae : te->getChildWithTagNameIterator ("AUTOMATION"))
        {
            auto lane = AutomationLane::fromId (ae->getStringAttribute ("param", "volume"));
            lane.visible = ae->getBoolAttribute ("visible", true);
            for (auto* pe : ae->getChildWithTagNameIterator ("P"))
                lane.points.push_back ({ pe->getDoubleAttribute ("b"), (float) juce::jlimit (0.0, 1.0, pe->getDoubleAttribute ("v", 0.5)) });
            std::sort (lane.points.begin(), lane.points.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
            t->automation.push_back (lane);
        }

        for (auto* ce : te->getChildWithTagNameIterator ("CLIP"))
        {
            auto c = std::make_shared<Clip>();
            c->id = ce->getIntAttribute ("id", 0);
            if (c->id <= 0) c->id = id + 100000;
            id = juce::jmax (id, c->id + 1);
            c->name = ce->getStringAttribute ("name");
            c->startBeat = ce->getDoubleAttribute ("start");
            c->locked = ce->getBoolAttribute ("locked");
            if (t->type == Track::Type::Audio)
            {
                c->type = Clip::Type::Audio;
                c->audio = audioCache.load (juce::File (ce->getStringAttribute ("file")));
                if (c->audio == nullptr) continue;   // missing file: skip
                c->offsetSeconds = ce->getDoubleAttribute ("offset");
                c->lengthSeconds = ce->getDoubleAttribute ("length", c->audio->getLengthSeconds());
                c->fadeInSeconds = ce->getDoubleAttribute ("fadeIn");
                c->fadeOutSeconds = ce->getDoubleAttribute ("fadeOut");
                if (auto* pe = ce->getChildByName ("PITCH"))
                {
                    c->pitch.active = true;
                    c->pitch.bypass = pe->getBoolAttribute ("bypass");
                    c->pitch.key = pe->getIntAttribute ("key");
                    c->pitch.scale = pe->getIntAttribute ("scale");
                    for (auto* ne : pe->getChildWithTagNameIterator ("PN"))
                        c->pitch.notes.push_back ({ ne->getStringAttribute ("s").getLargeIntValue(), ne->getStringAttribute ("e").getLargeIntValue(),
                                                    (float) ne->getDoubleAttribute ("d"), (float) ne->getDoubleAttribute ("sh"),
                                                    (float) ne->getDoubleAttribute ("dr"), (float) ne->getDoubleAttribute ("l", 0.5) });

                    // keep the corrected audio from before an undo / redo if it still matches
                    for (auto& [oid, ot] : oldTracks)
                        for (auto& oc : ot->clips)
                            if (oc->id == c->id && oc->pitchRendered != nullptr && oc->pitchRenderedHash == c->pitch.hash())
                            {
                                c->pitchRendered = oc->pitchRendered;
                                c->pitchRenderedHash = oc->pitchRenderedHash;
                            }
                }
            }
            else
            {
                c->type = Clip::Type::Midi;
                c->lengthBeats = ce->getDoubleAttribute ("lengthBeats", 4);
                for (auto* ne : ce->getChildWithTagNameIterator ("NOTE"))
                    c->notes.push_back ({ ne->getIntAttribute ("p"), ne->getDoubleAttribute ("s"),
                                          ne->getDoubleAttribute ("l"), (float) ne->getDoubleAttribute ("v", 0.8) });
            }
            t->clips.push_back (c);
        }
        newTracks.push_back (t);
    }

    std::vector<FxPtr> newMaster;
    std::vector<AutomationLane> newMasterLanes;
    bool newShowMaster = false;
    if (auto* me = root.getChildByName ("MASTERFX"))
    {
        newShowMaster = me->getBoolAttribute ("shown", false);
        for (auto* ae : me->getChildWithTagNameIterator ("AUTOMATION"))
        {
            auto lane = AutomationLane::fromId (ae->getStringAttribute ("param", "volume"));
            lane.visible = ae->getBoolAttribute ("visible", true);
            for (auto* pe : ae->getChildWithTagNameIterator ("P"))
                lane.points.push_back ({ pe->getDoubleAttribute ("b"), (float) juce::jlimit (0.0, 1.0, pe->getDoubleAttribute ("v", 0.5)) });
            std::sort (lane.points.begin(), lane.points.end(), [] (auto& a, auto& b) { return a.beat < b.beat; });
            newMasterLanes.push_back (lane);
        }
        int i = 0;
        for (auto* fe : me->getChildWithTagNameIterator ("FX"))
        {
            newMaster.push_back (fxFromXml (*fe, i < (int) masterFx.size() ? masterFx[(size_t) i] : nullptr));
            ++i;
        }
    }

    {
        const juce::ScopedLock sl (lock);
        bpm = juce::jlimit (20.0, 300.0, root.getDoubleAttribute ("bpm", 120));
        countIn = root.getIntAttribute ("countIn", 0);
        song = {};
        if (auto* se = root.getChildByName ("SONG"))
        {
            song.title = se->getStringAttribute ("title");     song.artist = se->getStringAttribute ("artist");
            song.album = se->getStringAttribute ("album");     song.writers = se->getStringAttribute ("writers");
            song.producers = se->getStringAttribute ("producers"); song.coverPath = se->getStringAttribute ("cover");
            song.format = se->getIntAttribute ("format", 0);   song.bitDepth = se->getIntAttribute ("bits", 24);
            song.oggKbps = se->getIntAttribute ("kbps", 256);  song.sampleRate = se->getDoubleAttribute ("rate", 0);
            song.stems = se->getBoolAttribute ("stems");       song.stemsWithMix = se->getBoolAttribute ("stemsWithMix", true);
        }
        beatsPerBar = root.getIntAttribute ("beatsPerBar", 4);
        loopEnabled = root.getBoolAttribute ("loopEnabled");
        loopStart = root.getDoubleAttribute ("loopStart", 0);
        loopEnd = root.getDoubleAttribute ("loopEnd", 16);
        masterVolume = (float) root.getDoubleAttribute ("masterVolume", 0.9);
        tracks = std::move (newTracks);
        masterFx = std::move (newMaster);
        master->automation = std::move (newMasterLanes);
        showMaster = newShowMaster;
        nextId = id;
    }
    changed();
}

bool Project::save (const juce::File& f)
{
    auto xml = toXml();
    if (xml == nullptr || ! xml->writeTo (f)) return false;
    file = f;
    dirty = false;
    sendChangeMessage();
    return true;
}

bool Project::load (const juce::File& f)
{
    auto xml = juce::parseXML (f);
    if (xml == nullptr || ! xml->hasTagName ("RENASOUND_PROJECT")) return false;
    fromXml (*xml);
    undoStack.clear();
    redoStack.clear();
    file = f;
    dirty = false;
    return true;
}

void Project::loadImported (const juce::XmlElement& xml)
{
    fromXml (xml);
    undoStack.clear();
    redoStack.clear();
    file = juce::File();
    dirty = true;
}

void Project::clear()
{
    {
        const juce::ScopedLock sl (lock);
        tracks.clear();
        masterFx.clear();
        master->automation.clear();
        showMaster = false;
        bpm = 120;
        loopEnabled = false;
        loopStart = 0; loopEnd = 16;
        masterVolume = 0.9f;
        song = {};
        countIn = 0;
    }
    undoStack.clear();
    redoStack.clear();
    file = juce::File();
    changed();
    dirty = false;
}

} // namespace rs
