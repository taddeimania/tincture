// OscMapping.cpp — the OSC MAPPING editor (keyboard bar): note and velocity
// ranges for SUB, OSC A-C, NOISE and ARP, with FOLD and WARP per row.
#include "Pages.h"
#include "SeqWidgets.h"

namespace tincture::ui
{
using namespace juce;
using namespace seq;

namespace
{
const char* kRowNames[] { "SUB", "OSC A", "OSC B", "OSC C", "NOISE", "ARP" };
constexpr int kRows = KeyboardSettings::kNumRows;
const Colour kBar { 0xff6fd08a };

class RangeArea final : public Component, public Refreshable, public SettableTooltipClient
{
public:
    explicit RangeArea (PanelContext& c) : ctx (c)
    {
        ctx.add (this);
        setTooltip ("Drag an edge to resize, the bar to move; double-click to type (e.g. C1 - B1)");
    }
    ~RangeArea() override { ctx.remove (this); }

    bool velocityView = false;
    static constexpr int kRowH = 38, kKeysH = 26;

    int lo() const { return velocityView ? 1 : 0; }
    int count() const { return velocityView ? 127 : 128; }
    float cellW() const { return (float) getWidth() / (float) count(); }
    float xOf (int v) const { return (float) (v - lo()) * cellW(); }
    int valueAt (float x) const { return jlimit (lo(), lo() + count() - 1, lo() + (int) std::floor (x / cellW())); }
    Rectangle<float> rowRect (int r) const { return { 0.0f, (float) (r * kRowH), (float) getWidth(), (float) kRowH }; }

    std::pair<int, int> range (const NoteRange& n) const { return velocityView ? std::pair { n.velLo, n.velHi } : std::pair { n.keyLo, n.keyHi }; }

    Rectangle<float> barRect (int r) const
    {
        const auto [a, b] = range (ctx.proc.patch().keyboard.mapping[(size_t) r]);
        const auto rr = rowRect (r).reduced (0.0f, 9.0f);
        return { xOf (a), rr.getY(), xOf (b + 1) - xOf (a), rr.getHeight() };
    }

    String label (int v) const { return velocityView ? String (v) : noteName (v); }

    void refresh() override
    {
        const auto& m = ctx.proc.patch().keyboard.mapping;
        if (m != shown) { shown = m; repaint(); }
    }

    void paint (Graphics& g) override
    {
        shown = ctx.proc.patch().keyboard.mapping;
        const float w = (float) getWidth();
        for (int r = 0; r < kRows; ++r)
        {
            const auto rr = rowRect (r);
            g.setColour (r % 2 ? colours::black.withAlpha (0.35f) : colours::black.withAlpha (0.55f));
            g.fillRect (rr);
            // Octave / tick lines.
            g.setColour (colours::grid);
            if (velocityView) { for (int v = 8; v < 128; v += 8) g.fillRect (xOf (v), rr.getY(), 1.0f, rr.getHeight()); }
            else for (int n = 12; n < 128; n += 12) g.fillRect (xOf (n), rr.getY(), 1.0f, rr.getHeight());
            const auto b = barRect (r);
            const bool hot = r == dragRow;
            g.setColour (kBar.withAlpha (hot ? 0.95f : 0.75f));
            g.fillRect (b);
            g.setColour (kBar.brighter (0.4f));
            g.drawRect (b, 1.0f);
            g.setColour (colours::black);
            g.fillRect (b.getX() + 2.0f, b.getCentreY() - 5.0f, 2.0f, 10.0f);
            g.fillRect (b.getRight() - 4.0f, b.getCentreY() - 5.0f, 2.0f, 10.0f);
            const auto [a, z] = range (shown[(size_t) r]);
            const String txt = label (a) + " - " + label (z);
            if (b.getWidth() > 90.0f)
                drawText (g, txt, b, 11.5f, colours::black, true);
        }
        // The axis: a keyboard (KEY) or velocity ticks (VEL).
        const auto axis = Rectangle<float> (0.0f, (float) (kRows * kRowH + 6), w, (float) kKeysH);
        if (velocityView)
        {
            g.setColour (colours::black);
            g.fillRect (axis);
            for (int v : { 1, 8, 16, 24, 32, 40, 48, 56, 64, 72, 80, 88, 96, 104, 112, 120, 127 })
            {
                const float x = xOf (v) + cellW() * 0.5f;
                g.setColour (colours::inkDim);
                g.fillRect (x, axis.getY(), 1.0f, 6.0f);
                drawText (g, String (v), { x - 14.0f, axis.getY() + 7.0f, 28.0f, 16.0f }, 10.5f, colours::inkDim, true);
            }
        }
        else
        {
            for (int n = 0; n < 128; ++n)
            {
                const int k = n % 12;
                const bool black = k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
                const auto r = Rectangle<float> (xOf (n), axis.getY(), cellW(), axis.getHeight());
                g.setColour (black ? colours::black : colours::cream);
                g.fillRect (r);
                if (k == 0)
                {
                    g.setColour (colours::panelEdge);
                    g.fillRect (r.getX(), r.getY(), 1.0f, r.getHeight());
                }
            }
            for (int n = 0; n < 128; n += 12)
            {
                const auto lr = Rectangle<float> (xOf (n) + 2.0f, axis.getBottom() + 1.0f, 40.0f, 13.0f);
                drawText (g, noteName (n), lr, 10.5f, colours::inkDim, true, Justification::centredLeft);
            }
        }
        if (dragRow >= 0)
        {
            // Highlight the dragged range on the axis.
            const auto [a, z] = range (shown[(size_t) dragRow]);
            g.setColour (kBar.withAlpha (0.45f));
            g.fillRect (Rectangle<float> (xOf (a), axis.getY(), xOf (z + 1) - xOf (a), axis.getHeight()));
        }
    }

    void mouseMove (const MouseEvent& e) override
    {
        const int r = (int) e.position.y / kRowH;
        if (r < 0 || r >= kRows) { setMouseCursor (MouseCursor::NormalCursor); return; }
        const auto b = barRect (r);
        if (std::abs (e.position.x - b.getX()) < 6.0f || std::abs (e.position.x - b.getRight()) < 6.0f) setMouseCursor (MouseCursor::LeftRightResizeCursor);
        else if (b.contains (e.position)) setMouseCursor (MouseCursor::DraggingHandCursor);
        else setMouseCursor (MouseCursor::NormalCursor);
    }

    void mouseDown (const MouseEvent& e) override
    {
        dragRow = -1;
        const int r = (int) e.position.y / kRowH;
        if (r < 0 || r >= kRows || e.mods.isPopupMenu()) return;
        const auto b = barRect (r);
        if (std::abs (e.position.x - b.getX()) < 6.0f) mode = 1;
        else if (std::abs (e.position.x - b.getRight()) < 6.0f) mode = 2;
        else if (b.contains (e.position)) mode = 3;
        else return;
        dragRow = r;
        start = range (ctx.proc.patch().keyboard.mapping[(size_t) r]);
        startCell = valueAt (e.position.x);
        beginDrag();
        repaint();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (dragRow < 0) return;
        const int v = valueAt (e.position.x);
        int a = start.first, z = start.second;
        if (mode == 1) a = jmin (v, z);
        else if (mode == 2) z = jmax (v, a);
        else
        {
            const int d = jlimit (lo() - a, lo() + count() - 1 - z, v - startCell);
            a += d; z += d;
        }
        apply (dragRow, a, z, "Edit Note Range");
        if (onReadout) onReadout (String (kRowNames[dragRow]) + ": " + label (a) + " - " + label (z));
    }

    void mouseUp (const MouseEvent&) override
    {
        if (dragRow < 0) return;
        dragRow = -1;
        endDrag (ctx);
        repaint();
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        const int r = (int) e.position.y / kRowH;
        if (r < 0 || r >= kRows || ! barRect (r).contains (e.position) || ! ctx.editValue) return;
        const auto [a, z] = range (ctx.proc.patch().keyboard.mapping[(size_t) r]);
        SafePointer<RangeArea> safe (this);
        ctx.editValue (*this, label (a) + " - " + label (z), [safe, r] (const String& t)
        {
            if (safe == nullptr) return;
            // "C-1 - B1": note names contain minus signs, so split on spaces.
            StringArray halves = StringArray::fromTokens (t, " ", "");
            halves.removeString ("-");
            halves.removeEmptyStrings();
            if (halves.size() != 2) return;
            auto parse = [&] (const String& s) { return safe->velocityView ? (s.containsOnly ("0123456789") ? s.getIntValue() : -1) : parseNoteName (s); };
            const int a2 = parse (halves[0]), z2 = parse (halves[1]);
            if (a2 < 0 || z2 < 0) return;
            safe->apply (r, jmin (a2, z2), jmax (a2, z2), "Edit Note Range");
        });
    }

    void apply (int row, int a, int z, const String& label)
    {
        const bool vel = velocityView;
        a = jlimit (lo(), 127, a);
        z = jlimit (a, 127, z);
        seq::edit (ctx, label, [row, a, z, vel] (Patch& p)
        {
            auto& n = p.keyboard.mapping[(size_t) row];
            if (vel) { n.velLo = a; n.velHi = z; } else { n.keyLo = a; n.keyHi = z; }
        });
        refresh();
    }

    std::function<void (const String&)> onReadout;

private:
    PanelContext& ctx;
    std::array<NoteRange, kRows> shown {};
    int dragRow = -1, mode = 0, startCell = 0;
    std::pair<int, int> start;
};

class OscMappingEditor final : public Component
{
public:
    OscMappingEditor (PanelContext& c, std::function<void()> close) : ctx (c), area (c), closeButton ("X"),
        keyView (c, "KEY", true), velView (c, "VEL", true)
    {
        closeButton.setTooltip ("Close OSC MAPPING");
        closeButton.onClick = [close] (const ModifierKeys&) { if (close) close(); };
        addAndMakeVisible (closeButton);
        addAndMakeVisible (area);
        area.velocityView = ctx.proc.uiState.getProperty ("oscMapVel", false);
        area.onReadout = [this] (const String& s) { readout = s; repaint (readoutBounds()); };
        for (int r = 0; r < kRows; ++r)
        {
            auto fold = std::make_unique<Check> (ctx, "");
            fold->accent = kBar;
            fold->setTooltip ("FOLD: notes outside the range are octave-folded into it");
            fold->ticked = [this, r] { return ctx.proc.patch().keyboard.mapping[(size_t) r].fold; };
            fold->onToggle = [this, r] { ctx.proc.editPatch ("Mapping Fold", [r] (Patch& p) { auto& n = p.keyboard.mapping[(size_t) r]; n.fold = ! n.fold; }); };
            addAndMakeVisible (*fold);
            folds.push_back (std::move (fold));
            if (r == KeyboardSettings::Arp) continue;
            auto warp = std::make_unique<Check> (ctx, "");
            warp->accent = kBar;
            warp->setTooltip ("WARP: keep the oscillator running outside its range as a warp/FM source");
            warp->ticked = [this, r] { return ctx.proc.patch().keyboard.mapping[(size_t) r].warp; };
            warp->onToggle = [this, r] { ctx.proc.editPatch ("Mapping Warp", [r] (Patch& p) { auto& n = p.keyboard.mapping[(size_t) r]; n.warp = ! n.warp; }); };
            addAndMakeVisible (*warp);
            warps.push_back (std::move (warp));
        }
        for (auto* b : { &keyView, &velView })
        {
            b->accent = colours::cream;
            addAndMakeVisible (*b);
        }
        keyView.ticked = [this] { return ! area.velocityView; };
        velView.ticked = [this] { return area.velocityView; };
        keyView.onToggle = [this] { setVelocityView (false); };
        velView.onToggle = [this] { setVelocityView (true); };
    }

    void setVelocityView (bool v)
    {
        area.velocityView = v;
        ctx.proc.uiState.setProperty ("oscMapVel", v, nullptr);
        area.repaint();
        keyView.refresh();
        velView.refresh();
    }

    Rectangle<int> readoutBounds() const { return { 400, getHeight() - 52, 400, 30 }; }

    void resized() override
    {
        closeButton.setBounds (getWidth() - 40, 6, 30, 22);
        area.setBounds (220, 52, getWidth() - 240, kRows * RangeArea::kRowH + 6 + RangeArea::kKeysH + 15);
        int wi = 0;
        for (int r = 0; r < kRows; ++r)
        {
            const int y = 52 + r * RangeArea::kRowH + (RangeArea::kRowH - 16) / 2;
            folds[(size_t) r]->setBounds (118, y, 16, 16);
            if (r != KeyboardSettings::Arp) warps[(size_t) wi++]->setBounds (168, y, 16, 16);
        }
        keyView.setBounds (40, getHeight() - 46, 54, 24);
        velView.setBounds (96, getHeight() - 46, 54, 24);
    }

    void paint (Graphics& g) override
    {
        drawEditorBackground (g, getLocalBounds(), "OSC MAPPING", kBar);
        drawText (g, "FOLD", { 100.0f, 32.0f, 52.0f, 16.0f }, 11.0f, colours::inkDim, true);
        drawText (g, "WARP", { 150.0f, 32.0f, 52.0f, 16.0f }, 11.0f, colours::inkDim, true);
        drawText (g, area.velocityView ? "VELOCITY RANGE" : "KEY RANGE", { 220.0f, 32.0f, 200.0f, 16.0f }, 11.0f, colours::inkDim, true, Justification::centredLeft);
        for (int r = 0; r < kRows; ++r)
            drawText (g, kRowNames[r], { 20.0f, (float) (52 + r * RangeArea::kRowH), 80.0f, (float) RangeArea::kRowH }, 13.0f, colours::ink, true, Justification::centredLeft);
        drawSection (g, { 20.0f, (float) getHeight() - 62.0f, 150.0f, 50.0f }, "MAPPING");
        drawText (g, readout, readoutBounds().toFloat(), 13.0f, colours::lcd, true, Justification::centredLeft);
    }

private:
    PanelContext& ctx;
    RangeArea area;
    PanelButton closeButton;
    Check keyView, velView;
    std::vector<std::unique_ptr<Check>> folds, warps;
    String readout;
};
} // namespace

std::unique_ptr<Component> createOscMappingEditor (PanelContext& ctx, std::function<void()> onClose)
{
    return std::make_unique<OscMappingEditor> (ctx, std::move (onClose));
}
} // namespace tincture::ui
