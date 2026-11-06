#include "../Source/Effects.h"
#include "../Source/Devices.h"
// Headless check: loads a project, renders it, verifies the audio and editing ops.
#include "../Source/AudioEngine.h"
#include "../Source/Pitch.h"
#include "../Source/MixcraftImport.h"
#include "../Source/AbletonImport.h"
#include "../Source/Drums.h"
#include "../Source/ClipOps.h"
#include "../Source/SynthFx.h"
#include "../Source/Export.h"
#include "../Source/MultiSampler.h"

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    // RenderTest --drums out.wav : a groove on every kit, one after the other
    if (argc >= 3 && juce::String (argv[1]) == "--drums")
    {
        rs::AudioCache cache;
        rs::Project project (cache);
        rs::AudioEngine engine (project);
        project.bpm = 96;
        // RenderTest --drums out.wav [kit.rskit ...]: kit files given replace the built-in kits
        juce::Array<juce::File> kitFiles;
        for (int i = 3; i < argc; ++i) kitFiles.add (juce::File (argv[i]));
        const int numKits = kitFiles.isEmpty() ? rs::DrumMachine::getKitNames().size() : kitFiles.size();
        for (int k = 0; k < numKits; ++k)
        {
            auto t = project.addTrack (rs::Track::Type::Instrument);
            juce::String e;
            auto slot = engine.createFx ("builtin:drums", nullptr, e);
            auto* dm = dynamic_cast<rs::DrumMachine*> (slot->processor.get());
            if (k < kitFiles.size()) { std::cout << "kit file " << kitFiles[k].getFileName() << " loaded " << dm->loadKitFile (kitFiles[k]) << " -> " << dm->getKitLabel() << "\n"; }
            else dm->loadKit (k);
            project.setInstrument (t->id, slot);
            auto c = project.addMidiClip (t->id, k * 8.0, 8.0);
            for (int bar = 0; bar < 2; ++bar)
            {
                const double b = bar * 4.0;
                auto add = [&] (int note, double at, float vel) { c->notes.push_back ({ note, b + at, 0.2, vel }); };
                add (36, 0, 1.0f); add (36, 1.75, 0.8f); add (36, 2.5, 0.9f);
                add (38, 1, 1.0f); add (38, 3, 1.0f); add (39, 3, 0.7f);
                for (int h = 0; h < 8; ++h) add (h == 7 ? 46 : 42, h * 0.5, h % 2 ? 0.6f : 0.9f);
                if (bar == 1) { add (48, 3.0, 0.9f); add (45, 3.25, 0.9f); add (41, 3.5, 0.9f); add (49, 0, 0.9f); }
                else { add (56, 2.0, 0.6f); add (37, 2.75, 0.7f); add (70, 1.5, 0.5f); }
            }
        }
        juce::String err;
        engine.renderToFile (juce::File (argv[2]), err);
        std::unique_ptr<juce::AudioFormatReader> rd (cache.formats.createReaderFor (juce::File (argv[2])));
        juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
        rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
        std::cout << "drums: " << rd->lengthInSamples / rd->sampleRate << " s, peak " << b.getMagnitude (0, b.getNumSamples())
                  << ", rms " << b.getRMSLevel (0, 0, b.getNumSamples()) << "\n";
        return 0;
    }

    // RenderTest --mixcraft project.mx9 out.wav : import a Mixcraft project and render it



    if (argc >= 2 && juce::String (argv[1]) == "--drumshape")
    {
        const double sr = 48000.0;
        auto db = [] (float v) { return juce::Decibels::gainToDecibels (v, -150.0f); };
        auto hit = [sr] (rs::DrumMachine& d, std::function<void (rs::DrumMachine&)> set)
        {
            if (set) set (d);
            d.setPlayConfigDetails (0, 2, sr, 512);
            d.prepareToPlay (sr, 512);
            juce::AudioBuffer<float> out (2, (int) sr);
            out.clear();
            for (int pos = 0; pos < out.getNumSamples(); pos += 512)
            {
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, juce::jmin (512, out.getNumSamples() - pos));
                juce::MidiBuffer m;
                if (pos == 0) m.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) 120), 0);
                d.processBlock (view, m);
            }
            return out;
        };
        auto set = [] (rs::DrumMachine& d, const juce::String& id, float v) { auto* p = d.state.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
        auto report = [&] (const char* name, juce::AudioBuffer<float>& b)
        {
            // length to -40 dB, brightness (zero crossings in the first 30 ms), early vs body level
            const float pk = b.getMagnitude (0, 0, b.getNumSamples());
            int last = 0; for (int i = 0; i < b.getNumSamples(); ++i) if (std::abs (b.getSample (0, i)) > pk * 0.01f) last = i;
            int zc = 0; for (int i = 1; i < (int) (0.03 * sr); ++i) if ((b.getSample (0, i - 1) < 0) != (b.getSample (0, i) < 0)) ++zc;
            std::cout << "  " << juce::String (name).paddedRight (' ', 26) << " peak " << juce::String (db (pk), 1) << " dB, length " << juce::String (last / sr * 1000.0, 0)
                      << " ms, first 5 ms " << juce::String (db (b.getMagnitude (0, 0, 240)), 1) << " dB, 30-80 ms " << juce::String (db (b.getMagnitude (0, 1440, 2400)), 1)
                      << " dB, zero crossings/30ms " << zc << "\n";
        };
        std::cout << "808 kick:\n";
        { rs::DrumMachine d; auto b = hit (d, {}); report ("neutral", b); }
        { rs::DrumMachine d; auto b = hit (d, [&] (rs::DrumMachine& x) { set (x, "dec0", 25); }); report ("decay 25%", b); }
        { rs::DrumMachine d; auto b = hit (d, [&] (rs::DrumMachine& x) { set (x, "tone0", -70); }); report ("tone -70 (darker)", b); }
        { rs::DrumMachine d; auto b = hit (d, [&] (rs::DrumMachine& x) { set (x, "tone0", 70); }); report ("tone +70 (brighter)", b); }
        { rs::DrumMachine d; auto b = hit (d, [&] (rs::DrumMachine& x) { set (x, "att0", -80); }); report ("attack -80 (soft)", b); }
        { rs::DrumMachine d; auto b = hit (d, [&] (rs::DrumMachine& x) { set (x, "att0", 80); }); report ("attack +80 (sharp)", b); }
        { rs::DrumMachine d; auto b = hit (d, [&] (rs::DrumMachine& x) { set (x, "drive0", 80); }); report ("drive 80", b); }
        { rs::DrumMachine d; auto b = hit (d, [&] (rs::DrumMachine& x) { set (x, "c10", 95); juce::MemoryBlock mb; x.getStateInformation (mb); x.setStateInformation (mb.getData(), (int) mb.getSize()); }); report ("sweep 95 (resynth)", b); }
        { rs::DrumMachine d; auto b = hit (d, [&] (rs::DrumMachine& x) { set (x, "c20", 100); juce::MemoryBlock mb; x.getStateInformation (mb); x.setStateInformation (mb.getData(), (int) mb.getSize()); }); report ("click 100 (resynth)", b); }
        return 0;
    }
    if (argc >= 3 && juce::String (argv[1]) == "--synth")
    {
        // every RENASYNTH sound: a C major chord for 1 s, then released
        const double sr = 48000.0;
        const juce::File outDir (argv[2]);
        outDir.createDirectory();
        juce::AudioBuffer<float> all (2, (int) (sr * 1.8) * (int) rs::getSynthPatches().size());
        all.clear();
        for (int i = 0; i < (int) rs::getSynthPatches().size(); ++i)
        {
            auto fx = rs::createBuiltinFx ("synth");
            rs::loadSynthPatch (fx.get(), i);
            fx->setPlayConfigDetails (2, 2, sr, 512);
            fx->prepareToPlay (sr, 512);
            const int len = (int) (sr * 1.8);
            juce::AudioBuffer<float> b (2, len);
            for (int pos = 0; pos < len; pos += 512)
            {
                const int n = juce::jmin (512, len - pos);
                juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), 2, pos, n);
                juce::MidiBuffer m;
                if (pos == 0) for (int note : { 48, 60, 64, 67 }) m.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
                if (pos <= (int) sr && pos + n > (int) sr) for (int note : { 48, 60, 64, 67 }) m.addEvent (juce::MidiMessage::noteOff (1, note), (int) sr - pos);
                fx->processBlock (view, m);
            }
            bool finite = true;
            for (int c = 0; c < 2; ++c) for (int k = 0; k < len; ++k) finite = finite && std::isfinite (b.getSample (c, k));
            const float peak = b.getMagnitude (0, len), rmsHeld = b.getRMSLevel (0, (int) (sr * 0.2), (int) (sr * 0.6));
            const float tail = b.getMagnitude (0, len - (int) (sr * 0.05), (int) (sr * 0.05));
            std::cout << juce::String (rs::getSynthPatchName (fx.get())).paddedRight (' ', 14) << " peak " << juce::String (juce::Decibels::gainToDecibels (peak), 1)
                      << " dB, held " << juce::String (juce::Decibels::gainToDecibels (rmsHeld), 1) << " dB rms, end " << juce::String (juce::Decibels::gainToDecibels (tail, -120.0f), 1)
                      << " dB" << (finite ? "" : "  NOT FINITE") << "\n";
            for (int c = 0; c < 2; ++c) all.copyFrom (c, i * len, b, c, 0, len);

            if (i == 1 || i == 8 || i == 12 || i == 13)   // a picture of the editor with the sound playing
            {
                std::unique_ptr<juce::AudioProcessorEditor> ed (fx->createEditor());
                juce::MidiBuffer m;
                for (int note : { 48, 60, 64, 67 }) m.addEvent (juce::MidiMessage::noteOn (1, note, 0.8f), 0);
                juce::AudioBuffer<float> blk (2, 512);
                fx->processBlock (blk, m);
                for (int f = 0; f < 30; ++f) { juce::MidiBuffer none; for (int r = 0; r < 4; ++r) fx->processBlock (blk, none); ed->createComponentSnapshot (ed->getLocalBounds()); juce::Thread::sleep (25); }
                auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
                juce::FileOutputStream os (outDir.getChildFile ("synth" + juce::String (i) + ".png"));
                os.setPosition (0); os.truncate();
                juce::PNGImageFormat().writeImageToStream (img, os);
            }
        }
        juce::String err;
        rs::exporter::write (all, sr, {}, {}, outDir.getChildFile ("synth-sounds.wav"), err);
        return 0;
    }

    if (argc >= 3 && juce::String (argv[1]) == "--export")
    {
        // a 2 s, 440 Hz tone written in every format with the song's details and a cover
        const double sr = 48000.0;
        const juce::File dir (argv[2]);
        dir.createDirectory();
        juce::AudioBuffer<float> tone (2, (int) sr * 2);
        for (int i = 0; i < tone.getNumSamples(); ++i) { const float v = 0.5f * (float) std::sin (i * 2.0 * juce::MathConstants<double>::pi * 440.0 / sr); tone.setSample (0, i, v); tone.setSample (1, i, v); }
        juce::Image cover (juce::Image::RGB, 600, 600, true);
        { juce::Graphics g (cover); g.fillAll (juce::Colour (0xffd8a96e)); g.setColour (juce::Colours::black); g.fillEllipse (150, 150, 300, 300); }
        auto coverFile = dir.getChildFile ("cover.png");
        { coverFile.deleteFile(); juce::FileOutputStream os (coverFile); juce::PNGImageFormat().writeImageToStream (cover, os); }
        rs::exporter::SongInfo info { "Golden Hour", "Rena", "Sandstone", "D. Vaca, A. Writer", "D. Vaca", coverFile };
        struct Case { rs::exporter::Format f; int bits; int kbps; double rate; const char* name; };
        for (auto c : { Case { rs::exporter::Format::Wav, 24, 0, 0, "wav24" }, Case { rs::exporter::Format::Wav, 32, 0, 44100, "wav32-441" },
                        Case { rs::exporter::Format::Aiff, 16, 0, 0, "aiff16" }, Case { rs::exporter::Format::Flac, 24, 0, 96000, "flac24-96" },
                        Case { rs::exporter::Format::Ogg, 16, 192, 0, "ogg192" } })
        {
            rs::exporter::Settings st; st.format = c.f; st.bitDepth = c.bits; st.oggKbps = c.kbps; st.sampleRate = c.rate;
            auto f = dir.getChildFile (juce::String (c.name) + rs::exporter::extension (c.f));
            juce::String err;
            const bool ok = rs::exporter::write (tone, sr, st, info, f, err);
            std::cout << c.name << ": " << (ok ? "ok" : err) << " (" << rs::exporter::describe (st) << ") " << f.getSize() << " bytes\n";
        }
        return 0;
    }

    if (argc >= 2 && juce::String (argv[1]) == "--clipops")
    {
        rs::AudioCache cache;
        rs::Project project (cache);
        project.bpm = 120;   // 0.5 s per beat
        // a ramp file: sample i = i / n, so reversing is easy to check
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rs-clipops");
        dir.createDirectory();
        auto rampFile = dir.getChildFile ("ramp.wav");
        {
            juce::AudioBuffer<float> b (2, 48000);
            for (int i = 0; i < 48000; ++i) { b.setSample (0, i, i / 48000.0f); b.setSample (1, i, i / 48000.0f); }
            rampFile.deleteFile();
            juce::WavAudioFormat wav;
            std::unique_ptr<juce::OutputStream> os (rampFile.createOutputStream());
            auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (48000).withNumChannels (2).withBitsPerSample (24));
            w->writeFromAudioSampleBuffer (b, 0, 48000);
        }
        auto at = project.addTrack (rs::Track::Type::Audio);
        auto a1 = project.addAudioClip (at->id, rampFile, 0);           // 1 s = 2 beats
        auto a2 = project.addAudioClip (at->id, rampFile, 4);           // beats 4..6
        a1->offsetSeconds = 0.25; a1->lengthSeconds = 0.5;              // middle half of the ramp
        rs::clipops::reverse (project, { a1->id });
        auto& rb = a1->audio->buffer;
        std::cout << "reverse audio: first " << rb.getSample (0, 0) << " (want ~0.75), last " << rb.getSample (0, rb.getNumSamples() - 1)
                  << " (want ~0.25), length " << a1->lengthSeconds << " s, name '" << a1->name << "'\n";

        auto it = project.addTrack (rs::Track::Type::Instrument);
        auto m1 = project.addMidiClip (it->id, 0, 4);
        m1->notes = { { 60, 0.0, 1.0, 0.8f }, { 64, 2.0, 0.5, 0.8f }, { 67, -1.0, 0.5, 0.8f } };   // the last one is trimmed away
        auto m2 = project.addMidiClip (it->id, 8, 4);
        m2->notes = { { 72, 1.0, 1.0, 0.8f } };
        rs::clipops::reverse (project, { m1->id });
        std::cout << "reverse midi:";
        for (auto& n : m1->notes) std::cout << " " << n.pitch << "@" << n.start << "+" << n.length;
        std::cout << "  (want 64@1.5+0.5 60@3+1)\n";

        const auto joined = rs::clipops::join (project, { m1->id, m2->id, a1->id, a2->id });
        std::cout << "join: " << joined.size() << " clips made; instrument track has " << it->clips.size() << " clip(s), audio track " << at->clips.size() << "\n";
        for (auto& c : it->clips)
        {
            std::cout << "  midi clip " << c->startBeat << "+" << c->lengthBeats << ":";
            for (auto& n : c->notes) std::cout << " " << n.pitch << "@" << n.start;
            std::cout << "  (want 0+12: 64@1.5 60@3 72@9)\n";
        }
        for (auto& c : at->clips)
        {
            auto& b = c->audio->buffer;
            const int sr = (int) c->audio->sampleRate;
            std::cout << "  audio clip " << c->startBeat << " len " << c->lengthSeconds << " s (want 3): at 0.1 s " << b.getSample (0, sr / 10)
                      << " (reversed ramp, want ~0.65), at 1.5 s " << b.getSample (0, sr * 3 / 2) << " (gap, want 0), at 2.5 s " << b.getSample (0, sr * 5 / 2) << " (want ~0.5)\n";
        }

        auto cb = rs::clipops::copy (project, { it->clips.front()->id });
        const auto pasted = rs::clipops::paste (project, cb, 16, it->id);
        std::cout << "paste: " << pasted.size() << " clip at " << project.findClip (pasted.front())->startBeat << " with "
                  << project.findClip (pasted.front())->notes.size() << " notes\n";
        dir.deleteRecursively();
        for (auto& c : at->clips) c->audio->file.deleteFile();
        return 0;
    }

    if (argc >= 2 && juce::String (argv[1]) == "--inputs")
    {
        // two armed audio tracks on different inputs record at once, each from its own input
        struct FakeDevice : juce::AudioIODevice
        {
            FakeDevice() : juce::AudioIODevice ("fake", "fake") {}
            juce::StringArray getOutputChannelNames() override { return { "Out 1", "Out 2" }; }
            juce::StringArray getInputChannelNames() override  { return { "In 1", "In 2", "In 3", "In 4" }; }
            juce::Array<double> getAvailableSampleRates() override { return { 48000.0 }; }
            juce::Array<int> getAvailableBufferSizes() override { return { 512 }; }
            int getDefaultBufferSize() override { return 512; }
            juce::String open (const juce::BigInteger&, const juce::BigInteger&, double, int) override { return {}; }
            void close() override {}
            bool isOpen() override { return true; }
            void start (juce::AudioIODeviceCallback*) override {}
            void stop() override {}
            bool isPlaying() override { return true; }
            juce::String getLastError() override { return {}; }
            int getCurrentBufferSizeSamples() override { return 512; }
            double getCurrentSampleRate() override { return 48000.0; }
            int getCurrentBitDepth() override { return 24; }
            juce::BigInteger getActiveOutputChannels() const override { juce::BigInteger b; b.setRange (0, 2, true); return b; }
            juce::BigInteger getActiveInputChannels() const override  { juce::BigInteger b; b.setRange (0, 4, true); return b; }
            int getOutputLatencyInSamples() override { return 0; }
            int getInputLatencyInSamples() override { return 0; }
        } dev;

        rs::AudioCache cache;
        rs::Project project (cache);
        rs::AudioEngine engine (project);
        engine.deviceManager.removeAudioCallback (&engine);
        engine.deviceManager.closeAudioDevice();
        engine.audioDeviceAboutToStart (&dev);
        auto a = project.addTrack (rs::Track::Type::Audio);
        auto b = project.addTrack (rs::Track::Type::Audio);
        auto c = project.addTrack (rs::Track::Type::Audio);
        a->audioInput = 2; a->audioInputStereo = true;  a->armed = true;   // In 3+4
        b->audioInput = 1; b->audioInputStereo = false; b->armed = true;   // In 2
        c->audioInput = -1; c->armed = true;                                // no input: records nothing
        engine.startRecording();
        std::vector<float> chans[4];
        const float* in[4];
        for (int ch = 0; ch < 4; ++ch) { chans[ch].assign (512, 0.1f * (ch + 1)); in[ch] = chans[ch].data(); }
        juce::AudioBuffer<float> out (2, 512);
        for (int i = 0; i < 200; ++i)
            engine.audioDeviceIOCallbackWithContext (in, 4, out.getArrayOfWritePointers(), 2, 512, {});
        engine.stop();
        juce::Thread::sleep (300);
        // play just track 2's clip: only its sound (equal left and right) comes out
        if (! b->clips.empty())
        {
            engine.playClip (b->clips[0]->id);
            float l = 0, r = 0;
            for (int i = 0; i < 20; ++i)
            {
                engine.audioDeviceIOCallbackWithContext (in, 4, out.getArrayOfWritePointers(), 2, 512, {});
                l = out.getSample (0, 256); r = out.getSample (1, 256);
            }
            std::cout << "play clip: playing id " << engine.getPlayingClipId() << ", out L " << l << " R " << r << " (want equal: only track 2)\n";
            engine.stop();
        }

        // count-in: one bar of clicks before recording starts
        {
            project.countIn = 101;
            a->armed = true; b->armed = c->armed = false;
            const double before = engine.getPositionBeats();
            engine.startRecording();
            int blocks = 0; float clickPeak = 0;
            while (engine.isCountingIn() && blocks < 1000)
            {
                engine.audioDeviceIOCallbackWithContext (in, 4, out.getArrayOfWritePointers(), 2, 512, {});
                clickPeak = juce::jmax (clickPeak, out.getMagnitude (0, 0, 512));
                ++blocks;
            }
            std::cout << "count-in: " << blocks * 512 / 48000.0 << " s of clicks (want 2 s at 120 bpm), click level " << clickPeak
                      << ", recording now " << (engine.isRecording() ? "yes" : "no") << ", playhead moved " << engine.getPositionBeats() - before << " beats\n";
            for (int i = 0; i < 40; ++i) engine.audioDeviceIOCallbackWithContext (in, 4, out.getArrayOfWritePointers(), 2, 512, {});
            engine.stop();
            project.countIn = 0;
            std::cout << "after count-in take: track 1 has " << a->clips.size() << " clips\n";
            if (a->clips.size() > 1) { a->clips.back()->audio->file.deleteFile(); project.removeClip (a->clips.back()->id); }
        }

        // stems: rendering one track gives only that track
        {
            juce::AudioBuffer<float> all, one;
            juce::String err;
            engine.renderMix (all, 0, true, {}, err);
            engine.renderMix (one, b->id, false, {}, err);
            std::cout << "stem of track 2: L " << one.getSample (0, 4800) << " R " << one.getSample (1, 4800) << " (want equal), full mix L " << all.getSample (0, 4800)
                      << " R " << all.getSample (1, 4800) << " (want different), " << one.getNumSamples() / 48000.0 << " s\n";
        }

        // MIDI recording shows up while it records
        auto inst = project.addTrack (rs::Track::Type::Instrument);
        a->armed = b->armed = c->armed = false;
        inst->armed = true;
        engine.liveInputTrackId = inst->id;
        engine.startRecording();
        engine.keyboardState.noteOn (1, 60, 0.8f);
        for (int i = 0; i < 50; ++i) engine.audioDeviceIOCallbackWithContext (in, 4, out.getArrayOfWritePointers(), 2, 512, {});
        {
            rs::AudioEngine::RecordingPreview pv;
            engine.getRecordingPreview (pv);
            std::cout << "recording preview: " << pv.notes.size() << " note(s) so far";
            if (! pv.notes.empty()) std::cout << ", pitch " << pv.notes[0].pitch << " held for " << pv.notes[0].length << " beats";
            std::cout << "\n";
        }
        engine.keyboardState.noteOff (1, 60, 0.0f);
        for (int i = 0; i < 10; ++i) engine.audioDeviceIOCallbackWithContext (in, 4, out.getArrayOfWritePointers(), 2, 512, {});
        engine.stop();
        std::cout << "after recording: instrument track has " << inst->clips.size() << " clip(s) with " << (inst->clips.empty() ? 0 : (int) inst->clips[0]->notes.size()) << " note(s)\n";

        for (auto& t : { a, b, c })
        {
            std::cout << "track " << t->id << " (" << rs::AudioEngine::describeInput (*t) << "): " << t->clips.size() << " clip(s)";
            if (! t->clips.empty() && t->clips[0]->audio != nullptr)
            {
                auto& buf = t->clips[0]->audio->buffer;
                const int mid = buf.getNumSamples() / 2;
                std::cout << ", L " << buf.getSample (0, mid) << " R " << buf.getSample (1, mid) << ", " << buf.getNumSamples() << " samples";
                t->clips[0]->audio->file.deleteFile();
            }
            std::cout << "\n";
        }
        return 0;
    }

    if (argc >= 3 && juce::String (argv[1]) == "--msamp")
    {
        // the Multi Sampler on a real folder (argv[2]), then on a made-up SFZ
        juce::ScopedJuceInitialiser_GUI gui;
        const double sr = 48000; const int block = 512;
        bool ok = true;
        auto freqOf = [&] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            int crossings = 0; double first = -1, last = 0;
            juce::AudioBuffer<float> m (1, to - from);
            for (int i = from; i < to; ++i) m.setSample (0, i - from, b.getSample (0, i) + b.getSample (1, i));
            // a gentle low-pass so harmonics don't add crossings
            float lp = 0; for (int i = 0; i < m.getNumSamples(); ++i) { lp += 0.08f * (m.getSample (0, i) - lp); m.setSample (0, i, lp); }
            for (int i = 1; i < m.getNumSamples(); ++i)
                if (m.getSample (0, i - 1) <= 0 && m.getSample (0, i) > 0)
                {
                    const double t = i - 1 + m.getSample (0, i - 1) / (m.getSample (0, i - 1) - m.getSample (0, i));
                    if (first < 0) first = t; else ++crossings;
                    last = t;
                }
            return crossings > 0 ? sr * crossings / (last - first) : 0.0;
        };
        auto fx = rs::createMultiSampler();
        fx->setPlayConfigDetails (2, 2, sr, block);
        fx->prepareToPlay (sr, block);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        juce::String loadErr;
        if (! rs::multiSamplerLoadNow (fx.get(), juce::File (argv[2]), loadErr)) { std::cout << "load failed: " << loadErr << "\n"; return 1; }
        auto in = rs::multiSamplerInstrument (fx.get());
        if (in == nullptr) { std::cout << "load failed\n"; return 1; }
        std::cout << "loaded \"" << in->name << "\" in " << (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0 << " s: " << in->samples << " sounds, "
                  << in->zones.size() << " zones, " << in->velocityLayers << " layers, " << in->roundRobins << " round robins, " << in->bytes / (1024 * 1024) << " MB, octave correction " << in->octaveCorrection << "\n";
        for (int a = 0; a < in->articulations.size(); ++a)
        {
            int zones = 0, lo = 127, hi = 0, rel = 0;
            for (auto& z : in->zones) if (z.articulation == a) { ++zones; lo = juce::jmin (lo, z.loKey); hi = juce::jmax (hi, z.hiKey); rel += z.release; }
            std::cout << "  artic " << a << " \"" << in->articulations[a] << "\" zones " << zones << " (release " << rel << ") keys " << lo << "-" << hi
                      << " keyswitch " << (a < (int) in->keyswitches.size() ? in->keyswitches[(size_t) a] : -1) << "\n";
        }
        auto play = [&] (std::vector<std::tuple<double, int, float, bool>> events, double seconds)
        {
            juce::AudioBuffer<float> out (2, (int) (sr * seconds));
            for (int pos = 0; pos < out.getNumSamples(); pos += block)
            {
                const int len = juce::jmin (block, out.getNumSamples() - pos);
                juce::MidiBuffer m;
                for (auto& [t, note, vel, on] : events)
                {
                    const int at = (int) (t * sr);
                    if (at >= pos && at < pos + len) m.addEvent (on ? juce::MidiMessage::noteOn (1, note, vel) : juce::MidiMessage::noteOff (1, note), at - pos);
                }
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, len);
                fx->processBlock (view, m);
            }
            return out;
        };
        auto* rsFx = dynamic_cast<rs::BuiltinFx*> (fx.get());
        auto zoneNow = [&] { for (auto& v : rsFx->getParameters()) juce::ignoreUnused (v); return 0; };
        juce::ignoreUnused (zoneNow);
        // 1) A3 (57) mf: the right pitch
        {
            auto out = play ({ { 0.0, 57, 0.6f, true }, { 1.0, 57, 0.6f, false } }, 1.6);
            auto pitchOf = [&] (const juce::AudioBuffer<float>& b) { juce::AudioBuffer<float> seg (2, (int) (sr * 0.9)); for (int c = 0; c < 2; ++c) seg.copyFrom (c, 0, b, c, 0, seg.getNumSamples()); return rs::msamp::detectPitchHz (seg, sr); };
            const double f = pitchOf (out);
            std::cout << "A3 plays at " << f << " Hz (220 expected), peak " << out.getMagnitude (0, out.getNumSamples()) << "\n";
            ok = ok && std::abs (1200 * std::log2 (f / 220.0)) < 30;
            // between two sampled notes: a note one semitone up is transposed
            auto out2 = play ({ { 0.0, 58, 0.6f, true }, { 1.0, 58, 0.6f, false } }, 1.6);
            const double f2 = pitchOf (out2);
            std::cout << "A#3 plays at " << f2 << " Hz (233.1 expected)\n";
            ok = ok && std::abs (1200 * std::log2 (f2 / 233.08)) < 30;
        }
        // 2) round robins: the same note again uses the next take; soft and loud use different layers
        {
            auto startNote = [&] (int note, float vel)
            {
                juce::AudioBuffer<float> b (2, block);
                juce::MidiBuffer none;
                for (int r = 0; r < 300; ++r) { juce::MidiBuffer m; if (r == 0) m.addEvent (juce::MidiMessage::allSoundOff (1), 0); fx->processBlock (b, r == 0 ? m : none); }
                juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, note, vel), 0);
                fx->processBlock (b, m);
                auto z = rs::multiSamplerSoundingZones (fx.get());
                juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                fx->processBlock (b, off);
                return z.empty() ? juce::String ("(none)") : in->zones[(size_t) z.back()].name;
            };
            juce::StringArray takes;
            for (int k = 0; k < 4; ++k) takes.add (startNote (57, 0.6f));
            std::cout << "A3 four times: " << takes.joinIntoString (" | ") << "\n";
            ok = ok && takes[0] != takes[1] && takes[0] == takes[2];
            const auto soft = startNote (57, 0.15f), loud = startNote (57, 1.0f);
            std::cout << "soft: " << soft << "   loud: " << loud << "\n";
            ok = ok && soft != loud;
        }
        // 3) release sounds and keyswitches: play, let go, a release sound follows
        {
            auto out = play ({ { 0.0, 57, 0.7f, true }, { 0.5, 57, 0.7f, false } }, 1.5);
            const float after = out.getRMSLevel (0, (int) (sr * 0.7), (int) (sr * 0.5));
            std::cout << "after letting go: " << juce::Decibels::gainToDecibels (after) << " dB rms (release sounds)\n";
            for (int a = 0; a < in->articulations.size(); ++a)
            {
                const int ks = in->keyswitches[(size_t) a];
                if (ks < 0) continue;
                auto o = play ({ { 0.0, ks, 0.8f, true }, { 0.01, ks, 0.8f, false }, { 0.05, 57, 0.7f, true }, { 0.8, 57, 0.7f, false } }, 1.2);
                std::cout << "  keyswitch " << ks << " -> artic " << rsFx->param ("artic") << " (" << in->articulations[a] << "): " << juce::Decibels::gainToDecibels (o.getRMSLevel (0, 0, o.getNumSamples())) << " dB\n";
                ok = ok && (int) rsFx->param ("artic") == a;
            }
        }
        // 4) a little phrase for listening: sustain melody, then spiccato, then pizzicato
        {
            auto setArt = [&] (const juce::String& name) { for (int a = 0; a < in->articulations.size(); ++a) if (in->articulations[a].startsWithIgnoreCase (name)) { auto* p = rsFx->state.getParameter ("artic"); p->setValueNotifyingHost (p->convertTo0to1 ((float) a)); return; } };
            juce::AudioBuffer<float> all (2, (int) (sr * 14)); all.clear();
            int at = 0;
            auto append = [&] (const juce::AudioBuffer<float>& b) { for (int c = 0; c < 2; ++c) all.copyFrom (c, at, b, c, 0, juce::jmin (b.getNumSamples(), all.getNumSamples() - at)); at += b.getNumSamples(); };
            setArt ("Sustain");
            const int mel[] { 62, 66, 69, 74, 73, 69, 71, 66 };
            std::vector<std::tuple<double, int, float, bool>> ev;
            for (int k = 0; k < 8; ++k) { ev.push_back ({ k * 0.55, mel[k], 0.45f + 0.06f * k, true }); ev.push_back ({ k * 0.55 + 0.5, mel[k], 0.5f, false }); }
            append (play (ev, 5.2));
            setArt ("Spicc");
            ev.clear();
            for (int k = 0; k < 16; ++k) { const int nn = mel[k % 8]; ev.push_back ({ k * 0.17, nn, 0.7f, true }); ev.push_back ({ k * 0.17 + 0.12, nn, 0.7f, false }); }
            append (play (ev, 3.6));
            setArt ("Pizz");
            ev.clear();
            for (int k = 0; k < 8; ++k) { ev.push_back ({ k * 0.3, mel[k] - 5, 0.8f, true }); ev.push_back ({ k * 0.3 + 0.2, mel[k] - 5, 0.8f, false }); }
            append (play (ev, 3.6));
            bool finite = true;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < all.getNumSamples(); ++i) finite = finite && std::isfinite (all.getSample (c, i));
            const float pk = all.getMagnitude (0, all.getNumSamples());
            if (pk > 0) all.applyGain (0.7f / pk);
            juce::String err;
            if (argc > 3) rs::exporter::write (all, sr, {}, {}, juce::File (argv[3]), err);
            std::cout << "phrase rendered, peak before normalising " << pk << (finite ? "" : " NON-FINITE") << "\n";
            ok = ok && finite;
        }
        if (argc > 4)   // a picture of the panel with notes held
        {
            if (auto* p = rsFx->state.getParameter ("artic")) p->setValueNotifyingHost (p->convertTo0to1 (0.0f));
            std::unique_ptr<juce::AudioProcessorEditor> ed (fx->createEditor());
            juce::MidiBuffer m;
            for (auto [nn, vv] : { std::pair { 62, 0.9f }, { 69, 0.5f }, { 74, 0.2f } }) m.addEvent (juce::MidiMessage::noteOn (1, nn, vv), 0);
            juce::AudioBuffer<float> b (2, block);
            fx->processBlock (b, m);
            for (int f = 0; f < 10; ++f) { juce::MidiBuffer none; fx->processBlock (b, none); ed->createComponentSnapshot (ed->getLocalBounds()); juce::Thread::sleep (40); }
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            juce::File pf (argv[4]); pf.deleteFile();
            juce::FileOutputStream os (pf);
            juce::PNGImageFormat().writeImageToStream (img, os);
        }
        // 5) SFZ: keyswitched articulations, round robins (seq), a release trigger, a loop, #define
        {
            auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("rs-sfz-test");
            dir.deleteRecursively();
            dir.getChildFile ("samples").createDirectory();
            auto tone = [&] (const juce::String& name, double hz, double secs, float amp)
            {
                juce::AudioBuffer<float> b (1, (int) (sr * secs));
                for (int i = 0; i < b.getNumSamples(); ++i) b.setSample (0, i, amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr));
                juce::WavAudioFormat wav;
                auto f = dir.getChildFile ("samples").getChildFile (name);
                std::unique_ptr<juce::OutputStream> os (f.createOutputStream());
                std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.release(), sr, 1, 24, {}, 0));
                w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
            };
            tone ("c4_a.wav", 261.63, 1.0, 0.5f); tone ("c4_b.wav", 261.63, 1.0, 0.25f);
            tone ("g4.wav", 392.0, 1.0, 0.5f); tone ("stac_c4.wav", 261.63, 0.15, 0.5f); tone ("rel.wav", 1000.0, 0.2, 0.3f);
            dir.getChildFile ("test.sfz").replaceWithText (
                "// a test instrument\n#define $LOWKEY c3\n<control> default_path=samples/\n"
                "<global> ampeg_release=0.25\n"
                "<group> sw_last=c1 sw_label=Long\n"
                "<region> sample=c4_a.wav lokey=$LOWKEY hikey=e4 pitch_keycenter=c4 seq_length=2 seq_position=1 loop_mode=loop_continuous loop_start=1000 loop_end=10175\n"
                "<region> sample=c4_b.wav lokey=$LOWKEY hikey=e4 pitch_keycenter=c4 seq_length=2 seq_position=2\n"
                "<region> sample=g4.wav lokey=f4 hikey=b5 pitch_keycenter=67\n"
                "<region> sample=rel.wav lokey=c3 hikey=b5 trigger=release volume=-6\n"
                "<group> sw_last=d1 sw_label=Short\n"
                "<region> sample=stac_c4.wav lokey=c3 hikey=b5 pitch_keycenter=c4\n");
            auto sfx = rs::createMultiSampler();
            sfx->setPlayConfigDetails (2, 2, sr, block);
            sfx->prepareToPlay (sr, block);
            juce::String e;
            const bool loaded = rs::multiSamplerLoadNow (sfx.get(), dir.getChildFile ("test.sfz"), e);
            auto sin = rs::multiSamplerInstrument (sfx.get());
            if (! loaded || sin == nullptr) { std::cout << "sfz load failed: " << e << "\n"; ok = false; }
            else
            {
                std::cout << "sfz: " << sin->zones.size() << " zones, articulations " << sin->articulations.joinIntoString (", ") << ", keyswitches " << sin->keyswitches[0] << " " << sin->keyswitches[1]
                          << ", round robins " << sin->roundRobins << ", release " << dynamic_cast<rs::BuiltinFx*> (sfx.get())->param ("release") << " ms\n";
                ok = ok && sin->zones.size() == 5 && sin->articulations.size() == 2 && sin->keyswitches[0] == 24 && sin->keyswitches[1] == 26 && sin->roundRobins == 2;
                // held for 2 s: the looping take keeps sounding past its 1 s length, the other one stops
                auto holdNote = [&] (int note)
                {
                    juce::AudioBuffer<float> out (2, (int) (sr * 2.0));
                    for (int pos = 0; pos < out.getNumSamples(); pos += block)
                    {
                        juce::MidiBuffer m;
                        if (pos == 0) m.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);
                        juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, juce::jmin (block, out.getNumSamples() - pos));
                        sfx->processBlock (view, m);
                    }
                    juce::MidiBuffer off; off.addEvent (juce::MidiMessage::noteOff (1, note), 0);
                    juce::AudioBuffer<float> tail (2, (int) (sr * 1.0));
                    juce::MidiBuffer none;
                    for (int pos = 0; pos < tail.getNumSamples(); pos += block) { juce::AudioBuffer<float> view (tail.getArrayOfWritePointers(), 2, pos, juce::jmin (block, tail.getNumSamples() - pos)); sfx->processBlock (view, pos == 0 ? off : none); }
                    return std::make_pair (out.getRMSLevel (0, (int) (sr * 1.3), (int) (sr * 0.5)), rs::msamp::detectPitchHz (tail, sr));
                };
                const auto first = holdNote (60), second = holdNote (60);
                std::cout << "  take 1 (loops) after 1.3 s: " << juce::Decibels::gainToDecibels (first.first) << " dB, take 2 (no loop): " << juce::Decibels::gainToDecibels (second.first, -120.0f) << " dB; release sound at " << second.second << " Hz\n";
                ok = ok && first.first > 0.05f && second.first < 0.001f && std::abs (second.second - 1000) < 20;
                // keyswitch to "Short": a 0.15 s note
                juce::AudioBuffer<float> b (2, (int) (sr * 0.6));
                for (int pos = 0; pos < b.getNumSamples(); pos += block)
                {
                    juce::MidiBuffer m;
                    if (pos == 0) { m.addEvent (juce::MidiMessage::noteOn (1, 26, 0.9f), 0); m.addEvent (juce::MidiMessage::noteOn (1, 62, 0.9f), 10); }
                    juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), 2, pos, juce::jmin (block, b.getNumSamples() - pos));
                    sfx->processBlock (view, m);
                }
                const float early = b.getRMSLevel (0, 0, (int) (sr * 0.1)), late = b.getRMSLevel (0, (int) (sr * 0.3), (int) (sr * 0.2));
                std::cout << "  keyswitch d1 -> " << sin->articulations[(int) dynamic_cast<rs::BuiltinFx*> (sfx.get())->param ("artic")] << ": " << juce::Decibels::gainToDecibels (early) << " dB then " << juce::Decibels::gainToDecibels (late, -120.0f) << " dB\n";
                ok = ok && early > 0.05f && late < 0.001f;
            }
            dir.deleteRecursively();
        }
        std::cout << (ok ? "MSAMP OK\n" : "MSAMP FAILED\n");
        return ok ? 0 : 1;
    }

    if (argc >= 3 && juce::String (argv[1]) == "--sf2")
    {
        // the Multi Sampler with a General MIDI SoundFont (argv[2]); argv[3] = a small synth bank; argv[4] = demo wav; argv[5] = picture
        juce::ScopedJuceInitialiser_GUI gui;
        const double sr = 48000; const int block = 512;
        bool ok = true;
        auto fx = rs::createMultiSampler();
        fx->setPlayConfigDetails (2, 2, sr, block);
        fx->prepareToPlay (sr, block);
        struct Ev { double t; int note; float vel; bool on; };
        auto play = [&] (std::vector<Ev> ev, double secs)
        {
            juce::AudioBuffer<float> out (2, (int) (sr * secs));
            for (int pos = 0; pos < out.getNumSamples(); pos += block)
            {
                const int len = juce::jmin (block, out.getNumSamples() - pos);
                juce::MidiBuffer m;
                for (auto& e : ev) { const int at = (int) (e.t * sr); if (at >= pos && at < pos + len) m.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.note, e.vel) : juce::MidiMessage::noteOff (1, e.note), at - pos); }
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, len);
                fx->processBlock (view, m);
            }
            return out;
        };
        auto seg = [&] (const juce::AudioBuffer<float>& b, double from, double len) { juce::AudioBuffer<float> x (2, (int) (sr * len)); for (int c = 0; c < 2; ++c) x.copyFrom (c, 0, b, c, (int) (sr * from), x.getNumSamples()); return x; };
        auto rms = [&] (const juce::AudioBuffer<float>& b, double from, double len) { return juce::jmax (b.getRMSLevel (0, (int) (sr * from), (int) (sr * len)), b.getRMSLevel (1, (int) (sr * from), (int) (sr * len))); };
        auto loadPreset = [&] (const juce::File& f, const juce::String& prefix) -> int
        {
            juce::String e;
            if (! rs::multiSamplerLoadNow (fx.get(), f, e, 0)) { std::cout << "load failed: " << e << "\n"; return -1; }
            auto in = rs::multiSamplerInstrument (fx.get());
            for (int i = 0; i < in->sf2Presets.size(); ++i) if (in->sf2Presets[i].startsWith (prefix)) { rs::multiSamplerLoadNow (fx.get(), f, e, i); return i; }
            return -1;
        };
        const juce::File gm (argv[2]);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        const int piano = loadPreset (gm, "000:000");
        auto in = rs::multiSamplerInstrument (fx.get());
        if (in == nullptr) return 1;
        std::cout << "GM bank: " << in->sf2Presets.size() << " presets, first \"" << in->sf2Presets[0] << "\"; piano preset " << piano << " \"" << in->name << "\" " << in->zones.size()
                  << " zones, " << in->velocityLayers << " layers, " << in->bytes / (1024 * 1024) << " MB, in " << (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0 << " s\n";
        {
            auto soft = play ({ { 0.0, 60, 0.3f, true }, { 1.5, 60, 0.3f, false } }, 2.0);
            auto hard = play ({ { 0.0, 60, 1.0f, true }, { 1.5, 60, 1.0f, false } }, 2.0);
            const double f = rs::msamp::detectPitchHz (seg (hard, 0.1, 0.6), sr);
            std::cout << "piano C4: " << f << " Hz, soft " << juce::Decibels::gainToDecibels (rms (soft, 0.02, 0.3)) << " dB, hard " << juce::Decibels::gainToDecibels (rms (hard, 0.02, 0.3)) << " dB, after release " << juce::Decibels::gainToDecibels (rms (hard, 1.9, 0.1), -120.0f) << " dB\n";
            ok = ok && std::abs (1200 * std::log2 (f / 261.63)) < 15 && rms (hard, 0.02, 0.3) > rms (soft, 0.02, 0.3) * 1.5 && rms (hard, 1.9, 0.1) < rms (hard, 1.4, 0.1) * 0.7f;
            auto a4 = play ({ { 0.0, 69, 0.8f, true }, { 1.0, 69, 0.8f, false } }, 1.3);
            const double fa = rs::msamp::detectPitchHz (seg (a4, 0.1, 0.6), sr);
            std::cout << "piano A4: " << fa << " Hz\n";
            ok = ok && std::abs (1200 * std::log2 (fa / 440.0)) < 15;
        }
        {
            const int strings = loadPreset (gm, "000:048");
            auto in2 = rs::multiSamplerInstrument (fx.get());
            auto held = play ({ { 0.0, 64, 0.8f, true }, { 5.5, 64, 0.8f, false } }, 6.5);
            int shortest = 1 << 30; bool looped = false;
            for (auto& z : in2->zones) { shortest = juce::jmin (shortest, z.sample->buffer.getNumSamples()); looped = looped || z.loopEnd > 0; }
            std::cout << "strings (" << strings << " \"" << in2->name << "\"): loops " << looped << ", still sounding at 5 s: " << juce::Decibels::gainToDecibels (rms (held, 5.0, 0.4)) << " dB; release envelope " << fx->state.getParameter ("release")->getCurrentValueAsText() << "\n";
            ok = ok && looped && rms (held, 5.0, 0.4) > 0.01f;
        }
        {
            const int drums = loadPreset (gm, "128:000");
            auto in3 = rs::multiSamplerInstrument (fx.get());
            auto d = play ({ { 0.0, 36, 1.0f, true }, { 0.1, 36, 1.0f, false }, { 0.5, 38, 1.0f, true }, { 0.6, 38, 1.0f, false }, { 1.0, 42, 1.0f, true }, { 1.1, 42, 1.0f, false } }, 1.5);
            std::cout << "drums (" << drums << " \"" << in3->name << "\", " << in3->zones.size() << " zones): kick " << juce::Decibels::gainToDecibels (rms (d, 0.0, 0.2)) << " dB, snare " << juce::Decibels::gainToDecibels (rms (d, 0.5, 0.2)) << " dB, hat " << juce::Decibels::gainToDecibels (rms (d, 1.0, 0.1)) << " dB\n";
            ok = ok && drums >= 0 && rms (d, 0.0, 0.2) > 0.02f && rms (d, 0.5, 0.2) > 0.02f && rms (d, 1.0, 0.1) > 0.005f;
        }
        if (argc > 3)
        {
            const juce::File small (argv[3]);
            juce::String e;
            const bool loadedSmall = rs::multiSamplerLoadNow (fx.get(), small, e, 0);
            if (! loadedSmall) std::cout << "small bank preset 0: " << e << "\n";
            auto in4 = rs::multiSamplerInstrument (fx.get());
            int playable = 0;
            int failed = 0;
            for (int i = 0; in4 && i < in4->sf2Presets.size(); ++i)
            {
                if (! rs::multiSamplerLoadNow (fx.get(), small, e, i)) { if (failed++ < 3) std::cout << "  preset " << i << ": " << e << "\n"; continue; }
                auto o = play ({ { 0.0, 60, 0.8f, true }, { 0.5, 60, 0.8f, false } }, 0.8);
                bool fin = true; for (int c = 0; c < 2; ++c) for (int k = 0; k < o.getNumSamples(); ++k) fin = fin && std::isfinite (o.getSample (c, k));
                if (fin && rms (o, 0.0, 0.5) > 0.001f) ++playable;
                ok = ok && fin;
            }
            std::cout << "small bank: " << (in4 ? in4->sf2Presets.size() : 0) << " presets, " << playable << " sound\n";
        }
        if (argc > 4)
        {
            // a little demo: piano chords, strings and drums
            juce::AudioBuffer<float> mix (2, (int) (sr * 8)); mix.clear();
            auto addPart = [&] (const juce::String& prefix, std::vector<Ev> ev, float gain)
            {
                loadPreset (gm, prefix);
                auto o = play (ev, 8.0);
                for (int c = 0; c < 2; ++c) mix.addFrom (c, 0, o, c, 0, o.getNumSamples(), gain);
            };
            std::vector<Ev> pianoEv, strEv, drumEv;
            const int chords[4][4] { { 48, 60, 64, 67 }, { 45, 57, 60, 64 }, { 41, 57, 60, 65 }, { 43, 55, 59, 62 } };
            for (int c = 0; c < 4; ++c)
                for (int r = 0; r < 4; ++r)
                    for (int k = 0; k < 4; ++k) { const double t = c * 2.0 + r * 0.5; pianoEv.push_back ({ t, chords[c][k] + (k == 0 ? 0 : 12), 0.55f + 0.1f * (r == 0), true }); pianoEv.push_back ({ t + 0.45, chords[c][k] + (k == 0 ? 0 : 12), 0.5f, false }); }
            for (int c = 0; c < 4; ++c) for (int k = 1; k < 4; ++k) { strEv.push_back ({ c * 2.0, chords[c][k], 0.6f, true }); strEv.push_back ({ c * 2.0 + 1.95, chords[c][k], 0.6f, false }); }
            for (int b = 0; b < 16; ++b)
            {
                const double t = b * 0.5;
                drumEv.push_back ({ t, b % 2 == 0 ? 36 : 38, 0.9f, true }); drumEv.push_back ({ t + 0.1, b % 2 == 0 ? 36 : 38, 0.9f, false });
                drumEv.push_back ({ t, 42, 0.6f, true }); drumEv.push_back ({ t + 0.05, 42, 0.6f, false });
                drumEv.push_back ({ t + 0.25, 42, 0.45f, true }); drumEv.push_back ({ t + 0.3, 42, 0.45f, false });
            }
            addPart ("000:000", pianoEv, 0.8f);
            addPart ("000:048", strEv, 0.5f);
            addPart ("128:000", drumEv, 0.7f);
            const float pk = mix.getMagnitude (0, mix.getNumSamples());
            if (pk > 0) mix.applyGain (0.8f / pk);
            juce::String err;
            rs::exporter::write (mix, sr, {}, {}, juce::File (argv[4]), err);
            std::cout << "demo written\n";
        }
        if (argc > 5)
        {
            loadPreset (gm, "000:000");
            std::unique_ptr<juce::AudioProcessorEditor> ed (fx->createEditor());
            juce::MidiBuffer m;
            for (int nn : { 48, 60, 64, 67 }) m.addEvent (juce::MidiMessage::noteOn (1, nn, (uint8_t) 100), 0);
            juce::AudioBuffer<float> b (2, block);
            fx->processBlock (b, m);
            for (int f = 0; f < 5; ++f) { juce::MidiBuffer none; fx->processBlock (b, none); }
            auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
            juce::File pf (argv[5]); pf.deleteFile();
            juce::FileOutputStream os (pf);
            juce::PNGImageFormat().writeImageToStream (img, os);
        }
        std::cout << (ok ? "SF2 OK\n" : "SF2 FAILED\n");
        return ok ? 0 : 1;
    }

    if (argc >= 3 && juce::String (argv[1]) == "--keysdemo")
    {
        // short listening demos of the Organ, Electric Piano and Acid Bass (into the folder argv[2])
        juce::ScopedJuceInitialiser_GUI gui;
        const double sr = 48000; const int block = 512;
        const juce::File dir (argv[2]); dir.createDirectory();
        auto setP = [] (rs::BuiltinFx& fx, const char* id, float v) { auto* p = fx.state.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
        struct Ev { double t; juce::MidiMessage m; };
        auto render = [&] (rs::BuiltinFx& fx, std::vector<Ev> ev, double secs, juce::AudioPlayHead* head = nullptr, rs::BuiltinFx* after = nullptr)
        {
            juce::AudioBuffer<float> out (2, (int) (sr * secs));
            out.clear();
            fx.setPlayConfigDetails (2, 2, sr, block); fx.prepareToPlay (sr, block);
            if (after) { after->setPlayConfigDetails (2, 2, sr, block); after->prepareToPlay (sr, block); }
            fx.setPlayHead (head);
            for (int pos = 0; pos < out.getNumSamples(); pos += block)
            {
                const int len = juce::jmin (block, out.getNumSamples() - pos);
                juce::MidiBuffer m;
                for (auto& e : ev) { const int at = (int) (e.t * sr); if (at >= pos && at < pos + len) m.addEvent (e.m, at - pos); }
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, len);
                if (head) static_cast<void> (0);
                fx.processBlock (view, m);
                if (after) { juce::MidiBuffer none; after->processBlock (view, none); }
            }
            fx.setPlayHead (nullptr);
            const float pk = out.getMagnitude (0, out.getNumSamples());
            if (pk > 0) out.applyGain (0.8f / pk);
            return out;
        };
        auto save = [&] (const juce::AudioBuffer<float>& b, const char* name) { juce::String e; rs::exporter::write (b, sr, {}, {}, dir.getChildFile (name), e); };
        auto on = [] (double t, int n, int v) { return Ev { t, juce::MidiMessage::noteOn (1, n, (uint8_t) v) }; };
        auto off = [] (double t, int n) { return Ev { t, juce::MidiMessage::noteOff (1, n) }; };
        {
            // organ: a gospel-ish phrase, the rotary speeding up half way (mod wheel)
            auto org = rs::createOrgan();
            static_cast<rs::BuiltinFx&> (*org);
            for (int i = 0; i < 9; ++i) setP (*org, (juce::String ("db") + juce::String (i + 1)).toRawUTF8(), (float) ("888600000"[i] - '0'));
            std::vector<Ev> ev;
            const int chords[4][4] { { 48, 60, 64, 67 }, { 53, 60, 65, 69 }, { 55, 62, 67, 71 }, { 48, 60, 64, 72 } };
            for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) { ev.push_back (on (c * 1.5, chords[c][k], 100)); ev.push_back (off (c * 1.5 + 1.4, chords[c][k])); }
            const int run[] { 72, 74, 76, 79, 81, 79, 76, 74, 72 };
            for (int k = 0; k < 9; ++k) { ev.push_back (on (6.0 + k * 0.15, run[k], 100)); ev.push_back (off (6.0 + k * 0.15 + 0.12, run[k])); }
            ev.push_back (on (7.5, 48, 100)); ev.push_back (on (7.5, 60, 100)); ev.push_back (on (7.5, 64, 100)); ev.push_back (on (7.5, 67, 100)); ev.push_back (on (7.5, 72, 100));
            ev.push_back ({ 3.0, juce::MidiMessage::controllerEvent (1, 1, 127) });
            for (int nn : { 48, 60, 64, 67, 72 }) ev.push_back (off (10.0, nn));
            save (render (*org, ev, 11.0), "organ.wav");
        }
        {
            // electric piano: soft then hard chords, with the shimmer reverb behind
            auto ep = rs::createEPiano();
            auto rev = rs::createShimmer();
            setP (*rev, "mix", 22); setP (*rev, "shimmer", 30);
            std::vector<Ev> ev;
            const int chords[4][4] { { 50, 60, 65, 69 }, { 55, 59, 65, 69 }, { 48, 59, 64, 67 }, { 45, 57, 64, 67 } };
            for (int c = 0; c < 4; ++c) for (int k = 0; k < 4; ++k) { ev.push_back (on (c * 1.6 + k * 0.02, chords[c][k], 45 + 25 * (c % 2))); ev.push_back (off (c * 1.6 + 1.5, chords[c][k])); }
            const int mel[] { 72, 74, 76, 77, 76, 72, 74, 69 };
            for (int k = 0; k < 8; ++k) { ev.push_back (on (6.4 + k * 0.3, mel[k], 115)); ev.push_back (off (6.4 + k * 0.3 + 0.25, mel[k])); }
            save (render (*ep, ev, 10.0, nullptr, rev.get()), "epiano.wav");
        }
        {
            // acid bass: its own pattern, 4 bars at 128 bpm, the filter opening up
            struct Head : juce::AudioPlayHead
            {
                double ppq = 0;
                juce::Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setBpm (128.0); p.setPpqPosition (ppq); p.setIsPlaying (true); return p; }
            } head;
            auto ab = rs::createAcidBass();
            setP (*ab, "seq", 1);
            juce::AudioBuffer<float> out (2, (int) (sr * 7.5)); out.clear();
            ab->setPlayConfigDetails (2, 2, sr, block); ab->prepareToPlay (sr, block); ab->setPlayHead (&head);
            for (int pos = 0; pos < out.getNumSamples(); pos += block)
            {
                const int len = juce::jmin (block, out.getNumSamples() - pos);
                head.ppq = pos / sr * 128.0 / 60.0;
                const float t = (float) pos / out.getNumSamples();
                setP (*ab, "cutoff", 15.0f + 45.0f * t); setP (*ab, "envmod", 40.0f + 40.0f * t);
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, len);
                juce::MidiBuffer none;
                ab->processBlock (view, none);
            }
            ab->setPlayHead (nullptr);
            const float pk = out.getMagnitude (0, out.getNumSamples()); if (pk > 0) out.applyGain (0.8f / pk);
            save (out, "acid.wav");
        }
        std::cout << "demos written\n";
        return 0;
    }

    if (argc >= 2 && juce::String (argv[1]) == "--keys")
    {
        // Organ, Electric Piano, Acid Bass and the fourth set of effects
        juce::ScopedJuceInitialiser_GUI gui;
        const double sr = 48000; const int block = 512;
        bool ok = true;
        auto setP = [] (rs::BuiltinFx& fx, const char* id, float v) { auto* p = fx.state.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
        struct Ev { double t; int note; float vel; bool on; };
        auto render = [&] (rs::BuiltinFx& fx, std::vector<Ev> ev, double secs, juce::AudioBuffer<float>* input = nullptr)
        {
            juce::AudioBuffer<float> out (2, (int) (sr * secs));
            out.clear();
            if (input) for (int c = 0; c < 2; ++c) out.copyFrom (c, 0, *input, c, 0, juce::jmin (input->getNumSamples(), out.getNumSamples()));
            fx.setPlayConfigDetails (2, 2, sr, block);
            fx.prepareToPlay (sr, block);
            for (int pos = 0; pos < out.getNumSamples(); pos += block)
            {
                const int len = juce::jmin (block, out.getNumSamples() - pos);
                juce::MidiBuffer m;
                for (auto& e : ev) { const int at = (int) (e.t * sr); if (at >= pos && at < pos + len) m.addEvent (e.on ? juce::MidiMessage::noteOn (1, e.note, e.vel) : juce::MidiMessage::noteOff (1, e.note), at - pos); }
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, len);
                fx.processBlock (view, m);
            }
            return out;
        };
        auto seg = [&] (const juce::AudioBuffer<float>& b, double from, double len) { juce::AudioBuffer<float> s (2, (int) (sr * len)); for (int c = 0; c < 2; ++c) s.copyFrom (c, 0, b, c, (int) (sr * from), s.getNumSamples()); return s; };
        auto pitch = [&] (const juce::AudioBuffer<float>& b, double from) { return rs::msamp::detectPitchHz (seg (b, from, 0.5), sr); };
        auto rms = [&] (const juce::AudioBuffer<float>& b, double from, double len) { return b.getRMSLevel (0, (int) (sr * from), (int) (sr * len)); };
        auto finite = [&] (const juce::AudioBuffer<float>& b) { for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) if (! std::isfinite (b.getSample (c, i))) return false; return true; };
        // the energy at one frequency (a single DFT bin)
        auto energyAt = [&] (const juce::AudioBuffer<float>& b, double from, double len, double hz)
        {
            double re = 0, im = 0; const int a = (int) (sr * from), n = (int) (sr * len);
            for (int i = 0; i < n; ++i) { const double w = 0.5 - 0.5 * std::cos (juce::MathConstants<double>::twoPi * i / n); const double v = 0.5 * (b.getSample (0, a + i) + b.getSample (1, a + i)) * w; re += v * std::cos (juce::MathConstants<double>::twoPi * hz * i / sr); im += v * std::sin (juce::MathConstants<double>::twoPi * hz * i / sr); }
            return std::sqrt (re * re + im * im) / n;
        };

        // ---- ORGAN ----
        {
            auto org = rs::createOrgan();
            for (int i = 0; i < 9; ++i) setP (*org, (juce::String ("db") + juce::String (i + 1)).toRawUTF8(), i == 2 ? 8.0f : 0.0f);   // only the 8'
            setP (*org, "perc", 0); setP (*org, "rotary", 0); setP (*org, "vibrato", 0); setP (*org, "drive", 0); setP (*org, "click", 0); setP (*org, "tone", 100);
            auto b = render (*org, { { 0.05, 60, 0.8f, true }, { 1.0, 60, 0.8f, false } }, 1.3);
            const double f = pitch (b, 0.3);
            const double fund = energyAt (b, 0.3, 0.5, 261.63), h2 = energyAt (b, 0.3, 0.5, 523.25), h3 = energyAt (b, 0.3, 0.5, 784.9);
            std::cout << "organ 8' only, C4: " << f << " Hz, 2nd harmonic " << juce::Decibels::gainToDecibels ((float) (h2 / fund)) << " dB, 3rd " << juce::Decibels::gainToDecibels ((float) (h3 / fund)) << " dB; after release " << juce::Decibels::gainToDecibels (rms (b, 1.1, 0.15), -120.0f) << " dB\n";
            ok = ok && std::abs (f - 261.63) < 1.5 && h2 / fund < 0.05 && rms (b, 1.1, 0.15) < 1e-4f && finite (b);
            // 16' and 5 1/3' land an octave below and a fifth above
            for (int i = 0; i < 9; ++i) setP (*org, (juce::String ("db") + juce::String (i + 1)).toRawUTF8(), i == 0 || i == 1 ? 8.0f : 0.0f);
            auto b2 = render (*org, { { 0.05, 60, 0.8f, true } }, 0.8);
            std::cout << "organ 16' + 5 1/3': at 130.8 Hz " << energyAt (b2, 0.3, 0.4, 130.81) << ", at 392 Hz " << energyAt (b2, 0.3, 0.4, 392.0) << ", at 261.6 Hz " << energyAt (b2, 0.3, 0.4, 261.63) << "\n";
            ok = ok && energyAt (b2, 0.3, 0.4, 130.81) > 0.01 && energyAt (b2, 0.3, 0.4, 392.0) > 0.01 && energyAt (b2, 0.3, 0.4, 261.63) < 0.002;
            // percussion: the 3rd harmonic (2 2/3') rings out on a detached note, not on a legato one
            for (int i = 0; i < 9; ++i) setP (*org, (juce::String ("db") + juce::String (i + 1)).toRawUTF8(), i == 2 ? 8.0f : 0.0f);
            setP (*org, "perc", 1); setP (*org, "percharm", 1);
            auto b3 = render (*org, { { 0.05, 60, 0.8f, true }, { 0.6, 64, 0.8f, true } }, 1.2);
            const double p0 = energyAt (b3, 0.06, 0.1, 784.9), p1 = energyAt (b3, 0.45, 0.1, 784.9), p2 = energyAt (b3, 0.62, 0.1, 1244.5);
            std::cout << "organ percussion: 3rd harmonic at the start " << p0 << ", 0.4 s later " << p1 << ", legato note's percussion " << p2 << "\n";
            ok = ok && p0 > p1 * 4 && p2 < p0 * 0.25;
            // full drawbars, a chord, rotary on: wide stereo, finite
            for (int i = 0; i < 9; ++i) setP (*org, (juce::String ("db") + juce::String (i + 1)).toRawUTF8(), 8.0f);
            setP (*org, "rotary", 2); setP (*org, "vibrato", 6); setP (*org, "drive", 60); setP (*org, "click", 80);
            auto b4 = render (*org, { { 0.0, 48, 1.0f, true }, { 0.0, 60, 1.0f, true }, { 0.0, 64, 1.0f, true }, { 0.0, 67, 1.0f, true }, { 0.0, 72, 1.0f, true } }, 1.5);
            double diff = 0; for (int i = 0; i < b4.getNumSamples(); ++i) diff += std::abs (b4.getSample (0, i) - b4.getSample (1, i));
            std::cout << "organ full + chord + rotary: peak " << b4.getMagnitude (0, b4.getNumSamples()) << ", L/R differ by " << diff / b4.getNumSamples() << "\n";
            ok = ok && finite (b4) && b4.getMagnitude (0, b4.getNumSamples()) < 1.5f && diff / b4.getNumSamples() > 0.005;
        }
        // ---- ELECTRIC PIANO ----
        {
            auto ep = rs::createEPiano();
            setP (*ep, "tremdepth", 0);
            auto soft = render (*ep, { { 0.0, 57, 0.25f, true }, { 2.5, 57, 0.25f, false } }, 3.0);
            auto hard = render (*ep, { { 0.0, 57, 1.0f, true }, { 2.5, 57, 1.0f, false } }, 3.0);
            const double f = pitch (soft, 0.4);
            // brightness: harmonics over the fundamental
            auto bright = [&] (const juce::AudioBuffer<float>& b) { return (energyAt (b, 0.05, 0.3, 440.0) + energyAt (b, 0.05, 0.3, 660.0)) / energyAt (b, 0.05, 0.3, 220.0); };
            std::cout << "epiano A3: " << f << " Hz; soft " << juce::Decibels::gainToDecibels (rms (soft, 0.05, 0.3)) << " dB, hard " << juce::Decibels::gainToDecibels (rms (hard, 0.05, 0.3))
                      << " dB; harmonics soft " << bright (soft) << " hard " << bright (hard) << "; decays " << juce::Decibels::gainToDecibels (rms (hard, 2.0, 0.3) / rms (hard, 0.1, 0.3))
                      << " dB by 2 s; after release " << juce::Decibels::gainToDecibels (rms (hard, 2.85, 0.1), -120.0f) << " dB\n";
            ok = ok && std::abs (f - 220.0) < 2 && rms (hard, 0.05, 0.3) > rms (soft, 0.05, 0.3) * 2 && bright (hard) > bright (soft) * 1.3
                    && rms (hard, 2.0, 0.3) < rms (hard, 0.1, 0.3) && rms (hard, 2.85, 0.1) < rms (hard, 2.0, 0.3) * 0.05 && finite (hard);
            setP (*ep, "model", 1);
            auto reed = render (*ep, { { 0.0, 57, 0.9f, true }, { 1.0, 57, 0.9f, false } }, 1.3);
            std::cout << "epiano reed A3: " << pitch (reed, 0.2) << " Hz\n";
            ok = ok && std::abs (pitch (reed, 0.2) - 220.0) < 2 && finite (reed);
        }
        // ---- ACID BASS ----
        {
            auto ab = rs::createAcidBass();
            setP (*ab, "seq", 0);
            auto b = render (*ab, { { 0.0, 45, 0.6f, true }, { 0.5, 45, 0.6f, false } }, 0.8);
            std::cout << "acid A2: " << pitch (b, 0.05) << " Hz, after release " << juce::Decibels::gainToDecibels (rms (b, 0.6, 0.1), -120.0f) << " dB\n";
            ok = ok && std::abs (pitch (b, 0.05) - 110.0) < 2 && rms (b, 0.6, 0.1) < 1e-3f;
            // legato slides into the next note (measured on a plain tone)
            setP (*ab, "reso", 0); setP (*ab, "envmod", 0); setP (*ab, "cutoff", 100);
            auto s = render (*ab, { { 0.0, 45, 0.6f, true }, { 0.3, 52, 0.6f, true }, { 0.6, 45, 0.6f, false }, { 0.6, 52, 0.6f, false } }, 0.8);
            const double pa = rs::msamp::detectPitchHz (seg (s, 0.02, 0.26), sr), pb = rs::msamp::detectPitchHz (seg (s, 0.36, 0.24), sr);
            std::cout << "acid slide: " << pa << " Hz then " << pb << " Hz\n";
            ok = ok && std::abs (pb - 164.8) < 4 && std::abs (pa - 110) < 2;
            // the sequencer, running with the song (a fake playhead at 120 bpm)
            struct Head : juce::AudioPlayHead
            {
                double ppq = 0; bool playing = true;
                juce::Optional<PositionInfo> getPosition() const override { PositionInfo p; p.setBpm (120.0); p.setPpqPosition (ppq); p.setIsPlaying (playing); return p; }
            } head;
            setP (*ab, "seq", 1);
            setP (*ab, "reso", 0); setP (*ab, "envmod", 0); setP (*ab, "cutoff", 100);   // a plain tone, to measure its pitch
            ab->setPlayHead (&head);
            ab->setPlayConfigDetails (2, 2, sr, block);
            ab->prepareToPlay (sr, block);
            juce::AudioBuffer<float> out (2, (int) (sr * 2.0));
            for (int pos = 0; pos < out.getNumSamples(); pos += block)
            {
                const int len = juce::jmin (block, out.getNumSamples() - pos);
                head.ppq = pos / sr * 2.0;   // 120 bpm = 2 beats a second
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, len);
                juce::MidiBuffer none;
                ab->processBlock (view, none);
            }
            // two bars of 16ths: 32 steps in 2 s; the first step plays the root (C2 = 65.4 Hz by default, root 36)
            const double first = rs::msamp::detectPitchHz (seg (out, 0.0, 0.11), sr);
            const float restLvl = rms (out, 5 * 0.125 + 0.02, 0.08);   // step 6 is a rest
            std::cout << "acid sequencer: step 1 at " << first << " Hz, a rest step " << juce::Decibels::gainToDecibels (restLvl, -120.0f) << " dB, the pattern " << juce::Decibels::gainToDecibels (rms (out, 0, 2.0)) << " dB\n";
            ok = ok && std::abs (first - 65.4) < 2 && restLvl < rms (out, 0, 2.0) * 0.2f && finite (out);
            ab->setPlayHead (nullptr);
        }
        // ---- MULTIBAND: flat when nothing is compressed, and only the loud band is pulled down ----
        {
            auto mb = rs::createMultiband();
            for (auto id : { "ratio1", "ratio2", "ratio3" }) setP (*mb, id, 1);
            float worst = 0;
            for (double hz : { 60.0, 220.0, 700.0, 3200.0, 9000.0 })
            {
                juce::AudioBuffer<float> in (2, (int) sr);
                for (int i = 0; i < in.getNumSamples(); ++i) { const float v = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr); in.setSample (0, i, v); in.setSample (1, i, v); }
                auto out = render (*mb, {}, 1.0, &in);
                worst = juce::jmax (worst, std::abs (juce::Decibels::gainToDecibels (rms (out, 0.5, 0.4) / rms (in, 0.5, 0.4))));
            }
            std::cout << "multiband with ratio 1: flat within " << worst << " dB\n";
            ok = ok && worst < 0.2f;
            setP (*mb, "ratio1", 8); setP (*mb, "thr1", -30); setP (*mb, "ratio3", 1);
            juce::AudioBuffer<float> in (2, (int) sr);
            for (int i = 0; i < in.getNumSamples(); ++i) { const float v = 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 60 * i / sr) + 0.1f * (float) std::sin (juce::MathConstants<double>::twoPi * 6000 * i / sr); in.setSample (0, i, v); in.setSample (1, i, v); }
            auto out = render (*mb, {}, 1.0, &in);
            const double low = energyAt (out, 0.5, 0.4, 60) / energyAt (in, 0.5, 0.4, 60), high = energyAt (out, 0.5, 0.4, 6000) / energyAt (in, 0.5, 0.4, 6000);
            std::cout << "multiband: low band " << juce::Decibels::gainToDecibels ((float) low) << " dB, high band " << juce::Decibels::gainToDecibels ((float) high) << " dB\n";
            ok = ok && low < 0.5 && std::abs (juce::Decibels::gainToDecibels ((float) high)) < 0.5f;
        }
        // ---- SHIMMER: an octave appears in the tail ----
        {
            double oct[2];
            for (int pass = 0; pass < 2; ++pass)
            {
                auto sh = rs::createShimmer();
                setP (*sh, "shimmer", pass == 0 ? 0.0f : 100.0f); setP (*sh, "mix", 100);
                juce::AudioBuffer<float> in (2, (int) (sr * 3)); in.clear();
                for (int i = 0; i < (int) (sr * 0.3); ++i) { const float v = 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 330 * i / sr); in.setSample (0, i, v); in.setSample (1, i, v); }
                auto out = render (*sh, {}, 3.0, &in);
                oct[pass] = energyAt (out, 1.0, 1.0, 660) / juce::jmax (1e-9, energyAt (out, 1.0, 1.0, 330));
                ok = ok && finite (out) && out.getMagnitude (0, out.getNumSamples()) < 2.0f;
            }
            std::cout << "shimmer: octave in the tail without " << oct[0] << ", with " << oct[1] << "\n";
            ok = ok && oct[1] > oct[0] * 5;
        }
        // ---- TUNER ----
        for (double hz : { 82.41, 110.0, 440.0, 329.63, 1046.5 })
        {
            auto tu = rs::createTuner();
            juce::AudioBuffer<float> in (2, (int) sr);
            for (int i = 0; i < in.getNumSamples(); ++i) { const float v = 0.3f * (float) (std::sin (juce::MathConstants<double>::twoPi * hz * i / sr) + 0.4 * std::sin (2 * juce::MathConstants<double>::twoPi * hz * i / sr)); in.setSample (0, i, v); in.setSample (1, i, v); }
            auto out = render (*tu, {}, 1.0, &in);
            const double got = tu->meterValue.load();
            std::cout << "tuner " << hz << " Hz -> " << got << " Hz (" << 1200 * std::log2 (got / hz) << " cents)\n";
            ok = ok && std::abs (1200 * std::log2 (got / hz)) < 2;
        }
        // ---- VOCAL REMOVER: a centred tone goes, a tone on one side stays (and the other way round) ----
        for (int mode = 0; mode < 2; ++mode)
        {
            auto vr = rs::createVocalRemover();
            setP (*vr, "mode", (float) mode);
            juce::AudioBuffer<float> in (2, (int) (sr * 1.5));
            for (int i = 0; i < in.getNumSamples(); ++i)
            {
                const float c = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 440 * i / sr), side = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 660 * i / sr);
                in.setSample (0, i, c + side); in.setSample (1, i, c);
            }
            auto out = render (*vr, {}, 1.5, &in);
            const int lat = vr->getLatencySamples();
            const double centre = energyAt (out, 0.8, 0.5, 440) / energyAt (in, 0.8 - lat / sr, 0.5, 440), side = energyAt (out, 0.8, 0.5, 660) / energyAt (in, 0.8 - lat / sr, 0.5, 660);
            std::cout << "vocal remover (" << (mode ? "keep centre" : "remove centre") << "): centre tone " << juce::Decibels::gainToDecibels ((float) centre, -100.0f) << " dB, side tone " << juce::Decibels::gainToDecibels ((float) side, -100.0f) << " dB\n";
            ok = ok && (mode == 0 ? (centre < 0.1 && side > 0.7) : (centre > 0.7 && side < 0.2));
        }
        // ---- ANALYSER passes sound through untouched ----
        {
            auto an = rs::createAnalyzer();
            juce::AudioBuffer<float> in (2, (int) sr);
            juce::Random r (3);
            for (int i = 0; i < in.getNumSamples(); ++i) { in.setSample (0, i, r.nextFloat() - 0.5f); in.setSample (1, i, r.nextFloat() - 0.5f); }
            auto out = render (*an, {}, 1.0, &in);
            float d = 0; for (int i = 0; i < in.getNumSamples(); ++i) d = juce::jmax (d, std::abs (out.getSample (0, i) - in.getSample (0, i)));
            std::cout << "analyser changes the sound by " << d << "\n";
            ok = ok && d < 1e-6f;
        }
        // Mixcraft's instruments find their RENASOUND counterparts
        for (auto [name, want] : { std::pair<const char*, const char*> { "VB3 Organ", "organ" }, { "Combo Organ Model F", "organ" }, { "Combo Organ Model V", "organ" },
                                   { "Lounge Lizard Electric Piano", "epiano" }, { "Alien 303 Bass Synthesizer", "acid" }, { "Messiah Version 2", "" },
                                   { "MinimogueVA", "" }, { "Acoustica Studio Drums", "" } })
        {
            const auto got = rs::mixcraftCounterpart (name);
            std::cout << "mixcraft \"" << name << "\" -> " << (got.isEmpty() ? juce::String ("(RENASYNTH / drums)") : got) << "\n";
            ok = ok && got == want;
        }
        std::cout << (ok ? "KEYS OK\n" : "KEYS FAILED\n");
        return ok ? 0 : 1;
    }

    if (argc >= 2 && juce::String (argv[1]) == "--fx2")
    {
        juce::ScopedJuceInitialiser_GUI gui;
        const double sr = 48000; const int block = 512;
        auto setP = [] (rs::BuiltinFx& fx, const char* id, float v)
        {
            auto* p = fx.state.getParameter (id);
            p->setValueNotifyingHost (p->convertTo0to1 (v));
        };
        auto run = [&] (rs::BuiltinFx& fx, juce::AudioBuffer<float>& buf)
        {
            fx.setPlayConfigDetails (2, 2, sr, block);
            fx.prepareToPlay (sr, block);
            juce::MidiBuffer m;
            for (int pos = 0; pos < buf.getNumSamples(); pos += block)
            {
                const int n = juce::jmin (block, buf.getNumSamples() - pos);
                juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, pos, n);
                fx.processBlock (view, m);
            }
        };
        auto sine = [&] (double hz, float amp, double secs)
        {
            juce::AudioBuffer<float> b (2, (int) (sr * secs));
            for (int i = 0; i < b.getNumSamples(); ++i) { const float v = amp * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / sr); b.setSample (0, i, v); b.setSample (1, i, v); }
            return b;
        };
        auto freqOf = [&] (const juce::AudioBuffer<float>& b, int from, int to)
        {
            int crossings = 0; double first = -1, last = 0;
            for (int i = from + 1; i < to; ++i)
                if (b.getSample (0, i - 1) <= 0 && b.getSample (0, i) > 0)
                {
                    const double t = i - 1 + b.getSample (0, i - 1) / (b.getSample (0, i - 1) - b.getSample (0, i));
                    if (first < 0) first = t; else ++crossings;
                    last = t;
                }
            return crossings > 0 ? sr * crossings / (last - first) : 0.0;
        };
        bool ok = true;

        // Auto-Tune: 452 Hz (A4 +47 cents) -> 440, and 425 Hz (-60 cents, nearer G#) -> 415.3
        for (double in : { 452.0, 425.0, 262.0 })
        {
            auto fx = rs::createAutoTune();
            setP (*fx, "speed", 0);
            setP (*fx, "humanize", 0);
            auto b = sine (in, 0.5f, 2.0);
            run (*fx, b);
            const double out = freqOf (b, (int) (sr * 1.0), (int) (sr * 2.0));
            const double want = 440.0 * std::pow (2.0, std::round (12.0 * std::log2 (in / 440.0)) / 12.0);
            const double cents = 1200.0 * std::log2 (out / want);
            std::cout << "autotune " << in << " Hz -> " << out << " Hz (want " << want << ", " << cents << " cents)\n";
            ok = ok && std::abs (cents) < 10.0;
        }
        // Auto-Tune in C major: 277 Hz (C#4) must go to C or D
        {
            auto fx = rs::createAutoTune();
            setP (*fx, "speed", 0); setP (*fx, "humanize", 0); setP (*fx, "scale", 1);
            auto b = sine (280.0, 0.5f, 2.0);
            run (*fx, b);
            const double out = freqOf (b, (int) sr, (int) (sr * 2));
            std::cout << "autotune C major 280 Hz -> " << out << " Hz\n";
            ok = ok && (std::abs (out - 261.6) < 3 || std::abs (out - 293.7) < 3);
        }
        // Voice Cleaner with nothing removed should give the input back (delayed by its latency)
        {
            auto fx = rs::createVoiceCleaner();
            setP (*fx, "noise", 0); setP (*fx, "reverb", 0);
            auto b = sine (330.0, 0.5f, 1.0);
            auto ref = b;
            run (*fx, b);
            const int lat = fx->getLatencySamples();
            double err = 0, e2 = 0;
            for (int i = 8192; i < b.getNumSamples(); ++i) { const double d = b.getSample (0, i) - ref.getSample (0, i - lat); err += d * d; e2 += ref.getSample (0, i - lat) * ref.getSample (0, i - lat); }
            const double ratio = b.getRMSLevel (0, 8192, b.getNumSamples() - 8192) / ref.getRMSLevel (0, 8192, b.getNumSamples() - 8192);
            std::cout << "voiceclean unity: gain " << ratio << " error " << juce::Decibels::gainToDecibels ((float) std::sqrt (err / e2)) << " dB\n";
            ok = ok && std::abs (ratio - 1.0) < 0.02;
        }
        // Voice Cleaner removes steady noise between words
        {
            auto fx = rs::createVoiceCleaner();
            setP (*fx, "noise", 100); setP (*fx, "reverb", 0);
            juce::AudioBuffer<float> b (2, (int) (sr * 3));
            juce::Random r (3);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                const bool talk = (i / (int) (sr * 0.5)) % 2 == 1;
                const float v = 0.03f * (r.nextFloat() * 2 - 1) + (talk ? 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 220 * i / sr) : 0.0f);
                b.setSample (0, i, v); b.setSample (1, i, v);
            }
            run (*fx, b);
            const int lat = fx->getLatencySamples();
            const float gap = b.getRMSLevel (0, (int) (sr * 2.1) + lat, (int) (sr * 0.3));   // a pause: noise only
            const float talk = b.getRMSLevel (0, (int) (sr * 2.6) + lat, (int) (sr * 0.3));
            std::cout << "voiceclean noise in pause: " << juce::Decibels::gainToDecibels (gap) << " dB (was " << juce::Decibels::gainToDecibels (0.03f / std::sqrt (3.0f)) << "), voice " << juce::Decibels::gainToDecibels (talk) << " dB\n";
            ok = ok && gap < 0.03f / std::sqrt (3.0f) * 0.5f && talk > 0.25f;
        }
        // Voice Cleaner shortens a reverb tail: tone bursts, each followed by a 1.2 s decaying "room"
        {
            float tails[2];
            for (int pass = 0; pass < 2; ++pass)
            {
                auto fx = rs::createVoiceCleaner();
                setP (*fx, "noise", 0); setP (*fx, "reverb", pass == 0 ? 0.0f : 100.0f);
                juce::AudioBuffer<float> b (2, (int) (sr * 4));
                juce::Random r (5);
                for (int i = 0; i < b.getNumSamples(); ++i)
                {
                    const double t = std::fmod (i / sr, 2.0);
                    const float dry = t < 0.3 ? 0.4f * (float) std::sin (juce::MathConstants<double>::twoPi * 300 * i / sr) : 0.0f;
                    const float room = (float) (0.12 * std::exp (-6.9 * juce::jmax (0.0, t - 0.3) / 1.2)) * (r.nextFloat() * 2 - 1) * (t > 0.02 ? 1.0f : 0.0f);
                    b.setSample (0, i, dry + room); b.setSample (1, i, dry + room);
                }
                run (*fx, b);
                tails[pass] = b.getRMSLevel (0, (int) (sr * 2.45) + fx->getLatencySamples(), (int) (sr * 0.6));
            }
            std::cout << "voiceclean reverb tail: " << juce::Decibels::gainToDecibels (tails[1] / tails[0]) << " dB with Reverb at 100%\n";
            ok = ok && tails[1] < tails[0] * 0.5f;
        }
        // Bus compressor reduces gain on a loud signal
        {
            auto fx = rs::createBusCompressor();
            setP (*fx, "threshold", -20); setP (*fx, "ratio", 4);
            auto b = sine (100, 0.5f, 1.0);
            run (*fx, b);
            std::cout << "buscomp GR " << fx->meterValue.load() << " dB, out rms " << juce::Decibels::gainToDecibels (b.getRMSLevel (0, (int) sr / 2, (int) sr / 2)) << " dB\n";
            ok = ok && fx->meterValue.load() < -3.0f;
        }
        // Limiter never goes past the ceiling
        {
            auto fx = rs::createLimiter();
            setP (*fx, "gain", 12); setP (*fx, "ceiling", -1);
            juce::AudioBuffer<float> b (2, (int) sr);
            juce::Random r (1);
            for (int i = 0; i < b.getNumSamples(); ++i) { const float v = (r.nextFloat() * 2 - 1) * (i % 9000 < 200 ? 1.0f : 0.2f); b.setSample (0, i, v); b.setSample (1, i, v); }
            run (*fx, b);
            const float pk = b.getMagnitude (0, b.getNumSamples());
            std::cout << "limiter peak " << juce::Decibels::gainToDecibels (pk) << " dB (ceiling -1)\n";
            ok = ok && pk <= juce::Decibels::decibelsToGain (-1.0f) + 1e-4f;
        }
        // Sub bass adds energy an octave down
        {
            auto fx = rs::createSubBass();
            setP (*fx, "dry", 0);
            auto b = sine (110, 0.5f, 1.0);
            run (*fx, b);
            const double f = freqOf (b, (int) (sr * 0.5), (int) sr);
            std::cout << "subbass 110 Hz -> " << f << " Hz, level " << juce::Decibels::gainToDecibels (b.getRMSLevel (0, (int) sr / 2, (int) sr / 2)) << " dB\n";
            ok = ok && std::abs (f - 55) < 2;
        }
        // Color EQ flat at zero gains, every character
        for (int ch = 0; ch < 4; ++ch)
        {
            auto fx = rs::createColorEq();
            setP (*fx, "model", (float) ch);
            float worst = 0;
            for (double hz : { 50.0, 200.0, 1000.0, 5000.0, 15000.0 }) worst = juce::jmax (worst, std::abs (juce::Decibels::gainToDecibels ((float) fx->getMagnitudeForFrequency (hz))));
            std::cout << "coloreq character " << ch << " flat within " << worst << " dB\n";
        }
        // everything new stays finite and sensible on noise + tone
        for (auto id : { "autotune", "buscomp", "coloreq", "voiceclean", "rotary", "deesser", "bitcrusher", "dubdelay", "limiter", "ringmod", "subbass" })
        {
            auto fx = rs::createBuiltinFx (id);
            juce::AudioBuffer<float> b (2, (int) (sr * 2));
            juce::Random r (7);
            for (int i = 0; i < b.getNumSamples(); ++i) { const float v = 0.1f * (r.nextFloat() * 2 - 1) + 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 200 * i / sr); b.setSample (0, i, v); b.setSample (1, i, v * 0.9f); }
            const float inRms = b.getRMSLevel (0, 0, b.getNumSamples());
            run (*fx, b);
            bool finite = true;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) finite = finite && std::isfinite (b.getSample (c, i));
            const float outRms = b.getRMSLevel (0, 0, b.getNumSamples()), pk = b.getMagnitude (0, b.getNumSamples());
            std::cout << "  " << id << ": " << juce::Decibels::gainToDecibels (outRms / inRms) << " dB vs input, peak " << pk << (finite ? "" : " NON-FINITE") << "\n";
            ok = ok && finite && pk < 4.0f;
        }
        // RENASYNTH mod matrix: on "Vibrato Lead" the mod wheel brings in the vibrato
        {
            int idx = -1;
            for (int i = 0; i < (int) rs::getSynthPatches().size(); ++i) if (juce::String (rs::getSynthPatches()[(size_t) i].name) == "Vibrato Lead") idx = i;
            double spread[2] {};
            for (int pass = 0; pass < 2; ++pass)
            {
                auto fx = rs::createBuiltinFx ("synth");
                rs::loadSynthPatch (fx.get(), idx);
                setP (*fx, "o2level", 0); setP (*fx, "o1wave", 0); setP (*fx, "o1shape", 0); setP (*fx, "glide", 0); setP (*fx, "ftype", 1); setP (*fx, "cutoff", 20000);   // a plain sine, to measure
                fx->setPlayConfigDetails (2, 2, sr, block);
                fx->prepareToPlay (sr, block);
                juce::AudioBuffer<float> b (2, (int) (sr * 2));
                for (int pos = 0; pos < b.getNumSamples(); pos += block)
                {
                    juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), 2, pos, juce::jmin (block, b.getNumSamples() - pos));
                    juce::MidiBuffer m;
                    if (pos == 0) { m.addEvent (juce::MidiMessage::controllerEvent (1, 1, pass == 0 ? 0 : 127), 0); m.addEvent (juce::MidiMessage::noteOn (1, 69, 0.8f), 0); }
                    fx->processBlock (view, m);
                }
                double lo = 1e9, hi = 0;
                for (int w = 0; w < 20; ++w) { const int a = (int) (sr * (0.5 + w * 0.06)); const double f = freqOf (b, a, a + (int) (sr * 0.03)); lo = juce::jmin (lo, f); hi = juce::jmax (hi, f); }
                spread[pass] = 1200.0 * std::log2 (hi / lo);
            }
            std::cout << "synth vibrato: mod wheel off " << spread[0] << " cents, full " << spread[1] << " cents\n";
            ok = ok && spread[0] < 5 && spread[1] > 40;
        }
        // the ladder at full resonance stays in bounds, and the matrix can't push anything to NaN
        {
            auto fx = rs::createBuiltinFx ("synth");
            setP (*fx, "ftype", 4); setP (*fx, "reso", 100); setP (*fx, "cutoff", 800); setP (*fx, "unison", 7);
            setP (*fx, "m1src", 1); setP (*fx, "m1dst", 2); setP (*fx, "m1amt", 100);
            setP (*fx, "m2src", 9); setP (*fx, "m2dst", 3); setP (*fx, "m2amt", -100);
            setP (*fx, "m3src", 6); setP (*fx, "m3dst", 12); setP (*fx, "m3amt", 100);
            setP (*fx, "lfodepth", 0); setP (*fx, "lforate", 7);
            fx->setPlayConfigDetails (2, 2, sr, block);
            fx->prepareToPlay (sr, block);
            juce::AudioBuffer<float> b (2, (int) (sr * 2));
            for (int pos = 0; pos < b.getNumSamples(); pos += block)
            {
                juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), 2, pos, juce::jmin (block, b.getNumSamples() - pos));
                juce::MidiBuffer m;
                if (pos == 0) for (int n : { 36, 48, 55, 60, 64, 67, 72, 84 }) m.addEvent (juce::MidiMessage::noteOn (1, n, 1.0f), 0);
                fx->processBlock (view, m);
            }
            bool finite = true;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) finite = finite && std::isfinite (b.getSample (c, i));
            std::cout << "synth ladder at full resonance: peak " << b.getMagnitude (0, b.getNumSamples()) << (finite ? "" : " NON-FINITE") << "\n";
            ok = ok && finite;
        }
        // Widener: the phase guard eases off a signal that cancels in mono; mono check really is mono
        {
            auto fx = rs::createWidener();
            juce::AudioBuffer<float> b (2, (int) sr);
            for (int i = 0; i < b.getNumSamples(); ++i) { const float v = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 300 * i / sr); b.setSample (0, i, v); b.setSample (1, i, -0.8f * v); }
            run (*fx, b);
            const float guardG = dynamic_cast<rs::BuiltinFx*> (fx.get()) != nullptr ? fx->state.getParameter ("guard")->getValue() : 0.0f;
            juce::ignoreUnused (guardG);
            auto fx2 = rs::createWidener();
            setP (*fx2, "monoCheck", 1);
            juce::AudioBuffer<float> c (2, (int) sr);
            juce::Random r (2);
            for (int i = 0; i < c.getNumSamples(); ++i) { c.setSample (0, i, r.nextFloat() - 0.5f); c.setSample (1, i, r.nextFloat() - 0.5f); }
            run (*fx2, c);
            float diff = 0;
            for (int i = 0; i < c.getNumSamples(); ++i) diff = juce::jmax (diff, std::abs (c.getSample (0, i) - c.getSample (1, i)));
            // what's left of the side signal with the guard on, compared with off
            auto fx3 = rs::createWidener();
            setP (*fx3, "guard", 0);
            juce::AudioBuffer<float> b2 (2, (int) sr);
            for (int i = 0; i < b2.getNumSamples(); ++i) { const float v = 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 300 * i / sr); b2.setSample (0, i, v); b2.setSample (1, i, -0.8f * v); }
            run (*fx3, b2);
            auto sideOf = [] (juce::AudioBuffer<float>& x) { double e = 0; for (int i = x.getNumSamples() / 2; i < x.getNumSamples(); ++i) { const double sd = x.getSample (0, i) - x.getSample (1, i); e += sd * sd; } return std::sqrt (e); };
            const double ratio = sideOf (b) / sideOf (b2);
            std::cout << "widener guard keeps " << juce::Decibels::gainToDecibels ((float) ratio) << " dB of an out-of-phase side, mono check L-R " << diff << "\n";
            ok = ok && ratio < 0.5 && diff < 1e-6f;
            // ordinary stereo at full width: the guard should not touch it
            float outs[2];
            for (int g2 = 0; g2 < 2; ++g2)
            {
                auto w = rs::createWidener();
                setP (*w, "width", 200); setP (*w, "guard", (float) g2);
                juce::AudioBuffer<float> x (2, (int) sr);
                juce::Random rr (4);
                float lpL = 0, lpR = 0;
                for (int i = 0; i < x.getNumSamples(); ++i)
                {
                    const float common = rr.nextFloat() - 0.5f, own = rr.nextFloat() - 0.5f;
                    lpL += 0.2f * (common + 0.35f * own - lpL); lpR += 0.2f * (common - 0.35f * own - lpR);
                    x.setSample (0, i, lpL); x.setSample (1, i, lpR);
                }
                run (*w, x);
                outs[g2] = x.getRMSLevel (0, 0, x.getNumSamples());
            }
            std::cout << "widener guard on ordinary stereo at 200%: " << juce::Decibels::gainToDecibels (outs[1] / outs[0]) << " dB difference\n";
            ok = ok && std::abs (juce::Decibels::gainToDecibels (outs[1] / outs[0])) < 0.5f;
        }
        // Kick: the Big Room setting is longer, louder in the tail, and stays finite
        {
            float tails[2] {}; float peaks[2] {};
            for (int pass = 0; pass < 2; ++pass)
            {
                auto fx = rs::createKickSynth();
                if (pass == 1) setP (*fx, "hold", 110), setP (*fx, "tail", 65), setP (*fx, "room", 30), setP (*fx, "punch", 75), setP (*fx, "decay", 700);
                fx->setPlayConfigDetails (2, 2, sr, block);
                fx->prepareToPlay (sr, block);
                juce::AudioBuffer<float> b (2, (int) sr);
                for (int pos = 0; pos < b.getNumSamples(); pos += block)
                {
                    juce::AudioBuffer<float> view (b.getArrayOfWritePointers(), 2, pos, juce::jmin (block, b.getNumSamples() - pos));
                    juce::MidiBuffer m;
                    if (pos == 0) m.addEvent (juce::MidiMessage::noteOn (1, 36, 1.0f), 0);
                    fx->processBlock (view, m);
                }
                tails[pass] = b.getRMSLevel (0, (int) (sr * 0.15), (int) (sr * 0.2));
                peaks[pass] = b.getMagnitude (0, b.getNumSamples());
                for (int i = 0; i < b.getNumSamples(); ++i) ok = ok && std::isfinite (b.getSample (0, i));
            }
            std::cout << "kick tail (150-350 ms): classic " << juce::Decibels::gainToDecibels (tails[0]) << " dB, big room " << juce::Decibels::gainToDecibels (tails[1]) << " dB, peaks " << peaks[0] << " / " << peaks[1] << "\n";
            ok = ok && tails[1] > tails[0] * 2 && peaks[1] < 2.0f;
        }
        // Vocoder: both characters respond to a vowel-ish modulator and stay finite
        for (int ch = 0; ch < 2; ++ch)
        {
            auto fx = rs::createVocoder();
            setP (*fx, "character", (float) ch);
            auto* user = dynamic_cast<rs::SidechainUser*> (fx.get());
            fx->setPlayConfigDetails (2, 2, sr, block);
            fx->prepareToPlay (sr, block);
            juce::AudioBuffer<float> out (2, (int) sr), mod (2, block);
            juce::Random r (9);
            long long n = 0;
            for (int pos = 0; pos < out.getNumSamples(); pos += block)
            {
                const int len = juce::jmin (block, out.getNumSamples() - pos);
                for (int i = 0; i < len; ++i, ++n)
                {
                    const bool ess = (n / 12000) % 4 == 3;   // every 4th quarter second: an "sss"
                    const float v = ess ? 0.2f * (r.nextFloat() * 2 - 1) : 0.3f * (float) (std::sin (n * 0.04) + 0.6 * std::sin (n * 0.17) + 0.3 * std::sin (n * 0.33));
                    mod.setSample (0, i, v); mod.setSample (1, i, v);
                }
                user->sidechain = &mod;
                juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, len);
                juce::MidiBuffer m;
                if (pos == 0) for (int note : { 48, 55, 60, 64 }) m.addEvent (juce::MidiMessage::noteOn (1, note, 0.9f), 0);
                fx->processBlock (view, m);
                user->sidechain = nullptr;
            }
            bool finite = true;
            for (int c = 0; c < 2; ++c) for (int i = 0; i < out.getNumSamples(); ++i) finite = finite && std::isfinite (out.getSample (c, i));
            const float rms = out.getRMSLevel (0, 0, out.getNumSamples());
            std::cout << "vocoder " << (ch ? "vintage" : "modern") << ": " << juce::Decibels::gainToDecibels (rms) << " dB rms, peak " << out.getMagnitude (0, out.getNumSamples()) << (finite ? "" : " NON-FINITE") << "\n";
            ok = ok && finite && rms > 0.01f;
        }
        std::cout << (ok ? "FX2 OK\n" : "FX2 FAILED\n");
        return ok ? 0 : 1;
    }

    if (argc >= 3 && juce::String (argv[1]) == "--faces")
    {
        // renders every effect faceplate while music-like audio runs through it
        const double sr = 48000.0;
        const juce::File outDir (argv[2]);
        outDir.createDirectory();
        juce::StringArray ids { "eq", "tubeeq", "compressor", "reverb", "delay", "noisegate", "gater", "phaser", "chorus" };
        if (argc > 3) { ids.clear(); for (int a = 3; a < argc; ++a) ids.add (argv[a]); }
        for (auto& idStr : ids)
        {
            const char* id = idStr.toRawUTF8();
            auto fx = rs::createBuiltinFx (id);
            const bool sung = idStr == "autotune" || idStr == "tuner", bass = idStr == "subbass";
            const bool instrument = idStr == "organ" || idStr == "epiano" || idStr == "acid";
            const bool stereo = idStr == "vocalremove" || idStr == "analyzer";
            double ph = 0, ph2 = 0;
            auto set = [&] (const char* p, float v) { if (auto* q = fx->state.getParameter (p)) q->setValueNotifyingHost (q->convertTo0to1 (v)); };
            if (juce::String (id) == "eq") { set ("lowGain", 4); set ("midGain", -5); set ("midFreq", 900); set ("highGain", 3); set ("lowCut", 40); }
            if (juce::String (id) == "tubeeq") { set ("lowBoost", 6); set ("lowAtten", 3); set ("highBoost", 5); set ("drive", 6); }
            if (juce::String (id) == "compressor") { set ("threshold", -24); set ("ratio", 4); set ("makeup", 4); }
            if (juce::String (id) == "reverb") { set ("size", 70); set ("damping", 35); set ("predelay", 20); }
            if (juce::String (id) == "delay") { set ("sync", 0); set ("time", 320); set ("feedback", 55); set ("pingpong", 1); }
            if (juce::String (id) == "noisegate") { set ("threshold", -38); }
            if (juce::String (id) == "gater") { set ("pattern", 2); set ("length", 70); }
            if (idStr == "autotune") { set ("scale", 1); set ("key", 2); }
            if (idStr == "buscomp") { set ("threshold", -22); set ("ratio", 4); }
            if (idStr == "coloreq") { set ("model", 1); set ("lowgain", 4); set ("midgain", -3); set ("highgain", 5); set ("hpf", 40); }
            if (idStr == "deesser") { set ("threshold", -40); }
            if (idStr == "bitcrusher") { set ("bits", 5); set ("rate", 3000); set ("drive", 12); }
            if (idStr == "limiter") { set ("gain", 10); }
            if (idStr == "rotary") { set ("speed", 1); }
            if (idStr == "acid") { set ("seq", 0); }
            if (idStr == "multiband") { set ("thr1", -30); set ("thr2", -28); set ("thr3", -40); }
            if (idStr == "shimmer") { set ("mix", 60); }
            fx->setPlayConfigDetails (2, 2, sr, 512);
            fx->prepareToPlay (sr, 512);
            std::unique_ptr<juce::AudioProcessorEditor> ed (fx->createEditor());
            juce::Random rnd (3);
            long long n = 0;
            for (int frame = 0; frame < (sung ? 120 : 40); ++frame)
            {
                for (int b = 0; b < 3; ++b)   // ~32 ms of audio per frame
                {
                    juce::AudioBuffer<float> buf (2, 512);
                    for (int i = 0; i < 512; ++i, ++n)
                    {
                        const double t = std::fmod ((double) n / sr, 0.5);
                        const float env = t < 0.3 ? (float) std::exp (-t / 0.12) : 0.0f;
                        const double hz = sung ? 290.0 * std::pow (2.0, (0.6 * std::sin (n / sr * 0.9) + 0.08 * std::sin (n / sr * 30.0)) / 12.0)
                                               : bass ? 70.0 : 152.8;
                        ph += hz / sr;
                        const float tone = (float) std::sin (ph * juce::MathConstants<double>::twoPi);
                        const float v = sung ? 0.4f * tone + 0.003f * (rnd.nextFloat() * 2 - 1)
                                             : env * (0.5f * tone + 0.15f * (rnd.nextFloat() * 2 - 1)) + 0.002f * (rnd.nextFloat() * 2 - 1);
                        ph2 += 523.25 / sr;
                        const float other = stereo ? 0.25f * (float) std::sin (ph2 * juce::MathConstants<double>::twoPi) : 0.0f;
                        buf.setSample (0, i, instrument ? 0.0f : v + other); buf.setSample (1, i, instrument ? 0.0f : v);
                    }
                    juce::MidiBuffer mb;
                    if (instrument)
                    {
                        // a chord on the keys (the bass: a short line)
                        const long long start = n - 512;
                        const int frameNo = (int) (start / 512);
                        if (idStr == "acid") { static const int line[] { 36, 36, 48, 39, 36, 43, 46, 36 }; if (frameNo % 6 == 0) { mb.addEvent (juce::MidiMessage::noteOn (1, line[(frameNo / 6) % 8], (uint8_t) ((frameNo / 6) % 3 == 0 ? 120 : 80)), 0); } if (frameNo % 6 == 4) for (int k = 0; k < 128; ++k) mb.addEvent (juce::MidiMessage::noteOff (1, k), 0); }
                        else if (frameNo == 0) for (int note : { 48, 55, 60, 64, 67, 71 }) mb.addEvent (juce::MidiMessage::noteOn (1, note, (uint8_t) 105), 0);
                    }
                    fx->processBlock (buf, mb);
                }
                auto img = ed->createComponentSnapshot (ed->getLocalBounds(), true, 1.0f);
                if (frame == (sung ? 119 : 39) || frame == 31)
                {
                    juce::FileOutputStream os (outDir.getChildFile (juce::String (id) + (frame == 31 ? "_a" : "") + ".png"));
                    os.setPosition (0); os.truncate();
                    juce::PNGImageFormat().writeImageToStream (img, os);
                }
                juce::Thread::sleep (30);
            }
            std::cout << id << " " << ed->getWidth() << "x" << ed->getHeight() << "\n";
            ed.reset();
        }
        return 0;
    }

    if (argc >= 2 && juce::String (argv[1]) == "--devices")
    {
        const double sr = 48000.0;
        auto set = [] (rs::BuiltinFx& fx, const char* id, float v) { auto* p = fx.state.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
        auto prep = [sr] (rs::BuiltinFx& fx) { fx.setPlayConfigDetails (2, 2, sr, 512); fx.prepareToPlay (sr, 512); };
        auto process = [] (rs::BuiltinFx& fx, juce::AudioBuffer<float>& all, juce::MidiBuffer* midiAll = nullptr)
        {
            for (int pos = 0; pos < all.getNumSamples(); pos += 512)
            {
                const int len = juce::jmin (512, all.getNumSamples() - pos);
                juce::AudioBuffer<float> view (all.getArrayOfWritePointers(), 2, pos, len);
                juce::MidiBuffer mb;
                if (midiAll != nullptr) mb.addEvents (*midiAll, pos, len, -pos);
                fx.processBlock (view, mb);
            }
        };
        auto db = [] (float v) { return juce::Decibels::gainToDecibels (v, -150.0f); };
        auto finite = [] (juce::AudioBuffer<float>& b) { for (int c = 0; c < 2; ++c) for (int i = 0; i < b.getNumSamples(); ++i) if (! std::isfinite (b.getSample (c, i))) return false; return true; };

        // drum-ish hits: noise bursts with a 150 ms decay, 4 per second
        juce::AudioBuffer<float> hits (2, (int) sr * 2);
        {
            juce::Random rnd (1);
            for (int i = 0; i < hits.getNumSamples(); ++i)
            {
                const double t = std::fmod (i / sr, 0.25);
                const float v = (rnd.nextFloat() * 2 - 1) * 0.1f * (float) std::exp (-t / 0.05);
                hits.setSample (0, i, v); hits.setSample (1, i, v);
            }
        }
        auto peakToRms = [&] (juce::AudioBuffer<float>& b) { return db (b.getMagnitude (0, 0, b.getNumSamples())) - db (b.getRMSLevel (0, 0, b.getNumSamples())); };
        {
            auto punch = rs::createBuiltinFx ("punch");
            set (*punch, "attack", 0); set (*punch, "hdr", 0);
            prep (*punch); juce::AudioBuffer<float> a (hits); process (*punch, a);
            std::cout << "punch neutral: crest " << peakToRms (a) << " dB (input " << peakToRms (hits) << ")\n";
            set (*punch, "attack", 100); prep (*punch); juce::AudioBuffer<float> b (hits); process (*punch, b);
            std::cout << "punch attack +100: crest " << peakToRms (b) << " dB (more = punchier)\n";
            set (*punch, "attack", 0); set (*punch, "sustain", 100); prep (*punch); juce::AudioBuffer<float> c (hits); process (*punch, c);
            std::cout << "punch sustain +100: crest " << peakToRms (c) << " dB (less = more body)\n";
            set (*punch, "sustain", 0); set (*punch, "hdr", 100); prep (*punch); juce::AudioBuffer<float> d (hits); process (*punch, d);
            auto tail = [&] (juce::AudioBuffer<float>& b) { return db (b.getRMSLevel (0, (int) (0.40 * sr), (int) (0.08 * sr))); };
            std::cout << "punch HDR 100: quiet tail " << tail (hits) << " -> " << tail (d) << " dB, peak " << db (hits.getMagnitude (0, 0, hits.getNumSamples()))
                      << " -> " << db (d.getMagnitude (0, 0, d.getNumSamples())) << " dB, finite " << finite (d) << "\n";
        }
        {
            // a stereo pad: different sines left and right plus a shared one
            juce::AudioBuffer<float> st (2, (int) sr);
            for (int i = 0; i < st.getNumSamples(); ++i)
            {
                const double t = i / sr;
                const float m = 0.3f * (float) std::sin (2 * juce::MathConstants<double>::pi * 440 * t);
                st.setSample (0, i, m + 0.1f * (float) std::sin (2 * juce::MathConstants<double>::pi * 660 * t));
                st.setSample (1, i, m + 0.1f * (float) std::sin (2 * juce::MathConstants<double>::pi * 770 * t));
            }
            auto wide = rs::createBuiltinFx ("widener");
            prep (*wide); juce::AudioBuffer<float> o (st); process (*wide, o);
            double err = 0, ref = 0, cIn = 0, cOut = 0, lIn = 0, rIn = 0, lO = 0, rO = 0;
            for (int i = 4800; i < st.getNumSamples(); ++i)
            {
                const double mi = st.getSample (0, i) + st.getSample (1, i), mo = o.getSample (0, i) + o.getSample (1, i);
                err += (mi - mo) * (mi - mo); ref += mi * mi;
                cIn += st.getSample (0, i) * st.getSample (1, i); lIn += st.getSample (0, i) * st.getSample (0, i); rIn += st.getSample (1, i) * st.getSample (1, i);
                cOut += o.getSample (0, i) * o.getSample (1, i); lO += o.getSample (0, i) * o.getSample (0, i); rO += o.getSample (1, i) * o.getSample (1, i);
            }
            std::cout << "widener: mono sum difference " << db ((float) std::sqrt (err / ref)) << " dB (lower = mono-safe), correlation "
                      << cIn / std::sqrt (lIn * rIn) << " -> " << cOut / std::sqrt (lO * rO) << "\n";
        }
        {
            auto amp = rs::createBuiltinFx ("amp");
            for (int ch = 0; ch < 3; ++ch)
            {
                set (*amp, "channel", (float) ch); prep (*amp);
                juce::AudioBuffer<float> g (2, (int) sr);
                for (int i = 0; i < g.getNumSamples(); ++i) { const float v = 0.25f * (float) std::sin (2 * juce::MathConstants<double>::pi * 220 * i / sr); g.setSample (0, i, v); g.setSample (1, i, v); }
                process (*amp, g);
                // harmonic content: energy outside 220 Hz via a crude correlation
                double re = 0, im = 0, tot = 0;
                for (int i = (int) sr / 2; i < (int) sr; ++i)
                {
                    const double ph = 2 * juce::MathConstants<double>::pi * 220 * i / sr;
                    re += g.getSample (0, i) * std::cos (ph); im += g.getSample (0, i) * std::sin (ph); tot += g.getSample (0, i) * g.getSample (0, i);
                }
                const double fund = (re * re + im * im) * 2.0 / (sr / 2) * 2.0 / (sr / 2);
                const double meanSq = tot / (sr / 2);
                std::cout << "amp channel " << ch << ": out " << db (g.getRMSLevel (0, (int) sr / 2, (int) sr / 2) * 1.414f) << " dB, distortion "
                          << juce::roundToInt (100.0 * std::sqrt (juce::jmax (0.0, meanSq - fund * 0.5) / meanSq)) << "%, finite " << finite (g) << "\n";
            }
        }
        {
            auto kick = rs::createBuiltinFx ("kick");
            prep (*kick);
            juce::AudioBuffer<float> k (2, (int) sr);
            k.clear();
            juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 36, (juce::uint8) 120), 0);
            process (*kick, k, &m);
            // zero crossings between 150 and 250 ms give the settled pitch
            int zc = 0; for (int i = (int) (0.15 * sr); i < (int) (0.25 * sr); ++i) if ((k.getSample (0, i - 1) < 0) != (k.getSample (0, i) < 0)) ++zc;
            std::cout << "kick: peak " << db (k.getMagnitude (0, 0, (int) sr)) << " dB, pitch at 200 ms ~" << zc / 2.0 / 0.1 << " Hz (tune 50), level at 600 ms "
                      << db (k.getMagnitude (0, (int) (0.6 * sr), 2400)) << " dB\n";
        }
        {
            auto voc = rs::createBuiltinFx ("vocoder");
            auto* sc = dynamic_cast<rs::SidechainUser*> (voc.get());
            prep (*voc);
            juce::AudioBuffer<float> mod (2, 512);
            const int total = (int) sr;
            juce::AudioBuffer<float> out (2, total);
            juce::MidiBuffer chord;
            for (int nn : { 48, 55, 60, 64 }) chord.addEvent (juce::MidiMessage::noteOn (1, nn, (juce::uint8) 100), 0);
            float voicedRms = 0, silentRms = 0;
            for (int pass = 0; pass < 2; ++pass)
            {
                prep (*voc);
                sc->sidechainTrackId = 99;
                double ph = 0;
                for (int pos = 0; pos < total; pos += 512)
                {
                    // the "voice": a 140 Hz pulse train (glottal-ish), present only on pass 0
                    for (int i = 0; i < 512; ++i)
                    {
                        ph += 140.0 / sr; if (ph >= 1) ph -= 1;
                        const float v = pass == 0 ? (ph < 0.08 ? 0.6f : -0.05f) : 0.0f;
                        mod.setSample (0, i, v); mod.setSample (1, i, v);
                    }
                    sc->sidechain = &mod;
                    juce::AudioBuffer<float> view (out.getArrayOfWritePointers(), 2, pos, juce::jmin (512, total - pos));
                    juce::MidiBuffer mb;
                    if (pos == 0) mb = chord;
                    voc->processBlock (view, mb);
                }
                (pass == 0 ? voicedRms : silentRms) = out.getRMSLevel (0, total / 2, total / 2);
            }
            std::cout << "vocoder: chord + voice " << db (voicedRms * 1.414f) << " dB, chord + silence " << db (silentRms * 1.414f) << " dB\n";
        }
        {
            // sampler: the built-in sound is C4; C5 should sound an octave up
            auto smp = rs::createBuiltinFx ("sampler");
            prep (*smp);
            juce::AudioBuffer<float> o (2, (int) sr);
            o.clear();
            juce::MidiBuffer m; m.addEvent (juce::MidiMessage::noteOn (1, 72, (juce::uint8) 110), 0);
            m.addEvent (juce::MidiMessage::noteOff (1, 72), (int) (sr * 0.5));
            process (*smp, o, &m);
            int zc = 0; for (int i = (int) (0.1 * sr); i < (int) (0.3 * sr); ++i) if ((o.getSample (0, i - 1) < 0) != (o.getSample (0, i) < 0)) ++zc;
            std::cout << "sampler: C5 pitch ~" << zc / 2.0 / 0.2 << " Hz (expect 523), peak " << db (o.getMagnitude (0, 0, (int) sr))
                      << " dB, after release " << db (o.getMagnitude (0, (int) (0.95 * sr), 2400)) << " dB\n";
        }
        if (argc >= 3)
        {
            // master track automation: volume fades to silence over the song
            rs::AudioCache cache;
            rs::Project project (cache);
            rs::AudioEngine engine (project);
            project.load (juce::File (argv[2]));
            project.showMaster = true;
            rs::AutomationLane lane = rs::AutomationLane::fromId ("volume");
            lane.points = { { 0.0, rs::AutomationLane::volumeToValue (0.9f) }, { 8.0, 0.0f }, { 16.0, 0.0f } };
            project.master->automation.push_back (lane);
            auto f = juce::File (argv[2]).getSiblingFile ("master-auto.wav");
            juce::String err;
            engine.renderToFile (f, err);
            std::unique_ptr<juce::AudioFormatReader> rd (cache.formats.createReaderFor (f));
            juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
            rd->read (&b, 0, b.getNumSamples(), 0, true, true);
            const int spb = (int) (rd->sampleRate * 60.0 / project.bpm);
            std::cout << "master automation: rms beats 0-1 " << db (b.getRMSLevel (0, 0, spb)) << " dB, beats 4-5 " << db (b.getRMSLevel (0, 4 * spb, spb))
                      << " dB, beats 9-10 " << db (b.getRMSLevel (0, 9 * spb, spb)) << " dB\n";
            auto saved = juce::File (argv[2]).getSiblingFile ("master-test.renasound");
            project.save (saved);
            rs::Project p2 (cache);
            p2.load (saved);
            std::cout << "master reload: shown " << p2.showMaster << ", lanes " << p2.master->automation.size() << "\n";
        }
        if (argc >= 3)
        {
            // through the engine: the Pads track plays a vocoder listening to the (muted) drum loop track
            rs::AudioCache cache;
            rs::Project project (cache);
            rs::AudioEngine engine (project);
            if (! project.load (juce::File (argv[2]))) { std::cout << "load failed\n"; return 1; }
            rs::TrackPtr pads, drums;
            for (auto& t : project.tracks) { if (t->name == "Pads") pads = t; if (t->name == "Drums") drums = t; }
            juce::String err;
            auto slot = engine.createFx ("builtin:vocoder", nullptr, err);
            project.setInstrument (pads->id, slot);
            auto* sc = dynamic_cast<rs::SidechainUser*> (slot->processor.get());
            drums->mute = true;
            for (auto& t : project.tracks) if (t != pads && t != drums) t->mute = true;
            for (int pass = 0; pass < 2; ++pass)
            {
                sc->sidechainTrackId = pass == 0 ? drums->id : 0;
                auto f = juce::File (argv[2]).getSiblingFile (pass == 0 ? "vocoder_engine.wav" : "vocoder_none.wav");
                engine.renderToFile (f, err);
                std::unique_ptr<juce::AudioFormatReader> rd (cache.formats.createReaderFor (f));
                juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
                rd->read (&b, 0, b.getNumSamples(), 0, true, true);
                std::cout << "engine vocoder " << (pass == 0 ? "listening to Drums (muted)" : "no modulator") << ": peak " << db (b.getMagnitude (0, 0, b.getNumSamples())) << " dB\n";
            }
            // save / reload keeps the modulator choice
            sc->sidechainTrackId = drums->id;
            auto saved = juce::File (argv[2]).getSiblingFile ("vocoder-test.renasound");
            project.save (saved);
            project.load (saved);
            for (auto& t : project.tracks)
                if (t->name == "Pads" && t->instrument)
                    if (auto* u = dynamic_cast<rs::SidechainUser*> (t->instrument->processor.get()))
                        std::cout << "reloaded vocoder modulator track id " << u->sidechainTrackId.load() << " (expect " << drums->id << ")\n";
        }
        return 0;
    }

    if (argc >= 2 && juce::String (argv[1]) == "--studioeq")
    {
        const double sr = 48000.0;
        auto set = [] (rs::BuiltinFx& fx, const char* id, float v) { auto* p = fx.state.getParameter (id); p->setValueNotifyingHost (p->convertTo0to1 (v)); };
        // run a sine through, report output level (dBFS, last half) and the 2nd harmonic
        auto run = [sr] (rs::BuiltinFx& fx, double freq, float amp, double* h2 = nullptr)
        {
            fx.setPlayConfigDetails (2, 2, sr, 512);
            fx.prepareToPlay (sr, 512);
            const int total = (int) sr;
            juce::AudioBuffer<float> all (2, total);
            juce::MidiBuffer mb;
            for (int pos = 0; pos < total; pos += 512)
            {
                juce::AudioBuffer<float> b (2, 512);
                for (int i = 0; i < 512; ++i)
                {
                    const float x = amp * (float) std::sin (juce::MathConstants<double>::twoPi * freq * (pos + i) / sr);
                    b.setSample (0, i, x); b.setSample (1, i, x);
                }
                fx.processBlock (b, mb);
                all.copyFrom (0, pos, b, 0, 0, juce::jmin (512, total - pos));
            }
            const float rms = all.getRMSLevel (0, total / 2, total / 2);
            if (h2 != nullptr)
            {
                // correlate with the 2nd harmonic
                double re = 0, im = 0;
                for (int i = total / 2; i < total; ++i)
                {
                    const double ph = juce::MathConstants<double>::twoPi * 2 * freq * i / sr;
                    re += all.getSample (0, i) * std::cos (ph); im += all.getSample (0, i) * std::sin (ph);
                }
                *h2 = juce::Decibels::gainToDecibels ((float) (2.0 * std::sqrt (re * re + im * im) / (total / 2)) / amp, -150.0f);
            }
            return juce::Decibels::gainToDecibels (rms * 1.41421356f, -150.0f);
        };

        auto dyn = rs::createBuiltinFx ("dyneq");
        std::cout << "dynamic eq, flat:            1k -6 dB in -> " << run (*dyn, 1000, 0.5f) << "\n";
        set (*dyn, "gain3", 6.0f); set (*dyn, "freq3", 1000.0f);
        std::cout << "static +6 dB bell at 1k:     1k -6 dB in -> " << run (*dyn, 1000, 0.5f) << "   (expect 0)\n";
        set (*dyn, "gain3", 0.0f); set (*dyn, "dyn3", 1.0f); set (*dyn, "thr3", -30.0f); set (*dyn, "range3", -12.0f);
        std::cout << "dynamic cut, thr -30 range -12:\n";
        std::cout << "   loud 1k  (-6 dB in)  -> " << run (*dyn, 1000, 0.5f) << "   (expect -18)\n";
        std::cout << "   quiet 1k (-40 dB in) -> " << run (*dyn, 1000, 0.01f) << "   (expect -40)\n";
        std::cout << "   loud 80 Hz (-6 dB in)-> " << run (*dyn, 80, 0.5f) << "   (other frequencies untouched: expect about -6)\n";
        set (*dyn, "range3", 9.0f);
        std::cout << "dynamic boost, range +9: loud 1k -> " << run (*dyn, 1000, 0.5f) << "   (expect +3)\n";
        set (*dyn, "dyn3", 0.0f); set (*dyn, "hpf", 200.0f);
        std::cout << "low cut 200 Hz: 50 Hz -> " << run (*dyn, 50, 0.5f) << "  (12 dB/oct: expect about -30)\n";

        auto tube = rs::createBuiltinFx ("tubeeq");
        set (*tube, "drive", 0.0f);
        set (*tube, "lowBoost", 10.0f); set (*tube, "lowAtten", 10.0f);
        std::cout << "tube eq 60 Hz, boost 10 + atten 10 (drive off):\n";
        for (double f : { 30.0, 45.0, 60.0, 130.0, 300.0, 1000.0 })
            std::cout << "   " << f << " Hz: measured " << run (*tube, f, 0.1f) + 20.0f << " dB, curve "
                      << juce::Decibels::gainToDecibels ((float) tube->getMagnitudeForFrequency (f)) << " dB\n";
        set (*tube, "lowBoost", 0.0f); set (*tube, "lowAtten", 0.0f); set (*tube, "highBoost", 6.0f);
        std::cout << "high boost 6 at 10k: 10k " << run (*tube, 10000, 0.1f) + 20.0f << " dB, 1k " << run (*tube, 1000, 0.1f) + 20.0f << " dB\n";
        set (*tube, "highBoost", 0.0f);
        for (float d : { 0.0f, 3.0f, 7.0f, 10.0f })
        {
            set (*tube, "drive", d);
            double h2q = 0, h2l = 0;
            const float quiet = run (*tube, 200, 0.03f, &h2q), loud = run (*tube, 200, 0.7f, &h2l);
            std::cout << "tube drive " << d << ": quiet in -30.5 -> " << quiet << ", loud in -3.1 -> " << loud
                      << ", 2nd harmonic (loud) " << h2l << " dB\n";
        }
        return 0;
    }

    if (argc >= 4 && juce::String (argv[1]) == "--mixcraft")
    {
        auto imp = juce::File (argv[2]).hasFileExtension ("als") ? rs::importAbletonProject (juce::File (argv[2]))
                                                                 : rs::importMixcraftProject (juce::File (argv[2]));
        std::cout << "mixcraft import ok=" << imp.ok << " " << imp.error << " | " << imp.summary
                  << " | notes " << imp.numNotes << " | missing " << imp.missingFiles.size() << ": "
                  << imp.missingFiles.joinIntoString (", ").substring (0, 300) << " | " << imp.notes.joinIntoString (" / ") << "\n";
        if (! imp.ok) return 1;
        rs::AudioCache cache;
        rs::Project project (cache);
        rs::AudioEngine engine (project);
        project.fromXml (*imp.project);
        int clips = 0, fades = 0;
        for (auto& t : project.tracks)
        {
            std::cout << "  " << (t->type == rs::Track::Type::Audio ? "audio " : "instr ") << t->name << "  vol " << t->volume
                      << " pan " << t->pan << (t->mute ? " MUTED" : "") << "  clips " << t->clips.size();
            for (auto& l : t->automation) std::cout << "  [lane " << l.getId() << " " << l.points.size() << " pts, v@8=" << l.valueAt (8.0) << "]";
            std::cout << "\n";
            for (auto& c : t->clips) { ++clips; fades += c->fadeInSeconds > 0; }
        }
        std::cout << "loaded clips " << clips << ", crossfades " << fades << ", bpm " << project.bpm << ", end beat " << project.getEndBeat() << "\n";
        juce::String err;
        const bool rendered = engine.renderToFile (juce::File (argv[3]), err);
        std::unique_ptr<juce::AudioFormatReader> rd (cache.formats.createReaderFor (juce::File (argv[3])));
        if (rendered && rd != nullptr)
        {
            juce::AudioBuffer<float> b (2, (int) rd->lengthInSamples);
            rd->read (&b, 0, (int) rd->lengthInSamples, 0, true, true);
            std::cout << "rendered " << rd->lengthInSamples / rd->sampleRate << " s, peak " << b.getMagnitude (0, b.getNumSamples())
                      << ", rms " << b.getRMSLevel (0, 0, b.getNumSamples()) << "\n";
        }
        else std::cout << "render failed: " << err << "\n";
        return 0;
    }

    if (argc < 3) { std::cerr << "usage: RenderTest project.renasound out.wav\n"; return 2; }

    // ---- pitch editor DSP: a voice-like test signal with three notes ----
    {
        const double sr = 44100.0;
        auto voice = std::make_shared<rs::AudioData>();
        voice->sampleRate = sr;
        voice->buffer.setSize (2, (int) (sr * 3.0));
        voice->buffer.clear();
        const float notesMidi[] = { 57.0f, 60.3f, 63.8f };   // A3, C4 a bit sharp, E4 a bit flat
        double phase = 0;
        juce::Random rnd (1);
        for (int i = 0; i < voice->buffer.getNumSamples(); ++i)
        {
            const double t = i / sr;
            const int n = (int) (t / 1.0);
            const double inNote = t - n;
            const float env = inNote < 0.85 ? (float) juce::jmin (1.0, inNote / 0.02) : 0.0f;   // gaps between notes
            const double midi = notesMidi[n] + 0.3 * std::sin (2 * juce::MathConstants<double>::pi * 5.5 * t);  // vibrato
            const double f = 440.0 * std::pow (2.0, (midi - 69.0) / 12.0);
            phase += f / sr;
            float v = 0;
            for (int h = 1; h < 12; ++h) v += (float) (std::sin (2 * juce::MathConstants<double>::pi * phase * h) / h) * (h == 3 || h == 4 ? 1.6f : 1.0f);
            v *= 0.2f * env;
            voice->buffer.setSample (0, i, v);
            voice->buffer.setSample (1, i, v);
        }
        auto an = rs::pitchdsp::analyse (*voice);
        std::cout << "pitch: found " << an->notes.size() << " notes:";
        for (auto& n : an->notes) std::cout << " " << n.detected << " (" << n.start / sr << "-" << n.end / sr << "s)";
        std::cout << "\n";

        auto notes = an->notes;
        if (notes.size() == 3)
        {
            notes[0].shift = 2.0f;                                                    // A3 -> B3
            notes[1].shift = rs::pitchdsp::snapToScale (notes[1].detected, 0, 0) - notes[1].detected;   // to C4
            notes[2].shift = rs::pitchdsp::snapToScale (notes[2].detected, 0, 0) - notes[2].detected;   // to E4
            notes[2].drift = 1.0f;                                                    // and flatten its vibrato
            auto out = rs::pitchdsp::render (*voice, *an, notes);
            auto an2 = rs::pitchdsp::analyse (*out);
            std::cout << "after edit:";
            for (auto& n : an2->notes) std::cout << " " << n.detected;
            // vibrato depth of the last note before / after
            auto depth = [] (const rs::PitchAnalysis& a, double t0, double t1)
            {
                float lo = 200, hi = 0;
                for (int f = a.frameAt ((juce::int64) (t0 * 44100)); f < a.frameAt ((juce::int64) (t1 * 44100)); ++f)
                    if (a.voiced (f)) { lo = std::min (lo, a.pitch[(size_t) f]); hi = std::max (hi, a.pitch[(size_t) f]); }
                return hi - lo;
            };
            std::cout << "\nvibrato on 3rd note: before " << depth (*an, 2.2, 2.7) << " st, after " << depth (*an2, 2.2, 2.7)
                      << " st;  level before " << voice->buffer.getRMSLevel (0, 0, voice->buffer.getNumSamples())
                      << " after " << out->buffer.getRMSLevel (0, 0, out->buffer.getNumSamples()) << "\n";
            juce::WavAudioFormat wav;
            for (auto [name, buf] : { std::pair<const char*, juce::AudioBuffer<float>*> { "pitch_before.wav", &voice->buffer }, { "pitch_after.wav", &out->buffer } })
            {
                std::unique_ptr<juce::OutputStream> os (juce::File (argv[2]).getSiblingFile (name).createOutputStream());
                juce::File (argv[2]).getSiblingFile (name).deleteFile();
                os = juce::File (argv[2]).getSiblingFile (name).createOutputStream();
                auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (sr).withNumChannels (2).withBitsPerSample (16));
                w->writeFromAudioSampleBuffer (*buf, 0, buf->getNumSamples());
            }
        }
    }

    rs::AudioCache cache;
    rs::Project project (cache);
    rs::AudioEngine engine (project);

    if (! project.load (juce::File (argv[1]))) { std::cerr << "load failed\n"; return 1; }
    std::cout << "tracks: " << project.tracks.size() << "  end beat: " << project.getEndBeat() << "\n";

    juce::String err;
    if (! engine.renderToFile (juce::File (argv[2]), err)) { std::cerr << "render failed: " << err << "\n"; return 1; }

    std::unique_ptr<juce::AudioFormatReader> r (cache.formats.createReaderFor (juce::File (argv[2])));
    juce::AudioBuffer<float> b (2, (int) r->lengthInSamples);
    r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
    std::cout << "rendered seconds: " << r->lengthInSamples / r->sampleRate
              << "  peak: " << b.getMagnitude (0, b.getNumSamples())
              << "  rms 0-4s: " << b.getRMSLevel (0, 0, (int) (4 * r->sampleRate)) << "\n";

    // per-track solo renders to check each one makes sound
    for (auto& t : project.tracks)
    {
        for (auto& o : project.tracks) o->solo = (o == t);
        auto f = juce::File (argv[2]).getSiblingFile ("solo_" + t->name + ".wav");
        engine.renderToFile (f, err);
        std::unique_ptr<juce::AudioFormatReader> rr (cache.formats.createReaderFor (f));
        juce::AudioBuffer<float> bb (2, (int) rr->lengthInSamples);
        rr->read (&bb, 0, (int) rr->lengthInSamples, 0, true, true);
        std::cout << "  " << t->name << " peak " << bb.getMagnitude (0, bb.getNumSamples()) << "\n";
    }
    for (auto& o : project.tracks) o->solo = false;

    // editing + undo
    auto midi = project.tracks[1]->clips[0];
    const auto notesBefore = midi->notes.size();
    project.beginEdit();
    auto right = project.splitClip (midi->id, 8.0);
    std::cout << "split: left notes " << project.findClip (midi->id)->notes.size() << ", right notes " << (right ? right->notes.size() : 0) << "\n";
    project.undo();
    std::cout << "after undo: clips " << project.tracks[1]->clips.size() << ", notes " << project.findClip (midi->id)->notes.size()
              << " (was " << notesBefore << ")\n";
    project.redo();
    std::cout << "after redo: clips " << project.tracks[1]->clips.size() << "\n";

    // ---- simulated recording: feed fake input into the audio callback ----
    auto vocals = project.tracks[3];
    auto pads = project.tracks[1];
    pads->armed = true;
    engine.liveInputTrackId = pads->id;
    engine.setPositionBeats (4.0);
    engine.startRecording();
    std::cout << "recording: " << engine.isRecording() << "\n";

    const int block = 512;
    std::vector<float> inL (block), inR (block), outL (block), outR (block);
    const float* ins[] = { inL.data(), inR.data() };
    float* outs[] = { outL.data(), outR.data() };
    double phase = 0;
    for (int b = 0; b < 44100 * 2 / block; ++b)
    {
        for (int i = 0; i < block; ++i) { inL[(size_t) i] = inR[(size_t) i] = 0.5f * (float) std::sin (phase); phase += 2 * juce::MathConstants<double>::pi * 440.0 / 44100.0; }
        if (b == 10) engine.keyboardState.noteOn (1, 64, 0.9f);
        if (b == 60) engine.keyboardState.noteOff (1, 64, 0.0f);
        engine.audioDeviceIOCallbackWithContext (ins, 2, outs, 2, block, {});
    }
    engine.stop();
    std::cout << "after stop: vocals clips " << vocals->clips.size();
    if (! vocals->clips.empty() && vocals->clips[0]->audio != nullptr)
        std::cout << " start " << vocals->clips[0]->startBeat << " len " << vocals->clips[0]->lengthSeconds << "s peak "
                  << vocals->clips[0]->audio->buffer.getMagnitude (0, vocals->clips[0]->audio->buffer.getNumSamples())
                  << " file " << vocals->clips[0]->audio->file.getFileName();
    std::cout << "\npads clips " << pads->clips.size();
    if (pads->clips.size() > 2)
    {
        auto& c = pads->clips.back();
        std::cout << " new clip start " << c->startBeat << " notes " << c->notes.size();
        if (! c->notes.empty()) std::cout << " pitch " << c->notes[0].pitch << " at " << c->notes[0].start << " len " << c->notes[0].length;
    }
    std::cout << "\n";
    if (! vocals->clips.empty() && vocals->clips[0]->audio != nullptr) vocals->clips[0]->audio->file.deleteFile();

    // ---- every built-in effect, alone on the drums, then all on the master ----
    auto drums = project.tracks[0];
    for (auto& t : project.tracks) t->solo = (t == drums);
    for (auto& info : rs::getBuiltinFxList())
    {
        juce::String e;
        auto slot = engine.createFx ("builtin:" + info.id, nullptr, e);
        project.addFx (drums->id, slot);
        auto f = juce::File (argv[2]).getSiblingFile ("fx_" + info.id + ".wav");
        engine.renderToFile (f, e);
        std::unique_ptr<juce::AudioFormatReader> rr (cache.formats.createReaderFor (f));
        juce::AudioBuffer<float> bb (2, (int) rr->lengthInSamples);
        rr->read (&bb, 0, (int) rr->lengthInSamples, 0, true, true);
        bool finite = true;
        for (int ch = 0; ch < 2; ++ch) for (int i = 0; i < bb.getNumSamples(); ++i) finite = finite && std::isfinite (bb.getSample (ch, i));
        const int tail = (int) (rr->sampleRate * 8.5);
        std::cout << "  fx " << info.name << ": peak " << bb.getMagnitude (0, bb.getNumSamples())
                  << " rms " << bb.getRMSLevel (0, 0, (int) (4 * rr->sampleRate))
                  << " tail-rms " << bb.getRMSLevel (0, tail, bb.getNumSamples() - tail)
                  << (finite ? "" : " NON-FINITE!") << "\n";
        project.removeFx (drums->id, slot);
    }
    for (auto& t : project.tracks) t->solo = false;

    // settings persist through save / load, and undo keeps the same effect object
    {
        juce::String e;
        auto eq = engine.createFx ("builtin:eq", nullptr, e);
        dynamic_cast<rs::BuiltinFx*> (eq->processor.get())->state.getParameter ("lowGain")->setValueNotifyingHost (0.9f);
        project.addFx (drums->id, eq);
        project.addFx (-1, engine.createFx ("builtin:reverb", nullptr, e));
        auto saved = juce::File (argv[2]).getSiblingFile ("fx-test.renasound");
        project.save (saved);
        project.beginEdit();
        project.tracks[0]->volume = 0.1f;
        project.undo();
        std::cout << "undo kept same EQ object: " << (project.tracks[0]->fx.size() == 1 && project.tracks[0]->fx[0] == eq) << "\n";
        rs::Project p2 (cache);
        p2.createFx = project.createFx;
        p2.load (saved);
        auto* eq2 = dynamic_cast<rs::BuiltinFx*> (p2.tracks[0]->fx[0]->processor.get());
        std::cout << "reloaded: track fx " << p2.tracks[0]->fx.size() << ", master fx " << p2.masterFx.size()
                  << ", EQ low gain " << eq2->param ("lowGain") << " dB\n";
    }

    // ---- pitch edits survive save / load ----
    {
        auto clip = project.tracks[0]->clips[0];
        clip->pitch.active = true;
        clip->pitch.key = 9;
        clip->pitch.notes.push_back ({ 1000, 9000, 57.2f, 1.8f, 0.5f, 0.7f });
        auto saved = juce::File (argv[2]).getSiblingFile ("pitch-test.renasound");
        project.save (saved);
        rs::Project p4 (cache);
        p4.load (saved);
        auto& pe = p4.tracks[0]->clips[0]->pitch;
        std::cout << "pitch reload: active " << pe.active << " key " << pe.key << " notes " << pe.notes.size()
                  << " shift " << (pe.notes.empty() ? 0 : pe.notes[0].shift) << " same hash " << (pe.hash() == clip->pitch.hash()) << "\n";
        clip->pitch = {};
    }

    // ---- automation: fade the drums in over 4 bars, pan them hard left, automate an EQ ----
    {
        auto drumsT = project.tracks[0];
        for (auto& t : project.tracks) t->solo = (t == drumsT);
        drumsT->fx.clear();
        juce::String e;
        auto eq = engine.createFx ("builtin:eq", nullptr, e);
        project.addFx (drumsT->id, eq);
        rs::AutomationLane vol;            vol.points = { { 0.0, 0.0f }, { 16.0, rs::AutomationLane::volumeToValue (1.0f) } };
        rs::AutomationLane pan;            pan.kind = rs::AutomationLane::Kind::Pan; pan.points = { { 0.0, 0.0f } };
        auto eqLane = rs::AutomationLane::fromId ("fx:" + juce::String (eq->uid) + ":0");   // EQ low cut
        eqLane.points = { { 0.0, 0.0f }, { 16.0, 0.0f } };
        drumsT->automation = { vol, pan, eqLane };
        auto f = juce::File (argv[2]).getSiblingFile ("automation.wav");
        engine.renderToFile (f, e);
        std::unique_ptr<juce::AudioFormatReader> rr (cache.formats.createReaderFor (f));
        juce::AudioBuffer<float> bb (2, (int) rr->lengthInSamples);
        rr->read (&bb, 0, (int) rr->lengthInSamples, 0, true, true);
        const int sec = (int) rr->sampleRate;
        std::cout << "automation: L rms 0-1s " << bb.getRMSLevel (0, 0, sec) << ", 3-4s " << bb.getRMSLevel (0, 3 * sec, sec)
                  << ", 7-8s " << bb.getRMSLevel (0, 7 * sec, sec) << "; R rms 7-8s " << bb.getRMSLevel (1, 7 * sec, sec) << "\n";
        auto saved = juce::File (argv[2]).getSiblingFile ("auto-test.renasound");
        project.save (saved);
        rs::Project p5 (cache);
        p5.createFx = project.createFx;
        p5.load (saved);
        auto& a = p5.tracks[0]->automation;
        std::cout << "automation reload: lanes " << a.size() << (a.size() == 3 ? (" ids " + a[0].getId() + ", " + a[1].getId() + ", " + a[2].getId()) : juce::String())
                  << ", fx uid kept " << (p5.tracks[0]->fx.size() == 1 && p5.tracks[0]->getAutomationParameter (a[2]) != nullptr) << "\n";
        drumsT->automation.clear();
        drumsT->fx.clear();
        for (auto& t : project.tracks) t->solo = false;
    }

    // ---- third-party VST3 plugin (path given as 3rd argument) ----
    if (argc > 3)
    {
        juce::OwnedArray<juce::PluginDescription> found;
        for (auto* fmt : engine.pluginHost.formats.getFormats())
            fmt->findAllTypesForFile (found, argv[3]);
        std::cout << "plugin descriptions found: " << found.size() << "\n";
        if (! found.isEmpty())
        {
            juce::String e;
            auto slot = engine.createFx ("plugin", found[0], e);
            std::cout << "loaded plugin: " << (slot != nullptr ? slot->getName() : "FAILED " + e) << "\n";
            if (slot != nullptr)
            {
                project.tracks[0]->fx.clear();
                project.masterFx.clear();
                drums = project.tracks[0];   // the undo above rebuilt the track objects
                for (auto& t : project.tracks) t->solo = (t == drums);
                const float before = [&] {
                    auto f = juce::File (argv[2]).getSiblingFile ("vst_before.wav"); engine.renderToFile (f, e);
                    std::unique_ptr<juce::AudioFormatReader> rr (cache.formats.createReaderFor (f));
                    juce::AudioBuffer<float> bb (2, (int) rr->lengthInSamples); rr->read (&bb, 0, (int) rr->lengthInSamples, 0, true, true);
                    return bb.getRMSLevel (0, 0, bb.getNumSamples()); }();
                project.addFx (drums->id, slot);
                auto f = juce::File (argv[2]).getSiblingFile ("vst_after.wav"); engine.renderToFile (f, e);
                std::unique_ptr<juce::AudioFormatReader> rr (cache.formats.createReaderFor (f));
                juce::AudioBuffer<float> bb (2, (int) rr->lengthInSamples); rr->read (&bb, 0, (int) rr->lengthInSamples, 0, true, true);
                std::cout << "rms without plugin " << before << ", with plugin " << bb.getRMSLevel (0, 0, bb.getNumSamples()) << "\n";

                // plugin state round-trip through a saved project
                auto saved = juce::File (argv[2]).getSiblingFile ("vst-test.renasound");
                project.save (saved);
                rs::Project p3 (cache);
                p3.createFx = project.createFx;
                p3.load (saved);
                std::cout << "reloaded plugin: " << (p3.tracks[0]->fx.empty() ? "none" : p3.tracks[0]->fx[0]->getName()) << "\n";
            }
        }
    }
    return 0;
}
