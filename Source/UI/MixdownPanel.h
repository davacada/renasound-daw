#pragma once
#include "AppContext.h"
#include "../Export.h"

namespace rs
{

// =============================================================================
//  The Mix down window: the song's details and cover, the file type and
//  quality, and whether to export one stereo mix or a file per track (stems).
// =============================================================================
class MixdownPanel : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit MixdownPanel (AppContext&);
    ~MixdownPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseUp (const juce::MouseEvent&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override { return true; }
    void filesDropped (const juce::StringArray&, int, int) override;

    std::function<void()> onClose;
    std::function<void (const juce::String& title, const juce::String& text)> showMessage;

private:
    AppContext& ctx;
    juce::TextEditor title, artist, album, writers, producers;
    juce::ComboBox format, quality, rate;
    juce::TextButton mixButton { "Stereo mix" }, stemsButton { "Stems" };
    juce::ToggleButton withMix { "Also the full mix" };
    juce::TextButton removeCover { "Remove picture" }, cancel { "Cancel" }, go { "Mix down..." };
    juce::Image coverImage;
    juce::File coverFile;
    juce::Rectangle<int> coverArea;
    std::unique_ptr<juce::FileChooser> chooser;

    void setCover (const juce::File&);
    void refreshQuality();
    void storeSettings();
    exporter::Settings settings() const;
    exporter::SongInfo songInfo() const;
    void start();
    juce::String summary() const;
};

} // namespace rs
