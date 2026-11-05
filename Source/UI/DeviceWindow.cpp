#include "../Devices.h"
#include "DeviceWindow.h"
#include "../Drums.h"

namespace rs
{

static void showLoadError (const juce::String& what, const juce::String& error)
{
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Couldn't load " + what,
                                            error.isNotEmpty() ? error : juce::String ("The plugin failed to open."));
}

// Groups plugins by manufacturer into a menu.
static void addPluginsToMenu (juce::PopupMenu& menu, const juce::Array<juce::PluginDescription>& plugins,
                              std::function<void (const juce::PluginDescription&)> chosen)
{
    std::map<juce::String, juce::PopupMenu> byMaker;
    for (auto& d : plugins)
    {
        const auto maker = d.manufacturerName.isNotEmpty() ? d.manufacturerName : juce::String ("Other");
        byMaker[maker].addItem (d.name + "  (" + d.pluginFormatName + ")", [d, chosen] { chosen (d); });
    }
    for (auto& [maker, sub] : byMaker)
        menu.addSubMenu (maker, sub);
}

void devicemenus::showAddEffectMenu (AppContext& ctx, int trackId, juce::Component* target, std::function<void (FxPtr)> added)
{
    auto add = [&ctx, trackId, added] (const juce::String& type, const juce::PluginDescription* d, const juce::String& name)
    {
        juce::String error;
        auto slot = ctx.engine.createFx (type, d, error);
        if (slot == nullptr) { showLoadError (name, error); return; }
        ctx.project.beginEdit();
        ctx.project.addFx (trackId, slot);
        if (added) added (slot);
    };

    juce::PopupMenu menu;
    menu.addSectionHeader ("RENASOUND EFFECTS");
    for (auto& fx : getBuiltinFxList())
        menu.addItem (fx.name, [add, fx] { add ("builtin:" + fx.id, nullptr, fx.name); });

    menu.addSectionHeader ("PLUGINS");
    const auto plugins = ctx.engine.pluginHost.getEffects();
    if (plugins.isEmpty())
        menu.addItem ("No plugins found yet", false, false, [] {});
    else
        addPluginsToMenu (menu, plugins, [add] (const juce::PluginDescription& d) { add ("plugin", &d, d.name); });
    menu.addSeparator();
    menu.addItem ("Scan for plugins...", [&ctx] { if (ctx.openPluginManager) ctx.openPluginManager(); });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

void devicemenus::chooseSynthSound (AppContext& ctx, int trackId, int patchIndex)
{
    auto track = ctx.project.findTrack (trackId);
    if (track == nullptr || track->type != Track::Type::Instrument) return;
    ctx.project.beginEdit();
    if (track->instrument != nullptr && track->instrument->type == "builtin:synth")
    {
        loadSynthPatch (track->instrument->processor.get(), patchIndex);
        ctx.project.changed();
        return;
    }
    juce::String error;
    auto slot = ctx.engine.createFx ("builtin:synth", nullptr, error);
    if (slot == nullptr) { showLoadError ("RENASYNTH", error); return; }
    loadSynthPatch (slot->processor.get(), patchIndex);
    ctx.project.setInstrument (trackId, slot);
}

void devicemenus::showInstrumentMenu (AppContext& ctx, int trackId, juce::Component* target)
{
    auto track = ctx.project.findTrack (trackId);
    if (track == nullptr || track->type != Track::Type::Instrument) return;

    juce::PopupMenu menu;
    menu.addItem ("Open " + AppContext::instrumentName (*track) + " window", [&ctx, track]
    {
        if (ctx.openDevice) ctx.openDevice (track->id, track->instrument, true);
    });
    menu.addSeparator();
    menu.addSectionHeader ("RENASYNTH");
    const bool hasSynth = track->instrument != nullptr && track->instrument->type == "builtin:synth";
    const auto current = hasSynth ? getSynthPatchName (track->instrument->processor.get()) : juce::String();
    const auto& patches = getSynthPatches();
    for (int i = 0; i < (int) patches.size(); ++i)
        menu.addItem (patches[(size_t) i].name, true, hasSynth && current == patches[(size_t) i].name,
                      [&ctx, trackId, i] { chooseSynthSound (ctx, trackId, i); });

    menu.addSectionHeader ("RENASOUND DRUMS");
    const auto kits = DrumMachine::getKitNames();
    for (int k = 0; k < kits.size(); ++k)
        menu.addItem ("Drums: " + kits[k], [&ctx, trackId, k]
        {
            juce::String error;
            auto slot = ctx.engine.createFx ("builtin:drums", nullptr, error);
            if (slot == nullptr) { showLoadError ("RENASOUND Drums", error); return; }
            if (auto* d = dynamic_cast<DrumMachine*> (slot->processor.get())) d->loadKit (k);
            ctx.project.beginEdit();
            ctx.project.setInstrument (trackId, slot);
            if (ctx.openDevice) ctx.openDevice (trackId, slot, true);
        });

    for (auto& kitFile : DrumMachine::findKitFiles())
        menu.addItem ("Drums: " + kitFile.getFileNameWithoutExtension(), [&ctx, trackId, kitFile]
        {
            juce::String error;
            auto slot = ctx.engine.createFx ("builtin:drums", nullptr, error);
            if (slot == nullptr) { showLoadError ("RENASOUND Drums", error); return; }
            if (auto* d = dynamic_cast<DrumMachine*> (slot->processor.get()); d != nullptr && ! d->loadKitFile (kitFile))
                showLoadError (kitFile.getFileNameWithoutExtension(), "The kit's samples weren't found next to the kit file.");
            ctx.project.beginEdit();
            ctx.project.setInstrument (trackId, slot);
            if (ctx.openDevice) ctx.openDevice (trackId, slot, true);
        });

    menu.addSectionHeader ("RENASOUND INSTRUMENTS");
    for (auto& info : getBuiltinInstrumentList())
    {
        const auto type = "builtin:" + info.id;
        const bool current = track->instrument != nullptr && track->instrument->type == type;
        menu.addItem (juce::PopupMenu::Item (info.name).setTicked (current).setAction ([&ctx, trackId, type, name = info.name]
        {
            juce::String error;
            auto slot = ctx.engine.createFx (type, nullptr, error);
            if (slot == nullptr) { showLoadError (name, error); return; }
            ctx.project.beginEdit();
            ctx.project.setInstrument (trackId, slot);
            if (ctx.openDevice) ctx.openDevice (trackId, slot, true);
        }));
    }

    menu.addSectionHeader ("PLUGIN INSTRUMENTS");
    const auto plugins = ctx.engine.pluginHost.getInstruments();
    if (plugins.isEmpty())
        menu.addItem ("No plugin instruments found yet", false, false, [] {});
    else
        addPluginsToMenu (menu, plugins, [&ctx, trackId] (const juce::PluginDescription& d)
        {
            juce::String error;
            auto slot = ctx.engine.createFx ("plugin", &d, error);
            if (slot == nullptr) { showLoadError (d.name, error); return; }
            ctx.project.beginEdit();
            ctx.project.setInstrument (trackId, slot);
            if (ctx.openDevice) ctx.openDevice (trackId, slot, true);
        });
    menu.addSeparator();
    menu.addItem ("Scan for plugins...", [&ctx] { if (ctx.openPluginManager) ctx.openPluginManager(); });

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

// =============================================================================
//  Built-in synth "editor": a grid of preset buttons
// =============================================================================
class SynthPresetPanel : public juce::Component
{
public:
    SynthPresetPanel (AppContext& c, TrackPtr t) : ctx (c), track (std::move (t))
    {
        const auto& presets = getSynthPresets();
        for (int i = 0; i < (int) presets.size(); ++i)
        {
            auto* b = buttons.add (new juce::TextButton (presets[(size_t) i].name));
            b->setClickingTogglesState (true);
            b->setRadioGroupId (1);
            b->setToggleState (track->synth->presetIndex == i, juce::dontSendNotification);
            b->onClick = [this, i] { track->synth->presetIndex = i; ctx.project.dirty = true; ctx.project.sendChangeMessage(); };
            addAndMakeVisible (b);
        }
        setSize (420, 80 + ((int) presets.size() + 2) / 3 * 46);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        g.setColour (theme::text);
        g.setFont (theme::font (17.0f, true));
        g.drawText ("RENASOUND SYNTH", 20, 8, getWidth() - 40, 34, juce::Justification::centredLeft);
        g.setColour (theme::textDim);
        g.setFont (theme::font (12.5f));
        g.drawText ("Pick a sound, or load a plugin instrument with Change.", 20, 38, getWidth() - 40, 18, juce::Justification::centredLeft);
    }

    void resized() override
    {
        for (int i = 0; i < buttons.size(); ++i)
            buttons[i]->setBounds (20 + (i % 3) * 130, 70 + (i / 3) * 46, 122, 38);
    }

private:
    AppContext& ctx;
    TrackPtr track;
    juce::OwnedArray<juce::TextButton> buttons;
};

// =============================================================================
//  One device's editor in a floating window
// =============================================================================
// =============================================================================
//  Preset bar: on top of every RENASOUND device. Presets are small files in
//  Music/RENASOUND/Presets/<device>/, holding the device's full settings.
// =============================================================================
class PresetBar : public juce::Component
{
public:
    PresetBar (AppContext& c, juce::AudioProcessor& p, const juce::String& deviceName) : ctx (c), proc (p), name (deviceName)
    {
        for (auto* b : { &prev, &next, &save, &remove })
        {
            addAndMakeVisible (*b);
            b->getProperties().set ("compact", true);
        }
        addAndMakeVisible (box);
        box.setTextWhenNothingSelected ("Default");
        box.onChange = [this] { boxChanged(); };
        prev.onClick = [this] { step (-1); };
        next.onClick = [this] { step (1); };
        save.onClick = [this] { askToSave(); };
        remove.onClick = [this] { deleteCurrent(); };
        prev.setTooltip ("Previous preset");
        next.setTooltip ("Next preset");
        save.setTooltip ("Save these settings as a preset");
        remove.setTooltip ("Delete this preset");
        refresh();
        setSize (400, 38);
    }

    juce::File folder() const
    {
        return juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile ("RENASOUND").getChildFile ("Presets")
                   .getChildFile (juce::File::createLegalFileName (name));
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::bg);
        g.setColour (theme::textDim);
        g.setFont (theme::font (10.5f, true));
        g.drawText ("PRESET", 12, 0, 60, getHeight(), juce::Justification::centredLeft);
        g.setColour (theme::grid);
        g.drawHorizontalLine (getHeight() - 1, 0, (float) getWidth());
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (8, 5).withTrimmedLeft (54);
        remove.setBounds (r.removeFromRight (64)); r.removeFromRight (2);
        save.setBounds (r.removeFromRight (64));   r.removeFromRight (8);
        next.setBounds (r.removeFromRight (28));
        prev.setBounds (r.removeFromRight (28));   r.removeFromRight (4);
        box.setBounds (r.removeFromLeft (juce::jmin (r.getWidth(), 260)));
    }

private:
    AppContext& ctx;
    juce::AudioProcessor& proc;
    juce::String name;
    juce::ComboBox box;
    juce::TextButton prev { juce::String::fromUTF8 ("\xe2\x97\x80") }, next { juce::String::fromUTF8 ("\xe2\x96\xb6") },
                     save { "Save..." }, remove { "Delete" };
    juce::Array<juce::File> files;
    juce::String current;
    std::unique_ptr<juce::AlertWindow> prompt;

    void refresh()
    {
        files.clear();
        for (const auto& e : juce::RangedDirectoryIterator (folder(), false, "*.rspreset", juce::File::findFiles)) files.add (e.getFile());
        struct ByName { static int compareElements (const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural (b.getFileName()); } } cmp;
        files.sort (cmp);
        box.clear (juce::dontSendNotification);
        box.addItem ("Default", 1);
        if (! files.isEmpty()) box.addSeparator();
        for (int i = 0; i < files.size(); ++i) box.addItem (files[i].getFileNameWithoutExtension(), 100 + i);
        box.addSeparator();
        box.addItem ("Show presets folder", 2);
        int sel = 0;
        for (int i = 0; i < files.size(); ++i) if (files[i].getFileNameWithoutExtension() == current) sel = 100 + i;
        if (sel > 0) box.setSelectedId (sel, juce::dontSendNotification); else box.setText (current.isEmpty() ? juce::String ("Default") : current, juce::dontSendNotification);
        remove.setEnabled (sel > 0);
    }

    void boxChanged()
    {
        const int id = box.getSelectedId();
        if (id == 1) { loadDefault(); return; }
        if (id == 2) { folder().createDirectory(); folder().revealToUser(); refresh(); return; }
        if (id >= 100 && id - 100 < files.size()) load (files[id - 100]);
    }

    void step (int dir)
    {
        if (files.isEmpty()) return;
        int idx = -1;
        for (int i = 0; i < files.size(); ++i) if (files[i].getFileNameWithoutExtension() == current) idx = i;
        idx = (idx + dir + files.size()) % files.size();
        if (idx < 0) idx = 0;
        load (files[idx]);
    }

    void load (const juce::File& f)
    {
        auto xml = juce::XmlDocument::parse (f);
        if (xml == nullptr || ! xml->hasTagName ("RENASOUND_PRESET")) return;
        juce::MemoryBlock mb;
        if (! mb.fromBase64Encoding (xml->getStringAttribute ("state"))) return;
        ctx.project.beginEdit();
        proc.setStateInformation (mb.getData(), (int) mb.getSize());
        ctx.project.dirty = true;
        current = f.getFileNameWithoutExtension();
        refresh();
    }

    void loadDefault()
    {
        ctx.project.beginEdit();
        for (auto* prm : proc.getParameters()) prm->setValueNotifyingHost (prm->getDefaultValue());
        ctx.project.dirty = true;
        current.clear();
        refresh();
    }

    void askToSave()
    {
        prompt = std::make_unique<juce::AlertWindow> ("Save preset", "A name for these " + name + " settings:", juce::MessageBoxIconType::NoIcon);
        prompt->addTextEditor ("name", current.isNotEmpty() ? current : juce::String ("My " + name), juce::String());
        prompt->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
        prompt->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        if (auto* te = prompt->getTextEditor ("name"))
        {
            te->setSelectAllWhenFocused (true);
            juce::MessageManager::callAsync ([sp = juce::Component::SafePointer<juce::TextEditor> (te)] { if (sp != nullptr) sp->grabKeyboardFocus(); });
        }
        prompt->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int result)
        {
            if (result != 1 || prompt == nullptr) return;
            const auto presetName = juce::File::createLegalFileName (prompt->getTextEditorContents ("name").trim());
            if (presetName.isEmpty()) return;
            juce::MemoryBlock mb;
            proc.getStateInformation (mb);
            juce::XmlElement xml ("RENASOUND_PRESET");
            xml.setAttribute ("device", name);
            xml.setAttribute ("state", mb.toBase64Encoding());
            folder().createDirectory();
            xml.writeTo (folder().getChildFile (presetName + ".rspreset"));
            current = presetName;
            refresh();
        }), false);
    }

    void deleteCurrent()
    {
        for (auto& f : files)
            if (f.getFileNameWithoutExtension() == current)
            {
                f.moveToTrash();
                current.clear();
                refresh();
                return;
            }
    }
};

class DeviceWindow::Content : public juce::Component
{
public:
    Content (AppContext& ctx, int trackId, FxPtr slot, bool instrument)
    {
        auto track = ctx.project.findTrack (trackId);
        if (instrument && slot == nullptr && track != nullptr && track->synth != nullptr)
            editor = std::make_unique<SynthPresetPanel> (ctx, track);
        else if (slot != nullptr && slot->processor != nullptr)
        {
            juce::Component* ed = slot->processor->hasEditor() ? slot->processor->createEditorAndMakeActive() : nullptr;
            if (ed == nullptr) ed = new juce::GenericAudioProcessorEditor (*slot->processor);
            editor.reset (ed);
        }
        else
            editor = std::make_unique<juce::Label> ("", slot != nullptr ? "\"" + slot->plugin.name + "\" isn't installed on this computer."
                                                                         : juce::String ("Nothing to show."));
        if (editor->getWidth() == 0) editor->setSize (420, 200);
        addAndMakeVisible (*editor);
        // every device gets the preset bar: RENASOUND's own and third-party plugins
        if (slot != nullptr && slot->processor != nullptr)
        {
            presets = std::make_unique<PresetBar> (ctx, *slot->processor, slot->processor->getName().upToFirstOccurrenceOf (":", false, false).trim());
            addAndMakeVisible (*presets);
        }
        layout();
    }

    int barHeight() const { return presets != nullptr ? 38 : 0; }
    void layout()
    {
        setSize (juce::jmax (editor->getWidth(), 420), editor->getHeight() + barHeight());
    }
    void resized() override
    {
        if (presets != nullptr) presets->setBounds (0, 0, getWidth(), barHeight());
        editor->setTopLeftPosition (0, barHeight());
    }

    ~Content() override { presets.reset(); editor.reset(); }   // editor goes before its processor

    void childBoundsChanged (juce::Component* c) override
    {
        if (c == editor.get()) layout();
    }

    std::unique_ptr<juce::Component> editor;
    std::unique_ptr<PresetBar> presets;
};

DeviceWindow::DeviceWindow (AppContext& c, int track, FxPtr s, bool instrument)
    : DocumentWindow ("", theme::bg, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      trackId (track), slot (std::move (s)), isInstrument (instrument), ctx (c)
{
    updateTitle();
    setUsingNativeTitleBar (true);
    setContentOwned (new Content (ctx, trackId, slot, isInstrument), true);
    setResizable (false, false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    ctx.project.addChangeListener (this);
    for (auto* l : ctx.keyListeners) addKeyListener (l);   // play the keyboard and use shortcuts from here too
    timerCallback();
    startTimerHz (4);
}

void DeviceWindow::timerCallback()
{
    // stays above the RENASOUND window, but not above other apps
    const bool front = juce::Process::isForegroundProcess();
    if (isAlwaysOnTop() != front) setAlwaysOnTop (front);
}

DeviceWindow::~DeviceWindow()
{
    stopTimer();
    for (auto* l : ctx.keyListeners) removeKeyListener (l);
    ctx.project.removeChangeListener (this);
    clearContentComponent();
}

void DeviceWindow::updateTitle()
{
    auto track = ctx.project.findTrack (trackId);
    const auto owner = track != nullptr ? track->name : juce::String ("Master");
    juce::String device = slot != nullptr ? slot->getName() : juce::String ("RENASOUND Synth");
    if (slot != nullptr && slot->processor != nullptr)
        if (auto n = getSynthPatchName (slot->processor.get()); n.isNotEmpty()) device = "RENASYNTH  /  " + n;
    setName (owner + " - " + device);
}

bool DeviceWindow::stillExists() const
{
    if (trackId >= 0 && ctx.project.findTrack (trackId) == nullptr) return false;
    if (isInstrument)
    {
        auto t = ctx.project.findTrack (trackId);
        return t != nullptr && t->instrument == slot;
    }
    auto* chain = ctx.project.getFxChain (trackId);
    return chain != nullptr && std::find (chain->begin(), chain->end(), slot) != chain->end();
}

void DeviceWindow::changeListenerCallback (juce::ChangeBroadcaster*)
{
    if (! stillExists()) { if (onClose) onClose(); return; }
    updateTitle();
}

// =============================================================================
PluginManagerWindow::PluginManagerWindow (AppContext& c)
    : DocumentWindow ("Plugins", theme::bg, DocumentWindow::closeButton), ctx (c)
{
    auto& host = ctx.engine.pluginHost;
    auto* list = new juce::PluginListComponent (host.formats, host.knownPlugins, host.getDeadMansPedalFile(),
                                                host.settings.get(), false);
    list->setOptionsButtonText ("Scan / options...");
    list->setSize (720, 520);
    setUsingNativeTitleBar (true);
    setContentOwned (list, true);
    setResizable (true, false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

void PluginManagerWindow::closeButtonPressed()
{
    ctx.engine.pluginHost.save();
    if (onClose) onClose();
}

} // namespace rs
