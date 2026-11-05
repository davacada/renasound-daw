#include "../Source/Effects.h"
#include "../Source/Devices.h"
// Headless check: loads a project, renders it, verifies the audio and editing ops.
#include "../Source/AudioEngine.h"
#include "../Source/Pitch.h"
#include "../Source/MixcraftImport.h"
#include "../Source/AbletonImport.h"
#include "../Source/Drums.h"

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
    if (! vocals->clips.empty())
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
    if (! vocals->clips.empty()) vocals->clips[0]->audio->file.deleteFile();

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
