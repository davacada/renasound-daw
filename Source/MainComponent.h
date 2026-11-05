#pragma once
#include "UI/TimelineView.h"
#include "UI/PianoRoll.h"
#include "UI/PitchEditor.h"
#include "UI/MixerView.h"
#include "UI/LibraryPanel.h"
#include "UI/DeviceWindow.h"
#include "MixcraftImport.h"
#include "AbletonImport.h"
#include "Updater.h"

namespace rs
{

enum CommandIDs
{
    cmdNew = 0x2000, cmdOpen, cmdSave, cmdSaveAs, cmdExport, cmdImport,
    cmdUndo, cmdRedo, cmdDeleteClip, cmdDuplicateClip, cmdSplitClip,
    cmdPlayPause, cmdRecord, cmdToStart, cmdToggleLoop, cmdToggleMetronome,
    cmdAddAudioTrack, cmdAddInstrumentTrack, cmdAudioSettings, cmdZoomIn, cmdZoomOut,
    cmdShowEditor, cmdShowMixer, cmdShowLibrary, cmdPluginManager, cmdTrackEffects, cmdMasterEffects, cmdImportMixcraft
};

class MainComponent : public juce::Component,
                      public juce::DragAndDropContainer,
                      public juce::ApplicationCommandTarget,
                      public juce::MenuBarModel,
                      public juce::ChangeListener,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent&) override;   // clicking the clock switches bars / time

    juce::ApplicationCommandManager commandManager;
    std::function<void (const juce::String&)> onTitleChanged;

    // Ask to save unsaved changes, then run `then` unless the user cancelled.
    void askToSaveThen (std::function<void()> then);
    bool loadProjectFile (const juce::File&);

    // ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands (juce::Array<juce::CommandID>&) override;
    void getCommandInfo (juce::CommandID, juce::ApplicationCommandInfo&) override;
    bool perform (const InvocationInfo&) override;

    // MenuBarModel
    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex (int, const juce::String&) override;
    void menuItemSelected (int, int) override {}

private:
    AudioCache audioCache;
    Project project { audioCache };
    AudioEngine engine { project };
    AppContext ctx { project, engine };
    PitchService pitchService { project };
    theme::LookAndFeel lookAndFeel;
    juce::TooltipWindow tooltips { this, 600 };

    // transport bar
    IconButton toStart { "Return to start", IconButton::Icon::ToStart },
               playButton { "Play", IconButton::Icon::Play },
               stopButton { "Stop", IconButton::Icon::Stop },
               recordButton { "Record", IconButton::Icon::Record },
               loopButton { "Loop", IconButton::Icon::Loop },
               clickButton { "Metronome", IconButton::Icon::Metronome };
    juce::Label timeDisplay, bpmLabel;
    juce::Slider bpm;
    juce::TextButton mixdown { "Mix down" }, settingsButton { "Audio setup" }, updateButton { "Update" };
    Updater updater;
    bool userAskedForUpdate = false;
    void updaterChanged();
    void updateButtonClicked();

    // views
    TimelineView timeline { ctx };
    PianoRoll pianoRoll { ctx };
    PitchEditor pitchEditor { ctx, pitchService };

    // The Editor tab: piano roll for MIDI clips, pitch editor for audio clips
    struct EditorPanel : juce::Component
    {
        EditorPanel (juce::Component& a, juce::Component& b) : midi (a), audio (b) { addAndMakeVisible (midi); addChildComponent (audio); }
        void showAudio (bool yes) { midi.setVisible (! yes); audio.setVisible (yes); }
        void resized() override { midi.setBounds (getLocalBounds()); audio.setBounds (getLocalBounds()); }
        juce::Component& midi;
        juce::Component& audio;
    } editorPanel { pianoRoll, pitchEditor };
    MixerView mixer { ctx };
    LibraryPanel library { ctx };
    juce::TabbedComponent tabs { juce::TabbedButtonBar::TabsAtTop };
    juce::StretchableLayoutManager layout;
    juce::StretchableLayoutResizerBar divider { &layout, 1, false };
    juce::MidiKeyboardComponent keyboard { engine.keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard };
    juce::Label keyboardLabel;

    std::vector<std::unique_ptr<DeviceWindow>> deviceWindows;
    std::unique_ptr<PluginManagerWindow> pluginManager;
    juce::Image logo;

    void openDevice (int trackId, FxPtr slot, bool instrument);
    void showInMixer (int trackId);
    void openPluginManager();

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> renameWindow;
    juce::ScopedMessageBox messageBox;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void updateTitle();
    void record();
    void addTrack (Track::Type);
    void renameTrack (int trackId);
    void importAudio();
    void openProject();
    void importMixcraft (const juce::File&, juce::Array<juce::File> searchFolders);
    void applyImport (DawImport&, const juce::File& source);
    juce::String suggestedProjectName;   // for "Save" after importing another DAW's project
    void saveProject (bool saveAs, std::function<void (bool)> done = {});
    void exportMixdown();
    void showAudioSettings();
    void showMessage (const juce::String& title, const juce::String& text);
};

} // namespace rs
