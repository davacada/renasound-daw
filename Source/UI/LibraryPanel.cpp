#include "LibraryPanel.h"
#include "../Drums.h"

namespace rs
{

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

LibraryPanel::~LibraryPanel() = default;

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
    allItems.clear();
    auto& formats = ctx.project.audioCache.formats;
    const auto wildcard = formats.getWildcardForAllFormats();

    for (auto& folder : folders)
    {
        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, wildcard, juce::File::findFiles))
        {
            Item it;
            it.file = entry.getFile();
            it.name = it.file.getFileNameWithoutExtension();
            // show the sound's own folder ("Congas") rather than the top one
            it.folder = folder == folders[0] ? juce::String ("Recording") : it.file.getParentDirectory().getFileName().trim();
            if (std::unique_ptr<juce::AudioFormatReader> r { formats.createReaderFor (it.file) }; r != nullptr && r->sampleRate > 0)
                it.seconds = (double) r->lengthInSamples / r->sampleRate;
            allItems.push_back (it);
        }
    }
    // newest recordings first, then sounds by folder and name
    const auto rec = folders[0];
    std::stable_sort (allItems.begin(), allItems.end(), [&rec] (const Item& a, const Item& b)
    {
        const bool ra = a.file.isAChildOf (rec), rb = b.file.isAChildOf (rec);
        if (ra != rb) return ra;
        if (ra) return a.file.getLastModificationTime() > b.file.getLastModificationTime();
        if (a.folder != b.folder) return a.folder.compareNatural (b.folder) < 0;
        return a.name.compareNatural (b.name) < 0;
    });

    // folder menu: every library folder, and under each one the subfolders that hold sounds
    const auto previous = folderBox.getText();
    folderBox.clear (juce::dontSendNotification);
    filterDirs.clear();
    folderBox.addItem ("All sounds", 1);
    for (int i = 0; i < folders.size(); ++i)
    {
        filterDirs.add (folders[i]);
        folderBox.addItem (i == 0 ? juce::String ("My recordings") : folders[i].getFileName().trim(), filterDirs.size() + 1);
        if (i == 0) continue;
        juce::Array<juce::File> subs;
        for (auto& it : allItems)
            if (it.file.isAChildOf (folders[i]) && it.file.getParentDirectory() != folders[i])
                subs.addIfNotAlreadyThere (it.file.getParentDirectory());
        struct ByName { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()); } } cmp;
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

    applyFilter();
}

void LibraryPanel::applyFilter()
{
    shown.clear();
    const auto q = search.getText().trim();
    const int folderIndex = folderBox.getSelectedId() - 2;
    for (auto& it : allItems)
    {
        if (q.isNotEmpty() && ! it.name.containsIgnoreCase (q)) continue;
        if (folderIndex >= 0 && folderIndex < filterDirs.size() && ! it.file.isAChildOf (filterDirs[folderIndex])) continue;
        shown.push_back (it);
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
    r.removeFromTop (6);
    list.setBounds (r);
}

void LibraryPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    if (shown.empty())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (14.0f));
        g.drawText (allItems.empty() ? "Nothing here yet. Arm a track and hit record: every take is saved to your library."
                                     : "No sounds match your search.",
                    list.getBounds(), juce::Justification::centred);
    }
}

void LibraryPanel::paintListBoxItem (int row, juce::Graphics& g, int w, int h, bool selected)
{
    if (row < 0 || row >= (int) shown.size()) return;
    auto& it = shown[(size_t) row];
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

void LibraryPanel::listBoxItemDoubleClicked (int row, const juce::MouseEvent&)
{
    if (row >= 0 && row < (int) shown.size())
        ctx.engine.preview (ctx.project.audioCache.load (shown[(size_t) row].file));
}

juce::var LibraryPanel::getDragSourceDescription (const juce::SparseSet<int>& rows)
{
    juce::StringArray paths;
    for (int i = 0; i < rows.size(); ++i)
        if (rows[i] >= 0 && rows[i] < (int) shown.size())
            paths.add (shown[(size_t) rows[i]].file.getFullPathName());
    return paths.joinIntoString ("\n");
}

} // namespace rs
