#include "AudioEngine.h"
#include "Devices.h"
#include "Drums.h"

namespace rs
{

// Gentle soft-clipper on the master bus so overs bend instead of crackling.
static void softClip (juce::AudioBuffer<float>& b, int numSamples)
{
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        auto* d = b.getWritePointer (ch);
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = d[i], ax = std::abs (x);
            if (ax > 0.8f)
                d[i] = std::copysign (0.8f + 0.2f * std::tanh ((ax - 0.8f) / 0.2f), x);
        }
    }
}

// =============================================================================
//  Plugin host
// =============================================================================
PluginHost::PluginHost()
{
    juce::addDefaultFormatsToManager (formats);

    juce::PropertiesFile::Options opts;
    opts.applicationName = "RENASOUND";
    opts.filenameSuffix = ".settings";
    opts.folderName = "Renasound";
    opts.osxLibrarySubFolder = "Application Support";
    settings = std::make_unique<juce::PropertiesFile> (getDeadMansPedalFile().getSiblingFile ("plugins.settings"), opts);

    if (auto xml = settings->getXmlValue ("knownPlugins"))
        knownPlugins.recreateFromXml (*xml);
}

PluginHost::~PluginHost() { save(); }

void PluginHost::save()
{
    if (auto xml = knownPlugins.createXml())
        settings->setValue ("knownPlugins", xml.get());
    settings->saveIfNeeded();
}

juce::File PluginHost::getDeadMansPedalFile() const
{
    auto dir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Renasound");
    dir.createDirectory();
    return dir.getChildFile ("plugin-scan-in-progress.txt");
}

juce::Array<juce::PluginDescription> PluginHost::getEffects() const
{
    juce::Array<juce::PluginDescription> out;
    for (auto& d : knownPlugins.getTypes())
        if (! d.isInstrument) out.add (d);
    return out;
}

juce::Array<juce::PluginDescription> PluginHost::getInstruments() const
{
    juce::Array<juce::PluginDescription> out;
    for (auto& d : knownPlugins.getTypes())
        if (d.isInstrument) out.add (d);
    return out;
}

// =============================================================================
AudioEngine::AudioEngine (Project& p) : project (p)
{
    project.onTrackCreated = [this] (Track& t)
    {
        if (t.synth) t.synth->prepare (sampleRate);
    };
    project.createFx = [this] (const juce::String& type, const juce::PluginDescription* d, juce::String& err)
    {
        return createFx (type, d, err);
    };
    SidechainUser::listTracks = [this]
    {
        juce::Array<std::pair<int, juce::String>> out;
        for (auto& t : project.tracks) out.add ({ t->id, t->name });
        return out;
    };

    writerThread.startThread();

    // 2 inputs (for recording) and 2 outputs; remembers the user's choice between launches.
    auto settingsFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("Renasound").getChildFile ("audio-device.xml");
    std::unique_ptr<juce::XmlElement> saved;
    if (settingsFile.existsAsFile()) saved = juce::parseXML (settingsFile);
    deviceManager.initialise (2, 2, saved.get(), true);

    for (auto& m : juce::MidiInput::getAvailableDevices())
        deviceManager.setMidiInputDeviceEnabled (m.identifier, true);
    deviceManager.addMidiInputDeviceCallback ({}, this);

    deviceManager.addAudioCallback (this);
}

AudioEngine::~AudioEngine()
{
    if (recording) finishRecording();
    deviceManager.removeAudioCallback (this);
    deviceManager.removeMidiInputDeviceCallback ({}, this);

    // release every plugin while the plugin host is still alive
    {
        const juce::ScopedLock sl (project.lock);
        for (auto& t : project.tracks) { t->fx.clear(); t->instrument.reset(); }
        project.masterFx.clear();
    }
    project.createFx = nullptr;
    SidechainUser::listTracks = nullptr;

    auto settingsFile = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                            .getChildFile ("Renasound").getChildFile ("audio-device.xml");
    if (auto xml = deviceManager.createStateXml())
    {
        settingsFile.getParentDirectory().createDirectory();
        xml->writeTo (settingsFile);
    }
    writerThread.stopThread (2000);
}

juce::File AudioEngine::getRecordingsFolder()
{
    auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory)
                   .getChildFile ("Renasound").getChildFile ("Recordings");
    dir.createDirectory();
    return dir;
}

// =============================================================================
//  Transport
// =============================================================================
void AudioEngine::play()
{
    playing = true;
}

void AudioEngine::stop()
{
    if (! playing && ! recording)
    {
        setPositionBeats (0);   // second press of stop returns to the start, like Mixcraft
        return;
    }
    playing = false;
    if (recording) finishRecording();
}

double AudioEngine::getPositionBeats() const
{
    return positionSeconds.load() * project.bpm / 60.0;
}

void AudioEngine::setPositionBeats (double beats)
{
    positionSeconds = juce::jmax (0.0, beats) * 60.0 / project.bpm;
    const juce::ScopedLock sl (project.lock);
    allNotesOff();
}

void AudioEngine::startRecording()
{
    if (recording) return;

    recordTrackId = -1;
    recordInstrumentTrackId = -1;
    {
        const juce::ScopedLock sl (project.lock);
        for (auto& t : project.tracks)
        {
            if (! t->armed) continue;
            if (t->type == Track::Type::Audio && recordTrackId < 0)            recordTrackId = t->id;
            if (t->type == Track::Type::Instrument && recordInstrumentTrackId < 0) recordInstrumentTrackId = t->id;
        }
    }
    if (recordTrackId < 0 && recordInstrumentTrackId < 0)
        return;

    recordStartBeat = getPositionBeats();

    if (recordTrackId >= 0)
    {
        recordingFile = getRecordingsFolder().getNonexistentChildFile (
            "Take " + juce::Time::getCurrentTime().formatted ("%Y-%m-%d %H-%M-%S"), ".wav");

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (recordingFile.createOutputStream());
        if (stream != nullptr)
        {
            auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                           .withSampleRate (sampleRate)
                                                           .withNumChannels (2)
                                                           .withBitsPerSample (24));
            if (writer != nullptr)
            {
                threadedWriter = std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (writer.release(), writerThread, 1 << 18);
                activeWriter = threadedWriter.get();
            }
        }
        if (activeWriter.load() == nullptr) recordTrackId = -1;
    }

    {
        const juce::SpinLock::ScopedLockType sl (previewLock);
        recordedNotes.clear();
        recordedNotes.reserve (4096);
        heldNoteStart.fill (-1.0);
    }

    recording = true;
    playing = true;
}

void AudioEngine::finishRecording()
{
    recording = false;
    const double endBeat = getPositionBeats();

    {
        const juce::SpinLock::ScopedLockType sl (previewLock);
        activeWriter = nullptr;
    }
    threadedWriter.reset();   // flushes and closes the file

    bool added = false;
    project.beginEdit();

    if (recordTrackId >= 0 && recordingFile.existsAsFile() && recordingFile.getSize() > 100)
        added = project.addAudioClip (recordTrackId, recordingFile, recordStartBeat) != nullptr || added;

    if (recordInstrumentTrackId >= 0)
    {
        std::vector<MidiNote> notes;
        {
            const juce::SpinLock::ScopedLockType sl (previewLock);
            notes = recordedNotes;
            for (int p = 0; p < 128; ++p)        // close notes still held when stopping
                if (heldNoteStart[(size_t) p] >= 0)
                    notes.push_back ({ p, heldNoteStart[(size_t) p], endBeat - heldNoteStart[(size_t) p], heldNoteVelocity[(size_t) p] });
        }

        if (! notes.empty())
        {
            const double startBar = std::floor (recordStartBeat / project.beatsPerBar) * project.beatsPerBar;
            const double endBar = std::ceil (juce::jmax (endBeat, startBar + 0.01) / project.beatsPerBar) * project.beatsPerBar;
            if (auto clip = project.addMidiClip (recordInstrumentTrackId, startBar, endBar - startBar))
            {
                const juce::ScopedLock sl (project.lock);
                for (auto n : notes)
                {
                    n.start -= startBar;
                    n.length = juce::jmax (0.05, n.length);
                    clip->notes.push_back (n);
                }
                added = true;
            }
        }
    }

    if (added) project.changed();
    if (onRecordingFinished) onRecordingFinished();
}

void AudioEngine::allNotesOff()
{
    for (auto& t : project.tracks)
    {
        if (t->synth) t->synth->allNotesOff();
        if (t->instrument) t->pendingNotesOff = true;
    }
}

// =============================================================================
//  Preview
// =============================================================================
void AudioEngine::preview (AudioDataPtr data)
{
    const juce::SpinLock::ScopedLockType sl (previewLock);
    previewData = std::move (data);
    previewPos = previewData != nullptr ? 0.0 : -1.0;
}

void AudioEngine::stopPreview()
{
    const juce::SpinLock::ScopedLockType sl (previewLock);
    previewPos = -1.0;
    previewData = nullptr;
}

// =============================================================================
//  Device callbacks
// =============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    sampleRate = device->getCurrentSampleRate();
    blockSize = device->getCurrentBufferSizeSamples();
    master.setSize (2, blockSize * 2);
    midiCollector.reset (sampleRate);

    maxBlock = juce::jmax (1024, blockSize);
    fxTemp.setSize (8, maxBlock);

    const juce::ScopedLock sl (project.lock);
    for (auto& t : project.tracks)
    {
        t->scratch.setSize (2, maxBlock);
        if (t->synth) t->synth->prepare (sampleRate);
        if (t->instrument && t->instrument->processor) prepareProcessor (*t->instrument->processor);
        for (auto& f : t->fx) if (f->processor) prepareProcessor (*f->processor);
    }
    for (auto& f : project.masterFx) if (f->processor) prepareProcessor (*f->processor);
}

// =============================================================================
//  Effects & plugins
// =============================================================================
void AudioEngine::prepareProcessor (juce::AudioProcessor& p)
{
    p.setPlayHead (this);
    p.setRateAndBufferSizeDetails (sampleRate, maxBlock);
    p.prepareToPlay (sampleRate, maxBlock);
}

FxPtr AudioEngine::createFx (const juce::String& type, const juce::PluginDescription* desc, juce::String& error)
{
    auto slot = std::make_shared<FxSlot>();
    slot->type = type;

    if (type == "builtin:drums")
    {
        slot->processor = std::make_unique<DrumMachine>();
    }
    else if (type.startsWith ("builtin:"))
    {
        slot->processor = createBuiltinFx (type.fromFirstOccurrenceOf (":", false, false));
        if (slot->processor == nullptr) { error = "Unknown effect " + type; return nullptr; }
    }
    else if (type == "plugin" && desc != nullptr)
    {
        slot->plugin = *desc;
        auto instance = pluginHost.formats.createPluginInstance (*desc, sampleRate, maxBlock, error);
        if (instance == nullptr) return nullptr;

        // stereo in / stereo out on the main buses; extra buses (sidechains etc.) off
        instance->disableNonMainBuses();
        juce::AudioProcessor::BusesLayout layout;
        if (instance->getBusCount (true) > 0)  layout.inputBuses.add (juce::AudioChannelSet::stereo());
        if (instance->getBusCount (false) > 0) layout.outputBuses.add (juce::AudioChannelSet::stereo());
        for (int i = 1; i < instance->getBusCount (true); ++i)  layout.inputBuses.add (juce::AudioChannelSet::disabled());
        for (int i = 1; i < instance->getBusCount (false); ++i) layout.outputBuses.add (juce::AudioChannelSet::disabled());
        instance->setBusesLayout (layout);
        slot->processor = std::move (instance);
    }
    else
    {
        error = "Nothing to load";
        return nullptr;
    }

    prepareProcessor (*slot->processor);
    return slot;
}

void AudioEngine::processFx (FxSlot& slot, juce::AudioBuffer<float>& stereo, juce::MidiBuffer& midi, int n)
{
    auto* p = slot.processor.get();
    if (p == nullptr || slot.bypassed.load()) return;

    const int ins = p->getTotalNumInputChannels(), outs = p->getTotalNumOutputChannels();
    const int needed = juce::jmax (ins, outs, 1);

    if (needed == 2)
    {
        juce::AudioBuffer<float> view (stereo.getArrayOfWritePointers(), 2, 0, n);
        p->processBlock (view, midi);
        return;
    }

    // mono or multi-channel plugins: run them in a scratch buffer
    if (fxTemp.getNumChannels() < needed || fxTemp.getNumSamples() < n)
        fxTemp.setSize (juce::jmax (needed, 2), juce::jmax (n, maxBlock), false, false, true);
    juce::AudioBuffer<float> view (fxTemp.getArrayOfWritePointers(), needed, 0, n);
    view.clear();
    if (ins == 1)
    {
        view.copyFrom (0, 0, stereo, 0, 0, n);
        view.applyGain (0, 0, n, 0.5f);
        view.addFrom (0, 0, stereo, 1, 0, n, 0.5f);
    }
    else
        for (int ch = 0; ch < juce::jmin (2, ins); ++ch) view.copyFrom (ch, 0, stereo, ch, 0, n);

    p->processBlock (view, midi);

    if (outs == 1) { stereo.copyFrom (0, 0, view, 0, 0, n); stereo.copyFrom (1, 0, view, 0, 0, n); }
    else           for (int ch = 0; ch < 2; ++ch) stereo.copyFrom (ch, 0, view, ch, 0, n);
}

void AudioEngine::processChain (std::vector<FxPtr>& chain, juce::AudioBuffer<float>& buffer, int n)
{
    for (auto& f : chain)
    {
        emptyMidi.clear();
        processFx (*f, buffer, emptyMidi, n);
    }
}

juce::Optional<juce::AudioPlayHead::PositionInfo> AudioEngine::getPosition() const
{
    PositionInfo info;
    const double bpm = project.bpm;
    info.setBpm (bpm);
    info.setTimeSignature (TimeSignature { project.beatsPerBar, 4 });
    info.setTimeInSeconds (playheadSeconds);
    info.setTimeInSamples ((juce::int64) (playheadSeconds * sampleRate));
    const double ppq = playheadSeconds * bpm / 60.0;
    info.setPpqPosition (ppq);
    info.setPpqPositionOfLastBarStart (std::floor (ppq / project.beatsPerBar) * project.beatsPerBar);
    info.setIsPlaying (playheadRunning);
    info.setIsRecording (recording.load());
    info.setIsLooping (project.loopEnabled);
    return info;
}

void AudioEngine::audioDeviceStopped() {}

void AudioEngine::handleIncomingMidiMessage (juce::MidiInput*, const juce::MidiMessage& m)
{
    midiCollector.addMessageToQueue (m);
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* in, int numIn, float* const* out, int numOut,
                                                    int numSamples, const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr) juce::FloatVectorOperations::clear (out[ch], numSamples);

    // ---- live MIDI from hardware + on-screen keyboard ----
    liveMidi.clear();
    midiCollector.removeNextBlockOfMessages (liveMidi, numSamples);
    keyboardState.processNextMidiBuffer (liveMidi, 0, numSamples, true);

    const bool run = playing.load();
    const double startPos = positionSeconds.load();

    // ---- recording ----
    if (recording && run)
    {
        const juce::SpinLock::ScopedTryLockType sl (previewLock);
        if (sl.isLocked())
        {
            if (auto* w = activeWriter.load(); w != nullptr && numIn > 0 && in[0] != nullptr)
            {
                const float* chans[2] = { in[0], (numIn > 1 && in[1] != nullptr) ? in[1] : in[0] };
                w->write (chans, numSamples);
            }
            if (recordInstrumentTrackId >= 0)
                captureLiveNotes (liveMidi, startPos, numSamples);
        }
    }

    const juce::ScopedTryLock tl (project.lock);
    if (! tl.isLocked()) return;   // the UI is mid-edit: skip one block rather than block the audio thread

    if (master.getNumSamples() < numSamples) master.setSize (2, numSamples, false, false, true);
    juce::AudioBuffer<float> mix (master.getArrayOfWritePointers(), 2, 0, numSamples);
    mix.clear();

    if (! run && wasPlaying) allNotesOff();
    wasPlaying = run;

    if (run)
    {
        double pos = startPos;
        int done = 0;
        while (done < numSamples)
        {
            int n = numSamples - done;
            const double loopStartSec = project.loopStart * 60.0 / project.bpm;
            const double loopEndSec   = project.loopEnd   * 60.0 / project.bpm;
            const bool looping = project.loopEnabled && ! recording && loopEndSec > loopStartSec;

            if (looping && pos < loopEndSec && pos + n / sampleRate > loopEndSec)
                n = juce::jlimit (1, n, (int) std::ceil ((loopEndSec - pos) * sampleRate));

            juce::AudioBuffer<float> sub (master.getArrayOfWritePointers(), 2, done, n);
            renderBlock (sub, pos, n, true, liveMidi, done, true);
            if (metronomeOn) renderMetronome (sub, pos, n);

            pos += n / sampleRate;
            if (looping && pos >= loopEndSec - 1.0e-9)
            {
                pos = loopStartSec;
                allNotesOff();
            }
            done += n;
        }
        // only advance if nobody moved the playhead meanwhile
        double expected = startPos;
        positionSeconds.compare_exchange_strong (expected, pos);
    }
    else
    {
        renderBlock (mix, startPos, numSamples, false, liveMidi, 0, true);
    }

    // ---- master ----
    playheadSeconds = startPos;
    playheadRunning = run;
    processChain (project.masterFx, mix, numSamples);
    mix.applyGain (project.masterVolume);
    softClip (mix, numSamples);
    project.masterMeterL = juce::jmax (mix.getMagnitude (0, 0, numSamples), project.masterMeterL.load() * 0.85f);
    project.masterMeterR = juce::jmax (mix.getMagnitude (1, 0, numSamples), project.masterMeterR.load() * 0.85f);

    // ---- library preview (not affected by master volume or meters) ----
    {
        const juce::SpinLock::ScopedTryLockType sl (previewLock);
        if (sl.isLocked() && previewData != nullptr && previewPos.load() >= 0)
        {
            double p = previewPos.load();
            const double step = previewData->sampleRate / sampleRate;
            const int len = previewData->buffer.getNumSamples();
            for (int i = 0; i < numSamples; ++i, p += step)
            {
                const int i0 = (int) p;
                if (i0 + 1 >= len) { p = -1; break; }
                const float frac = (float) (p - i0);
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* src = previewData->buffer.getReadPointer (ch);
                    mix.addSample (ch, i, 0.8f * (src[i0] + frac * (src[i0 + 1] - src[i0])));
                }
            }
            previewPos = p;
        }
    }

    for (int ch = 0; ch < numOut; ++ch)
        if (out[ch] != nullptr)
            juce::FloatVectorOperations::copy (out[ch], mix.getReadPointer (juce::jmin (ch, 1)), numSamples);
}

// =============================================================================
//  Rendering
// =============================================================================
void AudioEngine::renderBlock (juce::AudioBuffer<float>& out, double startSec, int numSamples, bool transportRunning,
                               const juce::MidiBuffer& live, int liveOffset, bool includeLive)
{
    bool anySolo = false;
    for (auto& t : project.tracks)
        anySolo = anySolo || t->solo;

    playheadSeconds = startSec;
    playheadRunning = transportRunning;

    // Devices that listen to another track (vocoder) run after the tracks they
    // listen to, which keep a copy of their audio for them.
    auto listeners = [] (Track& t, std::vector<SidechainUser*>& found)
    {
        found.clear();
        auto check = [&found] (const FxPtr& f)
        {
            if (f != nullptr && f->processor != nullptr)
                if (auto* u = dynamic_cast<SidechainUser*> (f->processor.get()); u != nullptr && u->sidechainTrackId.load() != 0)
                    found.push_back (u);
        };
        check (t.instrument);
        for (auto& f : t.fx) check (f);
    };
    // reused between blocks so the audio thread doesn't allocate once warmed up
    static thread_local std::vector<Track*> order;
    static thread_local std::vector<int> neededSources;
    static thread_local std::vector<SidechainUser*> users;
    order.clear(); neededSources.clear();
    for (auto& tp : project.tracks)
    {
        auto tagOwner = [&tp] (const FxPtr& f)
        {
            if (f != nullptr && f->processor != nullptr)
                if (auto* u = dynamic_cast<SidechainUser*> (f->processor.get())) u->ownerTrackId = tp->id;
        };
        tagOwner (tp->instrument);
        for (auto& f : tp->fx) tagOwner (f);
        listeners (*tp, users);
        if (users.empty()) order.push_back (tp.get());
        for (auto* u : users) neededSources.push_back (u->sidechainTrackId.load());
    }
    for (auto& tp : project.tracks)
    {
        listeners (*tp, users);
        if (! users.empty()) order.push_back (tp.get());
    }

    for (auto* tptr : order)
    {
        auto& t = *tptr;
        listeners (t, users);
        for (auto* u : users)
        {
            u->sidechain = nullptr;
            for (auto& src : project.tracks)
                if (src->id == u->sidechainTrackId.load() && src.get() != &t && src->sidechainOut.getNumSamples() >= numSamples)
                    u->sidechain = &src->sidechainOut;
        }

        // ---- automation: read every lane at this point in the song ----
        const double beat0 = startSec * project.bpm / 60.0;
        const double beat1 = (startSec + numSamples / sampleRate) * project.bpm / 60.0;
        float vol0 = t.volume, vol1 = t.volume, pan0 = t.pan, pan1 = t.pan;
        bool volAuto = false, panAuto = false;
        for (auto& lane : t.automation)
        {
            if (lane.points.empty()) continue;
            const float v0 = lane.valueAt (beat0), v1 = lane.valueAt (beat1);
            switch (lane.kind)
            {
                case AutomationLane::Kind::Volume: vol0 = AutomationLane::valueToVolume (v0); vol1 = AutomationLane::valueToVolume (v1); volAuto = true; break;
                case AutomationLane::Kind::Pan:    pan0 = AutomationLane::valueToPan (v0);    pan1 = AutomationLane::valueToPan (v1);    panAuto = true; break;
                case AutomationLane::Kind::Effect:
                case AutomationLane::Kind::Instrument:
                    if (auto* param = t.getAutomationParameter (lane); param != nullptr && std::abs (param->getValue() - v0) > 1.0e-4f)
                        param->setValue (v0);
                    break;
            }
        }
        t.autoVolume = volAuto ? vol1 : -1.0f;
        t.autoPan = panAuto ? pan1 : -2.0f;

        renderTrack (t, startSec, numSamples, transportRunning, live, liveOffset, includeLive);
        processChain (t.fx, t.scratch, numSamples);   // insert effects run even when stopped, so tails ring out
        for (auto* u : users) u->sidechain = nullptr;

        if (std::find (neededSources.begin(), neededSources.end(), t.id) != neededSources.end())
        {
            if (t.sidechainOut.getNumSamples() < numSamples) t.sidechainOut.setSize (2, juce::jmax (numSamples, maxBlock), false, false, true);
            for (int ch = 0; ch < 2; ++ch) t.sidechainOut.copyFrom (ch, 0, t.scratch, ch, 0, numSamples);
        }

        const bool audible = ! t.mute && (! anySolo || t.solo);
        auto left  = [] (float v, float p) { return v * juce::jmin (1.0f, 1.0f - p); };
        auto right = [] (float v, float p) { return v * juce::jmin (1.0f, 1.0f + p); };
        t.scratch.applyGainRamp (0, 0, numSamples, left (vol0, pan0), left (vol1, pan1));
        t.scratch.applyGainRamp (1, 0, numSamples, right (vol0, pan0), right (vol1, pan1));

        const float ml = audible ? t.scratch.getMagnitude (0, 0, numSamples) : 0.0f;
        const float mr = audible ? t.scratch.getMagnitude (1, 0, numSamples) : 0.0f;
        t.meterL = juce::jmax (ml, t.meterL.load() * 0.85f);
        t.meterR = juce::jmax (mr, t.meterR.load() * 0.85f);

        if (audible)
            for (int ch = 0; ch < 2; ++ch)
                out.addFrom (ch, 0, t.scratch, ch, 0, numSamples);
    }
}

void AudioEngine::renderTrack (Track& t, double startSec, int numSamples, bool transportRunning,
                               const juce::MidiBuffer& live, int liveOffset, bool includeLive)
{
    if (t.scratch.getNumSamples() < numSamples) t.scratch.setSize (2, numSamples, false, false, true);
    t.scratch.clear();

    const double spb = 60.0 / project.bpm;   // seconds per beat
    const double endSec = startSec + numSamples / sampleRate;

    if (t.type == Track::Type::Audio)
    {
        if (! transportRunning) return;

        for (auto& c : t.clips)
        {
            const AudioData* data = c->getPlaybackAudio();
            if (data == nullptr) continue;
            const double cs = c->startBeat * spb;
            const double ce = cs + c->lengthSeconds;
            if (ce <= startSec || cs >= endSec) continue;

            const auto& src = data->buffer;
            const int srcLen = src.getNumSamples();
            const double ratio = data->sampleRate / sampleRate;
            const int fadeLen = (int) (0.003 * sampleRate);
            const int clipLenSamples = (int) (c->lengthSeconds * sampleRate);

            const int i0 = juce::jmax (0, (int) std::ceil ((cs - startSec) * sampleRate));
            const int i1 = juce::jmin (numSamples, (int) std::ceil ((ce - startSec) * sampleRate));

            for (int i = i0; i < i1; ++i)
            {
                const double tInClip = startSec + i / sampleRate - cs;
                const double sp = (c->offsetSeconds + tInClip) * data->sampleRate;
                const int s0 = (int) sp;
                if (s0 < 0 || s0 + 1 >= srcLen) continue;
                const float frac = (float) (sp - s0);

                // tiny fades at the clip edges avoid clicks
                const int posInClip = (int) (tInClip * sampleRate);
                float g = 1.0f;
                if (posInClip < fadeLen) g = (float) posInClip / (float) fadeLen;
                else if (clipLenSamples - posInClip < fadeLen) g = (float) (clipLenSamples - posInClip) / (float) fadeLen;
                // longer fades / crossfades set on the clip (equal-power curves)
                if (c->fadeInSeconds > 0 && tInClip < c->fadeInSeconds)
                    g *= (float) std::sin (juce::MathConstants<double>::halfPi * tInClip / c->fadeInSeconds);
                if (c->fadeOutSeconds > 0 && c->lengthSeconds - tInClip < c->fadeOutSeconds)
                    g *= (float) std::sin (juce::MathConstants<double>::halfPi * juce::jmax (0.0, c->lengthSeconds - tInClip) / c->fadeOutSeconds);

                for (int ch = 0; ch < 2; ++ch)
                {
                    auto* s = src.getReadPointer (ch);
                    t.scratch.addSample (ch, i, g * (s[s0] + frac * (s[s0 + 1] - s[s0])));
                }
            }
            juce::ignoreUnused (ratio);
        }
        return;
    }

    // ---- instrument track ----
    auto& midi = t.midiScratch;
    midi.clear();

    if (transportRunning)
    {
        auto toSample = [&] (double sec) { return juce::jlimit (0, numSamples - 1, (int) ((sec - startSec) * sampleRate)); };

        // note-offs first so a repeated note retriggers cleanly
        for (int pass = 0; pass < 2; ++pass)
            for (auto& c : t.clips)
            {
                const double cs = c->startBeat * spb;
                if (cs >= endSec || cs + c->lengthBeats * spb < startSec - 1.0) continue;

                for (auto& n : c->notes)
                {
                    if (n.start >= c->lengthBeats) continue;
                    const double on  = cs + n.start * spb;
                    const double off = cs + juce::jmin (n.start + n.length, c->lengthBeats) * spb;

                    if (pass == 0 && off >= startSec && off < endSec)
                        midi.addEvent (juce::MidiMessage::noteOff (1, n.pitch), toSample (off));
                    if (pass == 1 && on >= startSec && on < endSec)
                        midi.addEvent (juce::MidiMessage::noteOn (1, n.pitch, n.velocity), toSample (on));
                }
            }
    }

    if (includeLive && t.id == liveInputTrackId.load())
        for (const auto meta : live)
            if (meta.samplePosition >= liveOffset && meta.samplePosition < liveOffset + numSamples)
                midi.addEvent (meta.getMessage(), meta.samplePosition - liveOffset);

    if (t.instrument != nullptr && t.instrument->processor != nullptr)
    {
        if (t.pendingNotesOff.exchange (false))
        {
            midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
            midi.addEvent (juce::MidiMessage::allSoundOff (1), 0);
        }
        processFx (*t.instrument, t.scratch, midi, numSamples);
    }
    else if (t.synth)
    {
        t.synth->render (t.scratch, midi, 0, numSamples);
    }
}

void AudioEngine::renderMetronome (juce::AudioBuffer<float>& out, double startSec, int numSamples)
{
    const double bps = project.bpm / 60.0;
    double prevBeat = startSec * bps;

    for (int i = 0; i < numSamples; ++i)
    {
        const double beat = (startSec + i / sampleRate) * bps;
        if ((long long) std::floor (beat) != (long long) std::floor (prevBeat) || (i == 0 && std::abs (beat - std::round (beat)) < 1.0e-9))
        {
            metronomeSamplesLeft = (int) (0.03 * sampleRate);
            metronomePhase = 0;
            metronomeAccent = ((int) std::round (std::floor (beat + 1.0e-9))) % project.beatsPerBar == 0;
        }
        prevBeat = beat;

        if (metronomeSamplesLeft > 0)
        {
            const double f = metronomeAccent ? 1600.0 : 1100.0;
            const float env = (float) metronomeSamplesLeft / (float) (0.03 * sampleRate);
            const float v = 0.35f * env * env * (float) std::sin (metronomePhase);
            metronomePhase += juce::MathConstants<double>::twoPi * f / sampleRate;
            --metronomeSamplesLeft;
            out.addSample (0, i, v);
            out.addSample (1, i, v);
        }
    }
}

void AudioEngine::captureLiveNotes (const juce::MidiBuffer& live, double blockStartSec, int)
{
    const double bps = project.bpm / 60.0;
    for (const auto meta : live)
    {
        const auto m = meta.getMessage();
        const double beat = (blockStartSec + meta.samplePosition / sampleRate) * bps;
        const int p = m.getNoteNumber();
        if (m.isNoteOn())
        {
            heldNoteStart[(size_t) p] = beat;
            heldNoteVelocity[(size_t) p] = m.getFloatVelocity();
        }
        else if (m.isNoteOff() && heldNoteStart[(size_t) p] >= 0)
        {
            if (recordedNotes.size() < recordedNotes.capacity())
                recordedNotes.push_back ({ p, heldNoteStart[(size_t) p], beat - heldNoteStart[(size_t) p], heldNoteVelocity[(size_t) p] });
            heldNoteStart[(size_t) p] = -1.0;
        }
    }
}

// =============================================================================
//  Mixdown
// =============================================================================
bool AudioEngine::renderToFile (const juce::File& file, juce::String& error)
{
    stop();
    const juce::ScopedLock sl (project.lock);   // the live callback goes silent while we render

    const double totalSec = project.getEndBeat() * 60.0 / project.bpm + 4.0;   // room for reverb / delay tails
    if (project.getEndBeat() <= 0) { error = "The project is empty."; return false; }

    file.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
    if (stream == nullptr) { error = "Couldn't write to " + file.getFullPathName(); return false; }

    auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                   .withSampleRate (sampleRate)
                                                   .withNumChannels (2)
                                                   .withBitsPerSample (24));
    if (writer == nullptr) { error = "Couldn't create the WAV file."; return false; }

    auto forEachProcessor = [this] (auto&& fn)
    {
        for (auto& t : project.tracks)
        {
            if (t->instrument && t->instrument->processor) fn (*t->instrument->processor);
            for (auto& f : t->fx) if (f->processor) fn (*f->processor);
        }
        for (auto& f : project.masterFx) if (f->processor) fn (*f->processor);
    };
    forEachProcessor ([] (juce::AudioProcessor& p) { p.setNonRealtime (true); p.reset(); });

    allNotesOff();
    const int block = 512;
    juce::AudioBuffer<float> buf (2, block);
    juce::MidiBuffer none;
    for (auto& t : project.tracks)
        if (t->scratch.getNumSamples() < block) t->scratch.setSize (2, block);

    const auto total = (juce::int64) (totalSec * sampleRate);
    for (juce::int64 done = 0; done < total; done += block)
    {
        const int n = (int) std::min<juce::int64> (block, total - done);
        juce::AudioBuffer<float> sub (buf.getArrayOfWritePointers(), 2, 0, n);
        sub.clear();
        renderBlock (sub, (double) done / sampleRate, n, true, none, 0, false);
        playheadSeconds = (double) done / sampleRate;
        processChain (project.masterFx, sub, n);
        sub.applyGain (project.masterVolume);
        softClip (sub, n);
        writer->writeFromAudioSampleBuffer (sub, 0, n);
    }
    allNotesOff();
    forEachProcessor ([] (juce::AudioProcessor& p) { p.reset(); p.setNonRealtime (false); });
    return true;
}

} // namespace rs
