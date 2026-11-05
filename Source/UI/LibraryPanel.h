#pragma once
#include "AppContext.h"

namespace rs
{

// =============================================================================
//  Sound library: every take you record lands here automatically, plus any
//  folders of loops/samples you add, your drum kit folders, and the sounds of
//  an installed Ableton Live (found automatically, used where they are).
//  Folders are scanned in the background. Drag a sound onto the timeline to use it.
// =============================================================================
class LibraryPanel : public juce::Component,
                     private juce::ListBoxModel,
                     private juce::Timer
{
public:
    explicit LibraryPanel (AppContext&);
    ~LibraryPanel() override;

    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Item { juce::File file; juce::String name; double seconds = -1; juce::String folder; int source = 0; };
    struct Source { juce::File dir; juce::String label; bool ableton = false; };
    class Scanner;

    AppContext& ctx;
    juce::ListBox list { "sounds", this };
    juce::TextEditor search;
    juce::TextButton addFolder { "Add folder..." }, showFolder { "Show recordings" }, stopButton { "Stop preview" };
    juce::ComboBox folderBox;
    juce::Array<juce::File> folders;           // [0] recordings, then added folders, then kit folders
    int numSavedFolders = 0;                    // folders[1 .. numSavedFolders] were added by the user
    std::vector<Source> sources;                // folders, then Ableton's libraries
    juce::Array<juce::File> filterDirs;         // what each folder-menu entry (id 2+) shows
    std::vector<Item> allItems;
    std::vector<int> shown;                     // indices into allItems
    std::unique_ptr<Scanner> scanner;
    juce::String status;
    juce::Rectangle<int> statusArea;
    void scanFinished (std::vector<Item>);
    void timerCallback() override;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::PropertiesFile> settings;

    void applyFilter();
    void saveFolders();

    int getNumRows() override { return (int) shown.size(); }
    void paintListBoxItem (int row, juce::Graphics&, int w, int h, bool selected) override;
    void listBoxItemDoubleClicked (int row, const juce::MouseEvent&) override;
    juce::var getDragSourceDescription (const juce::SparseSet<int>& rows) override;
    void deleteKeyPressed (int) override {}
};

} // namespace rs
