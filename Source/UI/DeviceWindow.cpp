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
    menu.addSectionHeader ("RENASOUND SYNTH");
    const auto& presets = getSynthPresets();
    for (int i = 0; i < (int) presets.size(); ++i)
        menu.addItem (presets[(size_t) i].name, true, track->instrument == nullptr && track->synth->presetIndex == i, [&ctx, track, i]
        {
            if (track->instrument != nullptr) { ctx.project.beginEdit(); ctx.project.setInstrument (track->id, nullptr); }
            track->synth->presetIndex = i;
            ctx.project.dirty = true;
            ctx.project.sendChangeMessage();
        });

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
        setSize (editor->getWidth(), editor->getHeight());
    }

    ~Content() override { editor.reset(); }   // editor goes before its processor

    void childBoundsChanged (juce::Component* c) override
    {
        if (c == editor.get()) setSize (editor->getWidth(), editor->getHeight());
    }

    std::unique_ptr<juce::Component> editor;
};

DeviceWindow::DeviceWindow (AppContext& c, int track, FxPtr s, bool instrument)
    : DocumentWindow ("", theme::bg, DocumentWindow::closeButton | DocumentWindow::minimiseButton),
      trackId (track), slot (std::move (s)), isInstrument (instrument), ctx (c)
{
    updateTitle();
    setUsingNativeTitleBar (true);
    setContentOwned (new Content (ctx, trackId, slot, isInstrument), true);
    setResizable (false, false);
    setAlwaysOnTop (false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
    ctx.project.addChangeListener (this);
}

DeviceWindow::~DeviceWindow()
{
    ctx.project.removeChangeListener (this);
    clearContentComponent();
}

void DeviceWindow::updateTitle()
{
    auto track = ctx.project.findTrack (trackId);
    const auto owner = track != nullptr ? track->name : juce::String ("Master");
    const auto device = slot != nullptr ? slot->getName() : juce::String ("RENASOUND Synth");
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
