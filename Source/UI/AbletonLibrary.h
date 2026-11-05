#pragma once
#include <JuceHeader.h>

// =============================================================================
//  Finds the sounds that come with an installed Ableton Live, where they
//  already are on this computer (nothing is copied):
//    Core Library   inside the Live app (Mac) or ProgramData (Windows)
//    Packs          the Factory Packs folder (or wherever Live installs packs)
//    User Library   your own Live sounds
//  Live's Library.cfg is read too, so moved libraries are found.
// =============================================================================

namespace rs::ableton
{

struct Source { juce::File folder; juce::String label; };

inline std::vector<Source> findLibraries()
{
    std::vector<Source> out;
    auto add = [&out] (const juce::File& f, const juce::String& label)
    {
        if (! f.isDirectory()) return;
        int same = 0;
        for (auto& s : out)
        {
            if (s.folder == f || f.isAChildOf (s.folder)) return;
            if (s.label == label) ++same;
        }
        out.push_back ({ f, same == 0 ? label : label + " (" + f.getFileName() + ")" });   // e.g. packs in two places
    };
    struct NewestLast { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()); } } byName;

    // ---- Core Library: the newest Live that's installed ----
    juce::Array<juce::File> cores;
   #if JUCE_MAC
    for (auto apps : { juce::File ("/Applications"), juce::File ("~/Applications") })
        for (const auto& e : juce::RangedDirectoryIterator (apps, false, "Ableton Live*.app", juce::File::findDirectories))
            cores.add (e.getFile().getChildFile ("Contents/App-Resources/Core Library"));
    const auto prefs = juce::File ("~/Library/Preferences/Ableton");
    const auto docs = juce::File ("~/Music/Ableton");
   #elif JUCE_WINDOWS
    const auto programData = juce::File (juce::SystemStats::getEnvironmentVariable ("ProgramData", "C:\\ProgramData")).getChildFile ("Ableton");
    for (const auto& e : juce::RangedDirectoryIterator (programData, false, "Live*", juce::File::findDirectories))
        cores.add (e.getFile().getChildFile ("Resources").getChildFile ("Core Library"));
    const auto prefs = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("Ableton");
    const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("Ableton");
   #else   // (no Live on Linux; the Mac layout is used so this can be tested)
    const auto prefs = juce::File ("~/Library/Preferences/Ableton");
    const auto docs = juce::File ("~/Music/Ableton");
   #endif
    cores.sort (byName);
    for (int i = cores.size(); --i >= 0;)
        if (cores[i].isDirectory()) { add (cores[i], "Ableton Core Library"); break; }

    // ---- the folders Live's own settings point at ----
    juce::Array<juce::File> cfgs;
    for (const auto& e : juce::RangedDirectoryIterator (prefs, true, "Library.cfg", juce::File::findFiles)) cfgs.add (e.getFile());
    cfgs.sort (byName);
    for (int i = cfgs.size(); --i >= 0 && i >= cfgs.size() - 1;)   // the newest Live version's settings
        if (auto xml = juce::XmlDocument::parse (cfgs[i]))
        {
            std::function<void (const juce::XmlElement&)> walk = [&] (const juce::XmlElement& el)
            {
                for (int a = 0; a < el.getNumAttributes(); ++a)
                {
                    const auto v = el.getAttributeValue (a);
                    if (! juce::File::isAbsolutePath (v)) continue;
                    const juce::File f (v);
                    const auto n = f.getFileName();
                    if (n.containsIgnoreCase ("Packs")) add (f, "Ableton Packs");
                    else if (n.containsIgnoreCase ("User Library")) add (f, "Ableton User Library");
                }
                for (auto* c : el.getChildIterator()) walk (*c);
            };
            walk (*xml);
        }

    // ---- the usual places ----
    add (docs.getChildFile ("Factory Packs"), "Ableton Packs");
    add (docs.getChildFile ("User Library"), "Ableton User Library");
    return out;
}

} // namespace rs::ableton
