#pragma once
#include "AppContext.h"
#include "DeviceWindow.h"

namespace rs
{

// =============================================================================
//  The insert slots on a channel strip: click an effect to open it, click its
//  light to bypass it, drag to reorder, x (or right-click) to remove, + to add.
// =============================================================================
class InsertList : public juce::Component
{
public:
    static constexpr int defaultRowH = 23;

    InsertList (AppContext& c, int id, int rowHeight = defaultRowH) : rowH (rowHeight), ctx (c), trackId (id) { refresh(); }

    const int rowH;

    void refresh()
    {
        if (auto* ch = ctx.project.getFxChain (trackId)) chain = *ch;
        else chain.clear();
        setSize (getWidth(), getIdealHeight());
        repaint();
    }

    int getIdealHeight() const { return ((int) chain.size() + 1) * rowH; }

    void paint (juce::Graphics& g) override
    {
        for (int i = 0; i < (int) chain.size(); ++i)
        {
            auto& slot = *chain[(size_t) i];
            auto r = rowBounds (i).reduced (0, 1);
            const bool on = ! slot.bypassed.load();
            const bool hover = i == hoverRow;

            g.setColour (dragIndex == i ? theme::accent.withAlpha (0.25f) : hover ? theme::bg.brighter (0.08f) : theme::bg.brighter (0.03f));
            g.fillRoundedRectangle (r.toFloat(), 4.0f);

            // on / bypass light
            auto led = ledBounds (i).toFloat().withSizeKeepingCentre (juce::jmin (12.0f, rowH - 7.0f), juce::jmin (12.0f, rowH - 7.0f));
            g.setColour (on ? theme::accent2 : theme::grid);
            g.fillEllipse (led);
            if (on) { g.setColour (theme::accent2.withAlpha (0.25f)); g.fillEllipse (led.expanded (2.0f)); }

            g.setColour (slot.processor == nullptr ? theme::record : on ? theme::text : theme::textDim);
            g.setFont (theme::font (juce::jmin (12.0f, rowH * 0.55f), on));
            auto text = r.withTrimmedLeft (20).withTrimmedRight (hover ? 18 : 4);
            g.drawText (slot.getName(), text, juce::Justification::centredLeft, true);

            if (hover)
            {
                g.setColour (theme::textDim);
                auto x = removeBounds (i).toFloat().withSizeKeepingCentre (7.0f, 7.0f);
                g.drawLine (x.getX(), x.getY(), x.getRight(), x.getBottom(), 1.5f);
                g.drawLine (x.getRight(), x.getY(), x.getX(), x.getBottom(), 1.5f);
            }
        }

        // + add slot
        auto add = rowBounds ((int) chain.size()).reduced (0, 1).toFloat();
        const bool addHover = hoverRow == (int) chain.size();
        g.setColour (addHover ? theme::accent.withAlpha (0.18f) : juce::Colours::transparentBlack);
        g.fillRoundedRectangle (add, 4.0f);
        juce::Path dash;
        dash.addRoundedRectangle (add.reduced (0.5f), 4.0f);
        juce::Path dashed;
        const float lengths[] = { 3.0f, 3.0f };
        juce::PathStrokeType (1.0f).createDashedStroke (dashed, dash, lengths, 2);
        g.setColour (addHover ? theme::accent : theme::gridBar);
        g.fillPath (dashed);
        g.setColour (addHover ? theme::accent : theme::textDim);
        g.setFont (theme::font (juce::jmin (12.0f, rowH * 0.55f), true));
        g.drawText (chain.empty() ? "+ Add effect" : "+", add, juce::Justification::centred);

        // drop indicator while dragging
        if (dragIndex >= 0 && dropIndex >= 0)
        {
            g.setColour (theme::accent);
            g.fillRect (0, dropIndex * rowH - 1, getWidth(), 2);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        const int r = rowAt (e.y);
        if (r != hoverRow) { hoverRow = r; repaint(); }
        setMouseCursor (r >= 0 && r < (int) chain.size() && ! ledBounds (r).contains (e.getPosition()) && ! removeBounds (r).contains (e.getPosition())
                            ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override { hoverRow = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int r = rowAt (e.y);
        dragIndex = -1;
        pressedRow = r;
        if (trackId >= 0) ctx.selectTrack (trackId);

        if (r == (int) chain.size())   // the + slot
        {
            devicemenus::showAddEffectMenu (ctx, trackId, this, [this] (FxPtr s) { if (ctx.openDevice) ctx.openDevice (trackId, s, false); });
            pressedRow = -1;
            return;
        }
        if (r < 0) return;

        auto slot = chain[(size_t) r];
        if (e.mods.isPopupMenu()) { showMenu (slot, r); pressedRow = -1; return; }

        if (ledBounds (r).contains (e.getPosition()))
        {
            slot->bypassed = ! slot->bypassed.load();
            ctx.project.dirty = true;
            pressedRow = -1;
            repaint();
            return;
        }
        if (hoverRow == r && removeBounds (r).contains (e.getPosition()))
        {
            pressedRow = -1;
            juce::MessageManager::callAsync ([&p = ctx.project, id = trackId, slot] { p.beginEdit(); p.removeFx (id, slot); });
            return;
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (pressedRow < 0 || pressedRow >= (int) chain.size() || e.getDistanceFromDragStart() < 5) return;
        dragIndex = pressedRow;
        dropIndex = juce::jlimit (0, (int) chain.size(), (e.y + rowH / 2) / rowH);
        repaint();
    }

    void mouseUp (const juce::MouseEvent& e) override
    {
        if (dragIndex >= 0)
        {
            int target = dropIndex;
            if (target > dragIndex) --target;
            if (target != dragIndex)
            {
                ctx.project.beginEdit();
                ctx.project.moveFx (trackId, chain[(size_t) dragIndex], target);
            }
        }
        else if (pressedRow >= 0 && pressedRow < (int) chain.size() && rowAt (e.y) == pressedRow && ! e.mods.isPopupMenu())
        {
            if (ctx.openDevice) ctx.openDevice (trackId, chain[(size_t) pressedRow], false);
        }
        dragIndex = dropIndex = pressedRow = -1;
        repaint();
    }

private:
    juce::Rectangle<int> rowBounds (int i) const    { return { 0, i * rowH, getWidth(), rowH }; }
    juce::Rectangle<int> ledBounds (int i) const    { return rowBounds (i).removeFromLeft (20); }
    juce::Rectangle<int> removeBounds (int i) const { return rowBounds (i).removeFromRight (20); }
    int rowAt (int y) const
    {
        const int r = y / rowH;
        return (y >= 0 && r <= (int) chain.size()) ? r : -1;
    }

    void showMenu (FxPtr slot, int index)
    {
        juce::PopupMenu m;
        m.addItem ("Open", [this, slot] { if (ctx.openDevice) ctx.openDevice (trackId, slot, false); });
        m.addItem (slot->bypassed ? "Turn on" : "Bypass", [this, slot] { slot->bypassed = ! slot->bypassed.load(); ctx.project.dirty = true; repaint(); });
        m.addSeparator();
        m.addItem ("Move up", index > 0, false, [this, slot, index] { ctx.project.beginEdit(); ctx.project.moveFx (trackId, slot, index - 1); });
        m.addItem ("Move down", index < (int) chain.size() - 1, false, [this, slot, index] { ctx.project.beginEdit(); ctx.project.moveFx (trackId, slot, index + 1); });
        m.addSeparator();
        m.addItem ("Remove", [this, slot] { ctx.project.beginEdit(); ctx.project.removeFx (trackId, slot); });
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this));
    }

    AppContext& ctx;
    const int trackId;
    std::vector<FxPtr> chain;
    int hoverRow = -1, pressedRow = -1, dragIndex = -1, dropIndex = -1;
};

} // namespace rs
