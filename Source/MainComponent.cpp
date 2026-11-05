#include "MainComponent.h"

namespace rs
{

MainComponent::MainComponent()
{
    setLookAndFeel (&lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // ---- transport ----
    for (auto* b : { &toStart, &playButton, &stopButton, &recordButton, &loopButton, &clickButton })
        addAndMakeVisible (b);

    toStart.onClick      = [this] { engine.setPositionBeats (0); };
    playButton.onClick   = [this] { engine.togglePlay(); };
    stopButton.onClick   = [this] { engine.stop(); };
    recordButton.onClick = [this] { record(); };
    loopButton.onClick   = [this] { commandManager.invokeDirectly (cmdToggleLoop, true); };
    clickButton.onClick  = [this] { commandManager.invokeDirectly (cmdToggleMetronome, true); };
    loopButton.onColour  = theme::accent2;
    toStart.setTooltip ("Return to start (Home)");
    playButton.setTooltip ("Play / pause (Space)");
    stopButton.setTooltip ("Stop. Press again to return to the start");
    recordButton.setTooltip ("Record onto armed tracks (R)");
    loopButton.setTooltip ("Loop (L). Shift-drag in the ruler to set the loop region");
    clickButton.setTooltip ("Metronome (C)");

    addAndMakeVisible (timeDisplay);
    timeDisplay.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 20.0f, juce::Font::bold)));
    timeDisplay.setJustificationType (juce::Justification::centred);
    timeDisplay.setColour (juce::Label::backgroundColourId, theme::bg);
    timeDisplay.setColour (juce::Label::textColourId, theme::accent);

    addAndMakeVisible (bpmLabel);
    bpmLabel.setText ("BPM", juce::dontSendNotification);
    bpmLabel.setColour (juce::Label::textColourId, theme::textDim);
    bpmLabel.setFont (theme::font (11.0f, true));
    bpmLabel.setJustificationType (juce::Justification::centredRight);

    addAndMakeVisible (bpm);
    bpm.setSliderStyle (juce::Slider::IncDecButtons);
    bpm.setTextBoxStyle (juce::Slider::TextBoxLeft, false, 52, 26);
    bpm.setRange (40.0, 240.0, 1.0);
    bpm.setValue (project.bpm, juce::dontSendNotification);
    bpm.onValueChange = [this]
    {
        const double beat = engine.getPositionBeats();
        {
            const juce::ScopedLock sl (project.lock);
            project.bpm = bpm.getValue();
        }
        engine.setPositionBeats (beat);
        project.dirty = true;
        timeline.repaint();
    };

    for (auto* b : { &addAudio, &addInstrument, &mixdown, &settingsButton })
        addAndMakeVisible (b);
    addAudio.onClick      = [this] { addTrack (Track::Type::Audio); };
    addInstrument.onClick = [this]
    {
        addTrack (Track::Type::Instrument);
        // straight away: which instrument should the new track play?
        if (! project.tracks.empty()) devicemenus::showInstrumentMenu (ctx, project.tracks.back()->id, &addInstrument);
    };
    mixdown.onClick       = [this] { exportMixdown(); };
    settingsButton.onClick = [this] { showAudioSettings(); };
    addAudio.setTooltip ("Add an audio track for recording or loops");
    addInstrument.setTooltip ("Add a virtual instrument track you play with MIDI");
    mixdown.setTooltip ("Export your song as a WAV file");
    mixdown.setColour (juce::TextButton::buttonColourId, theme::accent);

    // ---- views ----
    addAndMakeVisible (timeline);
    addAndMakeVisible (divider);
    addAndMakeVisible (tabs);
    tabs.setTabBarDepth (34);
    tabs.addTab ("Editor", theme::panel, &editorPanel, false);
    tabs.addTab ("Mixer", theme::panel, &mixer, false);
    tabs.addTab ("Library", theme::panel, &library, false);
    tabs.setCurrentTabIndex (1);

    layout.setItemLayout (0, 150, -1.0, -0.5);    // timeline
    layout.setItemLayout (1, 6, 6, 6);            // divider
    layout.setItemLayout (2, 220, -0.75, -0.5);   // tabs (mixer strips need room for effects)

    addAndMakeVisible (keyboard);
    keyboard.setAvailableRange (21, 108);
    keyboard.setScrollButtonsVisible (false);
    keyboard.setOctaveForMiddleC (4);   // middle C = C4, same as the piano roll
    addAndMakeVisible (keyboardLabel);
    keyboardLabel.setFont (theme::font (11.5f, true));
    keyboardLabel.setColour (juce::Label::textColourId, theme::textDim);
    keyboardLabel.setJustificationType (juce::Justification::centred);

    // ---- context callbacks ----
    ctx.openEditor = [this] (int clipId)
    {
        auto c = project.findClip (clipId);
        if (c == nullptr) return;
        if (c->type == Clip::Type::Midi) pianoRoll.showClip (clipId);
        else                             pitchEditor.showClip (clipId);
        editorPanel.showAudio (c->type == Clip::Type::Audio);
        tabs.setCurrentTabIndex (0);
    };
    ctx.renameTrack = [this] (int id) { renameTrack (id); };
    ctx.openDevice = [this] (int id, FxPtr slot, bool instrument) { openDevice (id, std::move (slot), instrument); };
    ctx.showInMixer = [this] (int id) { showInMixer (id); };
    ctx.openPluginManager = [this] { openPluginManager(); };
    ctx.openProjectFile = [this] (const juce::File& f)
    {
        askToSaveThen ([this, f] { if (! loadProjectFile (f)) showMessage ("Couldn't open project", "RENASOUND can open its own projects, Ableton Live sets and Mixcraft projects."); });
    };
    logo = juce::ImageCache::getFromMemory (BinaryData::logo_mark_png, BinaryData::logo_mark_pngSize);
    ctx.onSelectionChanged = [this]
    {
        if (auto c = project.findClip (ctx.selectedClipId))
        {
            if (c->type == Clip::Type::Midi) pianoRoll.showClip (c->id);
            else if (tabs.getCurrentTabIndex() == 0) pitchEditor.showClip (c->id);   // only analyse when the editor is open
            if (tabs.getCurrentTabIndex() == 0 || c->type == Clip::Type::Midi)
                editorPanel.showAudio (c->type == Clip::Type::Audio);
        }
        timeline.repaint();
        mixer.repaint();
    };

    engine.onRecordingFinished = [this] { library.refresh(); };
    project.addChangeListener (this);

    // ---- commands & shortcuts ----
    commandManager.registerAllCommandsForTarget (this);
    addKeyListener (commandManager.getKeyMappings());
    setWantsKeyboardFocus (true);

    // start with something to play with, like a fresh Mixcraft project
    project.addTrack (Track::Type::Audio);
    project.addTrack (Track::Type::Instrument);
    project.dirty = false;
    if (! project.tracks.empty()) ctx.selectTrack (project.tracks.back()->id);

    setSize (1360, 860);
    startTimerHz (20);
}

MainComponent::~MainComponent()
{
    deviceWindows.clear();
    pluginManager.reset();
    project.removeChangeListener (this);
    engine.stop();
    setLookAndFeel (nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

// =============================================================================
void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (theme::bg);

    auto bar = getLocalBounds().removeFromTop (56);
    g.setColour (theme::panel);
    g.fillRect (bar);

    // logo + wordmark
    if (logo.isValid())
    {
        const float h = 24.0f, w = h * (float) logo.getWidth() / (float) logo.getHeight();
        g.setOpacity (1.0f);
        g.drawImage (logo, juce::Rectangle<float> (18.0f, 16.0f, w, h), juce::RectanglePlacement::centred);
    }
    g.setColour (theme::text);
    // letter-spaced wordmark
    float x = 62.0f;
    for (auto ch : juce::String ("RENASOUND"))
    {
        juce::String letter;
        letter << juce::String::charToString (ch);
        g.setFont (theme::font (16.5f, true));
        g.drawText (letter, juce::Rectangle<float> (x, 0, 20, 56), juce::Justification::centredLeft);
        x += juce::GlyphArrangement::getStringWidth (theme::font (16.5f, true), letter) + 2.2f;
    }

    g.setColour (theme::panel);
    g.fillRect (getLocalBounds().removeFromBottom (72));
}

void MainComponent::resized()
{
    auto r = getLocalBounds();
    auto bar = r.removeFromTop (56).reduced (10, 10);
    bar.removeFromLeft (196);

    for (auto* b : { &toStart, &playButton, &stopButton, &recordButton })
    {
        b->setBounds (bar.removeFromLeft (44));
        bar.removeFromLeft (5);
    }
    bar.removeFromLeft (8);
    timeDisplay.setBounds (bar.removeFromLeft (190));
    bar.removeFromLeft (10);
    loopButton.setBounds (bar.removeFromLeft (40));  bar.removeFromLeft (5);
    clickButton.setBounds (bar.removeFromLeft (40)); bar.removeFromLeft (12);
    bpmLabel.setBounds (bar.removeFromLeft (34));    bar.removeFromLeft (4);
    bpm.setBounds (bar.removeFromLeft (110));

    settingsButton.setBounds (bar.removeFromRight (104)); bar.removeFromRight (6);
    mixdown.setBounds (bar.removeFromRight (96));         bar.removeFromRight (16);
    addInstrument.setBounds (bar.removeFromRight (110));  bar.removeFromRight (6);
    addAudio.setBounds (bar.removeFromRight (84));

    auto keys = r.removeFromBottom (72).reduced (8, 6);
    keyboardLabel.setBounds (keys.removeFromLeft (120));
    keyboard.setBounds (keys);
    keyboard.setKeyWidth (juce::jmax (12.0f, keys.getWidth() / 52.0f));   // 52 white keys from A0 to C8
    keyboard.setLowestVisibleKey (21);

    r.reduce (6, 6);
    juce::Component* comps[] = { &timeline, &divider, &tabs };
    layout.layOutComponents (comps, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);
}

void MainComponent::timerCallback()
{
    // time display: bar.beat and minutes:seconds
    const double beats = engine.getPositionBeats();
    const int bar = (int) (beats / project.beatsPerBar) + 1;
    const int beat = (int) std::fmod (beats, (double) project.beatsPerBar) + 1;
    const double secs = beats * 60.0 / project.bpm;
    timeDisplay.setText (juce::String (bar).paddedLeft ('0', 3) + "." + juce::String (beat)
                             + "   " + juce::String ((int) secs / 60) + ":" + juce::String (std::fmod (secs, 60.0), 1).paddedLeft ('0', 4),
                         juce::dontSendNotification);

    playButton.setIcon (engine.isPlaying() ? IconButton::Icon::Pause : IconButton::Icon::Play);
    playButton.setToggleState (engine.isPlaying() && ! engine.isRecording(), juce::dontSendNotification);
    recordButton.setToggleState (engine.isRecording(), juce::dontSendNotification);
    loopButton.setToggleState (project.loopEnabled, juce::dontSendNotification);
    clickButton.setToggleState (engine.metronomeOn.load(), juce::dontSendNotification);
    if (! bpm.isMouseButtonDown() && std::abs (bpm.getValue() - project.bpm) > 0.01)
        bpm.setValue (project.bpm, juce::dontSendNotification);

    juce::String plays = "Keyboard: no instrument";
    if (auto t = project.findTrack (engine.liveInputTrackId); t != nullptr)
        plays = "Keyboard plays\n" + t->name;
    keyboardLabel.setText (plays, juce::dontSendNotification);

    updateTitle();
}

void MainComponent::changeListenerCallback (juce::ChangeBroadcaster*)
{
    // keep the keyboard pointed at a valid instrument track
    if (project.findTrack (engine.liveInputTrackId) == nullptr)
    {
        engine.liveInputTrackId = -1;
        for (auto& t : project.tracks)
            if (t->type == Track::Type::Instrument) { engine.liveInputTrackId = t->id; break; }
    }
    commandManager.commandStatusChanged();
}

void MainComponent::updateTitle()
{
    auto name = project.file.existsAsFile() ? project.file.getFileNameWithoutExtension() : juce::String ("Untitled");
    if (onTitleChanged) onTitleChanged (name + (project.dirty ? " *" : "") + " - RENASOUND");
}

// =============================================================================
//  Actions
// =============================================================================
void MainComponent::addTrack (Track::Type type)
{
    project.beginEdit();
    auto t = project.addTrack (type);
    ctx.selectTrack (t->id);
}

void MainComponent::openDevice (int trackId, FxPtr slot, bool instrument)
{
    for (auto& w : deviceWindows)
        if (w->shows (trackId, slot, instrument)) { w->setVisible (true); w->toFront (true); return; }

    auto* w = deviceWindows.emplace_back (std::make_unique<DeviceWindow> (ctx, trackId, slot, instrument)).get();
    const int cascade = 28 * (int) ((deviceWindows.size() - 1) % 8);   // don't stack windows exactly on top of each other
    w->setTopLeftPosition (w->getX() + cascade, w->getY() + cascade);
    w->onClose = [sp = juce::Component::SafePointer<MainComponent> (this), w]
    {
        juce::MessageManager::callAsync ([sp, w]
        {
            if (sp == nullptr) return;
            auto& v = sp->deviceWindows;
            v.erase (std::remove_if (v.begin(), v.end(), [w] (auto& p) { return p.get() == w; }), v.end());
        });
    };
    w->toFront (true);
}

void MainComponent::showInMixer (int trackId)
{
    tabs.setCurrentTabIndex (1);
    if (trackId >= 0) ctx.selectTrack (trackId);
    mixer.scrollToTrack (trackId);
}

void MainComponent::openPluginManager()
{
    if (pluginManager == nullptr)
    {
        pluginManager = std::make_unique<PluginManagerWindow> (ctx);
        pluginManager->onClose = [sp = juce::Component::SafePointer<MainComponent> (this)]
        {
            juce::MessageManager::callAsync ([sp]
            {
                if (sp != nullptr) sp->pluginManager.reset();
            });
        };
    }
    pluginManager->toFront (true);
}

void MainComponent::record()
{
    if (engine.isRecording()) { engine.stop(); return; }

    bool anyArmed = false;
    for (auto& t : project.tracks) anyArmed = anyArmed || t->armed;

    if (! anyArmed)
    {
        // like Mixcraft: arm the selected track, or make an audio track to record into
        auto t = project.findTrack (ctx.selectedTrackId);
        if (t == nullptr)
        {
            project.beginEdit();
            t = project.addTrack (Track::Type::Audio);
            ctx.selectTrack (t->id);
        }
        const juce::ScopedLock sl (project.lock);
        t->armed = true;
    }
    engine.startRecording();
}

void MainComponent::renameTrack (int trackId)
{
    auto t = project.findTrack (trackId);
    if (t == nullptr) return;

    renameWindow = std::make_unique<juce::AlertWindow> ("Rename track", "", juce::MessageBoxIconType::NoIcon, this);
    renameWindow->addTextEditor ("name", t->name);
    renameWindow->addButton ("Rename", 1, juce::KeyPress (juce::KeyPress::returnKey));
    renameWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    renameWindow->enterModalState (true, juce::ModalCallbackFunction::create ([this, trackId] (int result)
    {
        if (result == 1 && renameWindow != nullptr)
        {
            const auto newName = renameWindow->getTextEditorContents ("name").trim();
            if (auto tr = project.findTrack (trackId); tr != nullptr && newName.isNotEmpty())
            {
                project.beginEdit();
                {
                    const juce::ScopedLock sl (project.lock);
                    tr->name = newName;
                }
                project.changed();
            }
        }
        juce::MessageManager::callAsync ([this] { renameWindow.reset(); });
    }), false);
}

void MainComponent::importAudio()
{
    chooser = std::make_unique<juce::FileChooser> ("Import audio", juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                   audioCache.formats.getWildcardForAllFormats());
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
    {
        if (fc.getResults().isEmpty()) return;
        project.beginEdit();
        auto target = project.findTrack (ctx.selectedTrackId);
        for (auto& f : fc.getResults())
        {
            if (target == nullptr || target->type != Track::Type::Audio || ! target->clips.empty())
                target = project.addTrack (Track::Type::Audio);
            if (auto c = project.addAudioClip (target->id, f, engine.getPositionBeats()))
                ctx.selectClip (c->id);
            target = nullptr;   // next file goes on a new track
        }
    });
}

void MainComponent::askToSaveThen (std::function<void()> then)
{
    if (! project.dirty) { then(); return; }

    auto opts = juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::QuestionIcon)
                    .withTitle ("Save changes?")
                    .withMessage ("Do you want to save the changes to your project first?")
                    .withButton ("Save", 1)
                    .withButton ("Don't save", 2)
                    .withButton ("Cancel", 0)
                    .withAssociatedComponent (this);

    messageBox = juce::AlertWindow::showScopedAsync (opts, [this, then] (int r)
    {
        if (r == 1) saveProject (false, [then] (bool ok) { if (ok) then(); });
        else if (r == 2) then();
    });
}

void MainComponent::openProject()
{
    askToSaveThen ([this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Open a RENASOUND, Ableton Live or Mixcraft project",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Renasound"),
                                                       "*.renasound;*.als;*.mx4;*.mx5;*.mx6;*.mx7;*.mx8;*.mx9;*.mx10;*.mx11;*.mx12");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                              [this] (const juce::FileChooser& fc)
        {
            if (! fc.getResult().existsAsFile()) return;
            if (! loadProjectFile (fc.getResult()))
                showMessage ("Couldn't open project", "That file isn't a RENASOUND, Ableton Live or Mixcraft project.");
        });
    });
}

bool MainComponent::loadProjectFile (const juce::File& f)
{
    if (isMixcraftProject (f) || isAbletonProject (f))
    {
        importMixcraft (f, {});
        return true;
    }
    engine.stop();
    deviceWindows.clear();
    if (! project.load (f)) return false;
    suggestedProjectName.clear();
    bpm.setValue (project.bpm, juce::dontSendNotification);
    engine.setPositionBeats (0);
    pianoRoll.showClip (-1);
    pitchEditor.showClip (-1);
    if (! project.tracks.empty()) ctx.selectTrack (project.tracks.front()->id);
    return true;
}

// =============================================================================
//  Mixcraft import
// =============================================================================
void MainComponent::importMixcraft (const juce::File& f, juce::Array<juce::File> searchFolders)
{
    auto imp = std::make_shared<DawImport> (isAbletonProject (f) ? importAbletonProject (f, searchFolders)
                                                                 : importMixcraftProject (f, searchFolders));
    if (! imp->ok)
    {
        showMessage ("Couldn't import", imp->error);
        return;
    }
    if (imp->missingFiles.isEmpty())
    {
        applyImport (*imp, f);
        return;
    }

    // some audio isn't next to the project: offer to look for it
    auto list = imp->missingFiles.size() <= 6 ? imp->missingFiles.joinIntoString ("\n")
                                              : imp->missingFiles.joinIntoString ("\n", 0, 5) + "\n...and " + juce::String (imp->missingFiles.size() - 5) + " more";
    auto opts = juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::NoIcon)
                    .withTitle (juce::String (imp->missingFiles.size()) + " audio file" + (imp->missingFiles.size() == 1 ? "" : "s") + " not found")
                    .withMessage ("This " + imp->fromApp + " project uses audio that isn't in its folder:\n\n" + list
                                  + "\n\nPoint RENASOUND to the folder that has them (it looks inside subfolders too), or import without them.")
                    .withButton ("Find them...", 1)
                    .withButton ("Import without them", 2)
                    .withButton ("Cancel", 0)
                    .withAssociatedComponent (this);

    messageBox = juce::AlertWindow::showScopedAsync (opts, [this, imp, f, searchFolders] (int r)
    {
        if (r == 2) { applyImport (*imp, f); return; }
        if (r != 1) return;
        chooser = std::make_unique<juce::FileChooser> ("Where are the audio files?", f.getParentDirectory());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this, f, searchFolders] (const juce::FileChooser& fc)
        {
            auto folders = searchFolders;
            if (fc.getResult().isDirectory()) folders.addIfNotAlreadyThere (fc.getResult());
            juce::MessageManager::callAsync ([this, f, folders] { importMixcraft (f, folders); });
        });
    });
}

void MainComponent::applyImport (DawImport& imp, const juce::File& source)
{
    engine.stop();
    deviceWindows.clear();
    project.loadImported (*imp.project);
    suggestedProjectName = source.getFileNameWithoutExtension();
    bpm.setValue (project.bpm, juce::dontSendNotification);
    engine.setPositionBeats (0);
    pianoRoll.showClip (-1);
    pitchEditor.showClip (-1);
    if (! project.tracks.empty()) ctx.selectTrack (project.tracks.front()->id);
    timeline.zoomToFit();

    juce::String text = "Imported \"" + source.getFileNameWithoutExtension() + "\": " + imp.summary + ".\n\n"
                        + imp.fromApp + "'s own instruments and effects can't be carried over, so instrument tracks use "
                        "RENASOUND sounds (drum kits get RENASOUND Drums; pick others, or load your plugins, on each track). "
                        + (imp.numAutomationLanes > 0 ? juce::String ("Volume and pan are copied, along with ") + juce::String (imp.numAutomationLanes)
                                                          + " volume/pan automation lane(s); other automation isn't."
                                                        : juce::String ("Volume and pan are copied; " + imp.fromApp + "'s automation isn't."));
    for (auto& n : imp.notes) text << "\n\n" << n;
    if (! imp.missingFiles.isEmpty())
        text << "\n\n" << imp.missingFiles.size() << " audio file(s) weren't found, so their clips were left out.";
    text << "\n\nSave to keep it as a RENASOUND project.";
    showMessage (imp.fromApp + " project imported", text);
}

void MainComponent::saveProject (bool saveAs, std::function<void (bool)> done)
{
    if (! saveAs && project.file != juce::File())
    {
        const bool ok = project.save (project.file);
        if (! ok) showMessage ("Couldn't save", "Renasound couldn't write " + project.file.getFullPathName());
        if (done) done (ok);
        return;
    }

    auto dir = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("Renasound");
    dir.createDirectory();
    chooser = std::make_unique<juce::FileChooser> ("Save project", dir.getChildFile ((suggestedProjectName.isNotEmpty() ? suggestedProjectName : juce::String ("My Song")) + ".renasound"), "*.renasound");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this, done] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) { if (done) done (false); return; }
        f = f.withFileExtension ("renasound");
        const bool ok = project.save (f);
        if (! ok) showMessage ("Couldn't save", "Renasound couldn't write " + f.getFullPathName());
        if (done) done (ok);
    });
}

void MainComponent::exportMixdown()
{
    if (pitchService.isBusy())
    {
        showMessage ("One moment", "RENASOUND is still processing pitch edits. Try the mixdown again in a few seconds.");
        return;
    }
    if (project.getEndBeat() <= 0)
    {
        showMessage ("Nothing to mix down", "Record or add some clips first.");
        return;
    }
    auto name = project.file.existsAsFile() ? project.file.getFileNameWithoutExtension() : juce::String ("My Song");
    chooser = std::make_unique<juce::FileChooser> ("Mix down to WAV",
                                                   juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (name + ".wav"), "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
    {
        auto f = fc.getResult();
        if (f == juce::File()) return;
        f = f.withFileExtension ("wav");
        juce::String error;
        if (engine.renderToFile (f, error))
        {
            showMessage ("Mixdown finished", "Saved " + f.getFileName());
            f.revealToUser();
        }
        else
            showMessage ("Mixdown failed", error);
    });
}

void MainComponent::showAudioSettings()
{
    auto* selector = new juce::AudioDeviceSelectorComponent (engine.deviceManager, 0, 2, 2, 2, true, false, true, false);
    selector->setSize (520, 460);

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (selector);
    o.dialogTitle = "Audio & MIDI setup";
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.launchAsync();
}

void MainComponent::showMessage (const juce::String& title, const juce::String& text)
{
    messageBox = juce::AlertWindow::showScopedAsync (juce::MessageBoxOptions()
                                                        .withIconType (juce::MessageBoxIconType::NoIcon)
                                                        .withTitle (title)
                                                        .withMessage (text)
                                                        .withButton ("OK", 1)
                                                        .withAssociatedComponent (this), nullptr);
}

// =============================================================================
//  Commands
// =============================================================================
void MainComponent::getAllCommands (juce::Array<juce::CommandID>& c)
{
    c.addArray ({ cmdNew, cmdOpen, cmdSave, cmdSaveAs, cmdExport, cmdImport,
                  cmdUndo, cmdRedo, cmdDeleteClip, cmdDuplicateClip, cmdSplitClip,
                  cmdPlayPause, cmdRecord, cmdToStart, cmdToggleLoop, cmdToggleMetronome,
                  cmdAddAudioTrack, cmdAddInstrumentTrack, cmdAudioSettings, cmdZoomIn, cmdZoomOut,
                  cmdShowEditor, cmdShowMixer, cmdShowLibrary, cmdPluginManager, cmdTrackEffects, cmdMasterEffects, cmdImportMixcraft });
}

void MainComponent::getCommandInfo (juce::CommandID id, juce::ApplicationCommandInfo& info)
{
    using KP = juce::KeyPress;
    const auto cmd = juce::ModifierKeys::commandModifier;
    const auto shift = juce::ModifierKeys::shiftModifier;

    switch (id)
    {
        case cmdNew:      info.setInfo ("New project", "", "File", 0);            info.addDefaultKeypress ('n', cmd); break;
        case cmdOpen:     info.setInfo ("Open project...", "", "File", 0);        info.addDefaultKeypress ('o', cmd); break;
        case cmdSave:     info.setInfo ("Save", "", "File", 0);                   info.addDefaultKeypress ('s', cmd); break;
        case cmdSaveAs:   info.setInfo ("Save as...", "", "File", 0);             info.addDefaultKeypress ('s', cmd | shift); break;
        case cmdImport:   info.setInfo ("Import audio...", "", "File", 0);        info.addDefaultKeypress ('i', cmd); break;
        case cmdExport:   info.setInfo ("Mix down to WAV...", "", "File", 0);     info.addDefaultKeypress ('e', cmd); break;
        case cmdAudioSettings: info.setInfo ("Audio & MIDI setup...", "", "File", 0); info.addDefaultKeypress (',', cmd); break;

        case cmdUndo:     info.setInfo ("Undo", "", "Edit", 0); info.setActive (project.canUndo()); info.addDefaultKeypress ('z', cmd); break;
        case cmdRedo:     info.setInfo ("Redo", "", "Edit", 0); info.setActive (project.canRedo()); info.addDefaultKeypress ('z', cmd | shift); break;
        case cmdDeleteClip:
            info.setInfo ("Delete clip", "", "Edit", 0);
            info.addDefaultKeypress (KP::deleteKey, 0);
            info.addDefaultKeypress (KP::backspaceKey, 0);
            break;
        case cmdDuplicateClip: info.setInfo ("Duplicate clip", "", "Edit", 0); info.addDefaultKeypress ('d', cmd); break;
        case cmdSplitClip:     info.setInfo ("Split clip at playhead", "", "Edit", 0); info.addDefaultKeypress ('s', 0); break;
        case cmdAddAudioTrack:      info.setInfo ("Add audio track", "", "Edit", 0);      info.addDefaultKeypress ('t', cmd); break;
        case cmdAddInstrumentTrack: info.setInfo ("Add instrument track", "", "Edit", 0); info.addDefaultKeypress ('t', cmd | shift); break;

        case cmdPlayPause: info.setInfo ("Play / pause", "", "Transport", 0); info.addDefaultKeypress (KP::spaceKey, 0); break;
        case cmdRecord:    info.setInfo ("Record", "", "Transport", 0);       info.addDefaultKeypress ('r', 0); break;
        case cmdToStart:   info.setInfo ("Return to start", "", "Transport", 0); info.addDefaultKeypress (KP::homeKey, 0); info.addDefaultKeypress (KP::returnKey, 0); break;
        case cmdToggleLoop: info.setInfo ("Loop", "", "Transport", 0); info.setTicked (project.loopEnabled); info.addDefaultKeypress ('l', 0); break;
        case cmdToggleMetronome: info.setInfo ("Metronome", "", "Transport", 0); info.setTicked (engine.metronomeOn); info.addDefaultKeypress ('c', 0); break;

        case cmdZoomIn:  info.setInfo ("Zoom in", "", "View", 0);  info.addDefaultKeypress ('=', cmd); break;
        case cmdZoomOut: info.setInfo ("Zoom out", "", "View", 0); info.addDefaultKeypress ('-', cmd); break;
        case cmdShowEditor:  info.setInfo ("Editor", "", "View", 0);  info.addDefaultKeypress ('1', cmd); break;
        case cmdShowMixer:   info.setInfo ("Mixer", "", "View", 0);   info.addDefaultKeypress ('2', cmd); break;
        case cmdShowLibrary: info.setInfo ("Library", "", "View", 0); info.addDefaultKeypress ('3', cmd); break;
        case cmdPluginManager: info.setInfo ("Plugins (scan VST3 / AU)...", "", "File", 0); info.addDefaultKeypress ('p', cmd | shift); break;
        case cmdImportMixcraft: info.setInfo ("Import Ableton Live or Mixcraft project...", "", "File", 0); break;
        case cmdTrackEffects:  info.setInfo ("Add effect to selected track...", "", "Edit", 0); info.addDefaultKeypress ('f', cmd); info.setActive (project.findTrack (ctx.selectedTrackId) != nullptr); break;
        case cmdMasterEffects: info.setInfo ("Add effect to master...", "", "Edit", 0); info.addDefaultKeypress ('f', cmd | shift); break;
        default: break;
    }
}

bool MainComponent::perform (const InvocationInfo& info)
{
    switch (info.commandID)
    {
        case cmdNew:
            askToSaveThen ([this]
            {
                engine.stop();
                deviceWindows.clear();
                suggestedProjectName.clear();
                project.clear();
                project.addTrack (Track::Type::Audio);
                project.addTrack (Track::Type::Instrument);
                project.dirty = false;
                bpm.setValue (project.bpm, juce::dontSendNotification);
                engine.setPositionBeats (0);
                pianoRoll.showClip (-1);
                pitchEditor.showClip (-1);
            });
            break;
        case cmdOpen:   openProject(); break;
        case cmdSave:   saveProject (false); break;
        case cmdSaveAs: saveProject (true); break;
        case cmdImport: importAudio(); break;
        case cmdExport: exportMixdown(); break;
        case cmdAudioSettings: showAudioSettings(); break;

        case cmdUndo: engine.stop(); project.undo(); break;
        case cmdRedo: engine.stop(); project.redo(); break;
        case cmdDeleteClip:
            if (ctx.selectedClipId >= 0) { project.beginEdit(); project.removeClip (ctx.selectedClipId); ctx.selectedClipId = -1; }
            break;
        case cmdDuplicateClip:
            if (ctx.selectedClipId >= 0) { project.beginEdit(); if (auto c = project.duplicateClip (ctx.selectedClipId)) ctx.selectClip (c->id); }
            break;
        case cmdSplitClip:
            if (ctx.selectedClipId >= 0) { project.beginEdit(); project.splitClip (ctx.selectedClipId, engine.getPositionBeats()); }
            break;
        case cmdAddAudioTrack:      addTrack (Track::Type::Audio); break;
        case cmdAddInstrumentTrack: addTrack (Track::Type::Instrument); break;

        case cmdPlayPause: engine.togglePlay(); break;
        case cmdRecord:    record(); break;
        case cmdToStart:   engine.setPositionBeats (0); break;
        case cmdToggleLoop:
        {
            const juce::ScopedLock sl (project.lock);
            project.loopEnabled = ! project.loopEnabled;
            if (project.loopEnd <= project.loopStart) { project.loopStart = 0; project.loopEnd = project.beatsPerBar * 4; }
            break;
        }
        case cmdToggleMetronome: engine.metronomeOn = ! engine.metronomeOn; break;

        case cmdZoomIn:  timeline.zoom (1.3f); break;
        case cmdZoomOut: timeline.zoom (1.0f / 1.3f); break;
        case cmdShowEditor:  tabs.setCurrentTabIndex (0); break;
        case cmdShowMixer:   tabs.setCurrentTabIndex (1); break;
        case cmdShowLibrary: library.refresh(); tabs.setCurrentTabIndex (2); break;
        case cmdPluginManager: openPluginManager(); break;
        case cmdImportMixcraft:
            askToSaveThen ([this]
            {
                chooser = std::make_unique<juce::FileChooser> ("Import an Ableton Live set or a Mixcraft project",
                                                               juce::File::getSpecialLocation (juce::File::userDocumentsDirectory),
                                                               "*.als;*.mx4;*.mx5;*.mx6;*.mx7;*.mx8;*.mx9;*.mx10;*.mx11;*.mx12");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [this] (const juce::FileChooser& fc)
                {
                    const auto f = fc.getResult();
                    if (! f.existsAsFile()) return;
                    if (isMixcraftProject (f) || isAbletonProject (f)) importMixcraft (f, {});
                    else showMessage ("Couldn't import", "That isn't an Ableton Live set or a Mixcraft project.");
                });
            });
            break;
        case cmdTrackEffects:
            if (project.findTrack (ctx.selectedTrackId) != nullptr)
            {
                showInMixer (ctx.selectedTrackId);
                devicemenus::showAddEffectMenu (ctx, ctx.selectedTrackId, &mixer,
                                                [this, id = ctx.selectedTrackId] (FxPtr s) { openDevice (id, s, false); });
            }
            break;
        case cmdMasterEffects:
            showInMixer (-1);
            devicemenus::showAddEffectMenu (ctx, -1, &mixer, [this] (FxPtr s) { openDevice (-1, s, false); });
            break;
        default: return false;
    }
    timeline.repaint();
    commandManager.commandStatusChanged();
    return true;
}

// =============================================================================
//  Menus (shown in the macOS menu bar, or at the top of the window on Windows)
// =============================================================================
juce::StringArray MainComponent::getMenuBarNames()
{
    return { "File", "Edit", "Transport", "View" };
}

juce::PopupMenu MainComponent::getMenuForIndex (int index, const juce::String&)
{
    juce::PopupMenu m;
    auto* cm = &commandManager;
    switch (index)
    {
        case 0:
            m.addCommandItem (cm, cmdNew); m.addCommandItem (cm, cmdOpen);
            m.addSeparator();
            m.addCommandItem (cm, cmdSave); m.addCommandItem (cm, cmdSaveAs);
            m.addSeparator();
            m.addCommandItem (cm, cmdImport); m.addCommandItem (cm, cmdImportMixcraft); m.addCommandItem (cm, cmdExport);
            m.addSeparator();
            m.addCommandItem (cm, cmdAudioSettings);
            m.addCommandItem (cm, cmdPluginManager);
           #if ! JUCE_MAC
            m.addSeparator();
            m.addCommandItem (cm, juce::StandardApplicationCommandIDs::quit);
           #endif
            break;
        case 1:
            m.addCommandItem (cm, cmdUndo); m.addCommandItem (cm, cmdRedo);
            m.addSeparator();
            m.addCommandItem (cm, cmdSplitClip); m.addCommandItem (cm, cmdDuplicateClip); m.addCommandItem (cm, cmdDeleteClip);
            m.addSeparator();
            m.addCommandItem (cm, cmdAddAudioTrack); m.addCommandItem (cm, cmdAddInstrumentTrack);
            m.addSeparator();
            m.addCommandItem (cm, cmdTrackEffects); m.addCommandItem (cm, cmdMasterEffects);
            break;
        case 2:
            m.addCommandItem (cm, cmdPlayPause); m.addCommandItem (cm, cmdRecord); m.addCommandItem (cm, cmdToStart);
            m.addSeparator();
            m.addCommandItem (cm, cmdToggleLoop); m.addCommandItem (cm, cmdToggleMetronome);
            break;
        case 3:
            m.addCommandItem (cm, cmdZoomIn); m.addCommandItem (cm, cmdZoomOut);
            m.addSeparator();
            m.addCommandItem (cm, cmdShowEditor); m.addCommandItem (cm, cmdShowMixer); m.addCommandItem (cm, cmdShowLibrary);
            break;
        default: break;
    }
    return m;
}

} // namespace rs
