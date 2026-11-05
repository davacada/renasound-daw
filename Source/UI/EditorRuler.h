#pragma once
#include "AppContext.h"

namespace rs
{

// =============================================================================
//  A thin ruler for the editors: bar numbers, the clip's span, and the playhead.
//  Click or drag it to move the playhead.
// =============================================================================
class EditorRuler : public juce::Component, public juce::SettableTooltipClient, private juce::Timer
{
public:
    explicit EditorRuler (AppContext& c) : ctx (c)
    {
        setMouseCursor (juce::MouseCursor::PointingHandCursor);
        setTooltip ("Click or drag to move the playhead");
        startTimerHz (30);
    }

    std::function<double (float x)> xToBeat;          // song beats
    std::function<float (double beat)> beatToX;
    std::function<std::pair<double, double>()> clipSpan;   // song beats; empty span = none

    void paint (juce::Graphics& g) override
    {
        g.fillAll (theme::panel);
        if (! xToBeat || ! beatToX) return;
        const int bpb = ctx.project.beatsPerBar;

        if (clipSpan)
        {
            const auto [a, b] = clipSpan();
            if (b > a)
            {
                g.setColour (theme::accent.withAlpha (0.12f));
                g.fillRect (juce::Rectangle<float> (beatToX (a), 0, beatToX (b) - beatToX (a), (float) getHeight()));
            }
        }

        const double b0 = std::floor (xToBeat (0)), b1 = xToBeat ((float) getWidth()) + 1;
        const float pxPerBeat = beatToX (1.0) - beatToX (0.0);
        g.setFont (theme::font (10.5f, true));
        for (double b = juce::jmax (0.0, b0); b <= b1; b += 1.0)
        {
            const float x = beatToX (b);
            const bool bar = ((int) b) % bpb == 0;
            if (bar)
            {
                g.setColour (theme::gridBar);
                g.drawVerticalLine ((int) x, 4.0f, (float) getHeight());
                const int barNum = (int) b / bpb + 1;
                if (pxPerBeat * bpb > 28 || barNum % 4 == 1)
                {
                    g.setColour (theme::textDim);
                    g.drawText (juce::String (barNum), (int) x + 3, 0, 40, getHeight(), juce::Justification::centredLeft);
                }
            }
            else if (pxPerBeat > 10)
            {
                g.setColour (theme::grid);
                g.drawVerticalLine ((int) x, (float) getHeight() - 5, (float) getHeight());
            }
        }

        const float px = beatToX (ctx.engine.getPositionBeats());
        juce::Path tri;
        tri.addTriangle (px - 5, 2, px + 5, 2, px, (float) getHeight() - 2);
        g.setColour (ctx.engine.isRecording() ? theme::record : theme::accent);
        g.fillPath (tri);
        g.setColour (theme::bg);
        g.drawHorizontalLine (getHeight() - 1, 0, (float) getWidth());
    }

    void mouseDown (const juce::MouseEvent& e) override { move (e); }
    void mouseDrag (const juce::MouseEvent& e) override { move (e); }

private:
    AppContext& ctx;
    double lastPos = -1;

    void move (const juce::MouseEvent& e)
    {
        if (! xToBeat) return;
        ctx.engine.setPositionBeats (juce::jmax (0.0, xToBeat ((float) e.x)));
        if (auto* p = getParentComponent()) p->repaint();
    }

    void timerCallback() override
    {
        const double pos = ctx.engine.getPositionBeats();
        if (std::abs (pos - lastPos) > 1.0e-6) { lastPos = pos; repaint(); }
    }
};

// The editors' "play this clip" button
inline void setUpPlayClipButton (IconButton& b)
{
    b.setTooltip ("Play just this clip (press again to stop)");
    b.onColour = theme::accent2;
}

} // namespace rs
