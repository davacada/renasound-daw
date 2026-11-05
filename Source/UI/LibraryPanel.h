#pragma once
#include "AppContext.h"

namespace rs
{

// =============================================================================
//  Sound library: every take you record lands here automatically, plus any
//  folders of loops/samples you add. Drag a sound onto the timeline to use it.
// =============================================================================
class LibraryPanel : public juce::Component,
                     private juce::ListBoxModel
{
public:
    explicit LibraryPanel (AppContext&);
    ~LibraryPanel() override;

    void refresh();
    void resized() override;
    void paint (juce::Graphics&) override;

private:
    struct Item { juce::File file; juce::String name; double seconds = 0; juce::String folder; };

    AppContext& ctx;
    juce::ListBox list { "sounds", this };
    juce::TextEditor search;
    juce::TextButton addFolder { "Add folder..." }, showFolder { "Show recordings" }, stopButton { "Stop preview" };
    juce::ComboBox folderBox;
    juce::Array<juce::File> folders;
    std::vector<Item> allItems, shown;
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
