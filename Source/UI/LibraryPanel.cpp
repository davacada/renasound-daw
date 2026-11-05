#include "LibraryPanel.h"
#include "AbletonLibrary.h"
#include "../Drums.h"

namespace rs
{

// walks the library folders off the message thread (an Ableton library can hold tens of thousands of sounds)
class LibraryPanel::Scanner : public juce::Thread
{
public:
    Scanner (std::vector<Source> s, juce::String wc, juce::Component::SafePointer<LibraryPanel> p)
        : juce::Thread ("RENASOUND library scan"), sources (std::move (s)), wildcard (std::move (wc)), panel (p) {}
    ~Scanner() override { stopThread (4000); }

    std::atomic<int> found { 0 };

    void run() override
    {
        std::vector<Item> items;
        for (int i = 0; i < (int) sources.size() && ! threadShouldExit(); ++i)
            for (const auto& entry : juce::RangedDirectoryIterator (sources[(size_t) i].dir, true, wildcard, juce::File::findFiles))
            {
                if (threadShouldExit()) return;
                Item it;
                it.file = entry.getFile();
                it.name = it.file.getFileNameWithoutExtension();
                it.folder = i == 0 ? juce::String ("Recording") : it.file.getParentDirectory().getFileName().trim();
                it.source = i;
                items.push_back (std::move (it));
                found = (int) items.size();
            }
        auto p = panel;
        juce::MessageManager::callAsync ([p, items = std::move (items)] () mutable { if (p != nullptr) p->scanFinished (std::move (items)); });
    }

private:
    std::vector<Source> sources;
    juce::String wildcard;
    juce::Component::SafePointer<LibraryPanel> panel;
};

LibraryPanel::LibraryPanel (AppContext& c) : ctx (c)
{
    juce::PropertiesFile::Options opts;
    opts.applicationName = "Renasound";
    opts.filenameSuffix = ".settings";
    opts.osxLibrarySubFolder = "Application Support";
    opts.folderName = "Renasound";
    settings = std::make_unique<juce::PropertiesFile> (opts);

    folders.add (AudioEngine::getRecordingsFolder());
    juce::StringArray saved;
    saved.addTokens (settings->getValue ("libraryFolders"), "\n", {});
    for (auto& s : saved)
        if (juce::File (s).isDirectory()) folders.addIfNotAlreadyThere (juce::File (s));
    numSavedFolders = folders.size() - 1;
    // drum kit folders (your sample packs) show up here too
    for (auto& f : DrumMachine::getKitFolders()) folders.addIfNotAlreadyThere (f);

    addAndMakeVisible (search);
    search.setTextToShowWhenEmpty ("Search sounds...", theme::textDim);
    search.setColour (juce::TextEditor::backgroundColourId, theme::panelHi);
    search.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    search.onTextChange = [this] { applyFilter(); };

    addAndMakeVisible (folderBox);
    folderBox.onChange = [this] { applyFilter(); };

    addAndMakeVisible (addFolder);
    addFolder.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Add a folder of sounds", juce::File::getSpecialLocation (juce::File::userMusicDirectory));
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
        {
            if (fc.getResult().isDirectory())
            {
                if (! folders.contains (fc.getResult())) { folders.insert (++numSavedFolders, fc.getResult()); }
                saveFolders();
                refresh();
            }
        });
    };

    addAndMakeVisible (showFolder);
    showFolder.onClick = [] { AudioEngine::getRecordingsFolder().revealToUser(); };

    addAndMakeVisible (stopButton);
    stopButton.onClick = [this] { ctx.engine.stopPreview(); };

    list.setRowHeight (30);
    list.setColour (juce::ListBox::backgroundColourId, theme::panel);
    list.setMultipleSelectionEnabled (true);
    addAndMakeVisible (list);

    refresh();
}

LibraryPanel::~LibraryPanel() { scanner.reset(); }

void LibraryPanel::saveFolders()
{
    juce::StringArray paths;
    for (int i = 1; i <= numSavedFolders && i < folders.size(); ++i)   // index 0 is always Recordings
        paths.add (folders[i].getFullPathName());
    settings->setValue ("libraryFolders", paths.joinIntoString ("\n"));
    settings->saveIfNeeded();
}

void LibraryPanel::refresh()
{
    if (scanner != nullptr && scanner->isThreadRunning()) return;   // already looking
    sources.clear();
    for (int i = 0; i < folders.size(); ++i) sources.push_back ({ folders[i], i == 0 ? juce::String ("My recordings") : folders[i].getFileName().trim(), false });
    for (auto& a : ableton::findLibraries())
    {
        bool dup = false;
        for (auto& s : sources) dup = dup || s.dir == a.folder;
        if (! dup) sources.push_back ({ a.folder, a.label, true });
    }
    scanner = std::make_unique<Scanner> (sources, ctx.project.audioCache.formats.getWildcardForAllFormats(), juce::Component::SafePointer<LibraryPanel> (this));
    scanner->startThread (juce::Thread::Priority::low);
    startTimerHz (4);
    timerCallback();
}

void LibraryPanel::timerCallback()
{
    if (scanner == nullptr || ! scanner->isThreadRunning()) { stopTimer(); return; }
    status = "Looking for sounds...  " + juce::String (scanner->found.load()) + " so far";
    repaint();
}

void LibraryPanel::scanFinished (std::vector<Item> items)
{
    allItems = std::move (items);
    stopTimer();

    // newest recordings first, then sounds by folder and name
    std::stable_sort (allItems.begin(), allItems.end(), [] (const Item& a, const Item& b)
    {
        if ((a.source == 0) != (b.source == 0)) return a.source == 0;
        if (a.source == 0) return a.file.getLastModificationTime() > b.file.getLastModificationTime();
        if (a.source != b.source) return a.source < b.source;
        if (a.folder != b.folder) return a.folder.compareNatural (b.folder) < 0;
        return a.name.compareNatural (b.name) < 0;
    });

    // the folder menu: your folders (with the subfolders that hold sounds), then Ableton's libraries
    // (with their packs / top folders)
    const auto previous = folderBox.getText();
    folderBox.clear (juce::dontSendNotification);
    filterDirs.clear();
    folderBox.addItem ("All sounds", 1);
    struct ByName { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()); } } cmp;
    bool abletonHeading = false;
    int abletonSounds = 0;
    juce::StringArray abletonFound;
    for (int i = 0; i < (int) sources.size(); ++i)
    {
        const auto& src = sources[(size_t) i];
        if (src.ableton && ! abletonHeading) { folderBox.addSectionHeading ("ABLETON LIVE"); abletonHeading = true; }
        filterDirs.add (src.dir);
        folderBox.addItem (src.label, filterDirs.size() + 1);
        if (i == 0) continue;
        juce::Array<juce::File> subs;
        for (auto& it : allItems)
        {
            if (it.source != i) continue;
            if (src.ableton) ++abletonSounds;
            auto d = it.file.getParentDirectory();
            if (d == src.dir) continue;
            if (src.ableton) while (d.getParentDirectory() != src.dir) d = d.getParentDirectory();   // just the packs / top folders
            subs.addIfNotAlreadyThere (d);
        }
        if (src.ableton) abletonFound.addIfNotAlreadyThere (src.label.fromFirstOccurrenceOf ("Ableton ", false, false));
        subs.sort (cmp);
        for (auto& d : subs)
        {
            filterDirs.add (d);
            folderBox.addItem (juce::String::fromUTF8 ("\xe2\x80\xba  ") + d.getFileName().trim(), filterDirs.size() + 1);
        }
    }
    folderBox.setSelectedId (1, juce::dontSendNotification);
    for (int i = 0; i < folderBox.getNumItems(); ++i)
        if (folderBox.getItemText (i) == previous) folderBox.setSelectedItemIndex (i, juce::dontSendNotification);

    status = juce::String ((int) allItems.size()) + " sounds";
    if (! abletonFound.isEmpty())
        status << juce::String::fromUTF8 ("   \xc2\xb7   from Ableton Live: ") << abletonFound.joinIntoString (", ")
               << " (" << abletonSounds << " sounds, used where they are)";
    applyFilter();
}

void LibraryPanel::applyFilter()
{
    shown.clear();
    const auto q = search.getText().trim();
    const int folderIndex = folderBox.getSelectedId() - 2;
    for (int i = 0; i < (int) allItems.size(); ++i)
    {
        const auto& it = allItems[(size_t) i];
        if (q.isNotEmpty() && ! it.name.containsIgnoreCase (q) && ! it.folder.containsIgnoreCase (q)) continue;
        if (folderIndex >= 0 && folderIndex < filterDirs.size() && ! it.file.isAChildOf (filterDirs[folderIndex])) continue;
        shown.push_back (i);
    }
    list.updateContent();
    list.setVisible (! shown.empty());
    list.repaint();
    repaint();
}

void LibraryPanel::resized()
{
    auto r = getLocalBounds().reduced (8);
    auto top = r.removeFromTop (28);
    stopButton.setBounds (top.removeFromRight (100)); top.removeFromRight (6);
    showFolder.setBounds (top.removeFromRight (130)); top.removeFromRight (6);
    addFolder.setBounds (top.removeFromRight (110));  top.removeFromRight (6);
    folderBox.setBounds (top.removeFromRight (150));  top.removeFromRight (6);
    search.setBounds (top);
    r.removeFromTop (4);
    statusArea = r.removeFromTop (18);
    r.removeFromTop (2);
    list.setBounds (r);
}

void LibraryPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::textDim);
    g.setFont (theme::font (11.5f, true));
    g.drawText (status, statusArea.reduced (4, 0), juce::Justification::centredLeft, true);
    if (shown.empty())
    {
        g.setFont (theme::font (14.0f));
        g.drawText (scanner != nullptr && scanner->isThreadRunning() ? "Looking for sounds..."
                    : allItems.empty() ? "Nothing here yet. Arm a track and hit record: every take is saved to your library."
                                       : "No sounds match your search.",
                    list.getBounds(), juce::Justification::centred);
    }
}

void LibraryPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) shown.size()) return;
    auto& it = allItems[(size_t) shown[(size_t) row]];
    if (it.seconds < 0)   // the length is read when the row first shows (scanning thousands of files up front would be slow)
    {
        it.seconds = 0;
        if (std::unique_ptr<juce::AudioFormatReader> r { ctx.project.audioCache.formats.createReaderFor (it.file) }; r != nullptr && r->sampleRate > 0)
            it.seconds = (double) r->lengthInSamples / r->sampleRate;
    }
    if (selected) { g.setColour (theme::accent.withAlpha (0.25f)); g.fillRoundedRectangle (2, 1, (float) w - 4, (float) h - 2, 4); }

    g.setColour (theme::accent2);
    juce::Path wave;
    for (int k = 0; k < 6; ++k)
    {
        const float bh = (k % 3 == 1 ? 14.0f : k % 2 ? 9.0f : 6.0f);
        wave.addRectangle (12.0f + k * 3.5f, h / 2.0f - bh / 2, 2.0f, bh);
    }
    g.fillPath (wave);

    g.setColour (theme::text);
    g.setFont (theme::font (13.5f));
    g.drawText (it.name, 42, 0, w - 220, h, juce::Justification::centredLeft, true);
    g.setColour (theme::textDim);
    g.setFont (theme::font (12.0f));
    g.drawText (it.folder, w - 170, 0, 100, h, juce::Justification::centredRight, true);
    const int secs = (int) std::round (it.seconds);
    const auto len = it.seconds < 10.0 ? juce::String (it.seconds, 1) + " s" : juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2);
    g.drawText (len, w - 60, 0, 50, h, juce::Justification::centredRight);
}

// one click plays a sound; so does moving through the list with the arrow keys
void LibraryPanel::previewRow (int row)
{
    if (row < 0 || row >= (int) shown.size()) return;
    lastPreviewRow = row;
    lastPreviewTime = juce::Time::getMillisecondCounter();
    ctx.engine.preview (ctx.project.audioCache.load (allItems[(size_t) shown[(size_t) row]].file));
}

void LibraryPanel::selectedRowsChanged (int lastRowSelected)
{
    previewRow (lastRowSelected);
}

void LibraryPanel::listBoxItemClicked (int row, const juce::MouseEvent& e)
{
    if (e.mods.isShiftDown() || e.mods.isCommandDown()) return;   // adding to a multi-selection
    // a click on the row that's already selected plays it again (a new selection already played above)
    if (row == lastPreviewRow && juce::Time::getMillisecondCounter() - lastPreviewTime < 150) return;
    previewRow (row);
}

juce::var LibraryPanel::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    juce::StringArray paths;
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i] >= 0 && rows[i] < (int) shown.size())
            paths.add (allItems[(size_t) shown[(size_t) rows[i]]].file.getFullPathName());
    return paths.joinIntoString ("\n");
}

} // namespace rs
