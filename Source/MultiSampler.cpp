#include "ReleasePool.h"
#include "MultiSampler.h"
#include "SoundMatch.h"
#include "DspHelpers.h"
#include "NcwReader.h"
#include "UI/Faceplate.h"
#include <map>
#include <set>
#include <mutex>

namespace rs::msamp
{

// =============================================================================
//  Loading one sound
// =============================================================================
SamplePtr loadSample (const juce::File& f, juce::AudioFormatManager& formats, juce::String& error)
{
    auto smp = std::make_shared<SampleData>();
    smp->file = f;
    if (f.hasFileExtension ("ncw"))
    {
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb)) { error = "Couldn't read " + f.getFileName(); return nullptr; }
        auto a = ncw::decode ((const uint8_t*) mb.getData(), mb.getSize());
        if (! a.error.empty()) { error = f.getFileName() + ": " + juce::String (a.error); return nullptr; }
        const int len = (int) a.data[0].size();
        smp->buffer.setSize (2, len);
        for (int c = 0; c < 2; ++c)
            std::memcpy (smp->buffer.getWritePointer (c), a.data[(size_t) juce::jmin (c, a.channels - 1)].data(), sizeof (float) * (size_t) len);
        smp->rate = a.sampleRate;
        return smp;
    }
    std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0) { error = "Couldn't read " + f.getFileName(); return nullptr; }
    const int len = (int) std::min<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 120.0));
    smp->buffer.setSize (2, len);
    r->read (&smp->buffer, 0, len, 0, true, true);
    if (r->numChannels == 1) smp->buffer.copyFrom (1, 0, smp->buffer, 0, 0, len);
    smp->rate = r->sampleRate;
    // loop points and root note stored in the file (WAV "smpl" chunk)
    const auto& md = r->metadataValues;
    if (md.getValue ("NumSampleLoops", "0").getIntValue() > 0)
    {
        smp->loopStart = md.getValue ("Loop0Start", "-1").getIntValue();
        smp->loopEnd = md.getValue ("Loop0End", "-1").getIntValue();
        if (smp->loopEnd <= smp->loopStart + 16 || smp->loopEnd >= len) smp->loopStart = smp->loopEnd = -1;
    }
    if (md.containsKey ("MidiUnityNote")) smp->unityNote = md.getValue ("MidiUnityNote", "-1").getIntValue();
    return smp;
}

// =============================================================================
//  What a file name says
// =============================================================================
namespace
{
    // "A3", "C#4", "Db2", "Cis1", "Es3", "H2", "a-1" -> MIDI note (C4 = 60), or -1
    int noteFromToken (const juce::String& t)
    {
        if (t.length() < 2 || t.length() > 6) return -1;
        static const int base[] { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
        const auto c0 = juce::CharacterFunctions::toUpperCase (t[0]);
        int pc;
        if (c0 >= 'A' && c0 <= 'G') pc = base[c0 - 'A'];
        else if (c0 == 'H') pc = 11;   // German B
        else return -1;
        int i = 1;
        auto rest = t.substring (1);
        auto low = rest.toLowerCase();
        if (low.startsWith ("is")) { pc += 1; i += 2; }
        else if (low.startsWith ("es") && c0 != 'E') { pc -= 1; i += 2; }
        else if (low.startsWith ("s") && (c0 == 'A' || c0 == 'E')) { pc -= 1; i += 1; }   // As, Es
        else if (rest[0] == '#') { pc += 1; i += 1; }
        else if (rest[0] == 'b' && rest.length() > 1) { pc -= 1; i += 1; }
        else if (c0 == 'B' && false) {}
        const auto oct = t.substring (i);
        if (oct.isEmpty() || ! (oct.containsOnly ("-0123456789")) || oct == "-") return -1;
        const int o = oct.getIntValue();
        if (o < -1 || o > 9) return -1;
        const int n = (o + 1) * 12 + pc;
        return n >= 0 && n <= 127 ? n : -1;
    }

    int dynFromToken (const juce::String& lowTok)
    {
        static const char* d[] { "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff" };
        for (int i = 0; i < 8; ++i) if (lowTok == d[i]) return i;
        for (auto pre : { "vel", "dyn", "layer", "v", "l" })
            if (lowTok.startsWith (pre) && lowTok.length() > (int) strlen (pre) && lowTok.substring ((int) strlen (pre)).containsOnly ("0123456789"))
                return 100 + lowTok.substring ((int) strlen (pre)).getIntValue();
        return -1;
    }
}

NameInfo parseName (const juce::String& nameIn)
{
    NameInfo ni;
    juce::StringArray toks;
    toks.addTokens (nameIn, " _-.()[]+", "");
    // (a "-1" octave got split by the "-": put "C" "1" back as "C-1")
    toks.removeEmptyStrings();
    for (int i = 0; i < toks.size(); ++i)
    {
        const auto t = toks[i];
        const auto low = t.toLowerCase();
        if (ni.note < 0) if (const int n = noteFromToken (t); n >= 0) { ni.note = n; continue; }
        if (ni.dyn < 0) if (const int d = dynFromToken (low); d >= 0) { ni.dyn = d; continue; }
        if (low.startsWith ("rr") && low.substring (2).containsOnly ("0123456789") && low.length() > 2) { ni.rr = low.substring (2).getIntValue(); continue; }
        for (auto pre : { "seq", "alt", "var", "take" })
            if (low.startsWith (pre) && low.length() > (int) strlen (pre) && low.substring ((int) strlen (pre)).containsOnly ("0123456789")) { ni.rr = low.substring ((int) strlen (pre)).getIntValue(); goto next; }
        if (low == "release" || low == "rel" || low == "rls" || low == "releases") { ni.release = true; continue; }
        if (low.containsOnly ("0123456789")) { ni.trailingNumber = low.getIntValue(); continue; }
        {
            // a word with a number on the end ("dirt4"): the word, and the number as a take
            auto word = low.trimCharactersAtEnd ("0123456789");
            if (word.length() < low.length() && word.isNotEmpty() && ni.rr == 0) ni.trailingNumber = low.substring (word.length()).getIntValue();
            if (word.isNotEmpty()) ni.words.add (word);
        }
        next:;
    }
    return ni;
}

// YIN-style pitch: the strongest repeat between 40 Hz and 2 kHz, in a steady part of the sound
double detectPitchHz (const juce::AudioBuffer<float>& b, double rate)
{
    const int len = b.getNumSamples();
    const int win = 4096;
    if (len < win + 64) return 0;
    // the loudest stretch after the attack
    int start = juce::jmin (len - win, (int) (rate * 0.12));
    float best = 0;
    for (int s = start; s + win <= len && s < (int) (rate * 1.5); s += win / 2)
    {
        const float r = b.getRMSLevel (0, s, win);
        if (r > best) { best = r; start = s; }
    }
    if (best < 1.0e-4f) return 0;
    std::vector<float> x ((size_t) win);
    for (int i = 0; i < win; ++i) x[(size_t) i] = 0.5f * (b.getSample (0, start + i) + b.getSample (1, start + i));
    const int minLag = (int) (rate / 2000.0), maxLag = juce::jmin (win / 2, (int) (rate / 40.0));
    std::vector<double> d ((size_t) maxLag + 1, 0.0);
    for (int tau = 1; tau <= maxLag; ++tau)
    {
        double s = 0;
        for (int i = 0; i < win - maxLag; ++i) { const double v = x[(size_t) i] - x[(size_t) (i + tau)]; s += v * v; }
        d[(size_t) tau] = s;
    }
    double run = 0;
    std::vector<double> cm ((size_t) maxLag + 1, 1.0);
    for (int tau = 1; tau <= maxLag; ++tau) { run += d[(size_t) tau]; cm[(size_t) tau] = run > 0 ? d[(size_t) tau] * tau / run : 1.0; }
    int pick = -1;
    for (int tau = minLag; tau < maxLag; ++tau)
        if (cm[(size_t) tau] < 0.15) { while (tau + 1 < maxLag && cm[(size_t) tau + 1] < cm[(size_t) tau]) ++tau; pick = tau; break; }
    if (pick < 0)
    {
        double lo = 1e9;
        for (int tau = minLag; tau < maxLag; ++tau) if (cm[(size_t) tau] < lo) { lo = cm[(size_t) tau]; pick = tau; }
        if (lo > 0.4) return 0;
    }
    // parabolic refinement
    double t = pick;
    if (pick > 1 && pick < maxLag)
    {
        const double a = cm[(size_t) pick - 1], bb = cm[(size_t) pick], c = cm[(size_t) pick + 1];
        const double den = a - 2 * bb + c;
        if (std::abs (den) > 1e-12) t += 0.5 * (a - c) / den;
    }
    return rate / t;
}

// =============================================================================
//  Mapping a set of files by their names
// =============================================================================
namespace
{
    bool isAudio (const juce::File& f)
    {
        return f.hasFileExtension ("wav;aif;aiff;flac;ogg;mp3;ncw");
    }

    // key ranges: each sampled note reaches halfway to its neighbours
    std::map<int, std::pair<int, int>> spreadKeys (const std::set<int>& roots)
    {
        std::map<int, std::pair<int, int>> out;
        std::vector<int> r (roots.begin(), roots.end());
        for (size_t i = 0; i < r.size(); ++i)
        {
            const int lo = i == 0 ? 0 : (r[i - 1] + r[i]) / 2 + 1;
            const int hi = i + 1 == r.size() ? 127 : (r[i] + r[i + 1]) / 2;
            out[r[i]] = { lo, hi };
        }
        return out;
    }
}

InstrumentPtr mapFiles (const juce::Array<juce::File>& filesIn, const juce::File& source, std::atomic<float>* progress, juce::String& error)
{
    juce::Array<juce::File> files;
    for (auto& f : filesIn) if (isAudio (f) && ! f.getParentDirectory().getFileName().containsIgnoreCase ("IR Samples") && ! f.getFileName().startsWithChar ('.')) files.add (f);
    if (files.isEmpty()) { error = "No sounds found there."; return nullptr; }
    files.sort();

    struct Item { juce::File file; NameInfo ni; juce::String artic; bool named = false; };
    std::vector<Item> items;
    bool anyNote = false;
    std::map<juce::String, int> wordCount;
    const bool manyFolders = [&] { for (auto& f : files) if (f.getParentDirectory() != files[0].getParentDirectory()) return true; return false; }();
    for (auto& f : files)
    {
        Item it { f, parseName (f.getFileNameWithoutExtension()), {} };
        anyNote = anyNote || it.ni.note >= 0;
        std::set<juce::String> seen (it.ni.words.begin(), it.ni.words.end());
        for (auto& w : seen) wordCount[w]++;
        items.push_back (it);
    }
    if (! anyNote)
    {
        // no note names: use a number in the name as the MIDI note ("Piano_060.wav")
        for (auto& it : items) if (it.ni.trailingNumber >= 0 && it.ni.trailingNumber <= 127) { it.ni.note = it.ni.trailingNumber; it.ni.trailingNumber = -1; anyNote = true; }
    }
    // words in every file name ("strohgeige") are the instrument's name, not an articulation
    juce::StringArray common;
    for (auto& [w, n] : wordCount) if (n == (int) items.size()) common.add (w);
    auto inst = std::make_shared<Instrument>();
    inst->source = source;
    inst->name = common.isEmpty() ? source.getFileNameWithoutExtension() : common.joinIntoString (" ").toUpperCase().substring (0, 1) + common.joinIntoString (" ").substring (1);
    if (inst->name.isEmpty()) inst->name = source.getFileNameWithoutExtension();

    for (auto& it : items)
    {
        juce::StringArray w;
        for (auto& x : it.ni.words) if (! common.contains (x)) w.add (x);
        auto artic = w.joinIntoString (" ");
        if (manyFolders && it.file.getParentDirectory() != source) artic = (it.file.getParentDirectory().getFileName() + " " + artic).trim();
        if (artic.isEmpty()) artic = "Sustain";
        it.artic = artic.substring (0, 1).toUpperCase() + artic.substring (1);
    }
    // the articulation list: "Sustain" first, then by size (most samples first)
    std::map<juce::String, int> articSize;
    for (auto& it : items) articSize[it.artic]++;
    std::vector<std::pair<juce::String, int>> order (articSize.begin(), articSize.end());
    std::stable_sort (order.begin(), order.end(), [] (auto& a, auto& b)
    {
        if ((a.first == "Sustain") != (b.first == "Sustain")) return a.first == "Sustain";
        return a.second > b.second;
    });
    for (auto& [name, n] : order) inst->articulations.add (name);

    // load every sound (the slow part)
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::vector<SamplePtr> loaded (items.size());
    for (size_t i = 0; i < items.size(); ++i)
    {
        juce::String e;
        loaded[i] = loadSample (items[i].file, formats, e);
        if (progress != nullptr) *progress = 0.9f * (float) (i + 1) / (float) items.size();
        if (juce::Thread::currentThreadShouldExit()) { error = "Cancelled"; return nullptr; }
    }
    for (auto& it : items) it.named = it.ni.note >= 0;
    // a sound with no note in its name: the file's own root, or listen to it
    for (size_t i = 0; i < items.size(); ++i)
        if (items[i].ni.note < 0 && loaded[i] != nullptr)
        {
            if (loaded[i]->unityNote >= 0) items[i].ni.note = loaded[i]->unityNote;
            else if (items.size() == 1) items[i].ni.note = 60;
            else if (const double hz = detectPitchHz (loaded[i]->buffer, loaded[i]->rate); hz > 0)
                items[i].ni.note = juce::jlimit (0, 127, (int) std::lround (69 + 12 * std::log2 (hz / 440.0)));
        }

    // do the names agree with what the sounds actually are? Some libraries number octaves differently:
    // listen to a spread of the sounds and correct whole octaves when most of them agree.
    {
        std::map<int, int> votes;
        int asked = 0;
        const size_t stride = std::max<size_t> (1, items.size() / 60);
        for (size_t i = 0; i < items.size(); i += stride)
        {
            if (! items[i].named || loaded[i] == nullptr || items[i].ni.release) continue;
            const double hz = detectPitchHz (loaded[i]->buffer, loaded[i]->rate);
            if (hz <= 0) continue;
            const double heard = 69 + 12 * std::log2 (hz / 440.0);
            const double diff = heard - items[i].ni.note;
            const int oct = (int) std::lround (diff / 12.0);
            if (std::abs (diff - oct * 12.0) < 1.5) votes[oct]++;   // only count clear octave relationships
            ++asked;
        }
        int bestOct = 0, bestN = 0;
        for (auto& [o, n] : votes) if (n > bestN) { bestN = n; bestOct = o; }
        if (bestOct != 0 && asked > 0 && bestN * 10 >= asked * 6)
        {
            inst->octaveCorrection = bestOct;
            for (auto& it : items) if (it.named) it.ni.note = juce::jlimit (0, 127, it.ni.note + 12 * bestOct);
        }
    }

    // sounds placed by listening, in a library that's mostly named: fold them by octaves into the instrument's range
    {
        int lo = 127, hi = 0;
        for (auto& it : items) if (it.named) { lo = juce::jmin (lo, it.ni.note); hi = juce::jmax (hi, it.ni.note); }
        if (hi >= lo)
            for (auto& it : items)
                if (! it.named && it.ni.note >= 0)
                {
                    while (it.ni.note < lo && it.ni.note + 12 <= 127) it.ni.note += 12;
                    while (it.ni.note > hi && it.ni.note - 12 >= lo) it.ni.note -= 12;
                }
    }

    // build the zones, articulation by articulation
    int maxLayers = 0, maxRR = 0;
    for (int a = 0; a < inst->articulations.size(); ++a)
    {
        const auto& artic = inst->articulations[a];
        for (int rel = 0; rel < 2; ++rel)
        {
            std::vector<size_t> mine;
            for (size_t i = 0; i < items.size(); ++i)
                if (items[i].artic == artic && items[i].ni.release == (rel == 1) && items[i].ni.note >= 0 && loaded[i] != nullptr) mine.push_back (i);
            if (mine.empty()) continue;
            std::set<int> roots, dyns;
            for (auto i : mine) { roots.insert (items[i].ni.note); dyns.insert (items[i].ni.dyn); }
            const auto keys = spreadKeys (roots);
            // where each loudness sits on the 1..127 scale (evenly spread over all the layers this articulation has)
            std::vector<int> dynList (dyns.begin(), dyns.end());
            std::map<int, double> centre;
            for (size_t k = 0; k < dynList.size(); ++k) centre[dynList[k]] = 1.0 + 126.0 * (k + 0.5) / dynList.size();
            maxLayers = juce::jmax (maxLayers, (int) dynList.size());
            for (int root : roots)
            {
                // the layers this note has: split the velocity range between them (no gaps if one is missing)
                std::set<int> here;
                for (auto i : mine) if (items[i].ni.note == root) here.insert (items[i].ni.dyn);
                std::vector<int> hl (here.begin(), here.end());
                std::map<int, std::pair<int, int>> velRange;
                for (size_t k = 0; k < hl.size(); ++k)
                {
                    const int lo = k == 0 ? 1 : (int) std::floor ((centre[hl[k - 1]] + centre[hl[k]]) * 0.5) + 1;
                    const int hi = k + 1 == hl.size() ? 127 : (int) std::floor ((centre[hl[k]] + centre[hl[k + 1]]) * 0.5);
                    velRange[hl[k]] = { lo, hi };
                }
                for (int dy : hl)
                {
                    // the takes of this note at this loudness: explicit RR numbers, or numbered files in order
                    std::vector<size_t> takes;
                    for (auto i : mine) if (items[i].ni.note == root && items[i].ni.dyn == dy) takes.push_back (i);
                    std::stable_sort (takes.begin(), takes.end(), [&] (size_t x, size_t y)
                    {
                        const auto kx = items[x].ni.rr > 0 ? items[x].ni.rr : items[x].ni.trailingNumber, ky = items[y].ni.rr > 0 ? items[y].ni.rr : items[y].ni.trailingNumber;
                        return kx < ky;
                    });
                    maxRR = juce::jmax (maxRR, (int) takes.size());
                    for (size_t t = 0; t < takes.size(); ++t)
                    {
                        const auto i = takes[t];
                        Zone z;
                        z.sample = loaded[i];
                        z.root = root;
                        z.loKey = keys.at (root).first; z.hiKey = keys.at (root).second;
                        z.loVel = velRange[dy].first; z.hiVel = velRange[dy].second;
                        z.rr = takes.size() > 1 ? (int) t + 1 : 0;
                        z.articulation = a;
                        z.release = rel == 1;
                        z.loopStart = loaded[i]->loopStart; z.loopEnd = loaded[i]->loopEnd;
                        z.name = items[i].file.getFileNameWithoutExtension();
                        inst->zones.push_back (z);
                    }
                }
            }
        }
    }
    // keyswitches: below the lowest playable note, one per articulation (C0 upwards, if there's room)
    if (inst->articulations.size() > 1)
    {
        int lowest = 127;
        for (auto& z : inst->zones) lowest = juce::jmin (lowest, z.root);
        const int startKey = juce::jlimit (0, 127, juce::jmin (24, lowest - 2 - inst->articulations.size()));
        for (int a = 0; a < inst->articulations.size(); ++a) inst->keyswitches.push_back (startKey + a < lowest ? startKey + a : -1);
        // the lowest sampled zone shouldn't reach down over the keyswitches
        for (auto& z : inst->zones) if (z.loKey <= startKey + inst->articulations.size() - 1) z.loKey = juce::jmin (z.root, startKey + inst->articulations.size());
    }
    else inst->keyswitches.assign ((size_t) inst->articulations.size(), -1);

    inst->velocityLayers = maxLayers;
    inst->roundRobins = maxRR;
    std::set<const SampleData*> uniq;
    for (auto& z : inst->zones) if (uniq.insert (z.sample.get()).second) inst->bytes += (size_t) z.sample->buffer.getNumSamples() * 2 * sizeof (float);
    inst->samples = (int) uniq.size();
    if (inst->zones.empty()) { error = "Couldn't work out which notes these sounds are."; return nullptr; }
    if (progress != nullptr) *progress = 1.0f;
    return inst;
}

InstrumentPtr loadFolder (const juce::File& folder, std::atomic<float>* progress, juce::String& error)
{
    juce::Array<juce::File> files;
    for (const auto& e : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        if (isAudio (e.getFile())) files.add (e.getFile());
    // a library folder: its sounds are usually in "Samples"
    return mapFiles (files, folder, progress, error);
}

// =============================================================================
//  SFZ
// =============================================================================
namespace
{
    int sfzNote (const juce::String& v)
    {
        const auto t = v.trim();
        if (t.containsOnly ("-0123456789")) return t.getIntValue();
        return noteFromToken (t);   // SFZ: c4 = 60, the same as ours
    }

    juce::String readSfzText (const juce::File& f, std::map<juce::String, juce::String>& defines, int depth = 0)
    {
        juce::String out;
        juce::StringArray lines;
        lines.addLines (f.loadFileAsString());
        for (auto line : lines)
        {
            if (const int c = line.indexOf ("//"); c >= 0) line = line.substring (0, c);
            auto t = line.trim();
            if (t.startsWith ("#define"))
            {
                juce::StringArray p; p.addTokens (t, " \t", "\""); p.removeEmptyStrings();
                if (p.size() >= 3) defines[p[1]] = p[2];
                continue;
            }
            if (t.startsWith ("#include"))
            {
                const auto name = t.fromFirstOccurrenceOf ("\"", false, false).upToLastOccurrenceOf ("\"", false, false);
                if (depth < 8 && name.isNotEmpty()) out << readSfzText (f.getParentDirectory().getChildFile (name.replaceCharacter ('\\', '/')), defines, depth + 1) << "\n";
                continue;
            }
            for (auto& [k, v] : defines) line = line.replace (k, v);
            out << line << "\n";
        }
        return out;
    }
}

InstrumentPtr loadSfz (const juce::File& sfz, std::atomic<float>* progress, juce::String& error)
{
    std::map<juce::String, juce::String> defines;
    const auto text = readSfzText (sfz, defines);
    using Opcodes = std::map<juce::String, juce::String>;
    Opcodes control, global, master, group, region;
    std::vector<Opcodes> regions;
    juce::String level;

    // walk the text: headers <...> and opcode=value (values may contain spaces, up to the next opcode)
    auto flushRegion = [&] { if (level == "region") { Opcodes all = global; for (auto& [k, v] : master) all[k] = v; for (auto& [k, v] : group) all[k] = v; for (auto& [k, v] : region) all[k] = v; for (auto& [k, v] : control) if (! all.count (k)) all[k] = v; regions.push_back (all); } };
    int i = 0;
    const int n = text.length();
    while (i < n)
    {
        const auto ch = text[i];
        if (ch == '<')
        {
            const int close = text.indexOf (i, ">");
            if (close < 0) break;
            flushRegion();
            level = text.substring (i + 1, close).trim().toLowerCase();
            if (level == "control") control.clear();
            if (level == "global") { global.clear(); master.clear(); group.clear(); }
            if (level == "master") { master.clear(); group.clear(); }
            if (level == "group") group.clear();
            region.clear();
            i = close + 1;
            continue;
        }
        if (juce::CharacterFunctions::isWhitespace (ch)) { ++i; continue; }
        const int eq = text.indexOf (i, "=");
        if (eq < 0) break;
        const auto key = text.substring (i, eq).trim().toLowerCase();
        // the value runs until the next "word=" or "<" or end of line
        int end = eq + 1;
        int lineEnd = text.indexOf (eq, "\n"); if (lineEnd < 0) lineEnd = n;
        int nextHeader = text.indexOf (eq, "<"); if (nextHeader < 0 || nextHeader > lineEnd) nextHeader = lineEnd;
        end = nextHeader;
        for (int k = eq + 1; k < nextHeader; ++k)
            if (text[k] == '=')
            {
                int s = k - 1;
                while (s > eq && ! juce::CharacterFunctions::isWhitespace (text[s])) --s;
                end = s;
                break;
            }
        const auto value = text.substring (eq + 1, end).trim();
        Opcodes* target = level == "region" ? &region : level == "group" ? &group : level == "master" ? &master : level == "global" ? &global : &control;
        (*target)[key] = value;
        i = end;
    }
    flushRegion();
    if (regions.empty()) { error = "No regions in that SFZ file."; return nullptr; }

    auto inst = std::make_shared<Instrument>();
    inst->source = sfz;
    inst->name = sfz.getFileNameWithoutExtension();
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::map<juce::String, SamplePtr> cache;
    std::map<int, int> switchToArtic;   // keyswitch note -> articulation
    std::map<juce::String, std::vector<int>> randGroups;
    int done = 0;
    for (auto& r : regions)
    {
        auto get = [&r] (const char* k, const juce::String& def = {}) { auto it = r.find (k); return it != r.end() ? it->second : def; };
        auto samplePath = get ("sample");
        if (samplePath.isEmpty()) continue;
        auto path = (get ("default_path") + samplePath).replaceCharacter ('\\', '/');
        const auto file = juce::File::isAbsolutePath (path) ? juce::File (path) : sfz.getParentDirectory().getChildFile (path);
        SamplePtr smp;
        if (auto it = cache.find (file.getFullPathName()); it != cache.end()) smp = it->second;
        else
        {
            juce::String e;
            smp = loadSample (file, formats, e);
            cache[file.getFullPathName()] = smp;
            if (smp == nullptr) { if (error.isEmpty()) error = e; }
        }
        if (progress != nullptr) *progress = 0.95f * (float) (++done) / (float) regions.size();
        if (smp == nullptr) continue;
        Zone z;
        z.sample = smp;
        z.name = file.getFileNameWithoutExtension();
        if (get ("key").isNotEmpty()) { z.root = z.loKey = z.hiKey = sfzNote (get ("key")); }
        if (get ("lokey").isNotEmpty()) z.loKey = sfzNote (get ("lokey"));
        if (get ("hikey").isNotEmpty()) z.hiKey = sfzNote (get ("hikey"));
        if (get ("pitch_keycenter").isNotEmpty()) z.root = sfzNote (get ("pitch_keycenter"));
        else if (get ("key").isEmpty()) z.root = smp->unityNote >= 0 ? smp->unityNote : 60;
        z.loVel = juce::jlimit (1, 127, get ("lovel", "1").getIntValue());
        z.hiVel = juce::jlimit (1, 127, get ("hivel", "127").getIntValue());
        z.tuneCents = get ("tune", "0").getFloatValue() + 100.0f * get ("transpose", "0").getFloatValue();
        z.gainDb = get ("volume", "0").getFloatValue();
        z.offset = get ("offset", "0").getIntValue();
        if (get ("end").isNotEmpty()) z.end = get ("end").getIntValue();
        z.keyTrack = get ("pitch_keytrack", "100").getFloatValue() != 0.0f;
        z.release = get ("trigger") == "release";
        const auto loopMode = get ("loop_mode", get ("loopmode"));
        const auto ls = get ("loop_start", get ("loopstart")), le = get ("loop_end", get ("loopend"));
        if (loopMode == "loop_continuous" || loopMode == "loop_sustain")
        {
            z.loopStart = ls.isNotEmpty() ? ls.getIntValue() : smp->loopStart;
            z.loopEnd = le.isNotEmpty() ? le.getIntValue() : smp->loopEnd;
            if (z.loopEnd <= z.loopStart) z.loopStart = z.loopEnd = -1;
        }
        else if (loopMode.isEmpty() && smp->loopStart >= 0) { z.loopStart = smp->loopStart; z.loopEnd = smp->loopEnd; }
        z.oneShot = loopMode == "one_shot";
        if (get ("seq_length").getIntValue() > 1) z.rr = juce::jmax (1, get ("seq_position", "1").getIntValue());
        if (get ("lorand").isNotEmpty() || get ("hirand").isNotEmpty())
        {
            // random round robins: rank them by where their slice starts
            const int slot = (int) std::lround (get ("lorand", "0").getFloatValue() * 1000.0);
            z.rr = 1000 + slot;
        }
        if (get ("sw_last").isNotEmpty())
        {
            const int ks = sfzNote (get ("sw_last"));
            auto it = switchToArtic.find (ks);
            if (it == switchToArtic.end())
            {
                const auto label = get ("sw_label", get ("group_label", juce::MidiMessage::getMidiNoteName (ks, true, true, 4)));
                inst->articulations.add (label);
                inst->keyswitches.push_back (ks);
                it = switchToArtic.emplace (ks, inst->articulations.size() - 1).first;
            }
            z.articulation = it->second;
        }
        if (inst->attack < 0 && get ("ampeg_attack").isNotEmpty())  inst->attack = get ("ampeg_attack").getFloatValue() * 1000.0f;
        if (inst->decay < 0 && get ("ampeg_decay").isNotEmpty())    inst->decay = get ("ampeg_decay").getFloatValue() * 1000.0f;
        if (inst->sustain < 0 && get ("ampeg_sustain").isNotEmpty()) inst->sustain = get ("ampeg_sustain").getFloatValue();
        if (inst->release < 0 && get ("ampeg_release").isNotEmpty()) inst->release = get ("ampeg_release").getFloatValue() * 1000.0f;
        inst->zones.push_back (z);
    }
    if (inst->zones.empty()) { if (error.isEmpty()) error = "None of the SFZ's sounds could be found."; return nullptr; }
    // random round robins -> 1, 2, 3 ... in order
    {
        std::set<int> slots;
        for (auto& z : inst->zones) if (z.rr >= 1000) slots.insert (z.rr);
        std::map<int, int> rank; int k = 1; for (int s : slots) rank[s] = k++;
        for (auto& z : inst->zones) if (z.rr >= 1000) z.rr = rank[z.rr];
    }
    if (inst->articulations.isEmpty()) { inst->articulations.add ("Default"); inst->keyswitches.push_back (-1); }
    std::set<std::pair<int, int>> layers;
    std::set<const SampleData*> uniq;
    for (auto& z : inst->zones)
    {
        layers.insert ({ z.loVel, z.hiVel });
        inst->roundRobins = juce::jmax (inst->roundRobins, z.rr);
        if (uniq.insert (z.sample.get()).second) inst->bytes += (size_t) z.sample->buffer.getNumSamples() * 2 * sizeof (float);
    }
    inst->velocityLayers = (int) layers.size();
    inst->samples = (int) uniq.size();
    error.clear();
    if (progress != nullptr) *progress = 1.0f;
    return inst;
}

// =============================================================================
//  SoundFont (SF2)
// =============================================================================
namespace
{
    struct Sf2Header { std::string name; int start, end, loopStart, loopEnd, rate, root, corr, type; };
    struct Sf2Gen { int oper; int amount; };   // amount: signed, or (lo | hi << 8) for ranges
    struct Sf2Zone { std::vector<Sf2Gen> gens; };
    struct Sf2Preset { std::string name; int program, bank; std::vector<Sf2Zone> zones; };
    struct Sf2Inst { std::string name; std::vector<Sf2Zone> zones; };

    struct Sf2File
    {
        juce::File file;
        std::vector<int16_t> pcm;
        std::vector<Sf2Header> samples;
        std::vector<Sf2Preset> presets;   // sorted by bank, then program
        std::vector<Sf2Inst> insts;
        std::mutex m;
        std::map<int, SamplePtr> converted;   // each sound turned into floats once, when a preset first uses it

        SamplePtr sample (int idx)
        {
            const std::lock_guard<std::mutex> g (m);
            if (auto it = converted.find (idx); it != converted.end()) return it->second;
            const auto& h = samples[(size_t) idx];
            const int len = juce::jmax (0, juce::jmin (h.end, (int) pcm.size()) - h.start);
            auto smp = std::make_shared<SampleData>();
            smp->file = file;
            smp->rate = h.rate > 0 ? h.rate : 44100;
            smp->buffer.setSize (2, juce::jmax (1, len));
            smp->buffer.clear();
            auto* d = smp->buffer.getWritePointer (0);
            for (int i = 0; i < len; ++i) d[i] = pcm[(size_t) (h.start + i)] / 32768.0f;
            smp->buffer.copyFrom (1, 0, smp->buffer, 0, 0, juce::jmax (1, len));
            converted[idx] = smp;
            return smp;
        }
    };

    std::shared_ptr<Sf2File> openSf2 (const juce::File& f, juce::String& error)
    {
        static std::mutex cacheLock;
        static std::map<juce::String, std::weak_ptr<Sf2File>> cache;
        const auto key = f.getFullPathName() + "|" + juce::String (f.getLastModificationTime().toMilliseconds());
        {
            const std::lock_guard<std::mutex> g (cacheLock);
            if (auto it = cache.find (key); it != cache.end()) if (auto p = it->second.lock()) return p;
        }
        juce::MemoryBlock mb;
        if (! f.loadFileAsData (mb) || mb.getSize() < 12) { error = "Couldn't read " + f.getFileName(); return nullptr; }
        const auto* d = (const uint8_t*) mb.getData();
        const size_t size = mb.getSize();
        auto u32 = [d] (size_t at) { return (uint32_t) d[at] | ((uint32_t) d[at + 1] << 8) | ((uint32_t) d[at + 2] << 16) | ((uint32_t) d[at + 3] << 24); };
        auto u16 = [d] (size_t at) { return (uint16_t) (d[at] | (d[at + 1] << 8)); };
        auto tag = [d] (size_t at) { return std::string ((const char*) d + at, 4); };
        if (tag (0) != "RIFF" || tag (8) != "sfbk") { error = f.getFileName() + " isn't a SoundFont."; return nullptr; }
        auto sf = std::make_shared<Sf2File>();
        sf->file = f;
        std::map<std::string, std::pair<size_t, size_t>> chunks;   // pdta sub-chunks and smpl: where and how big
        size_t pos = 12;
        while (pos + 8 <= size)
        {
            const auto id = tag (pos); const size_t len = u32 (pos + 4);
            if (id == "LIST" && pos + 12 <= size)
            {
                const auto kind = tag (pos + 8);
                size_t sub = pos + 12, endL = juce::jmin (size, pos + 8 + len);
                while (sub + 8 <= endL)
                {
                    const auto sid = tag (sub); const size_t slen = u32 (sub + 4);
                    if (sub + 8 + slen > size) break;
                    if (kind == "sdta" || kind == "pdta") chunks[sid] = { sub + 8, slen };
                    sub += 8 + slen + (slen & 1);
                }
            }
            pos += 8 + len + (len & 1);
        }
        for (auto need : { "smpl", "phdr", "pbag", "pgen", "inst", "ibag", "igen", "shdr" })
            if (! chunks.count (need)) { error = f.getFileName() + ": a damaged SoundFont (no " + need + ")."; return nullptr; }
        {
            auto [at, len] = chunks["smpl"];
            sf->pcm.resize (len / 2);
            std::memcpy (sf->pcm.data(), d + at, (len / 2) * 2);   // little-endian 16-bit, as on every platform we build for
        }
        auto readGens = [&] (const char* bagId, const char* genId, int first, int last, std::vector<Sf2Zone>& zones)
        {
            auto [bagAt, bagLen] = chunks[bagId]; auto [genAt, genLen] = chunks[genId];
            const int bags = (int) (bagLen / 4), gens = (int) (genLen / 4);
            for (int b = first; b < last && b + 1 < bags; ++b)
            {
                Sf2Zone z;
                const int g0 = u16 (bagAt + (size_t) b * 4), g1 = u16 (bagAt + (size_t) (b + 1) * 4);
                for (int gi = g0; gi < g1 && gi < gens; ++gi)
                    z.gens.push_back ({ (int) u16 (genAt + (size_t) gi * 4), (int) (int16_t) u16 (genAt + (size_t) gi * 4 + 2) });
                zones.push_back (std::move (z));
            }
        };
        {
            auto [at, len] = chunks["shdr"];
            for (size_t r = 0; r + 1 < len / 46; ++r)   // (the last record is a terminator)
            {
                const size_t o = at + r * 46;
                Sf2Header h;
                h.name = std::string ((const char*) d + o, strnlen ((const char*) d + o, 20));
                h.start = (int) u32 (o + 20); h.end = (int) u32 (o + 24); h.loopStart = (int) u32 (o + 28); h.loopEnd = (int) u32 (o + 32);
                h.rate = (int) u32 (o + 36); h.root = d[o + 40]; h.corr = (int) (int8_t) d[o + 41]; h.type = u16 (o + 44);
                sf->samples.push_back (h);
            }
        }
        {
            auto [at, len] = chunks["inst"];
            const int n = (int) (len / 22);
            for (int r = 0; r + 1 < n; ++r)
            {
                const size_t o = at + (size_t) r * 22;
                Sf2Inst in;
                in.name = std::string ((const char*) d + o, strnlen ((const char*) d + o, 20));
                readGens ("ibag", "igen", u16 (o + 20), u16 (o + 22 + 20), in.zones);
                sf->insts.push_back (std::move (in));
            }
        }
        {
            auto [at, len] = chunks["phdr"];
            const int n = (int) (len / 38);
            for (int r = 0; r + 1 < n; ++r)
            {
                const size_t o = at + (size_t) r * 38;
                Sf2Preset p;
                p.name = std::string ((const char*) d + o, strnlen ((const char*) d + o, 20));
                p.program = u16 (o + 20); p.bank = u16 (o + 22);
                readGens ("pbag", "pgen", u16 (o + 24), u16 (o + 38 + 24), p.zones);
                sf->presets.push_back (std::move (p));
            }
            std::stable_sort (sf->presets.begin(), sf->presets.end(), [] (const Sf2Preset& a, const Sf2Preset& b) { return a.bank != b.bank ? a.bank < b.bank : a.program < b.program; });
        }
        if (sf->presets.empty()) { error = f.getFileName() + " has no presets."; return nullptr; }
        const std::lock_guard<std::mutex> g (cacheLock);
        cache[key] = sf;
        return sf;
    }

    enum { G_StartOff = 0, G_EndOff = 1, G_LoopStartOff = 2, G_LoopEndOff = 3, G_StartCoarse = 4, G_EndCoarse = 12, G_Pan = 17,
           G_Attack = 34, G_Decay = 36, G_Sustain = 37, G_Release = 38, G_Instrument = 41, G_KeyRange = 43, G_VelRange = 44,
           G_LoopStartCoarse = 45, G_Atten = 48, G_LoopEndCoarse = 50, G_Coarse = 51, G_Fine = 52, G_SampleId = 53, G_Modes = 54,
           G_ScaleTune = 56, G_RootKey = 58 };

    const Sf2Gen* findGen (const Sf2Zone& z, int oper) { for (auto& g : z.gens) if (g.oper == oper) return &g; return nullptr; }
}

std::vector<Sf2PresetInfo> sf2PresetList (const juce::File& file)
{
    std::vector<Sf2PresetInfo> out;
    juce::String error;
    if (auto sf = openSf2 (file, error))
        for (auto& p : sf->presets) out.push_back ({ p.bank, p.program, juce::String (p.name) });
    return out;
}

InstrumentPtr loadSf2 (const juce::File& file, int presetIndex, std::atomic<float>* progress, juce::String& error)
{
    auto sf = openSf2 (file, error);
    if (sf == nullptr) return nullptr;
    auto inst = std::make_shared<Instrument>();
    inst->source = file;
    for (auto& p : sf->presets)
        inst->sf2Presets.add (juce::String (p.bank).paddedLeft ('0', 3) + ":" + juce::String (p.program).paddedLeft ('0', 3) + "  " + juce::String (p.name));
    presetIndex = juce::jlimit (0, (int) sf->presets.size() - 1, presetIndex);
    inst->sf2Preset = presetIndex;
    const auto& preset = sf->presets[(size_t) presetIndex];
    inst->name = juce::String (preset.name);
    inst->articulations.add ("Default");
    inst->keyswitches.push_back (-1);

    // a value for a generator: the instrument zone's, else the instrument's global zone, else a default
    auto get = [] (const Sf2Zone& z, const Sf2Zone* glob, int oper, int def)
    {
        if (auto* g = findGen (z, oper)) return g->amount;
        if (glob != nullptr) if (auto* g = findGen (*glob, oper)) return g->amount;
        return def;
    };
    auto range = [] (int amount) { return std::make_pair (amount & 0xff, (amount >> 8) & 0xff); };
    const Sf2Zone* pGlobal = (! preset.zones.empty() && findGen (preset.zones[0], G_Instrument) == nullptr) ? &preset.zones[0] : nullptr;
    int done = 0, total = 0;
    for (auto& pz : preset.zones) if (findGen (pz, G_Instrument)) ++total;
    bool envSet = false;
    for (auto& pz : preset.zones)
    {
        const auto* ig = findGen (pz, G_Instrument);
        if (ig == nullptr || ig->amount < 0 || ig->amount >= (int) sf->insts.size()) continue;
        const auto& in = sf->insts[(size_t) ig->amount];
        const Sf2Zone* iGlobal = (! in.zones.empty() && findGen (in.zones[0], G_SampleId) == nullptr) ? &in.zones[0] : nullptr;
        // the preset zone adds to (or narrows) what the instrument says
        auto pAdd = [&] (int oper) { if (auto* g = findGen (pz, oper)) return g->amount; if (pGlobal) if (auto* g = findGen (*pGlobal, oper)) return g->amount; return 0; };
        const auto pKeys = range (findGen (pz, G_KeyRange) ? findGen (pz, G_KeyRange)->amount : (pGlobal && findGen (*pGlobal, G_KeyRange) ? findGen (*pGlobal, G_KeyRange)->amount : (127 << 8)));
        const auto pVels = range (findGen (pz, G_VelRange) ? findGen (pz, G_VelRange)->amount : (pGlobal && findGen (*pGlobal, G_VelRange) ? findGen (*pGlobal, G_VelRange)->amount : (127 << 8)));
        for (auto& iz : in.zones)
        {
            const auto* sg = findGen (iz, G_SampleId);
            if (sg == nullptr || sg->amount < 0 || sg->amount >= (int) sf->samples.size()) continue;
            const auto& h = sf->samples[(size_t) sg->amount];
            if (h.type & 0x8000) continue;   // ROM samples: not in the file
            const auto keys = range (get (iz, iGlobal, G_KeyRange, 127 << 8));
            const auto vels = range (get (iz, iGlobal, G_VelRange, 127 << 8));
            Zone z;
            z.loKey = juce::jmax (keys.first, pKeys.first); z.hiKey = juce::jmin (keys.second, pKeys.second);
            z.loVel = juce::jmax (1, juce::jmax (vels.first, pVels.first)); z.hiVel = juce::jmin (vels.second, pVels.second);
            if (z.loKey > z.hiKey || z.loVel > z.hiVel) continue;
            z.sample = sf->sample (sg->amount);
            const int rootOverride = get (iz, iGlobal, G_RootKey, -1);
            z.root = rootOverride >= 0 ? rootOverride : (h.root <= 127 ? h.root : 60);
            const int scaleTune = get (iz, iGlobal, G_ScaleTune, 100);
            z.keyTrack = scaleTune != 0;
            z.tuneCents = (float) (100 * (get (iz, iGlobal, G_Coarse, 0) + pAdd (G_Coarse)) + get (iz, iGlobal, G_Fine, 0) + pAdd (G_Fine) + h.corr);
            // attenuation in centibels (scaled the way most SoundFont players do, so banks sound as their makers heard them)
            z.gainDb = -0.04f * (float) (get (iz, iGlobal, G_Atten, 0) + pAdd (G_Atten));
            z.pan = juce::jlimit (-1.0f, 1.0f, (get (iz, iGlobal, G_Pan, 0) + pAdd (G_Pan)) / 500.0f);
            const int len = z.sample->buffer.getNumSamples();
            z.offset = juce::jlimit (0, len - 1, get (iz, iGlobal, G_StartOff, 0) + 32768 * get (iz, iGlobal, G_StartCoarse, 0));
            z.end = juce::jlimit (z.offset + 1, len, len + get (iz, iGlobal, G_EndOff, 0) + 32768 * get (iz, iGlobal, G_EndCoarse, 0));
            const int modes = get (iz, iGlobal, G_Modes, 0) & 3;
            if (modes == 1 || modes == 3)
            {
                z.loopStart = h.loopStart - h.start + get (iz, iGlobal, G_LoopStartOff, 0) + 32768 * get (iz, iGlobal, G_LoopStartCoarse, 0);
                z.loopEnd = h.loopEnd - h.start + get (iz, iGlobal, G_LoopEndOff, 0) + 32768 * get (iz, iGlobal, G_LoopEndCoarse, 0);
                if (z.loopEnd <= z.loopStart + 4 || z.loopStart < 0 || z.loopEnd > len) z.loopStart = z.loopEnd = -1;
            }
            z.name = juce::String (h.name);
            if (! envSet)
            {
                // the first sound's volume envelope becomes the instrument's (time in timecents, sustain as attenuation)
                auto secs = [] (int tc) { return (float) std::pow (2.0, tc / 1200.0); };
                inst->attack = juce::jlimit (0.0f, 3000.0f, secs (get (iz, iGlobal, G_Attack, -12000)) * 1000.0f);
                inst->decay = juce::jlimit (5.0f, 10000.0f, secs (get (iz, iGlobal, G_Decay, -12000)) * 1000.0f);
                if (get (iz, iGlobal, G_Decay, -12000) <= -11000) inst->decay = 10000.0f;   // no decay given: hold
                inst->sustain = juce::jlimit (0.0f, 100.0f, 100.0f * (float) std::pow (10.0, -get (iz, iGlobal, G_Sustain, 0) / 200.0));
                inst->release = juce::jlimit (5.0f, 8000.0f, secs (get (iz, iGlobal, G_Release, -12000)) * 1000.0f);
                envSet = true;
            }
            inst->zones.push_back (z);
        }
        if (progress != nullptr) *progress = (float) (++done) / (float) juce::jmax (1, total);
    }
    if (inst->zones.empty()) { error = "That preset has no sounds."; return nullptr; }
    std::set<std::pair<int, int>> layers;
    std::set<const SampleData*> uniq;
    for (auto& z : inst->zones)
    {
        layers.insert ({ z.loVel, z.hiVel });
        if (uniq.insert (z.sample.get()).second) inst->bytes += (size_t) z.sample->buffer.getNumSamples() * 2 * sizeof (float);
    }
    inst->velocityLayers = (int) layers.size();
    inst->samples = (int) uniq.size();
    if (progress != nullptr) *progress = 1.0f;
    error.clear();
    return inst;
}

InstrumentPtr load (const juce::File& f, std::atomic<float>* progress, juce::String& error, int sf2Preset)
{
    if (f.hasFileExtension ("sf2")) return loadSf2 (f, sf2Preset, progress, error);
    if (f.isDirectory())
    {
        // a folder with an SFZ in it: use the SFZ
        juce::Array<juce::File> sfzs;
        for (const auto& e : juce::RangedDirectoryIterator (f, false, "*.sfz", juce::File::findFiles)) sfzs.add (e.getFile());
        if (sfzs.size() == 1) return loadSfz (sfzs[0], progress, error);
        juce::Array<juce::File> sf2s;
        for (const auto& e : juce::RangedDirectoryIterator (f, false, "*.sf2", juce::File::findFiles)) sf2s.add (e.getFile());
        if (sf2s.size() == 1) return loadSf2 (sf2s[0], sf2Preset, progress, error);
        return loadFolder (f, progress, error);
    }
    if (f.hasFileExtension ("sfz")) return loadSfz (f, progress, error);
    if (f.hasFileExtension ("nki;nkm;nkx;nkc;nkr"))
    {
        // a Kontakt instrument file: we can't read its mapping, but the sounds next to it can be mapped by name
        auto lib = f.getParentDirectory();
        if (lib.getFileName().equalsIgnoreCase ("Instruments")) lib = lib.getParentDirectory();
        auto samples = lib.getChildFile ("Samples");
        return loadFolder (samples.isDirectory() ? samples : lib, progress, error);
    }
    juce::Array<juce::File> one; one.add (f);
    return mapFiles (one, f, progress, error);
}

} // namespace rs::msamp

// =============================================================================
//  The instrument
// =============================================================================
namespace rs
{
using namespace msamp;

class MultiSamplerFx : public BuiltinFx, private juce::Thread
{
public:
    using BuiltinFx::processBlock;
    static constexpr int maxVoices = 64;

    MultiSamplerFx() : BuiltinFx ("multisampler", "Multi Sampler", layout()), juce::Thread ("RENASOUND sample loader") {}
    ~MultiSamplerFx() override { stopThread (10000); }

    bool acceptsMidi() const override { return true; }
    double getTailLengthSeconds() const override { return 4.0; }

    static APVTS::ParameterLayout layout()
    {
        APVTS::ParameterLayout l;
        l.add (std::make_unique<juce::AudioParameterInt> (juce::ParameterID { "artic", 1 }, "Articulation", 0, 31, 0));
        l.add (std::make_unique<juce::AudioParameterChoice> (juce::ParameterID { "rrmode", 1 }, "Round Robin", juce::StringArray { "Cycle", "Random", "Off" }, 0));
        l.add (floatParam ("attack", "Attack", skewed (0, 3000, 80), 2, "ms"));
        l.add (floatParam ("decay", "Decay", skewed (5, 10000, 600), 2000, "ms"));
        l.add (floatParam ("sustain", "Sustain", Range (0, 100), 100, "%"));
        l.add (floatParam ("release", "Release", skewed (5, 8000, 400), 300, "ms"));
        l.add (floatParam ("velocity", "Velocity", Range (0, 100), 60, "%"));
        l.add (floatParam ("relvol", "Release Sounds", Range (-30, 6), -6, "dB"));
        l.add (floatParam ("tune", "Tune", Range (-24, 24, 1), 0, "st"));
        l.add (floatParam ("fine", "Fine", Range (-100, 100), 0, "ct"));
        l.add (floatParam ("cutoff", "Brightness", skewed (200, 20000, 2500), 20000, "Hz"));
        l.add (floatParam ("level", "Level", Range (-24, 12), 0, "dB"));
        return l;
    }

    // ---- loading ----
    bool startLoading (const juce::File& f, int sf2Preset = 0)
    {
        if (! f.exists()) return false;
        stopThread (10000);
        pending = f;
        pendingPreset = sf2Preset;
        progress = 0;
        loadError.clear();
        loading = true;
        startThread();
        return true;
    }
    bool isLoading() const { return loading.load(); }
    float loadProgress() const { return progress.load(); }
    juce::String lastError() const { const juce::SpinLock::ScopedLockType sl (lock); return loadError; }
    InstrumentPtr getInstrument() const { const juce::SpinLock::ScopedLockType sl (lock); return inst; }
    void setInstrument (InstrumentPtr p)
    {
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            rs::ReleasePool::get().add (inst);   // the audio side may still be playing it: free it later, here
            inst = std::move (p);
            killAll = true;
        }
        auto i = getInstrument();
        if (i != nullptr)
        {
            // the instrument's own envelope (from an SFZ), and start on the first articulation
            auto set = [this] (const char* id, float v) { if (v >= 0) if (auto* p = state.getParameter (id)) p->setValueNotifyingHost (p->convertTo0to1 (v)); };
            set ("attack", i->attack); set ("decay", i->decay); set ("sustain", i->sustain); set ("release", i->release);
            if (param ("artic") >= (float) i->articulations.size()) set ("artic", 0);
        }
    }

    void getStateInformation (juce::MemoryBlock& mb) override
    {
        auto i = getInstrument();
        const auto src = i != nullptr ? i->source.getFullPathName() : (isLoading() ? pending.getFullPathName() : juce::String());
        state.state.setProperty ("source", src, nullptr);
        state.state.setProperty ("sf2preset", i != nullptr ? i->sf2Preset : pendingPreset, nullptr);
        BuiltinFx::getStateInformation (mb);
    }
    void setStateInformation (const void* d, int size) override
    {
        BuiltinFx::setStateInformation (d, size);
        const juce::File f (state.state.getProperty ("source").toString());
        if (f.getFullPathName().isNotEmpty() && f.exists())
        {
            restoring = true;   // keep the saved knob settings, not the SFZ's
            startLoading (f, (int) state.state.getProperty ("sf2preset", 0));
        }
    }

    // ---- playing ----
    void prepareToPlay (double sr, int) override { currentSampleRate = sr; for (auto& v : voices) v.active = false; }
    void releaseResources() override {}

    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        juce::ScopedNoDenormals nd;
        buffer.clear();
        const int n = buffer.getNumSamples();
        if (buffer.getNumChannels() < 2) return;
        InstrumentPtr in;
        {
            const juce::SpinLock::ScopedTryLockType sl (lock);
            if (! sl.isLocked()) return;
            in = inst;
            if (killAll) { for (auto& v : voices) v.active = false; killAll = false; }
        }
        playingInst = in;   // voices point into this; keep it alive while they play
        if (in == nullptr) return;

        const double sr = currentSampleRate;
        const float att = (float) (1.0 / juce::jmax (1.0, param ("attack") * 0.001 * sr));
        const float dec = (float) (1.0 - std::exp (-4.6 / juce::jmax (1.0, param ("decay") * 0.001 * sr)));
        const float sus = param ("sustain") * 0.01f;
        const float rel = (float) (1.0 - std::exp (-4.6 / juce::jmax (1.0, param ("release") * 0.001 * sr)));
        const float gain = juce::Decibels::decibelsToGain (param ("level"));
        const float cutoff = juce::jmin (param ("cutoff"), (float) sr * 0.45f);
        const bool filterOn = cutoff < 19000.0f;
        const float lpK = 1.0f - std::exp (-juce::MathConstants<float>::twoPi * cutoff / (float) sr);

        if (const int a = auditionOn.exchange (-1); a >= 0) noteOn (*in, a, 0.8f);
        if (const int a = auditionOff.exchange (-1); a >= 0) noteOff (*in, a);

        auto* L = buffer.getWritePointer (0);
        auto* R = buffer.getWritePointer (1);
        auto it = midi.cbegin();
        for (int i = 0; i < n; ++i)
        {
            while (it != midi.cend() && (*it).samplePosition <= i)
            {
                const auto m = (*it).getMessage();
                if (m.isNoteOn()) noteOn (*in, m.getNoteNumber(), m.getFloatVelocity());
                else if (m.isNoteOff()) noteOff (*in, m.getNoteNumber());
                else if (m.isControllerOfType (64)) { sustainPedal = m.getControllerValue() >= 64; if (! sustainPedal) releasePedalled (*in); }
                else if (m.isAllNotesOff() || m.isAllSoundOff()) for (auto& v : voices) { v.releasing = true; if (m.isAllSoundOff()) v.active = false; }
                ++it;
            }
            float outL = 0, outR = 0;
            for (auto& v : voices)
            {
                if (! v.active) continue;
                const Zone& z = *v.zone;
                const auto& buf = z.sample->buffer;
                // envelope
                if (v.releasing) v.env -= rel * v.env;
                else if (v.attacking) { v.env += att; if (v.env >= 1.0f) { v.env = 1.0f; v.attacking = false; } }
                else v.env += dec * (sus - v.env);
                if (v.releasing && v.env < 1.0e-4f) { v.active = false; continue; }
                if (v.fade < 1.0f) { v.fade += v.fadeStep; if (v.fade > 1.0f) v.fade = 1.0f; }
                // position: loop, or stop at the end
                const int endPos = z.end > 0 ? juce::jmin (z.end, buf.getNumSamples()) : buf.getNumSamples();
                if (z.loopEnd > 0 && ! (v.releasing && z.oneShot) && v.pos >= z.loopEnd && z.loopEnd <= endPos)
                    v.pos -= (z.loopEnd - z.loopStart);
                if (v.pos >= endPos - 3) { v.active = false; continue; }
                const int p0 = (int) v.pos;
                const float t = (float) (v.pos - p0);
                float a = hermite (buf.getReadPointer (0), p0, t, endPos), b = hermite (buf.getReadPointer (1), p0, t, endPos);
                const double left = endPos - v.pos;
                if (z.loopEnd <= 0 && left < 256.0) { const float k = (float) (left / 256.0); a *= k; b *= k; }   // no click at the very end
                v.pos += v.step;
                if (filterOn) { v.lpL += lpK * (a - v.lpL); v.lpR += lpK * (b - v.lpR); a = v.lpL; b = v.lpR; }
                const float g = v.env * v.gain * v.fade;
                outL += a * g * v.panL; outR += b * g * v.panR;
            }
            L[i] = outL * gain;
            R[i] = outR * gain;
        }
        // what the editor shows
        std::array<bool, 128> held {};
        int count = 0;
        for (auto& v : voices) if (v.active) { ++count; if (! v.releasing) held[(size_t) v.note] = true; }
        for (int k = 0; k < 128; ++k) keyDown[(size_t) k] = held[(size_t) k];
        voicesPlaying = count;
        for (size_t k = 0; k < voices.size(); ++k) voiceZone[k] = voices[k].active ? (int) (voices[k].zone - in->zones.data()) : -1;
    }

    // live readings
    std::array<std::atomic<bool>, 128> keyDown {};
    std::array<std::atomic<int>, maxVoices> voiceZone {};
    std::atomic<int> voicesPlaying { 0 }, auditionOn { -1 }, auditionOff { -1 };

    juce::AudioProcessorEditor* createEditor() override;

private:
    struct Voice
    {
        bool active = false, releasing = false, attacking = true;
        int note = 60;
        const Zone* zone = nullptr;
        double pos = 0, step = 1;
        float env = 0, gain = 1, fade = 1, fadeStep = 0, vel = 1, lpL = 0, lpR = 0, panL = 1, panR = 1;
        bool pedalHeld = false;
        juce::int64 age = 0;
    };
    std::array<Voice, maxVoices> voices;
    InstrumentPtr inst, playingInst;
    std::vector<const Zone*> matchScratch = reserved<const Zone*> (4096);
    std::vector<int> takesScratch = reserved<int> (256);
    template <typename T> static std::vector<T> reserved (size_t n) { std::vector<T> v; v.reserve (n); return v; }
    mutable juce::SpinLock lock;
    bool killAll = false, sustainPedal = false;
    std::array<int, 128> rrCounter {};
    std::array<float, 128> noteVel {};
    std::array<bool, 128> keyHeld {};
    juce::int64 counter = 0;
    juce::Random rnd;
    juce::File pending;
    int pendingPreset = 0;
    std::atomic<bool> loading { false };
    std::atomic<float> progress { 0 };
    juce::String loadError;
    bool restoring = false;

    static float hermite (const float* d, int i, float t, int len)
    {
        const float xm1 = d[juce::jmax (0, i - 1)], x0 = d[i], x1 = d[juce::jmin (len - 1, i + 1)], x2 = d[juce::jmin (len - 1, i + 2)];
        const float c = (x1 - xm1) * 0.5f, v = x0 - x1, w = c + v, a = w + v + (x2 - x0) * 0.5f, bNeg = w + a;
        return ((((a * t) - bNeg) * t + c) * t + x0);
    }

    // libraries already in memory (two tracks with the same violin share one copy)
    static InstrumentPtr cached (const juce::File& f, int preset, std::atomic<float>* prog, juce::String& err)
    {
        static std::mutex m;
        static std::map<juce::String, std::weak_ptr<const Instrument>> cache;
        const auto key = f.getFullPathName() + "|" + juce::String (f.getLastModificationTime().toMilliseconds()) + "|" + juce::String (preset);
        {
            const std::lock_guard<std::mutex> g (m);
            if (auto it = cache.find (key); it != cache.end()) if (auto p = it->second.lock()) { if (prog) *prog = 1.0f; return p; }
        }
        auto p = msamp::load (f, prog, err, preset);
        if (p != nullptr) { const std::lock_guard<std::mutex> g (m); cache[key] = p; }
        return p;
    }

    void run() override
    {
        juce::String err;
        auto f = pending;
        auto result = cached (f, pendingPreset, &progress, err);
        if (threadShouldExit()) { loading = false; return; }
        if (result != nullptr && result->sf2Presets.size() >= 100)
            soundmatch::noticeSoundFont (result->source);   // a full General MIDI bank: imports can use it too
        if (result != nullptr)
        {
            if (restoring)
            {
                // a saved project: keep its knob settings
                auto copy = std::make_shared<Instrument> (*result);
                copy->attack = copy->decay = copy->sustain = copy->release = -1;
                result = copy;
            }
            setInstrument (result);   // (safe from this thread: a lock-protected swap and parameter changes)
        }
        {
            const juce::SpinLock::ScopedLockType sl (lock);
            loadError = result == nullptr ? err : juce::String();
        }
        restoring = false;
        loading = false;
    }

    int currentArtic (const Instrument& in) const { return juce::jlimit (0, juce::jmax (0, in.articulations.size() - 1), (int) param ("artic")); }

    void noteOn (const Instrument& in, int note, float vel)
    {
        // a keyswitch: change articulation, play nothing
        for (size_t a = 0; a < in.keyswitches.size(); ++a)
            if (in.keyswitches[a] == note)
            {
                if (auto* p = state.getParameter ("artic")) p->setValueNotifyingHost (p->convertTo0to1 ((float) a));
                return;
            }
        noteVel[(size_t) note] = vel;
        keyHeld[(size_t) note] = true;
        startZones (in, note, vel, false);
    }

    void startZones (const Instrument& in, int note, float vel, bool releaseTrigger)
    {
        const int artic = currentArtic (in);
        const int v127 = juce::jlimit (1, 127, (int) std::lround (vel * 127.0f));
        // the zones that match this key, loudness and articulation
        auto& match = matchScratch;   // (reserved up front: no allocation while playing)
        match.clear();
        int maxRR = 0;
        for (auto& z : in.zones)
            if (z.release == releaseTrigger && note >= z.loKey && note <= z.hiKey && v127 >= z.loVel && v127 <= z.hiVel
                && (z.articulation < 0 || z.articulation == artic))
            {
                if (match.size() < match.capacity()) match.push_back (&z);
                maxRR = juce::jmax (maxRR, z.rr);
            }
        if (match.empty()) return;
        if (maxRR > 0)
        {
            // round robin: the takes this note has, in turn (or at random)
            auto& takes = takesScratch;
            takes.clear();
            for (auto* z : match) if (z->rr > 0 && std::find (takes.begin(), takes.end(), z->rr) == takes.end()) takes.push_back (z->rr);
            std::sort (takes.begin(), takes.end());
            const int mode = (int) param ("rrmode");
            int pick = takes[0];
            if (mode == 0) pick = takes[(size_t) (rrCounter[(size_t) note]++ % (int) takes.size())];
            else if (mode == 1) pick = takes[(size_t) rnd.nextInt ((int) takes.size())];
            match.erase (std::remove_if (match.begin(), match.end(), [pick] (const Zone* z) { return z->rr > 0 && z->rr != pick; }), match.end());
        }
        const float velAmt = param ("velocity") * 0.01f;
        const float velGain = (1.0f - velAmt) + velAmt * vel * vel * 1.4f;
        const double transpose = param ("tune") + param ("fine") * 0.01;
        // retriggering the same key: the old note goes into its release instead of stacking up
        if (! releaseTrigger)
            for (auto& v : voices) if (v.active && v.note == note && ! v.releasing && ! v.zone->release) v.releasing = true;
        for (auto* z : match)
        {
            Voice* v = nullptr;
            for (auto& x : voices) if (! x.active) { v = &x; break; }
            if (v == nullptr) { v = &voices[0]; for (auto& x : voices) if ((x.releasing && ! v->releasing) || (x.releasing == v->releasing && x.age < v->age)) v = &x; }
            const bool stolen = v->active;
            *v = Voice();
            v->active = true; v->note = note; v->zone = z; v->vel = vel; v->age = ++counter;
            v->pos = z->offset;
            const double semis = (z->keyTrack ? (note - z->root) : 0) + z->tuneCents * 0.01 + transpose;
            v->step = z->sample->rate / currentSampleRate * std::pow (2.0, semis / 12.0);
            v->gain = velGain * juce::Decibels::decibelsToGain (z->gainDb) * (releaseTrigger ? juce::Decibels::decibelsToGain (param ("relvol")) : 1.0f);
            if (stolen) { v->fade = 0; v->fadeStep = (float) (1.0 / (0.002 * currentSampleRate)); }
            v->panL = juce::jmin (1.0f, 1.0f - z->pan); v->panR = juce::jmin (1.0f, 1.0f + z->pan);
            if (releaseTrigger) { v->attacking = false; v->env = 1.0f; }
        }
    }

    void noteOff (const Instrument& in, int note)
    {
        // a release sound plays whenever a key that was down comes up (even if its note has already died away)
        const bool wasDown = keyHeld[(size_t) note];
        keyHeld[(size_t) note] = false;
        bool pedalled = false;
        for (auto& v : voices)
            if (v.active && v.note == note && ! v.releasing && ! v.zone->release && ! v.zone->oneShot)
            {
                if (sustainPedal) { v.pedalHeld = true; pedalled = true; continue; }
                v.releasing = true;
            }
        if (wasDown && ! pedalled) startZones (in, note, noteVel[(size_t) note], true);
    }
    void releasePedalled (const Instrument& in)
    {
        std::set<int> notes;
        for (auto& v : voices) if (v.active && v.pedalHeld && ! v.releasing) { v.releasing = true; notes.insert (v.note); }
        for (int nn : notes) startZones (in, nn, noteVel[(size_t) nn], true);
    }

};

// =============================================================================
//  The editor: the zone map, the articulations, a playable keyboard
// =============================================================================
class MultiSamplerEditor : public FaceEditor, public juce::FileDragAndDropTarget, public juce::DragAndDropTarget
{
public:
    explicit MultiSamplerEditor (MultiSamplerFx& p)
        : FaceEditor (p, { KnobStyle::Glossy, juce::Colour (0xff1d2230), juce::Colour (0xffffd27a), juce::Colour (0xff5fb4ff), juce::Colour (0xff262c3c),
                           juce::Colour (0xffe8ecf5), juce::Colour (0xff8a93a8), juce::Colour (0xff121620), juce::Colour (0xffe8ecf5) }),
          fx (p), rrMode (p, "rrmode")
    {
        for (auto [id, name] : { std::pair { "attack", "ATTACK" }, { "decay", "DECAY" }, { "sustain", "SUSTAIN" }, { "release", "RELEASE" },
                                 { "velocity", "VELOCITY" }, { "relvol", "RELEASE SOUNDS" }, { "tune", "TUNE" }, { "cutoff", "BRIGHTNESS" }, { "level", "LEVEL" } })
        {
            auto* k = knobs.add (new Knob (p, id, name, 60));
            k->label.setColour (juce::Label::textColourId, look.s.textDim);
            addAndMakeVisible (k);
            k->sendLookAndFeelChange();
        }
        addAndMakeVisible (rrMode);
        rrMode.sendLookAndFeelChange();
        for (auto* b : { &loadFolder, &loadFile })
        {
            addAndMakeVisible (*b);
            b->setColour (juce::TextButton::textColourOffId, look.s.text);
        }
        loadFolder.setTooltip ("A folder of sounds named by note (\"Cello A2 mf RR1\"), or a sample library's folder");
        loadFile.setTooltip ("An SFZ or SoundFont (.sf2) instrument, a Kontakt library's .nki (its sounds are mapped by name), or a single sound");
        presetBox.setTextWhenNothingSelected ("SoundFont preset");
        presetBox.onChange = [this]
        {
            auto in = fx.getInstrument();
            const int idx = presetBox.getSelectedItemIndex();
            if (in != nullptr && idx >= 0 && idx != in->sf2Preset) fx.startLoading (in->source, idx);
        };
        addChildComponent (presetBox);
        loadFolder.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose a folder of sounds", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().isDirectory()) startLoad (fc.getResult()); });
        };
        loadFile.onClick = [this]
        {
            chooser = std::make_unique<juce::FileChooser> ("Choose an instrument or a sound", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                           "*.sfz;*.sf2;*.nki;*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3;*.ncw");
            chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                  [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) startLoad (fc.getResult()); });
        };
        setSize (1000, 616);
        timerCallback();   // the articulation buttons straight away
        startTimerHz (30);
    }

    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray& files, int, int) override { dragOver = false; if (! files.isEmpty()) startLoad (juce::File (files[0])); }
    void fileDragEnter (const juce::StringArray&, int, int) override { dragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override { dragOver = false; repaint(); }
    bool isInterestedInDragSource (const SourceDetails&) override { return true; }
    void itemDropped (const SourceDetails& d) override
    {
        const auto first = d.description.toString().upToFirstOccurrenceOf ("\n", false, false);
        if (juce::File::isAbsolutePath (first)) startLoad (juce::File (first));
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        juce::ColourGradient bg (juce::Colour (0xff1b2030), 0, 0, juce::Colour (0xff0d1018), 0, r.getHeight(), false);
        g.setGradientFill (bg);
        g.fillAll();
        facekit::title (g, { 22, 10, 600, 32 }, "MULTI SAMPLER", "ZONES  /  LAYERS  /  ROUND ROBINS  /  ARTICULATIONS", look.s.text, look.s.pointer);

        const auto in = fx.getInstrument();
        // the info line
        g.setFont (theme::font (12.5f, true));
        g.setColour (look.s.text);
        auto info = infoArea;
        if (in != nullptr)
        {
            g.drawText (in->name, info.removeFromTop (18), juce::Justification::centredLeft);
            g.setColour (look.s.textDim);
            g.setFont (theme::font (11.0f, false));
            juce::String s;
            s << in->samples << " sounds  /  " << in->articulations.size() << (in->articulations.size() == 1 ? " articulation  /  " : " articulations  /  ")
              << in->velocityLayers << (in->velocityLayers == 1 ? " layer" : " layers");
            if (in->roundRobins > 1) s << "  /  " << in->roundRobins << " round robins";
            s << "  /  " << juce::String (juce::jmax (1, juce::roundToInt ((double) in->bytes / (1024.0 * 1024.0)))) << " MB";
            g.drawText (s, info.removeFromTop (16), juce::Justification::centredLeft);
            if (in->octaveCorrection != 0)
            {
                g.setColour (look.s.pointer);
                g.drawText ("The file names were " + juce::String (std::abs (in->octaveCorrection)) + (std::abs (in->octaveCorrection) == 1 ? " octave " : " octaves ")
                            + (in->octaveCorrection > 0 ? "low" : "high") + ": corrected by listening to the sounds", info.removeFromTop (16), juce::Justification::centredLeft);
            }
        }
        else
        {
            g.setColour (look.s.textDim);
            g.drawText ("Drop a folder of sounds, an SFZ, or a sample library here", info.removeFromTop (18), juce::Justification::centredLeft);
        }

        drawMap (g, in);
        drawKeyboard (g, in);
        g.setColour (look.s.textDim); g.setFont (theme::font (10.5f, true));
        g.drawText ("ROUND ROBIN", rrArea, juce::Justification::bottomLeft);
        if (in != nullptr && in->articulations.size() > 1)
        {
            g.drawText ("ARTICULATION  (OR PLAY A KEYSWITCH)", articArea.translated (0, -14).withHeight (12), juce::Justification::left);
        }
        else if (in != nullptr && ! in->sf2Presets.isEmpty())
            g.drawText ("SOUNDFONT PRESET  (" + juce::String (in->sf2Presets.size()) + ")", articArea.translated (0, -14).withHeight (12), juce::Justification::left);

        if (fx.isLoading())
        {
            auto ov = mapArea.toFloat();
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.fillRoundedRectangle (ov, 8.0f);
            auto bar = ov.withSizeKeepingCentre (ov.getWidth() * 0.5f, 10);
            g.setColour (look.s.arcBg); g.fillRoundedRectangle (bar, 5);
            g.setColour (look.s.arc); g.fillRoundedRectangle (bar.withWidth (bar.getWidth() * fx.loadProgress()), 5);
            g.setColour (look.s.text); g.setFont (theme::font (13.0f, true));
            g.drawText ("Loading sounds...  " + juce::String (juce::roundToInt (fx.loadProgress() * 100)) + "%", bar.translated (0, -28).withHeight (20).expanded (100, 0), juce::Justification::centred);
        }
        else if (const auto e = fx.lastError(); e.isNotEmpty())
        {
            g.setColour (juce::Colour (0xffff6b6b)); g.setFont (theme::font (12.0f, true));
            g.drawText (e, mapArea.toFloat().reduced (10).removeFromTop (20), juce::Justification::centredRight);
        }
        if (dragOver) { g.setColour (look.s.arc.withAlpha (0.15f)); g.fillRoundedRectangle (mapArea.toFloat(), 8.0f); }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (20, 10);
        r.removeFromTop (38);
        auto top = r.removeFromTop (54);
        auto btns = top.removeFromRight (300);
        loadFolder.setBounds (btns.removeFromLeft (146).reduced (0, 10));
        btns.removeFromLeft (8);
        loadFile.setBounds (btns.reduced (0, 10));
        infoArea = top;
        r.removeFromTop (18);   // the "ARTICULATION" label
        articArea = r.removeFromTop (30);
        r.removeFromTop (8);
        mapArea = r.removeFromTop (210);
        r.removeFromTop (4);
        keysArea = r.removeFromTop (54);
        r.removeFromTop (12);
        auto ctl = r.removeFromTop (124);
        auto left = ctl.removeFromLeft (200);
        rrArea = left.removeFromTop (20);
        rrMode.setBounds (left.removeFromTop (30));
        ctl.removeFromLeft (14);
        const int w = ctl.getWidth() / knobs.size();
        for (auto* k : knobs) k->setBounds (ctl.removeFromLeft (w).reduced (2, 0));
        layoutArticButtons();
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (keysArea.contains (e.getPosition()))
        {
            const int note = noteAt (e.position);
            if (note >= 0) { fx.auditionOn = note; heldNote = note; }
        }
    }
    void mouseUp (const juce::MouseEvent&) override
    {
        if (heldNote >= 0) { fx.auditionOff = heldNote; heldNote = -1; }
    }

private:
    MultiSamplerFx& fx;
    Switch rrMode;
    juce::OwnedArray<Knob> knobs;
    juce::TextButton loadFolder { "Load folder..." }, loadFile { "Load SFZ / SF2 / file..." };
    juce::ComboBox presetBox;
    juce::OwnedArray<juce::TextButton> articButtons;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> infoArea, articArea, mapArea, keysArea, rrArea;
    bool dragOver = false;
    int heldNote = -1;
    const Instrument* shownFor = nullptr;
    int shownArtic = -1;

    void startLoad (const juce::File& f)
    {
        fx.startLoading (f);
        repaint();
    }

    // the range of keys shown: the instrument's (plus its keyswitches), at least 3 octaves
    std::pair<int, int> keyRange (const Instrument* in) const
    {
        int lo = 36, hi = 96;
        if (in != nullptr && ! in->zones.empty())
        {
            lo = 127; hi = 0;
            for (auto& z : in->zones) { lo = juce::jmin (lo, z.root); hi = juce::jmax (hi, z.root); }
            for (int k : in->keyswitches) if (k >= 0) lo = juce::jmin (lo, k);
            lo -= 1; hi += 2;
            if (hi - lo < 36) { const int c = (lo + hi) / 2; lo = c - 18; hi = c + 18; }
        }
        lo = juce::jlimit (0, 100, lo - lo % 12);           // start on a C
        hi = juce::jlimit (lo + 12, 127, hi);
        return { lo, hi };
    }
    static bool isBlack (int n) { const int pc = n % 12; return pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10; }

    void drawMap (juce::Graphics& g, InstrumentPtr in)
    {
        auto a = mapArea.toFloat();
        g.setColour (juce::Colour (0xff0a0d14));
        g.fillRoundedRectangle (a, 8.0f);
        auto plot = a.reduced (10, 10);
        auto relLane = plot.removeFromBottom (14);
        plot.removeFromBottom (4);
        const auto range = keyRange (in.get());
        const int lo = range.first, hi = range.second;
        const float kw = plot.getWidth() / (float) (hi - lo + 1);
        auto keyX = [&] (int k) { return plot.getX() + (float) (juce::jlimit (lo, hi + 1, k) - lo) * kw; };
        auto velY = [&] (int v) { return plot.getBottom() - (float) v / 127.0f * plot.getHeight(); };
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (a.toNearestInt());
        // octave lines and velocity guides
        g.setColour (juce::Colours::white.withAlpha (0.05f));
        for (int k = lo; k <= hi; ++k) if (k % 12 == 0) g.drawVerticalLine ((int) keyX (k), plot.getY(), plot.getBottom());
        for (int v : { 32, 64, 96 }) g.drawHorizontalLine ((int) velY (v), plot.getX(), plot.getRight());
        g.setColour (look.s.textDim); g.setFont (theme::font (9.5f, true));
        g.drawText ("VELOCITY", plot.withWidth (80).withHeight (12), juce::Justification::topLeft);
        if (in == nullptr) return;

        const int artic = juce::jlimit (0, juce::jmax (0, in->articulations.size() - 1), (int) fx.param ("artic"));
        std::set<int> sounding;
        for (auto& vz : fx.voiceZone) if (const int z = vz.load(); z >= 0) sounding.insert (z);
        // zones of this articulation: blocks of key range x velocity range, brighter for the ones playing
        std::set<std::tuple<int, int, int, int, bool>> drawn;
        for (size_t i = 0; i < in->zones.size(); ++i)
        {
            const auto& z = in->zones[i];
            if (! (z.articulation < 0 || z.articulation == artic)) continue;
            const bool live = sounding.count ((int) i) > 0;
            const auto keyT = std::make_tuple (z.loKey, z.hiKey, z.loVel, z.hiVel, z.release);
            if (! live && ! drawn.insert (keyT).second) continue;   // round robins share a block
            auto box = juce::Rectangle<float> (keyX (z.loKey) + 1, velY (z.hiVel), keyX (z.hiKey + 1) - keyX (z.loKey) - 2, velY (z.loVel - 1) - velY (z.hiVel) - 2);
            if (z.release) box = juce::Rectangle<float> (box.getX(), relLane.getY(), box.getWidth(), relLane.getHeight());   // release sounds: their own lane
            if (box.getWidth() <= 0) continue;
            const float vt = (float) (z.loVel + z.hiVel) / 254.0f;
            const auto col = (z.release ? juce::Colour (0xffb98cff) : look.s.arc.interpolatedWith (look.s.pointer, vt));
            g.setColour (col.withAlpha (live ? 0.95f : 0.28f));
            g.fillRoundedRectangle (box, 2.0f);
            g.setColour (col.withAlpha (live ? 1.0f : 0.55f));
            g.drawRoundedRectangle (box, 2.0f, 1.0f);
            // the sampled note: a dot
            if (! z.release)
            {
                g.setColour (juce::Colours::white.withAlpha (live ? 1.0f : 0.5f));
                g.fillEllipse (keyX (z.root) + kw * 0.5f - 2, box.getBottom() - 6, 4, 4);
            }
        }
        g.setColour (look.s.textDim); g.setFont (theme::font (10.0f, true));
        g.drawText (in->articulations[artic].toUpperCase() + (in->roundRobins > 1 ? "   /   ROUND ROBINS STACKED IN EACH BLOCK" : ""), plot.removeFromTop (14).translated (0, -12), juce::Justification::topRight);
        bool anyRel = false; for (auto& z : in->zones) anyRel = anyRel || z.release;
        if (anyRel) { g.setColour (juce::Colour (0xffb98cff)); g.drawText ("RELEASE", relLane.withWidth (60).translated (-2, 0), juce::Justification::centredLeft); }
    }

    void drawKeyboard (juce::Graphics& g, InstrumentPtr in)
    {
        // one column per key, lined up with the map above: white keys full height, black keys shorter on top
        auto a = keysArea.toFloat().reduced (10, 0);
        const auto range = keyRange (in.get());
        const int lo = range.first, hi = range.second;
        const float kw = a.getWidth() / (float) (hi - lo + 1);
        std::set<int> roots, ks;
        if (in != nullptr) { for (auto& z : in->zones) roots.insert (z.root); for (int k : in->keyswitches) if (k >= 0) ks.insert (k); }
        const int artic = in != nullptr ? (int) fx.param ("artic") : -1;
        auto colourOf = [&] (int k)
        {
            if (fx.keyDown[(size_t) k].load() || k == heldNote) return look.s.arc;
            if (in != nullptr && artic >= 0 && artic < (int) in->keyswitches.size() && in->keyswitches[(size_t) artic] == k) return juce::Colour (0xffff7a1a);
            if (ks.count (k)) return isBlack (k) ? juce::Colour (0xffa0581c) : juce::Colour (0xffffb870);
            return isBlack (k) ? juce::Colour (0xff141720) : juce::Colour (0xffe4e7ee);
        };
        g.setColour (juce::Colour (0xffe4e7ee));
        g.fillRoundedRectangle (a, 3.0f);
        for (int k = lo; k <= hi; ++k)
            if (! isBlack (k))
            {
                auto key = juce::Rectangle<float> (a.getX() + (float) (k - lo) * kw, a.getY(), kw, a.getHeight());
                g.setColour (colourOf (k)); g.fillRect (key.reduced (0.5f, 0));
                // white keys sit between their black neighbours: draw the gap line on the white side only
                if (! isBlack (k + 1) || k == hi) { g.setColour (juce::Colour (0xff9aa0ad)); g.drawVerticalLine ((int) key.getRight(), key.getY(), key.getBottom()); }
                if (k % 12 == 0)
                {
                    g.setColour (juce::Colour (0xff5a6274)); g.setFont (theme::font (8.5f, true));
                    g.drawText (juce::MidiMessage::getMidiNoteName (k, true, true, 4), key.withTrimmedTop (key.getHeight() - 13).expanded (6, 0), juce::Justification::centred);
                }
            }
        for (int k = lo; k <= hi; ++k)
            if (isBlack (k))
            {
                auto key = juce::Rectangle<float> (a.getX() + (float) (k - lo) * kw, a.getY(), kw, a.getHeight() * 0.6f);
                g.setColour (colourOf (k)); g.fillRoundedRectangle (key.reduced (0.5f, 0), 1.5f);
            }
        // where the sampled notes are
        for (int k : roots)
            if (k >= lo && k <= hi)
            {
                const float x = a.getX() + ((float) (k - lo) + 0.5f) * kw;
                g.setColour (look.s.pointer.darker (isBlack (k) ? 0.0f : 0.3f));
                g.fillEllipse (x - 2, (isBlack (k) ? a.getY() + a.getHeight() * 0.6f - 7 : a.getBottom() - 18), 4, 4);
            }
    }

    int noteAt (juce::Point<float> p) const
    {
        const auto range = keyRange (fx.getInstrument().get());
        const int lo = range.first, hi = range.second;
        auto a = keysArea.toFloat().reduced (10, 0);
        const int k = lo + (int) ((p.x - a.getX()) / (a.getWidth() / (float) (hi - lo + 1)));
        return k >= lo && k <= hi ? k : -1;
    }

    void layoutArticButtons()
    {
        auto r = articArea;
        if (presetBox.isVisible()) { presetBox.setBounds (r.removeFromLeft (360)); r.removeFromLeft (10); }
        for (auto* b : articButtons)
        {
            const int w = juce::jmin (180, juce::jmax (90, juce::roundToInt (juce::GlyphArrangement::getStringWidth (theme::font (12.0f, true), b->getButtonText())) + 30));
            if (r.getWidth() < w) { b->setVisible (false); continue; }
            b->setVisible (true);
            b->setBounds (r.removeFromLeft (w));
            r.removeFromLeft (6);
        }
    }

    void timerCallback() override
    {
        auto in = fx.getInstrument();
        const int artic = (int) fx.param ("artic");
        if (in.get() != shownFor)
        {
            shownFor = in.get();
            articButtons.clear();
            presetBox.setVisible (in != nullptr && ! in->sf2Presets.isEmpty());
            if (presetBox.isVisible())
            {
                presetBox.clear (juce::dontSendNotification);
                presetBox.addItemList (in->sf2Presets, 1);
                presetBox.setSelectedItemIndex (in->sf2Preset, juce::dontSendNotification);
                presetBox.sendLookAndFeelChange();
            }
            if (in != nullptr && in->articulations.size() > 1)
                for (int a = 0; a < in->articulations.size(); ++a)
                {
                    auto* b = articButtons.add (new juce::TextButton (in->articulations[a]));
                    const int ks = a < (int) in->keyswitches.size() ? in->keyswitches[(size_t) a] : -1;
                    if (ks >= 0) b->setTooltip ("Keyswitch: " + juce::MidiMessage::getMidiNoteName (ks, true, true, 4));
                    b->onClick = [this, a] { if (auto* p = fx.state.getParameter ("artic")) p->setValueNotifyingHost (p->convertTo0to1 ((float) a)); };
                    b->setColour (juce::TextButton::buttonOnColourId, look.s.arc);
                    addAndMakeVisible (b);
                    b->sendLookAndFeelChange();
                }
            layoutArticButtons();
            shownArtic = -1;
        }
        if (artic != shownArtic)
        {
            shownArtic = artic;
            for (int i = 0; i < articButtons.size(); ++i) articButtons[i]->setToggleState (i == artic, juce::dontSendNotification);
        }
        repaint();
    }
};

juce::AudioProcessorEditor* MultiSamplerFx::createEditor() { return new MultiSamplerEditor (*this); }

std::unique_ptr<BuiltinFx> createMultiSampler() { return std::make_unique<MultiSamplerFx>(); }
bool multiSamplerLoad (juce::AudioProcessor* p, const juce::File& f, int preset) { if (auto* m = dynamic_cast<MultiSamplerFx*> (p)) return m->startLoading (f, preset); return false; }
bool multiSamplerIsLoading (juce::AudioProcessor* p) { if (auto* m = dynamic_cast<MultiSamplerFx*> (p)) return m->isLoading(); return false; }
InstrumentPtr multiSamplerInstrument (juce::AudioProcessor* p) { if (auto* m = dynamic_cast<MultiSamplerFx*> (p)) return m->getInstrument(); return nullptr; }
bool multiSamplerLoadNow (juce::AudioProcessor* p, const juce::File& f, juce::String& error, int preset)
{
    auto* m = dynamic_cast<MultiSamplerFx*> (p);
    if (m == nullptr) return false;
    auto in = msamp::load (f, nullptr, error, preset);
    if (in == nullptr) return false;
    m->setInstrument (in);
    return true;
}
std::vector<int> multiSamplerSoundingZones (juce::AudioProcessor* p)
{
    std::vector<int> out;
    if (auto* m = dynamic_cast<MultiSamplerFx*> (p)) for (auto& z : m->voiceZone) if (z.load() >= 0) out.push_back (z.load());
    return out;
}

} // namespace rs
