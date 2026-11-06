#include "MainComponent.h"
#include "AudioFit.h"
#include "SoundMatch.h"
#include "MidiImport.h"
#include "UI/MixdownPanel.h"
#include "Devices.h"

namespace rs
{

MainComponent::MainComponent()
{
    setLookAndFeel (&lookAndFeel);
    juce::LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

    // ---- transport ----
    for (auto* b : { &toStart, &playButton, &stopButton, &recordButton, &loopButton, &clickButton, &typingButton })
        addAndMakeVisible (b);
    typingButton.onClick = [this] { typing.setEnabled (! typing.isEnabled()); };
    typingButton.setTooltip ("Musical typing: play the instrument with your computer keyboard.\n"
                             "A S D F G H J K L = white keys, W E T Y U O P = black keys, Z / X = octave, C / V = velocity");
    addAndMakeVisible (countInBox);
    {
        const std::pair<const char*, int> counts[] { { "No count-in", 0 }, { "1 click", 1 }, { "2 clicks", 2 }, { "3 clicks", 3 },
                                                     { "4 clicks", 4 }, { "1 bar", 101 }, { "2 bars", 102 } };
        for (auto& [name, code] : counts) countInBox.addItem (name, code + 1000);
        countInBox.setSelectedId (1000, juce::dontSendNotification);
        countInBox.setTooltip ("Count-in: how many metronome clicks play before recording starts");
        countInBox.onChange = [this] { project.countIn = countInBox.getSelectedId() - 1000; project.dirty = true; };
    }
    addAndMakeVisible (snapButton);
    addAndMakeVisible (gridBox);
    snapButton.setClickingTogglesState (true);
    snapButton.setToggleState (true, juce::dontSendNotification);
    snapButton.getProperties().set ("compact", true);
    snapButton.setTooltip ("Snap to grid: clips, the playhead and the loop land on the grid. Hold Alt while dragging to place freely");
    snapButton.onClick = [this] { ctx.snapOn = snapButton.getToggleState(); gridBox.setEnabled (ctx.snapOn); };
    {
        const std::pair<const char*, double> grids[] { { "Auto", 0.0 }, { "1 bar", -1.0 }, { "1/2", 2.0 }, { "1/4", 1.0 }, { "1/8", 0.5 }, { "1/16", 0.25 }, { "1/32", 0.125 } };
        int id = 1;
        for (auto& [name, beats] : grids) gridBox.addItem (name, id++);
        gridBox.setSelectedId (1, juce::dontSendNotification);
        gridBox.setTooltip ("Grid size (Auto follows the zoom)");
        gridBox.onChange = [this]
        {
            const double values[] { 0.0, -1.0, 2.0, 1.0, 0.5, 0.25, 0.125 };
            const double v = values[juce::jlimit (0, 6, gridBox.getSelectedId() - 1)];
            ctx.gridBeats = v < 0 ? (double) project.beatsPerBar : v;
            timeline.repaint();
        };
    }
    typing.onChange = [this]
    {
        typingButton.setToggleState (typing.isEnabled(), juce::dontSendNotification);
        keyboard.repaint();
    };

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
    timeDisplay.setTooltip ("Click to switch between bars / beats and minutes / seconds");
    timeDisplay.setMouseCursor (juce::MouseCursor::PointingHandCursor);
    timeDisplay.addMouseListener (this, false);
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

    for (auto* b : { &mixdown, &settingsButton })
        addAndMakeVisible (b);
    // + Vocoder / + Sampler: a new instrument track with that device loaded and its window open
    auto addDeviceTrack = [this] (const juce::String& type, const juce::String& name, std::function<void (FxSlot&)> setup = {}) -> FxPtr
    {
        juce::String error;
        auto slot = engine.createFx (type, nullptr, error);
        if (slot == nullptr) { showMessage ("Couldn't add " + name, error); return nullptr; }
        project.beginEdit();
        auto t = project.addTrack (Track::Type::Instrument);
        t->name = name;
        if (setup) setup (*slot);
        project.setInstrument (t->id, slot);
        ctx.selectTrack (t->id);
        project.changed();
        openDevice (t->id, slot, true);
        return slot;
    };
    timeline.addVocoderButton.onClick = [this, addDeviceTrack]
    {
        // listen to the vocal: the selected audio track, else one named like a vocal, else the first audio track
        int source = 0;
        if (auto sel = project.findTrack (ctx.selectedTrackId); sel != nullptr && sel->type == Track::Type::Audio) source = sel->id;
        if (source == 0)
            for (auto& t : project.tracks)
                if (t->type == Track::Type::Audio)
                    for (auto* w : { "vocal", "vox", "voice", "sing", "lead" })
                        if (source == 0 && t->name.containsIgnoreCase (w)) source = t->id;
        if (source == 0)
            for (auto& t : project.tracks) if (source == 0 && t->type == Track::Type::Audio) source = t->id;
        addDeviceTrack ("builtin:vocoder", "Vocoder", [source] (FxSlot& s)
        {
            if (auto* sc = dynamic_cast<SidechainUser*> (s.processor.get())) sc->sidechainTrackId = source;
        });
    };
    timeline.addSamplerButton.onClick = [addDeviceTrack] { addDeviceTrack ("builtin:sampler", "Sampler"); };

    // the add-track buttons live on the timeline, above the track headers
    timeline.addAudioButton.onClick = [this] { addTrack (Track::Type::Audio); };
    timeline.addInstrumentButton.onClick = [this]
    {
        addTrack (Track::Type::Instrument);
        // straight away: which instrument should the new track play?
        if (! project.tracks.empty()) devicemenus::showInstrumentMenu (ctx, project.tracks.back()->id, &timeline.addInstrumentButton);
    };
    mixdown.onClick       = [this] { exportMixdown(); };
    settingsButton.onClick = [this] { showAudioSettings(); };
    addAndMakeVisible (updateButton);
    updateButton.onClick = [this] { updateButtonClicked(); };
    updateButton.setTooltip ("Get the newest RENASOUND (this is build " + juce::String (Updater::currentBuild()) + ")");
    updater.onStateChanged = [this] { updaterChanged(); };
    juce::Timer::callAfterDelay (4000, [sp = juce::Component::SafePointer<MainComponent> (this)] { if (sp != nullptr) sp->updater.check(); });
    mixdown.setTooltip ("Export your song as a WAV file");
    mixdown.setColour (juce::TextButton::textColourOffId, theme::accent);

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
    ctx.addMidiFile = [this] (const juce::File& f, double beat) { addMidiFile (f, beat); };
    ctx.fitClipToSong = [this] (int clipId, bool automatic) { return fitClipToSong (clipId, automatic); };
    ctx.notify = [this] (const juce::String& t) { notice.show (t); resized(); };
    addChildComponent (notice);
    // sound matching for imports: the General MIDI SoundFont the user has (remembered between runs)
    soundmatch::setGmSoundFont (juce::File (engine.pluginHost.settings->getValue ("gmSoundFont")));
    soundmatch::onGmSoundFontChanged = [this] (const juce::File& f)
    {
        engine.pluginHost.settings->setValue ("gmSoundFont", f.getFullPathName());
        engine.pluginHost.settings->saveIfNeeded();
    };
    ctx.openProjectFile = [this] (const juce::File& f)
    {
        askToSaveThen ([this, f] { if (! loadProjectFile (f)) showMessage ("Couldn't open project", "RENASOUND can open its own projects, Ableton Live sets, Mixcraft projects and MIDI files."); });
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
    addKeyListener (&typing);   // added last, so it hears keys before the shortcuts
    commandManager.setFirstCommandTarget (this);
    ctx.keyListeners = { commandManager.getKeyMappings(), &typing };   // device windows listen too
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
    removeKeyListener (&typing);
    deviceWindows.clear();
    pluginManager.reset();
    project.removeChangeListener (this);
    engine.stop();
    setLookAndFeel (nullptr);
    juce::LookAndFeel::setDefaultLookAndFeel (nullptr);
}

// =============================================================================
// ---- updates ----
void MainComponent::updaterChanged()
{
    using S = Updater::State;
    const auto st = updater.getState();
   #ifdef JUCE_APPLICATION_VERSION_STRING
    const juce::String version = JUCE_APPLICATION_VERSION_STRING;
   #else
    const juce::String version;
   #endif
    // the version you're running, so it's easy to tell whether an update arrived
    juce::String label = version.isNotEmpty() ? juce::String::fromUTF8 ("Update  \xc2\xb7  v") + version.upToLastOccurrenceOf (".0", false, false) : juce::String ("Update");
    if (version.isNotEmpty() && ! version.endsWith (".0")) label = juce::String::fromUTF8 ("Update  \xc2\xb7  v") + version;
    auto colour = theme::text;
    switch (st)
    {
        case S::Idle:           break;
        case S::Checking:       label = "Checking..."; colour = theme::textDim; break;
        case S::UpToDate:       label = "Up to date"; colour = theme::textDim; break;
        case S::Available:      label = juce::String::fromUTF8 ("Update \xe2\x97\x8f"); colour = theme::accent; break;
        case S::Downloading:    label = "Updating " + juce::String (juce::roundToInt (updater.getProgress() * 100.0f)) + "%"; colour = theme::accent; break;
        case S::ReadyToInstall: label = "Restart to update"; colour = theme::accent; break;
        case S::Failed:         break;
    }
    updateButton.setButtonText (label);
    updateButton.setColour (juce::TextButton::textColourOffId, colour);

    if (! userAskedForUpdate) return;
    if (st == S::UpToDate)
    {
        userAskedForUpdate = false;
        showMessage ("You're up to date", "This is the newest RENASOUND (version " + version + ", build " + juce::String (Updater::currentBuild()) + ").");
    }
    else if (st == S::Failed)
    {
        userAskedForUpdate = false;
        showMessage ("Couldn't update", updater.getError());
    }
    else if (st == S::Available)
        updater.download();   // they clicked Update: go straight on to downloading
    else if (st == S::ReadyToInstall)
    {
        userAskedForUpdate = false;
        updateButtonClicked();
    }
}

void MainComponent::updateButtonClicked()
{
    using S = Updater::State;
    switch (updater.getState())
    {
        case S::Checking: case S::Downloading: return;
        case S::Available: userAskedForUpdate = true; updater.download(); return;
        case S::ReadyToInstall:
            askToSaveThen ([this]
            {
                if (updater.installAndRestart()) juce::JUCEApplication::getInstance()->quit();
                else showMessage ("Couldn't update", "The new version couldn't be installed automatically. Download it from github.com/"
                                                     + juce::String (Updater::repository) + "/releases instead.");
            });
            return;
        case S::Idle: case S::UpToDate: case S::Failed:
            userAskedForUpdate = true;
            updater.check();
            return;
    }
}

void MainComponent::mouseUp (const juce::MouseEvent& e)
{
    if (e.eventComponent == &timeDisplay && ! e.mouseWasDraggedSinceMouseDown())
    {
        ctx.showTime = ! ctx.showTime;
        timeDisplay.setColour (juce::Label::textColourId, ctx.showTime ? theme::accent2 : theme::accent);
        timerCallback();
        timeline.repaint();
    }
}

void MainComponent::paintOverChildren (juce::Graphics& g)
{
    // the sandstone grain, over everything
    g.setTiledImageFill (theme::grain(), 0, 0, 1.0f);
    g.fillAll();
}

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

    // the right-hand buttons first, so they never get squeezed out
    updateButton.setBounds (bar.removeFromRight (112));   bar.removeFromRight (2);
    settingsButton.setBounds (bar.removeFromRight (98));  bar.removeFromRight (2);
    mixdown.setBounds (bar.removeFromRight (86));         bar.removeFromRight (10);

    // the transport shrinks a little on narrow windows
    const bool tight = bar.getWidth() < 1080;
    for (auto* b : { &toStart, &playButton, &stopButton, &recordButton })
    {
        b->setBounds (bar.removeFromLeft (tight ? 38 : 44));
        bar.removeFromLeft (tight ? 3 : 5);
    }
    bar.removeFromLeft (6);
    timeDisplay.setBounds (bar.removeFromLeft (tight ? 128 : 170));
    bar.removeFromLeft (8);
    loopButton.setBounds (bar.removeFromLeft (38));  bar.removeFromLeft (3);
    clickButton.setBounds (bar.removeFromLeft (38)); bar.removeFromLeft (2);
    countInBox.setBounds (bar.removeFromLeft (94).reduced (0, 4)); bar.removeFromLeft (8);
    bpmLabel.setBounds (bar.removeFromLeft (30));    bar.removeFromLeft (2);
    bpm.setBounds (bar.removeFromLeft (100));
    bar.removeFromLeft (6);
    typingButton.setBounds (bar.removeFromLeft (38));
    bar.removeFromLeft (6);
    snapButton.setBounds (bar.removeFromLeft (50).reduced (0, 4));
    bar.removeFromLeft (2);
    gridBox.setBounds (bar.removeFromLeft (70).reduced (0, 4));

    auto keys = r.removeFromBottom (72).reduced (8, 6);
    keyboardLabel.setBounds (keys.removeFromLeft (120));
    keyboard.setBounds (keys);
    keyboard.setKeyWidth (juce::jmax (12.0f, keys.getWidth() / 52.0f));   // 52 white keys from A0 to C8
    keyboard.setLowestVisibleKey (21);

    r.reduce (6, 6);
    juce::Component* comps[] = { &timeline, &divider, &tabs };
    layout.layOutComponents (comps, 3, r.getX(), r.getY(), r.getWidth(), r.getHeight(), true, true);

    const int nw = juce::jmin (620, timeline.getWidth() - 40);
    const int lines = notice.text.containsChar ('\n') ? 3 : 1;
    notice.setBounds (timeline.getX() + (timeline.getWidth() - nw) / 2, timeline.getY() + 34, nw, 18 * lines + 16);
}

void MainComponent::timerCallback()
{
    engine.updateClipPlayback();

    // instrument tracks take the name of the instrument chosen for them (until renamed by hand)
    bool renamed = false;
    for (auto& t : project.tracks)
        if (t->autoName && t->type == Track::Type::Instrument)
            if (const auto n = AppContext::instrumentName (*t); n.isNotEmpty() && n != t->name)
            {
                const juce::ScopedLock sl (project.lock);
                t->name = n;
                renamed = true;
            }
    if (renamed) project.changed();

    // time display: bar.beat and minutes:seconds
    const double beats = engine.getPositionBeats();
    const int bar = (int) (beats / project.beatsPerBar) + 1;
    const int beat = (int) std::fmod (beats, (double) project.beatsPerBar) + 1;
    const double secs = beats * 60.0 / project.bpm;
    if (ctx.showTime)
    {
        const int ms = (int) (std::fmod (secs, 1.0) * 1000.0);
        timeDisplay.setText (juce::String ((int) secs / 60) + ":" + juce::String ((int) secs % 60).paddedLeft ('0', 2) + "." + juce::String (ms).paddedLeft ('0', 3),
                             juce::dontSendNotification);
    }
    else
    {
        const int sixteenth = (int) (std::fmod (beats, 1.0) * 4.0) + 1;
        timeDisplay.setText (juce::String (bar).paddedLeft ('0', 3) + "." + juce::String (beat) + "." + juce::String (sixteenth), juce::dontSendNotification);
    }

    playButton.setIcon (engine.isPlaying() ? IconButton::Icon::Pause : IconButton::Icon::Play);
    playButton.setToggleState (engine.isPlaying() && ! engine.isRecording(), juce::dontSendNotification);
    recordButton.setToggleState (engine.isRecording() || (engine.isCountingIn() && (juce::Time::getMillisecondCounter() / 250) % 2 == 0), juce::dontSendNotification);
    if (engine.isCountingIn())
        timeDisplay.setText ("COUNT  " + juce::String (engine.countInBeatsLeft()), juce::dontSendNotification);
    if (countInBox.getSelectedId() != project.countIn + 1000) countInBox.setSelectedId (project.countIn + 1000, juce::dontSendNotification);
    loopButton.setToggleState (project.loopEnabled, juce::dontSendNotification);
    clickButton.setToggleState (engine.metronomeOn.load(), juce::dontSendNotification);
    if (! bpm.isMouseButtonDown() && std::abs (bpm.getValue() - project.bpm) > 0.01)
        bpm.setValue (project.bpm, juce::dontSendNotification);

    juce::String plays = "Keyboard: no instrument";
    if (auto t = project.findTrack (engine.liveInputTrackId); t != nullptr)
        plays = "Keyboard plays\n" + t->name;
    if (typing.isEnabled())
        plays << "\nTyping: " << juce::MidiMessage::getMidiNoteName (typing.baseNote, true, true, 4) << ", vel " << typing.velocity;
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
    engine.resolveMidiInputs();
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
    // opening an instrument makes the keyboard (on screen, computer, MIDI) play it
    if (instrument && trackId >= 0) ctx.selectTrack (trackId);

    // a device always has one window: bring it back rather than opening another
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
    if (engine.isRecording() || engine.isCountingIn()) { engine.stop(); return; }

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
                    tr->autoName = false;   // a name the user chose stays
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
                                                   audioCache.formats.getWildcardForAllFormats() + ";*.mid;*.midi;*.kar");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::canSelectMultipleItems,
                          [this] (const juce::FileChooser& fc)
    {
        if (fc.getResults().isEmpty()) return;
        project.beginEdit();
        auto target = project.findTrack (ctx.selectedTrackId);
        for (auto& f : fc.getResults())
        {
            if (isMidiFile (f))
            {
                const auto beat = engine.getPositionBeats();
                juce::MessageManager::callAsync ([this, f, beat] { addMidiFile (f, std::floor (beat / project.beatsPerBar) * project.beatsPerBar); });
                continue;
            }
            if (target == nullptr || target->type != Track::Type::Audio || ! target->clips.empty())
                target = project.addTrack (Track::Type::Audio);
            if (auto c = project.addAudioClip (target->id, f, engine.getPositionBeats()))
            {
                ctx.selectClip (c->id);
                if (const auto what = fitClipToSong (c->id, true); what.isNotEmpty()) notice.show (what);
            }
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
    if (isMidiFile (f))
    {
        soundmatch::setInstalledPlugins (engine.pluginHost.knownPlugins.getTypes());
        auto imp = importMidiFile (f);
        if (! imp.ok) { showMessage ("Couldn't open the MIDI file", imp.error); return true; }
        applyImport (imp, f);
        return true;
    }
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
    soundmatch::setInstalledPlugins (engine.pluginHost.knownPlugins.getTypes());
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

    juce::String text = "Imported \"" + source.getFileNameWithoutExtension() + "\": " + imp.summary + ".";
    juce::StringArray matched, other;
    for (auto& n : imp.notes) (n.startsWithChar ('"') ? matched : other).add (n);
    if (! matched.isEmpty())
    {
        text << "\n\nEach instrument track got the closest sound RENASOUND could find:\n";
        for (int i = 0; i < juce::jmin (12, matched.size()); ++i) text << "\n  " << matched[i];
        if (matched.size() > 12) text << "\n  ...and " << (matched.size() - 12) << " more";
        if (soundmatch::getGmSoundFont() == juce::File() && imp.fromApp != "Ableton Live")
            text << "\n\nTip: load a General MIDI SoundFont (.sf2) once in the Multi Sampler, and imports will use its real pianos, guitars, strings and drums.";
    }
    if (imp.fromApp != "MIDI file")
        text << "\n\n" << (imp.numAutomationLanes > 0 ? juce::String ("Volume and pan are copied, along with ") + juce::String (imp.numAutomationLanes)
                                                          + " volume/pan automation lane(s); " + imp.fromApp + "'s effects and other automation aren't."
                                                        : imp.fromApp + "'s effects and automation aren't copied; volume and pan are.");
    for (auto& n : other) text << "\n\n" << n;
    if (! imp.missingFiles.isEmpty())
        text << "\n\n" << imp.missingFiles.size() << " audio file(s) weren't found, so their clips were left out.";
    text << "\n\nSave to keep it as a RENASOUND project.";
    showMessage (imp.fromApp == "MIDI file" ? juce::String ("MIDI file opened") : imp.fromApp + " project imported", text);
}

juce::String MainComponent::fitClipToSong (int clipId, bool automatic)
{
    TrackPtr track;
    auto clip = project.findClip (clipId, &track);
    if (clip == nullptr || clip->type != Clip::Type::Audio || clip->audio == nullptr || clip->locked) return {};
    if (! automatic && clip->audio->getLengthSeconds() > 600)
    {
        showMessage ("Too long to fit", "Fitting works on clips up to 10 minutes long.");
        return {};
    }

    juce::MouseCursor::showWaitCursor();
    const juce::ScopeGuard restoreCursor { [] { juce::MouseCursor::hideWaitCursor(); } };
    const auto src = clip->audio;
    const auto name = clip->name;
    const auto a = audiofit::analyse (*src, src->file.getFileNameWithoutExtension(), project.beatsPerBar);
    bool adoptTempo = false;
    const auto f = audiofit::plan (a, project, adoptTempo, automatic);

    // the only clip in an empty song: the song takes the loop's tempo
    bool othersHaveMusic = false;
    for (auto& t : project.tracks) for (auto& c : t->clips) if (c->id != clipId) othersHaveMusic = true;
    if (automatic && a.isLoop && ! othersHaveMusic && a.bpm >= 40 && a.bpm <= 240)
    {
        const double rounded = std::abs (a.bpm - std::round (a.bpm)) < 0.05 ? std::round (a.bpm) : a.bpm;
        {
            const juce::ScopedLock sl (project.lock);
            project.bpm = rounded;
            clip->startBeat = std::round (clip->startBeat);
        }
        bpm.setValue (rounded, juce::dontSendNotification);
        project.changed();
        return name + ": the song now runs at the loop's " + juce::String (rounded, std::abs (rounded - std::round (rounded)) < 1.0e-9 ? 0 : 2) + " BPM"
               + (a.key >= 0 ? " (" + audiofit::keyName (a.key, a.minor) + ")" : juce::String());
    }
    if (! f.tempo && ! f.key)
    {
        if (! automatic)
            showMessage ("Already fits", name + (a.bpm > 0 ? " is at " + juce::String (a.bpm, 1) + " BPM" : juce::String (" has no steady beat"))
                                      + (a.key >= 0 ? ", in " + audiofit::keyName (a.key, a.minor) : juce::String())
                                      + ", so there's nothing to change for this song.");
        return {};
    }

    juce::String error;
    const auto file = audiofit::renderToFile (*src, f, src->file.getFileNameWithoutExtension(), error);
    auto data = file.existsAsFile() ? project.audioCache.load (file) : nullptr;
    if (data == nullptr) { if (! automatic) showMessage ("Couldn't fit the clip", error); return {}; }
    if (! automatic) project.beginEdit();
    {
        const juce::ScopedLock sl (project.lock);
        clip->audio = data;
        clip->offsetSeconds *= f.stretch;
        clip->lengthSeconds = juce::jmin (clip->lengthSeconds * f.stretch, data->getLengthSeconds() - clip->offsetSeconds);
        clip->fadeInSeconds *= f.stretch; clip->fadeOutSeconds *= f.stretch;
        clip->pitch = {};   // pitch edits were made on the old audio
        clip->pitchRendered = nullptr;
    }
    project.changed();
    return name + ": " + f.description;
}

void MainComponent::addMidiFile (const juce::File& f, double beat)
{
    soundmatch::setInstalledPlugins (engine.pluginHost.knownPlugins.getTypes());
    auto imp = importMidiFile (f);
    if (! imp.ok) { showMessage ("Couldn't import the MIDI file", imp.error); return; }

    bool songIsEmpty = true;
    for (auto& t : project.tracks) if (! t->clips.empty()) songIsEmpty = false;
    if (songIsEmpty && ! project.dirty)
    {
        applyImport (imp, f);   // nothing to keep: the file becomes the song, tempo and all
        return;
    }

    // add its parts to this song, at the bar it was dropped on (the song keeps its tempo)
    project.beginEdit();
    auto xml = project.toXml();
    int firstId = project.newId() + 1000;
    const int added = appendImportedTracks (*xml, *imp.project, beat, firstId);
    project.fromXml (*xml);
    project.changed();

    juce::String text = juce::String (added) + " track" + (added == 1 ? "" : "s") + " added from \"" + f.getFileNameWithoutExtension() + "\"";
    if (std::abs (imp.bpm - project.bpm) > 0.5) text << " (the file is at " << juce::String (imp.bpm, 1) << " BPM; the song stays at " << juce::String (project.bpm, 1) << ")";
    text << ".\n";
    for (auto& n : imp.notes) if (n.startsWithChar ('"')) text << "\n  " << n;
    showMessage ("MIDI file added", text);
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
    auto* panel = new MixdownPanel (ctx);
    juce::Component::SafePointer<MixdownPanel> sp (panel);
    panel->onClose = [sp]
    {
        if (sp != nullptr)
            if (auto* dw = sp->findParentComponentOfClass<juce::DialogWindow>()) dw->exitModalState (0);
    };
    panel->showMessage = [this] (const juce::String& t, const juce::String& m) { showMessage (t, m); };

    juce::DialogWindow::LaunchOptions o;
    o.content.setOwned (panel);
    o.dialogTitle = "Mix down";
    o.dialogBackgroundColour = theme::panel;
    o.escapeKeyTriggersCloseButton = true;
    o.useNativeTitleBar = true;
    o.resizable = false;
    o.componentToCentreAround = this;
    o.launchAsync();
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
                  cmdShowEditor, cmdShowMixer, cmdShowLibrary, cmdPluginManager, cmdTrackEffects, cmdMasterEffects, cmdImportMixcraft,
                  cmdCut, cmdCopy, cmdPaste, cmdSelectAll, cmdJoin, cmdReverse, cmdLock, cmdNudgeLeft, cmdNudgeRight, cmdDeselect });
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
        case cmdImport:   info.setInfo ("Import audio or MIDI...", "", "File", 0);        info.addDefaultKeypress ('i', cmd); break;
        case cmdExport:   info.setInfo ("Mix down to WAV...", "", "File", 0);     info.addDefaultKeypress ('e', cmd); break;
        case cmdAudioSettings: info.setInfo ("Audio & MIDI setup...", "", "File", 0); info.addDefaultKeypress (',', cmd); break;

        case cmdUndo:     info.setInfo ("Undo", "", "Edit", 0); info.setActive (project.canUndo()); info.addDefaultKeypress ('z', cmd); break;
        case cmdRedo:     info.setInfo ("Redo", "", "Edit", 0); info.setActive (project.canRedo()); info.addDefaultKeypress ('z', cmd | shift); break;
        case cmdCut:       info.setInfo ("Cut", "", "Edit", 0);        info.addDefaultKeypress ('x', cmd); info.setActive (timeline.hasClipSelection()); break;
        case cmdCopy:      info.setInfo ("Copy", "", "Edit", 0);       info.addDefaultKeypress ('c', cmd); info.setActive (timeline.hasClipSelection()); break;
        case cmdPaste:     info.setInfo ("Paste at playhead", "", "Edit", 0); info.addDefaultKeypress ('v', cmd); info.setActive (! ctx.clipboard.empty()); break;
        case cmdSelectAll: info.setInfo ("Select all clips", "", "Edit", 0); info.addDefaultKeypress ('a', cmd); break;
        case cmdJoin:      info.setInfo ("Join clips into one", "", "Edit", 0); info.addDefaultKeypress ('j', cmd); info.setActive (ctx.getSelectedClips().size() > 1); break;
        case cmdReverse:   info.setInfo ("Reverse clip", "", "Edit", 0); info.addDefaultKeypress ('r', cmd); info.setActive (timeline.hasClipSelection()); break;
        case cmdLock:      info.setInfo ("Lock / unlock clip", "", "Edit", 0); info.addDefaultKeypress ('l', cmd); info.setActive (timeline.hasClipSelection()); break;
        case cmdNudgeLeft:  info.setInfo ("Nudge clips left", "", "Edit", 0);  info.addDefaultKeypress (KP::leftKey, 0); break;
        case cmdNudgeRight: info.setInfo ("Nudge clips right", "", "Edit", 0); info.addDefaultKeypress (KP::rightKey, 0); break;
        case cmdDeselect:  info.setInfo ("Deselect", "", "Edit", 0); info.addDefaultKeypress (KP::escapeKey, 0); break;
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
        case cmdImportMixcraft: info.setInfo ("Import Ableton Live, Mixcraft or MIDI file...", "", "File", 0); break;
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
        case cmdDeleteClip:    timeline.deleteSelection(); break;
        case cmdDuplicateClip: timeline.duplicateSelection(); break;
        case cmdSplitClip:     timeline.splitSelection (engine.getPositionBeats()); break;
        case cmdCut:           timeline.cutSelection(); break;
        case cmdCopy:          timeline.copySelection(); break;
        case cmdPaste:         timeline.pasteAt (engine.getPositionBeats()); break;
        case cmdSelectAll:     timeline.selectAll(); break;
        case cmdJoin:          timeline.joinSelection(); break;
        case cmdReverse:       timeline.reverseSelection(); break;
        case cmdLock:          timeline.toggleLockSelection(); break;
        case cmdNudgeLeft:     timeline.nudgeSelection (-1); break;
        case cmdNudgeRight:    timeline.nudgeSelection (1); break;
        case cmdDeselect:      ctx.clearClipSelection(); ctx.rangeStart = ctx.rangeEnd = -1; break;
        case cmdAddAudioTrack:      addTrack (Track::Type::Audio); break;
        case cmdAddInstrumentTrack: addTrack (Track::Type::Instrument); break;

        case cmdPlayPause: engine.togglePlay(); break;
        case cmdRecord:    record(); break;
        case cmdToStart:   engine.setPositionBeats (0); break;
        case cmdToggleLoop:
        {
            // a highlighted area that isn't the loop yet: loop it
            if (ctx.hasRange() && ! (project.loopEnabled && std::abs (project.loopStart - ctx.rangeStart) < 1.0e-6 && std::abs (project.loopEnd - ctx.rangeEnd) < 1.0e-6))
            {
                timeline.loopSelection();
                break;
            }
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
                                                               "*.als;*.mid;*.midi;*.kar;*.mx4;*.mx5;*.mx6;*.mx7;*.mx8;*.mx9;*.mx10;*.mx11;*.mx12");
                chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                      [this] (const juce::FileChooser& fc)
                {
                    const auto f = fc.getResult();
                    if (! f.existsAsFile()) return;
                    if (isMixcraftProject (f) || isAbletonProject (f)) importMixcraft (f, {});
                    else if (isMidiFile (f)) loadProjectFile (f);
                    else showMessage ("Couldn't import", "That isn't an Ableton Live set, a Mixcraft project or a MIDI file.");
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
            m.addCommandItem (cm, cmdCut); m.addCommandItem (cm, cmdCopy); m.addCommandItem (cm, cmdPaste);
            m.addCommandItem (cm, cmdSelectAll);
            m.addSeparator();
            m.addCommandItem (cm, cmdSplitClip); m.addCommandItem (cm, cmdDuplicateClip); m.addCommandItem (cm, cmdJoin);
            m.addCommandItem (cm, cmdReverse); m.addCommandItem (cm, cmdLock); m.addCommandItem (cm, cmdDeleteClip);
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
