#include "LibraryPanel.h"

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
                folders.addIfNotAlreadyThere (fc.getResult());
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
    for (int i = 1; i < folders.size(); ++i)   // index 0 is always Recordings
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
            it.folder = folder.getFileName();
            if (std::unique_ptr<juce::AudioFormatReader> r { formats.createReaderFor (it.file) }; r != nullptr && r->sampleRate > 0)
                it.seconds = (double) r->lengthInSamples / r->sampleRate;
            allItems.push_back (it);
        }
    }
    // newest recordings first
    std::sort (allItems.begin(), allItems.end(), [] (const Item& a, const Item& b)
    {
        return a.file.getLastModificationTime() > b.file.getLastModificationTime();
    });

    const auto previous = folderBox.getSelectedId();
    folderBox.clear (juce::dontSendNotification);
    folderBox.addItem ("All folders", 1);
    for (int i = 0; i < folders.size(); ++i)
        folderBox.addItem (i == 0 ? juce::String ("My recordings") : folders[i].getFileName(), i + 2);
    folderBox.setSelectedId (previous > 0 ? previous : 1, juce::dontSendNotification);

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
        if (folderIndex >= 0 && ! it.file.isAChildOf (folders[folderIndex])) continue;
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
    g.drawText (juce::String (secs / 60) + ":" + juce::String (secs % 60).paddedLeft ('0', 2), w - 60, 0, 50, h, juce::Justification::centredRight);
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
