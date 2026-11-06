#include "SoundMatch.h"
#include "Effects.h"
#include "SynthFx.h"
#include "Drums.h"
#include "MultiSampler.h"
#include <mutex>
#include <set>

namespace rs::soundmatch
{

namespace
{
    std::mutex installedLock;
    juce::Array<juce::PluginDescription> installed;
    juce::File gmFont;

    const char* const gmNames[128] = {
        "Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano", "Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavinet",
        "Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
        "Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
        "Nylon Guitar", "Steel Guitar", "Jazz Guitar", "Clean Guitar", "Muted Guitar", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
        "Acoustic Bass", "Finger Bass", "Pick Bass", "Fretless Bass", "Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
        "Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
        "String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2", "Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
        "Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
        "Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
        "Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
        "Square Lead", "Saw Lead", "Calliope Lead", "Chiff Lead", "Charang Lead", "Voice Lead", "Fifths Lead", "Bass + Lead",
        "New Age Pad", "Warm Pad", "Polysynth Pad", "Choir Pad", "Bowed Pad", "Metallic Pad", "Halo Pad", "Sweep Pad",
        "Rain", "Soundtrack", "Crystal", "Atmosphere", "Brightness", "Goblins", "Echoes", "Sci-fi",
        "Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bagpipe", "Fiddle", "Shanai",
        "Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
        "Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot"
    };

    // what names say. `special`: -1 none, 100 + n = RENASYNTH sound n, 200 = Acid Bass, 201 = reed electric piano,
    // 202 = combo organ (Italian), 203 = combo organ (American), 210..214 = RENASOUND Drums kit
    struct Word { const char* text; int gm; int special = -1; };
    const Word words[] = {
        // drums first: kits and kit pieces
        { "drum", 128 }, { "drums", 128 }, { "kit", 128 }, { "perc", 128 }, { "beat", 128 }, { "groove", 128 }, { "snare", 128 }, { "kick", 128 },
        { "hihat", 128 }, { "hi-hat", 128 }, { "hi hat", 128 }, { "hats", 128 }, { "clap", 128 }, { "cymbal", 128 }, { "toms", 128 }, { "break", 128 },
        { "808 kit", 128, 210 }, { "808 drums", 128, 210 }, { "tr-808", 128, 210 }, { "909", 128, 211 }, { "tr-909", 128, 211 }, { "trap", 128, 212 },
        { "lofi", 128, 213 }, { "lo-fi", 128, 213 }, { "lo fi", 128, 213 }, { "house", 128, 214 }, { "drum machine", 128, 210 },
        { "studio drums", 128 }, { "drum kit", 128 }, { "drum kits", 128 }, { "impulse", 128 }, { "drum rack", 128 }, { "melodic tom", 117 }, { "taiko", 116 },
        { "steel drum", 114 }, { "timpani", 47 },
        // keys
        { "piano", 0 }, { "grand", 0 }, { "acoustic piano", 0 }, { "grand piano", 0 }, { "concert grand", 0 }, { "upright", 0 }, { "bright piano", 1 },
        { "honky", 3 }, { "electric grand", 2 }, { "keys", 4 }, { "electric piano", 4 }, { "e-piano", 4 }, { "e piano", 4 }, { "epiano", 4 },
        { "rhodes", 4 }, { "lounge lizard", 4 }, { "mark i", 4 }, { "suitcase", 4 }, { "wurli", 4, 201 }, { "wurlitzer", 4, 201 }, { "reed piano", 4, 201 },
        { "dx7", 5 }, { "fm piano", 5 }, { "fm keys", 5 }, { "harpsichord", 6 }, { "clav", 7 }, { "clavinet", 7 },
        { "celesta", 8 }, { "glock", 9 }, { "glockenspiel", 9 }, { "music box", 10 }, { "vibraphone", 11 }, { "vibes", 11 }, { "marimba", 12 },
        { "xylophone", 13 }, { "bell", 14 }, { "bells", 14 }, { "tubular", 14 }, { "chime", 14 }, { "dulcimer", 15 }, { "mallet", 12 },
        // organs
        { "organ", 16 }, { "drawbar", 16 }, { "tonewheel", 16 }, { "hammond", 16 }, { "b3", 16 }, { "vb3", 16 }, { "jazz organ", 16 },
        { "percussive organ", 17 }, { "rock organ", 18 }, { "church", 19 }, { "pipe organ", 19 }, { "cathedral", 19 }, { "reed organ", 20 },
        { "combo organ", 20, 203 }, { "combo organ model v", 20, 203 }, { "combo organ model f", 20, 202 }, { "farfisa", 20, 202 }, { "vox continental", 20, 203 },
        { "accordion", 21 }, { "harmonica", 22 }, { "bandoneon", 23 },
        // guitars
        { "guitar", 27 }, { "nylon", 24 }, { "nylon guitar", 24 }, { "nylon string", 24 }, { "classical guitar", 24 }, { "spanish guitar", 24 }, { "acoustic guitar", 25 }, { "steel guitar", 25 },
        { "jazz guitar", 26 }, { "clean guitar", 27 }, { "muted guitar", 28 }, { "overdrive", 29 }, { "crunch", 29 }, { "distortion", 30 },
        { "dist guitar", 30 }, { "power chord", 30 }, { "harmonics", 31 },
        // bass
        { "bass", 33 }, { "acoustic bass", 32 }, { "upright bass", 32 }, { "double bass", 32 }, { "finger bass", 33 }, { "electric bass", 33 },
        { "pick bass", 34 }, { "fretless", 35 }, { "slap", 36 }, { "synth bass", 38 }, { "analog bass", 38 }, { "moog bass", 38 }, { "minimoog", 38 },
        { "acid", 38, 200 }, { "303", 38, 200 }, { "tb-303", 38, 200 }, { "alien 303", 38, 200 }, { "squelch", 38, 200 },
        { "sub bass", 39, 106 }, { "sub", 39, 106 }, { "808 bass", 39, 106 }, { "808", 39, 106 }, { "reese", 39, 108 },
        { "wobble", 39, 108 }, { "dubstep", 39, 108 }, { "growl", 39, 108 },
        // strings
        { "violin", 40 }, { "viola", 41 }, { "cello", 42 }, { "contrabass", 43 }, { "tremolo", 44 }, { "pizz", 45 }, { "pizzicato", 45 }, { "harp", 46 },
        { "string", 48 }, { "strings", 48 }, { "ensemble", 48 }, { "orchestra", 48 }, { "orchestral", 48 }, { "slow strings", 49 }, { "synth strings", 50 },
        { "string machine", 50 }, { "solina", 50 },
        // voices
        { "choir", 52 }, { "aah", 52 }, { "aahs", 52 }, { "ooh", 53 }, { "oohs", 53 }, { "vocal", 54 }, { "voice", 54 }, { "vox", 54 }, { "orchestra hit", 55 }, { "stab", 55 },
        // brass and winds
        { "trumpet", 56 }, { "trombone", 57 }, { "tuba", 58 }, { "muted trumpet", 59 }, { "french horn", 60 }, { "horn", 60 }, { "horns", 61 },
        { "brass", 61 }, { "brass section", 61 }, { "synth brass", 62 },
        { "sax", 65 }, { "saxophone", 65 }, { "soprano sax", 64 }, { "alto sax", 65 }, { "tenor sax", 66 }, { "tenor", 66 }, { "baritone sax", 67 }, { "bari sax", 67 },
        { "oboe", 68 }, { "english horn", 69 }, { "bassoon", 70 }, { "clarinet", 71 }, { "piccolo", 72 }, { "flute", 73 }, { "recorder", 74 },
        { "pan flute", 75 }, { "pan pipe", 75 }, { "panpipe", 75 }, { "bottle", 76 }, { "shakuhachi", 77 }, { "whistle", 78 }, { "ocarina", 79 },
        // synths
        { "square", 80 }, { "chip", 80 }, { "chiptune", 80 }, { "8bit", 80 }, { "8-bit", 80 }, { "8 bit", 80 }, { "lead", 81 }, { "saw", 81 }, { "sawtooth", 81 },
        { "synth lead", 81 }, { "mono synth", 81 }, { "monosynth", 81 }, { "calliope", 82 }, { "chiff", 83 }, { "charang", 84 }, { "fifths", 86 },
        { "supersaw", 90, 107 }, { "super saw", 90, 107 }, { "trance", 90, 107 }, { "hoover", 90, 107 }, { "poly", 90 }, { "polysynth", 90 }, { "stack", 90, 107 },
        { "pluck", 45, 102 }, { "plucked", 45, 102 }, { "plucks", 45, 102 }, { "arp", 45, 102 }, { "arpeggio", 45, 102 }, { "sequence", 45, 102 },
        { "pad", 89 }, { "pads", 89 }, { "warm pad", 89 }, { "soft pad", 89 }, { "new age", 88 }, { "evolving", 88, 113 }, { "atmos", 99, 113 },
        { "atmosphere", 99, 113 }, { "ambient", 88, 113 }, { "drone", 88, 113 }, { "texture", 88, 113 }, { "cinematic", 88, 113 }, { "sweep", 95 },
        { "halo", 94 }, { "metallic", 93 }, { "bowed", 92 }, { "crystal", 98 }, { "sci-fi", 103 }, { "fx", 102 }, { "riser", 102, 113 },
        // world
        { "sitar", 104 }, { "banjo", 105 }, { "shamisen", 106 }, { "koto", 107 }, { "guzheng", 107 }, { "zither", 107 }, { "kalimba", 108 },
        { "mbira", 108 }, { "loungelizard", 4 }, { "stringstudio", 25 }, { "tension", 25 }, { "collision", 11 }, { "bagpipe", 109 }, { "fiddle", 110 }, { "shanai", 111 }, { "tinkle", 112 }, { "agogo", 113 }, { "woodblock", 115 },
    };

    juce::String normalise (const juce::String& s)
    {
        // lower case, separators as single spaces, padded so " word " finds whole words
        juce::String out;
        for (auto c : s.toLowerCase())
            out << (juce::CharacterFunctions::isLetterOrDigit (c) || c == '-' || c == '+' ? juce::String::charToString (c) : juce::String (" "));
        while (out.contains ("  ")) out = out.replace ("  ", " ");
        return " " + out.trim() + " ";
    }

    // the best keyword in `text`: the longest one wins ("bass" loses to "synth bass")
    const Word* bestWord (const juce::String& text, int& strength)
    {
        const auto t = normalise (text);
        const Word* best = nullptr;
        strength = 0;
        for (auto& w : words)
        {
            const juce::String k (w.text);
            // whole words, or a word starting with it ("strings", "pianos"), never inside another word ("bassoon" isn't "bass")
            if (t.contains (" " + k + " ") || t.contains (" " + k + "s ") || (k.length() >= 5 && t.contains (" " + k)))
                if (k.length() > strength) { strength = k.length(); best = &w; }
        }
        return best;
    }

    int patchFor (int gm)
    {
        // RENASYNTH sounds: 0 Saw Lead, 1 Soft Pad, 2 Pluck, 3 Bass, 4 E-Piano, 5 Square Keys, 6 Sub Bass, 7 Super Saw,
        // 8 Wobble Bass, 9 Bell, 10 Brass, 11 Warm Strings, 12 Acid Ladder, 13 Evolving Pad, 14 Vibrato Lead
        if (gm == 6) return 2;
        if (gm >= 8 && gm <= 14) return 9;
        if (gm == 15) return 2;
        if (gm >= 24 && gm <= 28) return 2;
        if (gm == 29 || gm == 30) return 0;
        if (gm == 31) return 2;
        if (gm >= 32 && gm <= 38) return 3;
        if (gm == 39 || gm == 47) return 6;
        if (gm >= 40 && gm <= 44) return 11;
        if (gm == 45 || gm == 46) return 2;
        if (gm >= 48 && gm <= 51) return 11;
        if (gm >= 52 && gm <= 54) return 1;
        if (gm >= 55 && gm <= 63) return 10;
        if (gm >= 64 && gm <= 79) return 14;
        if (gm == 80) return 5;
        if (gm == 81 || gm == 84) return 0;
        if (gm == 82) return 14;
        if (gm == 83) return 2;
        if (gm == 85) return 1;
        if (gm == 86 || gm == 90) return 7;
        if (gm == 87) return 12;
        if (gm == 89 || gm == 91) return 1;
        if (gm == 92) return 11;
        if (gm == 98 || (gm >= 112 && gm <= 114)) return 9;
        if (gm >= 104 && gm <= 108) return 2;
        if (gm >= 109 && gm <= 111) return 14;
        if (gm >= 115 && gm <= 119) return 2;
        if (gm >= 88) return 13;
        return 4;
    }

    juce::MemoryBlock builtinState (const juce::String& id, std::initializer_list<std::pair<const char*, float>> params,
                                    std::function<void (juce::ValueTree&)> extra = {})
    {
        juce::MemoryBlock mb;
        auto fx = createBuiltinFx (id);
        if (fx == nullptr) return mb;
        for (auto& [pid, v] : params)
            if (auto* p = fx->state.getParameter (pid)) p->setValueNotifyingHost (p->convertTo0to1 (v));
        auto tree = fx->state.copyState();
        if (extra) extra (tree);
        if (auto xml = tree.createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, mb);
        return mb;
    }

    juce::MemoryBlock drumsState (int kit)
    {
        juce::MemoryBlock mb;
        DrumMachine d;
        d.loadKit (kit);
        d.getStateInformation (mb);
        return mb;
    }

    // a General MIDI bank, if the user has one: the preset for this program (-1 if none)
    int soundFontPreset (const juce::File& font, int gm)
    {
        if (! font.existsAsFile()) return -1;
        const auto list = msamp::sf2PresetList (font);
        int fallback = -1;
        for (int i = 0; i < (int) list.size(); ++i)
        {
            const auto& p = list[(size_t) i];
            if (gm == 128)
            {
                if (p.bank == 128 && p.program == 0) return i;
                if (p.bank >= 120 && fallback < 0) fallback = i;
            }
            else if (p.bank == 0 && p.program == gm) return i;
            else if (p.program == gm && p.bank < 120 && fallback < 0) fallback = i;
        }
        return fallback;
    }

    juce::String key (const juce::String& s) { return s.toLowerCase().retainCharacters ("abcdefghijklmnopqrstuvwxyz0123456789"); }
}

// ---------------------------------------------------------------------------
const char* gmName (int program)
{
    if (program == 128) return "Drum Kit";
    return gmNames[juce::jlimit (0, 127, program)];
}

int gmFromText (const juce::String& text, int* strength)
{
    int s = 0;
    const auto* w = bestWord (text, s);
    if (strength != nullptr) *strength = s;
    return w != nullptr ? w->gm : -1;
}

bool notesLookLikeDrums (const std::vector<NoteInfo>& notes)
{
    // drum parts: short hits on the General MIDI drum keys, a few pitches used over and over,
    // and several of them at once (kick + hat) - unlike a bass line in the same range
    if (notes.size() < 6) return false;
    int inMap = 0, shortNotes = 0, together = 0;
    std::set<int> pitches;
    auto sorted = notes;
    std::sort (sorted.begin(), sorted.end(), [] (auto& a, auto& b) { return a.start < b.start; });
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        const auto& n = sorted[i];
        if (n.pitch >= 35 && n.pitch <= 59) ++inMap;
        if (n.length <= 0.3) ++shortNotes;
        pitches.insert (n.pitch);
        if ((i > 0 && std::abs (sorted[i - 1].start - n.start) < 0.02 && sorted[i - 1].pitch != n.pitch)
            || (i + 1 < sorted.size() && std::abs (sorted[i + 1].start - n.start) < 0.02 && sorted[i + 1].pitch != n.pitch)) ++together;
    }
    int core = 0;
    for (int p : { 35, 36, 38, 40, 42, 44, 46, 49, 51 }) if (pitches.count (p)) ++core;
    const double nn = (double) notes.size();
    return inMap / nn > 0.85 && shortNotes / nn > 0.5 && pitches.size() <= 16 && core >= 3 && together / nn > 0.15;
}

int gmFromNotes (const std::vector<NoteInfo>& notes)
{
    if (notes.empty()) return 0;
    if (notesLookLikeDrums (notes)) return 128;

    std::vector<int> ps;
    double totalLen = 0;
    for (auto& n : notes) { ps.push_back (n.pitch); totalLen += n.length; }
    std::nth_element (ps.begin(), ps.begin() + (long) ps.size() / 2, ps.end());
    const int median = ps[ps.size() / 2];
    const double avgLen = totalLen / (double) notes.size();

    // how many notes sound at once, on average, when a note starts
    auto sorted = notes;
    std::sort (sorted.begin(), sorted.end(), [] (auto& a, auto& b) { return a.start < b.start; });
    double polySum = 0;
    size_t first = 0;
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        int sounding = 0;
        while (first < i && sorted[first].start + sorted[first].length <= sorted[i].start - 4.0) ++first;   // (notes ending long before can't overlap)
        for (size_t j = first; j <= i; ++j)
            if (sorted[j].start + sorted[j].length > sorted[i].start + 1.0e-6 || j == i) ++sounding;
        polySum += sounding;
    }
    const double poly = polySum / (double) sorted.size();

    if (median < 48 && poly < 1.5) return avgLen < 0.4 ? 38 : 33;   // bass
    if (poly >= 2.2) return avgLen >= 1.5 ? 89 : 0;                  // pad, or keys
    if (poly < 1.4 && median >= 57) return avgLen < 0.3 ? 45 : 81;   // pluck, or lead
    return 0;                                                       // keys
}

void setInstalledPlugins (const juce::Array<juce::PluginDescription>& list)
{
    const std::lock_guard<std::mutex> g (installedLock);
    installed = list;
}

std::function<void (const juce::File&)> onGmSoundFontChanged;

bool isGeneralMidiBank (const juce::File& f)
{
    // most of the 128 melodic programs in bank 0
    std::set<int> programs;
    for (auto& p : msamp::sf2PresetList (f)) if (p.bank == 0) programs.insert (p.program);
    return programs.size() >= 100;
}

void noticeSoundFont (const juce::File& f)
{
    if (! f.hasFileExtension ("sf2") || getGmSoundFont().existsAsFile() || ! isGeneralMidiBank (f)) return;
    setGmSoundFont (f);
    juce::MessageManager::callAsync ([f] { if (onGmSoundFontChanged) onGmSoundFontChanged (f); });
}

void setGmSoundFont (const juce::File& f) { const std::lock_guard<std::mutex> g (installedLock); gmFont = f; }
juce::File getGmSoundFont()               { const std::lock_guard<std::mutex> g (installedLock); return gmFont; }

// ---------------------------------------------------------------------------
Choice match (const Hints& h)
{
    Choice c;

    // 1. the very same plugin, if it's installed here
    if (h.device.isNotEmpty())
    {
        const std::lock_guard<std::mutex> g (installedLock);
        const auto want = key (h.device);
        for (auto& d : installed)
            if (d.isInstrument && want.length() >= 3 && (key (d.name) == want || key (d.descriptiveName) == want))
            {
                c.type = "plugin";
                c.plugin = d;
                c.state = h.pluginState;
                c.soundName = d.name + (h.pluginState.getSize() > 0 ? juce::String() : juce::String (" (default sound)"));
                c.because = "the same plugin is installed";
                return c;
            }
    }

    // 2. what the names say (the preset counts most, then the track, then the category and the instrument)
    int gm = -1, special = -1, best = 0;
    juce::String because;
    const std::pair<const juce::String*, const char*> sources[] = {
        { &h.preset, "the preset name" }, { &h.trackName, "the track name" }, { &h.device, "the instrument it used" }, { &h.category, "its category" } };
    int weight = 4;
    for (auto& [text, why] : sources)
    {
        int s = 0;
        if (text->isNotEmpty())
            if (const auto* w = bestWord (*text, s); w != nullptr && s * weight > best)
            {
                best = s * weight; gm = w->gm; special = w->special; because = why;
            }
        --weight;
    }
    if (h.icon.containsIgnoreCase ("drumkit") && gm < 0) { gm = 128; because = "its category"; }

    // a MIDI file's own program change / drum channel beats guessing from names
    if (h.gmDrums) { gm = 128; special = -1; because = "the MIDI drum channel"; }
    else if (h.gmProgram >= 0 && h.gmProgram < 128 && (gm < 0 || best < 4 * 5)) { gm = h.gmProgram; special = -1; because = "the MIDI file's instrument"; }

    // the notes can overrule a name that only half fits (a "Nightlife" preset, a track called "MIDI 3")
    if (gm < 0 && ! h.notes.empty()) { gm = gmFromNotes (h.notes); because = gm == 128 ? "its notes: a drum pattern" : "the notes it plays"; }
    if (gm < 0) { gm = 0; because = "a default"; }
    c.gmProgram = gm;

    // 3. a real sampled instrument from the user's General MIDI SoundFont, if there is one
    //    (except where RENASOUND has a dedicated instrument for it: organs, electric pianos, the 303)
    const bool dedicated = (gm >= 16 && gm <= 20) || (gm >= 4 && gm <= 5) || special == 200 || special == 201 || special == 202 || special == 203;
    if (! dedicated)
    {
        const auto font = getGmSoundFont();
        const int preset = soundFontPreset (font, gm);
        if (preset >= 0)
        {
            c = multiSamplerFrom (font, preset, because);
            c.gmProgram = gm;
            const auto list = msamp::sf2PresetList (font);
            c.soundName = list[(size_t) preset].name.trim() + " (" + font.getFileNameWithoutExtension() + ")";
            c.because = because;
            return c;
        }
    }

    // 4. RENASOUND's own instruments
    c.because = because;
    if (gm == 128)
    {
        int kit = special >= 210 && special <= 214 ? special - 210 : 0;
        c.type = "builtin:drums";
        c.state = drumsState (kit);
        c.soundName = "RENASOUND Drums: " + DrumMachine::getKitNames()[kit];
        return c;
    }
    if (special == 200)
    {
        c.type = "builtin:acid";
        c.state = builtinState ("acid", { { "seq", 0.0f } });   // the track's own notes play it
        c.soundName = "Acid Bass";
        return c;
    }
    if (special == 201 || (gm >= 0 && gm <= 5) || gm == 7)
    {
        c.type = "builtin:epiano";
        const bool reed = special == 201 || gm == 7;
        if (gm <= 3)       c.state = builtinState ("epiano", { { "model", 0.0f }, { "bell", 30.0f }, { "tone", 70.0f }, { "tremdepth", 0.0f } });
        else if (gm == 5)  c.state = builtinState ("epiano", { { "model", 0.0f }, { "bell", 85.0f } });
        else if (gm == 7)  c.state = builtinState ("epiano", { { "model", 1.0f }, { "bark", 80.0f }, { "tremdepth", 0.0f } });
        else               c.state = builtinState ("epiano", { { "model", reed ? 1.0f : 0.0f } });
        c.soundName = gm <= 3 ? "Electric Piano (piano setting)" : reed ? "Electric Piano (reed)" : "Electric Piano";
        return c;
    }
    if (special == 202 || special == 203 || (gm >= 16 && gm <= 23))
    {
        c.type = "builtin:organ";
        auto drawbars = [] (const char* reg) { std::vector<std::pair<juce::String, float>> v;
                                               for (int i = 0; i < 9; ++i) v.push_back ({ "db" + juce::String (i + 1), (float) (reg[i] - '0') }); return v; };
        std::vector<std::pair<juce::String, float>> ps;
        if (special == 202 || gm == 21 || gm == 22 || gm == 23) ps = { { "model", 1 }, { "rotary", 0 }, { "perc", 0 } };
        else if (special == 203 || gm == 20)                    ps = { { "model", 2 }, { "rotary", 0 }, { "perc", 0 } };
        else if (gm == 17) { ps = drawbars ("888000000"); ps.push_back ({ "perc", 1 }); }
        else if (gm == 18) { ps = drawbars ("888888000"); ps.push_back ({ "drive", 45 }); ps.push_back ({ "rotary", 2 }); }
        else if (gm == 19) { ps = drawbars ("888888888"); ps.push_back ({ "rotary", 0 }); ps.push_back ({ "vibrato", 0 }); ps.push_back ({ "click", 0 }); }
        auto fx = createBuiltinFx ("organ");
        if (fx != nullptr)
        {
            for (auto& [pid, v] : ps) if (auto* p = fx->state.getParameter (pid)) p->setValueNotifyingHost (p->convertTo0to1 (v));
            if (auto xml = fx->state.copyState().createXml()) juce::AudioProcessor::copyXmlToBinary (*xml, c.state);
        }
        c.soundName = gm == 19 ? "Organ (church)" : gm == 18 ? "Organ (rock)" : (special == 202 || special == 203 || (gm >= 20 && gm <= 23)) ? "Organ (combo)" : "Organ";
        return c;
    }

    c.synthPatch = special >= 100 && special < 100 + (int) getSynthPatches().size() ? special - 100 : patchFor (gm);
    c.soundName = getSynthPatches()[(size_t) c.synthPatch].name;
    return c;
}

Choice multiSamplerFrom (const juce::File& source, int sf2Preset, const juce::String& because)
{
    Choice c;
    c.type = "builtin:multisampler";
    c.state = builtinState ("multisampler", {}, [&] (juce::ValueTree& t)
    {
        t.setProperty ("source", source.getFullPathName(), nullptr);
        t.setProperty ("sf2preset", sf2Preset, nullptr);
    });
    c.soundName = "Multi Sampler: " + source.getFileNameWithoutExtension();
    c.because = because;
    return c;
}

void applyToTrackXml (const Choice& c, juce::XmlElement& te)
{
    te.setAttribute ("preset", c.type.isEmpty() ? c.synthPatch : 0);
    if (auto* old = te.getChildByName ("INSTRUMENT")) te.removeChildElement (old, true);
    if (c.type.isEmpty())
    {
        // RENASYNTH with this sound
        auto* ie = te.createNewChildElement ("INSTRUMENT");
        ie->setAttribute ("type", "builtin:synth");
        ie->setAttribute ("bypass", false);
        auto synth = createSynth();
        loadSynthPatch (synth.get(), c.synthPatch);
        juce::MemoryBlock mb;
        synth->getStateInformation (mb);
        ie->setAttribute ("state", mb.toBase64Encoding());
        return;
    }
    auto* ie = te.createNewChildElement ("INSTRUMENT");
    ie->setAttribute ("type", c.type);
    ie->setAttribute ("bypass", false);
    ie->setAttribute ("state", c.state.getSize() > 0 ? c.state.toBase64Encoding() : juce::String());
    if (c.type == "plugin")
        if (auto d = c.plugin.createXml()) ie->addChildElement (d.release());
}

juce::String describe (const Hints&, const Choice& c)
{
    return c.soundName + "   (from " + c.because + ")";
}

} // namespace rs::soundmatch
