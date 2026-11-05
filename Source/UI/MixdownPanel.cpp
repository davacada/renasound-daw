#include "MixdownPanel.h"

namespace rs
{

namespace
{
    const int rates[] { 0, 44100, 48000, 88200, 96000 };

    // renders the mix and / or the stems with a progress bar, off the message thread
    class ExportJob : public juce::ThreadWithProgressWindow
    {
    public:
        struct Item { int trackId; bool master; juce::File file; juce::String label; };

        ExportJob (AudioEngine& e, std::vector<Item> i, exporter::Settings s, exporter::SongInfo info)
            : ThreadWithProgressWindow ("Mixing down...", true, true), engine (e), items (std::move (i)), settings (s), song (info) {}

        void run() override
        {
            for (size_t k = 0; k < items.size() && ! threadShouldExit(); ++k)
            {
                const auto& it = items[k];
                setStatusMessage (items.size() > 1 ? "Rendering " + it.label + "  (" + juce::String ((int) k + 1) + " of " + juce::String ((int) items.size()) + ")"
                                                   : juce::String ("Rendering the mix"));
                juce::AudioBuffer<float> audio;
                auto progress = [this, k] (double p) { setProgress (((double) k + p * 0.9) / (double) items.size()); return ! threadShouldExit(); };
                if (! engine.renderMix (audio, it.trackId, it.master, progress, error)) return;
                setStatusMessage ("Writing " + it.file.getFileName());
                auto info = song;
                if (! it.master && it.trackId != 0) info.title = (song.title.isNotEmpty() ? song.title + " - " : juce::String()) + it.label;
                if (! exporter::write (audio, engine.getSampleRate(), settings, info, it.file, error)) return;
                written.add (it.file);
            }
        }

        // called on the message thread when done; the job then deletes itself
        std::function<void (ExportJob&, bool cancelled)> onDone;
        void threadComplete (bool cancelled) override
        {
            if (onDone) onDone (*this, cancelled);
            juce::MessageManager::callAsync ([this] { delete this; });
        }

        AudioEngine& engine;
        std::vector<Item> items;
        exporter::Settings settings;
        exporter::SongInfo song;
        juce::String error;
        juce::Array<juce::File> written;
    };

    void styleEditor (juce::TextEditor& e, const juce::String& hint)
    {
        e.setTextToShowWhenEmpty (hint, theme::textDim.withAlpha (0.6f));
        e.setFont (theme::font (14.5f));
        e.setColour (juce::TextEditor::backgroundColourId, theme::bg);
        e.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
        e.setColour (juce::TextEditor::focusedOutlineColourId, theme::accent.withAlpha (0.6f));
        e.setColour (juce::TextEditor::textColourId, theme::text);
        e.setIndents (10, 7);
    }
}

MixdownPanel::MixdownPanel (AppContext& c) : ctx (c)
{
    const auto& s = ctx.project.song;
    for (auto [ed, hint, text] : { std::tuple { &title, "Song name", s.title }, { &artist, "Artist", s.artist }, { &album, "Album", s.album },
                                   { &writers, "Writers (comma between names)", s.writers }, { &producers, "Producers", s.producers } })
    {
        styleEditor (*ed, hint);
        ed->setText (text, false);
        ed->onTextChange = [this] { repaint(); };
        addAndMakeVisible (*ed);
    }
    if (title.isEmpty() && ctx.project.file.existsAsFile()) title.setText (ctx.project.file.getFileNameWithoutExtension(), false);

    format.addItemList ({ "WAV", "AIFF", "FLAC", "Ogg Vorbis" }, 1);
    format.setSelectedItemIndex (juce::jlimit (0, 3, s.format), juce::dontSendNotification);
    format.onChange = [this] { refreshQuality(); repaint(); };
    rate.addItem ("Project rate (" + juce::String (ctx.engine.getSampleRate() / 1000.0, 1) + " kHz)", 1);
    rate.addItemList ({ "44.1 kHz", "48 kHz", "88.2 kHz", "96 kHz" }, 2);
    int rateIndex = 0;
    for (int i = 0; i < 5; ++i) if (rates[i] == (int) s.sampleRate) rateIndex = i;
    rate.setSelectedItemIndex (rateIndex, juce::dontSendNotification);
    rate.onChange = [this] { repaint(); };
    quality.onChange = [this] { repaint(); };
    for (auto* cb : { &format, &quality, &rate }) addAndMakeVisible (*cb);
    refreshQuality();

    for (auto* b : { &mixButton, &stemsButton })
    {
        b->setClickingTogglesState (true);
        b->setRadioGroupId (77);
        b->onClick = [this] { withMix.setEnabled (stemsButton.getToggleState()); repaint(); };
        addAndMakeVisible (*b);
    }
    mixButton.setToggleState (! s.stems, juce::dontSendNotification);
    stemsButton.setToggleState (s.stems, juce::dontSendNotification);
    withMix.setToggleState (s.stemsWithMix, juce::dontSendNotification);
    withMix.setEnabled (s.stems);
    addAndMakeVisible (withMix);

    removeCover.getProperties().set ("compact", true);
    removeCover.onClick = [this] { setCover ({}); };
    addChildComponent (removeCover);
    setCover (juce::File (s.coverPath));

    cancel.onClick = [this] { storeSettings(); if (onClose) onClose(); };
    go.onClick = [this] { start(); };
    go.setToggleState (true, juce::dontSendNotification);   // drawn in the accent colour
    go.setColour (juce::TextButton::buttonOnColourId, theme::accent);
    go.setColour (juce::TextButton::textColourOnId, juce::Colours::black);
    addAndMakeVisible (cancel);
    addAndMakeVisible (go);
    setSize (760, 560);
}

MixdownPanel::~MixdownPanel() = default;

void MixdownPanel::refreshQuality()
{
    const int f = format.getSelectedItemIndex();
    const auto& s = ctx.project.song;
    quality.clear (juce::dontSendNotification);
    if (f == 3)
    {
        const int kbps[] { 128, 160, 192, 224, 256, 320 };
        for (int i = 0; i < 6; ++i) quality.addItem (juce::String (kbps[i]) + " kbps", kbps[i]);
        quality.setSelectedId (s.oggKbps, juce::dontSendNotification);
        if (quality.getSelectedId() == 0) quality.setSelectedId (256, juce::dontSendNotification);
    }
    else
    {
        quality.addItem ("16-bit", 16);
        quality.addItem ("24-bit", 24);
        if (f != 2) quality.addItem ("32-bit float", 32);
        quality.setSelectedId (s.bitDepth, juce::dontSendNotification);
        if (quality.getSelectedId() == 0) quality.setSelectedId (24, juce::dontSendNotification);
    }
}

exporter::Settings MixdownPanel::settings() const
{
    exporter::Settings s;
    s.format = (exporter::Format) juce::jlimit (0, 3, format.getSelectedItemIndex());
    if (s.format == exporter::Format::Ogg) s.oggKbps = quality.getSelectedId();
    else s.bitDepth = quality.getSelectedId();
    s.sampleRate = rates[juce::jlimit (0, 4, rate.getSelectedItemIndex())];
    return s;
}

exporter::SongInfo MixdownPanel::songInfo() const
{
    return { title.getText().trim(), artist.getText().trim(), album.getText().trim(), writers.getText().trim(), producers.getText().trim(), coverFile };
}

void MixdownPanel::storeSettings()
{
    auto& s = ctx.project.song;
    const auto st = settings();
    const auto info = songInfo();
    const bool changed = s.title != info.title || s.artist != info.artist || s.album != info.album || s.writers != info.writers
                      || s.producers != info.producers || s.coverPath != coverFile.getFullPathName();
    s.title = info.title; s.artist = info.artist; s.album = info.album; s.writers = info.writers; s.producers = info.producers;
    s.coverPath = coverFile.getFullPathName();
    s.format = (int) st.format; s.bitDepth = st.bitDepth; s.oggKbps = st.oggKbps; s.sampleRate = st.sampleRate;
    s.stems = stemsButton.getToggleState(); s.stemsWithMix = withMix.getToggleState();
    if (changed) ctx.project.dirty = true;
}

void MixdownPanel::setCover (const juce::File& f)
{
    coverImage = f.existsAsFile() ? juce::ImageFileFormat::loadFrom (f) : juce::Image();
    coverFile = coverImage.isValid() ? f : juce::File();
    removeCover.setVisible (coverImage.isValid());
    repaint();
}

void MixdownPanel::filesDropped (const juce::StringArray& files, int, int)
{
    for (auto& f : files)
        if (juce::File (f).hasFileExtension ("jpg;jpeg;png;gif")) { setCover (juce::File (f)); return; }
}

void MixdownPanel::mouseUp (const juce::MouseEvent& e)
{
    if (! coverArea.contains (e.getPosition())) return;
    chooser = std::make_unique<juce::FileChooser> ("Choose a cover picture", juce::File::getSpecialLocation (juce::File::userPicturesDirectory), "*.jpg;*.jpeg;*.png");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc) { if (fc.getResult().existsAsFile()) setCover (fc.getResult()); });
}

juce::String MixdownPanel::summary() const
{
    const double sec = ctx.project.getEndBeat() * 60.0 / ctx.project.bpm;
    const int m = (int) sec / 60, s = (int) sec % 60;
    auto st = settings();
    juce::String text = exporter::describe (st);
    if (st.sampleRate <= 0) text << ", " << juce::String (ctx.engine.getSampleRate() / 1000.0, 1) << " kHz";
    text << "   /   " << m << ":" << juce::String (s).paddedLeft ('0', 2);
    if (stemsButton.getToggleState())
    {
        int n = 0;
        for (auto& t : ctx.project.tracks) if (! t->clips.empty()) ++n;
        text << "   /   " << n << " stem" << (n == 1 ? "" : "s") << (withMix.getToggleState() ? " + the mix" : "");
    }
    return text;
}

void MixdownPanel::paint (juce::Graphics& g)
{
    g.fillAll (theme::panel);
    g.setColour (theme::text);
    g.setFont (juce::Font (juce::FontOptions (22.0f, juce::Font::bold)).withExtraKerningFactor (0.08f));
    g.drawText ("MIX DOWN", 24, 14, 300, 32, juce::Justification::centredLeft);

    // the cover
    auto cr = coverArea.toFloat();
    if (coverImage.isValid())
    {
        g.saveState();
        juce::Path clip; clip.addRoundedRectangle (cr, 8.0f);
        g.reduceClipRegion (clip);
        g.drawImage (coverImage, cr, juce::RectanglePlacement::centred | juce::RectanglePlacement::fillDestination);
        g.restoreState();
    }
    else
    {
        g.setColour (theme::bg);
        g.fillRoundedRectangle (cr, 8.0f);
        juce::Path dash; dash.addRoundedRectangle (cr.reduced (6), 6.0f);
        juce::Path dashed;
        const float d[] { 6.0f, 5.0f };
        juce::PathStrokeType (1.2f).createDashedStroke (dashed, dash, d, 2);
        g.setColour (theme::textDim.withAlpha (0.5f));
        g.fillPath (dashed);
        g.setColour (theme::textDim);
        g.setFont (theme::font (13.0f, true));
        g.drawFittedText ("COVER PICTURE\n\nClick to choose,\nor drop an image here", coverArea.reduced (20), juce::Justification::centred, 5);
    }

    // labels
    g.setColour (theme::textDim);
    g.setFont (theme::font (10.5f, true));
    auto label = [&g] (juce::Component& c, const char* text) { g.drawText (text, c.getBounds().translated (0, -16).withHeight (14), juce::Justification::left); };
    label (title, "SONG"); label (artist, "ARTIST"); label (album, "ALBUM"); label (writers, "WRITERS"); label (producers, "PRODUCERS");
    label (format, "FILE TYPE"); label (quality, format.getSelectedItemIndex() == 3 ? "BIT RATE" : "BIT DEPTH"); label (rate, "SAMPLE RATE");
    label (mixButton, "EXPORT");

    g.setColour (theme::grid);
    g.drawHorizontalLine (format.getY() - 30, 24.0f, (float) getWidth() - 24);

    g.setColour (theme::text);
    g.setFont (theme::font (12.5f, true));
    g.drawText (summary(), 24, go.getY(), go.getX() - 40, go.getHeight(), juce::Justification::centredLeft);
    if (stemsButton.getToggleState())
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText ("One file per track, with its effects, volume and pan; master effects stay off the stems.",
                    mixButton.getX(), mixButton.getBottom() + 8, getWidth() - mixButton.getX() - 24, 16, juce::Justification::left);
    }
    if (format.getSelectedItemIndex() == 3)
    {
        g.setColour (theme::textDim);
        g.setFont (theme::font (11.5f));
        g.drawText ("The cover is saved next to Ogg files.", rate.getRight() + 14, rate.getY(), 260, rate.getHeight(), juce::Justification::centredLeft);
    }
}

void MixdownPanel::resized()
{
    auto r = getLocalBounds().reduced (24, 0);
    r.removeFromTop (60);
    auto top = r.removeFromTop (250);
    coverArea = top.removeFromLeft (230).withHeight (230);
    removeCover.setBounds (coverArea.getX(), coverArea.getBottom() + 2, coverArea.getWidth(), 20);
    top.removeFromLeft (24);
    for (auto* ed : { &title, &artist, &album, &writers, &producers })
    {
        top.removeFromTop (16);
        ed->setBounds (top.removeFromTop (32));
        top.removeFromTop (2);
    }
    r.removeFromTop (44);
    auto row = r.removeFromTop (30);
    format.setBounds (row.removeFromLeft (170)); row.removeFromLeft (14);
    quality.setBounds (row.removeFromLeft (150)); row.removeFromLeft (14);
    rate.setBounds (row.removeFromLeft (200));
    r.removeFromTop (34);
    auto ex = r.removeFromTop (32);
    mixButton.setBounds (ex.removeFromLeft (140)); ex.removeFromLeft (4);
    stemsButton.setBounds (ex.removeFromLeft (140)); ex.removeFromLeft (16);
    withMix.setBounds (ex.removeFromLeft (200));
    auto bottom = getLocalBounds().reduced (24, 18).removeFromBottom (36);
    go.setBounds (bottom.removeFromRight (150));
    bottom.removeFromRight (8);
    cancel.setBounds (bottom.removeFromRight (100));
    go.setBounds (go.getBounds());
}

void MixdownPanel::start()
{
    storeSettings();
    if (ctx.project.getEndBeat() <= 0) { if (showMessage) showMessage ("Nothing to mix down", "Record or add some clips first."); return; }
    ctx.engine.stop();
    const auto st = settings();
    const auto info = songInfo();
    const auto ext = exporter::extension (st.format);
    auto baseName = juce::File::createLegalFileName (info.title.isNotEmpty() ? info.title
                                                     : ctx.project.file.existsAsFile() ? ctx.project.file.getFileNameWithoutExtension() : juce::String ("My Song"));
    const bool stems = stemsButton.getToggleState(), alsoMix = withMix.getToggleState();
    const auto music = juce::File::getSpecialLocation (juce::File::userMusicDirectory);

    juce::Component::SafePointer<MixdownPanel> sp (this);
    auto run = [sp, st, info] (std::vector<ExportJob::Item> items)
    {
        if (sp == nullptr) return;
        auto* job = new ExportJob (sp->ctx.engine, std::move (items), st, info);
        job->onDone = [sp] (ExportJob& j, bool cancelled)
        {
            if (sp == nullptr) return;
            auto show = sp->showMessage;
            if (cancelled || j.error.isNotEmpty())
            {
                if (show && ! cancelled && j.error != "Cancelled.") show ("Mixdown stopped", j.error);
                return;
            }
            if (! j.written.isEmpty()) j.written.getFirst().revealToUser();
            if (show)
                show ("Mixdown finished", j.written.size() == 1 ? "Saved " + j.written.getFirst().getFileName()
                                                                 : "Saved " + juce::String (j.written.size()) + " files in " + j.written.getFirst().getParentDirectory().getFileName());
            if (sp->onClose) sp->onClose();
        };
        job->launchThread();
    };

    if (! stems)
    {
        chooser = std::make_unique<juce::FileChooser> ("Mix down to", music.getChildFile (baseName + ext), "*" + ext);
        chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::warnAboutOverwriting,
                              [run, ext] (const juce::FileChooser& fc)
        {
            auto f = fc.getResult();
            if (f == juce::File()) return;
            run ({ { 0, true, f.withFileExtension (ext), "the mix" } });
        });
        return;
    }

    chooser = std::make_unique<juce::FileChooser> ("Choose a folder for the stems", music, "");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                          [this, run, ext, baseName, alsoMix] (const juce::FileChooser& fc) mutable
    {
        auto dir = fc.getResult();
        if (dir == juce::File()) return;
        if (! dir.isDirectory()) dir = dir.getParentDirectory();
        dir = dir.getChildFile (baseName + " stems");
        dir.createDirectory();
        std::vector<ExportJob::Item> items;
        int n = 1;
        for (auto& t : ctx.project.tracks)
        {
            if (t->clips.empty()) continue;
            const auto name = juce::String (n++).paddedLeft ('0', 2) + " " + juce::File::createLegalFileName (t->name);
            items.push_back ({ t->id, false, dir.getChildFile (baseName + " - " + name + ext).getNonexistentSibling(), t->name });
        }
        if (alsoMix) items.push_back ({ 0, true, dir.getChildFile (baseName + " - Mix" + ext).getNonexistentSibling(), "the mix" });
        run (std::move (items));
    });
}

} // namespace rs
