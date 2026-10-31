// ArpEditor.cpp — the ARP module (spec E 1): GLOBAL, PATTERN, TRANSPOSE,
// PLAYBACK, RETRIGGER and VELOCITY panes over the selected slot, the Arp
// Pattern Editor (SHAPE = Pattern) or a preview of the note order, the 12
// arp slots and the MIDI OUT box. Arp settings are patch data (ArpBank).
#include "SeqWidgets.h"

#include <map>

namespace tincture::ui
{
using namespace juce;
using namespace seq;

namespace
{
const Colour kStep { 0xff4fc4c0 };

const ArpBank& bankOf (PanelContext& c) { return c.proc.patch().arp; }
const ArpSlot& slotOf (PanelContext& c) { const auto& b = bankOf (c); return b.slots[(size_t) b.selected]; }
String slotTitle (PanelContext& c, const String& what) { return "Arp " + String (bankOf (c).selected + 1) + " " + what; }

std::map<const void*, ArpSlot>& slotClipboard() { static std::map<const void*, ArpSlot> m; return m; }

std::vector<uint8_t> signature (const ArpBank& b)
{
    ArpBank c = b;
    c.selected = 0; c.editAll = false; c.lockModule = false; c.showNames = true; c.triggerOctave = 0;
    Node n;
    c.save (n);
    return n.toBinary();
}

// ---------------------------------------------------------------------------
// Factory content (generated).
// ---------------------------------------------------------------------------
std::vector<std::pair<String, ArpBank>> factoryBanks()
{
    std::vector<std::pair<String, ArpBank>> out;
    {
        ArpBank b; b.init(); b.name = "Basic Shapes";
        const ArpShape shapes[] { ArpShape::Up, ArpShape::Down, ArpShape::UpDown, ArpShape::DownUp, ArpShape::UpPlusDown, ArpShape::Converge,
                                  ArpShape::Diverge, ArpShape::ThumbUp, ArpShape::PinkyUp, ArpShape::AsPlayed, ArpShape::Random, ArpShape::Pattern };
        for (int i = 0; i < kNumSeqSlots; ++i) b.slots[(size_t) i].shape = shapes[i];
        out.push_back ({ "Basic Shapes", b });
    }
    out.push_back ({ "Init", [] { ArpBank b; b.init(); return b; }() });
    {
        ArpBank b; b.init(); b.name = "Octave Runs";
        for (int i = 0; i < kNumSeqSlots; ++i)
        {
            auto& s = b.slots[(size_t) i];
            s.shape = i % 2 ? ArpShape::UpDown : ArpShape::Up;
            s.range = 1 + i / 4;
            s.shift = 12;
            s.rangeShape = i % 3;
            s.rate = i < 6 ? 7 : 8;
            s.gate = 0.6f;
        }
        out.push_back ({ "Octave Runs", b });
    }
    return out;
}

std::vector<std::pair<String, ArpPattern>> factoryPatterns()
{
    std::vector<std::pair<String, ArpPattern>> out;
    auto make = [] (const char* name, std::initializer_list<int> rows, double len)
    {
        ArpPattern p;
        p.name = name;
        p.steps.clear();
        int i = 0;
        for (int r : rows)
        {
            if (r != 0) { ArpPatternStep s; s.start = i * len; s.length = len; s.row = r; p.steps.push_back (s); }
            ++i;
        }
        p.length = std::max (len, (double) i * len);
        return p;
    };
    out.push_back ({ "3 Steps Up", make ("3 Steps Up", { 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 1, 2, 3, 4 }, 0.25) });
    out.push_back ({ "Bounce", make ("Bounce", { 1, 3, 2, 4, 1, 3, 2, 4 }, 0.5) });
    out.push_back ({ "Init", [] { auto p = ArpPattern::makeInit(); p.name = "Init"; return p; }() });
    out.push_back ({ "Mild Salsa", make ("Mild Salsa", { 1, 0, 0, 3, 0, 0, 2, 0, 0, 0, 4, 0, 3, 0, 0, 0 }, 0.25) });
    return out;
}

// ---------------------------------------------------------------------------
// The 12 slots.
// ---------------------------------------------------------------------------
class SlotRow final : public Component, public Refreshable
{
public:
    explicit SlotRow (PanelContext& c) : ctx (c) { ctx.add (this); }
    ~SlotRow() override { ctx.remove (this); }

    Rectangle<float> slotRect (int i) const
    {
        const float gap = 6.0f, w = ((float) getWidth() - gap * 11.0f) / 12.0f;
        return { (w + gap) * (float) i, 0.0f, w, (float) getHeight() };
    }
    Rectangle<float> playBar (int i) const { return slotRect (i).removeFromBottom (24.0f); }
    int slotAt (Point<float> p) const { for (int i = 0; i < 12; ++i) if (slotRect (i).contains (p)) return i; return -1; }

    void refresh() override
    {
        const auto& t = ctx.proc.seqTelemetry();
        const int playing = t.arpSlot.load(), step = t.arpStep.load();
        const auto& b = bankOf (ctx);
        String names;
        for (auto& s : b.slots) names << s.displayName() << "|";
        if (playing != shownPlaying || (playing >= 0 && step != shownStep) || b.selected != shownSelected || names != shownNames || b.showNames != shownShow)
        {
            shownPlaying = playing; shownStep = step; shownSelected = b.selected; shownNames = names; shownShow = b.showNames;
            repaint();
        }
    }

    void paint (Graphics& g) override
    {
        const auto& b = bankOf (ctx);
        for (int i = 0; i < 12; ++i)
        {
            const auto r = slotRect (i);
            const bool sel = b.selected == i, playing = shownPlaying == i;
            const auto& s = b.slots[(size_t) i];
            const int k = i % 12;
            const bool blackKey = k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
            g.setColour (blackKey ? colours::black : colours::black.brighter (0.08f));
            g.fillRect (r);
            g.setColour (sel ? arpColour : colours::inkDim);
            g.drawRect (r, sel ? 2.0f : 1.0f);
            auto head = r.withHeight (18.0f).reduced (1.0f);
            if (sel) { g.setColour (arpColour); g.fillRect (head); }
            drawText (g, String (i + 1), head.reduced (5.0f, 0.0f), 11.5f, sel ? colours::black : colours::ink, true, Justification::centredLeft);
            if (s.shape == ArpShape::Pattern)
                drawText (g, "PAT", head.reduced (5.0f, 0.0f), 10.5f, sel ? colours::black : arpColour, true, Justification::centredRight);
            auto body = r.withTrimmedTop (20.0f).withTrimmedBottom (26.0f).reduced (5.0f, 2.0f);
            if (b.showNames)
            {
                g.setColour (colours::ink);
                g.setFont (panelFont (11.5f));
                g.drawFittedText (String (s.displayName()), body.toNearestInt(), Justification::centredTop, 3);
            }
            else
            {
                drawText (g, String (arpchoices::shapes()[(size_t) s.shape]), body.removeFromTop (16.0f), 11.5f, colours::ink, true);
                drawText (g, String (s.rateLabel()), body.removeFromTop (16.0f), 11.0f, colours::inkDim, true);
            }
            // Play bar.
            auto pb = playBar (i).reduced (3.0f);
            g.setColour (playing ? arpColour : (sel ? arpColour.withAlpha (0.25f) : colours::panel));
            g.fillRect (pb);
            g.setColour (sel || playing ? arpColour : colours::inkDim);
            g.drawRect (pb, 1.0f);
            const auto icon = pb.withSizeKeepingCentre (14.0f, 14.0f).withX (pb.getX() + 6.0f);
            if (playing)
            {
                g.setColour (colours::black);
                g.fillRect (icon.reduced (2.5f));
                // Step dots.
                const int dots = 8;
                for (int d = 0; d < dots; ++d)
                {
                    const auto c = Point<float> (pb.getX() + 30.0f + (float) d * (pb.getWidth() - 40.0f) / (float) (dots - 1), pb.getCentreY());
                    g.setColour (d == shownStep % dots ? colours::black : colours::black.withAlpha (0.3f));
                    g.fillEllipse (Rectangle<float> (5.0f, 5.0f).withCentre (c));
                }
            }
            else
                drawArrow (g, icon, 1, sel ? colours::ink : colours::inkDim);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const int i = slotAt (e.position);
        if (i < 0) return;
        if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), slotMenu (i)); return; }
        ctx.proc.editPatch ({}, [i] (Patch& p) { p.arp.selected = i; });
        if (playBar (i).contains (e.position))
        {
            if (ctx.proc.seqTelemetry().arpSlot.load() == i) ctx.proc.post ({ SeqCommand::ArpStop, (int8_t) i });
            else ctx.proc.post ({ SeqCommand::ArpLaunch, (int8_t) i });
        }
        refresh();
        if (onSelect) onSelect();
    }

    Menu slotMenu (int i)
    {
        auto& proc = ctx.proc;
        Menu m;
        SafePointer<SlotRow> safe (this);
        m.add ("Rename Pattern", [safe, i] { if (safe != nullptr) safe->rename (i); });
        m.add ("Copy Pattern", [&proc, i] { slotClipboard()[&proc] = proc.patch().arp.copySlot (i); });
        const bool has = slotClipboard().count (&proc) > 0;
        m.add ("Paste Pattern", [&proc, i]
        {
            auto it = slotClipboard().find (&proc);
            if (it == slotClipboard().end()) return;
            const ArpSlot s = it->second;
            proc.editPatch ("Paste Pattern", [i, s] (Patch& p) { p.arp.pasteSlot (i, s); });
        }, false, has);
        m.add ("Erase Pattern", [&proc, i] { proc.editPatch ("Erase Pattern", [i] (Patch& p) { p.arp.eraseSlot (i); }); });
        m.toggle ("Show Pattern Names", bankOf (ctx).showNames, [&proc] { proc.editPatch ("Show Pattern Names", [] (Patch& p) { p.arp.showNames = ! p.arp.showNames; }); });
        return m;
    }

    void rename (int i)
    {
        if (! ctx.editValue) return;
        auto& proc = ctx.proc;
        // The text box goes over the slot header.
        renameTarget.setBounds (slotRect (i).withHeight (22.0f).toNearestInt());
        addChildComponent (renameTarget);
        ctx.editValue (renameTarget, String (bankOf (ctx).slots[(size_t) i].displayName()), [&proc, i] (const String& t)
        {
            const auto name = t.trim().toStdString();
            proc.editPatch ("Rename Pattern", [i, name] (Patch& p) { p.arp.renameSlot (i, name); });
        });
    }

    std::function<void()> onSelect;

private:
    PanelContext& ctx;
    Component renameTarget;
    int shownPlaying = -2, shownStep = -1, shownSelected = -1;
    bool shownShow = true;
    String shownNames;
};

// ---------------------------------------------------------------------------
// Note-order preview (any SHAPE but Pattern): four held notes C E G B.
// ---------------------------------------------------------------------------
class OrderPreview final : public Component, public Refreshable
{
public:
    explicit OrderPreview (PanelContext& c) : ctx (c) { ctx.add (this); }
    ~OrderPreview() override { ctx.remove (this); }

    void refresh() override
    {
        Node n;
        slotOf (ctx).save (n);
        auto sig = n.toBinary();
        if (sig != shown) { shown = std::move (sig); repaint(); }
    }

    void paint (Graphics& g) override
    {
        const auto& s = slotOf (ctx);
        auto r = getLocalBounds().toFloat();
        drawLcdBox (g, r);
        const int held = 4;
        const int heldNotes[held] { 48, 52, 55, 59 };
        int8_t order[256];
        int len = buildArpOrder (s.shape, held, order, 256);
        if (len <= 0) { order[0] = 0; len = 1; }
        if (s.shape == ArpShape::Random || s.shape == ArpShape::RandNoDup || s.shape == ArpShape::RandDrift || s.shape == ArpShape::RandOnce)
        {
            uint32_t x = 0x9e3779b9u;
            for (int i = 0; i < len; ++i) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; order[i] = (int8_t) (x % held); }
        }
        int8_t kOrder[64];
        int kLen = s.range > 0 ? buildArpOrder (rangeShapeAsShape (s.rangeShape), s.range + 1, kOrder, 64) : 0;
        if (kLen <= 0) { kOrder[0] = 0; kLen = 1; }
        // Sequence of chords (pitches) per step.
        std::vector<std::vector<int>> steps;
        for (int c = 0; c < kLen && (int) steps.size() < 32; ++c)
        {
            const int k = kOrder[c];
            for (int i = 0; i < len && (int) steps.size() < 32; ++i)
            {
                std::vector<int> chord;
                auto addK = [&] (int note) { if (k < 0) for (int kk = 0; kk <= s.range; ++kk) chord.push_back (note + kk * s.shift); else chord.push_back (note + k * s.shift); };
                const int idx = order[(i + s.offset % len + len) % len];
                if (idx < 0) for (int h = 0; h < held; ++h) addK (heldNotes[h]);
                else addK (heldNotes[jlimit (0, held - 1, (int) idx)]);
                steps.push_back (chord);
            }
            if (s.range == 0) break;
        }
        const size_t cycle = steps.size();
        for (size_t i = 0; cycle > 0 && steps.size() < 32; ++i) { auto copy = steps[i % cycle]; steps.push_back (std::move (copy)); }
        int lo = 127, hi = 0;
        for (auto& st : steps) for (int p : st) { lo = jmin (lo, p); hi = jmax (hi, p); }
        if (lo > hi) { lo = 48; hi = 60; }
        lo -= 2; hi += 2;
        auto plot = r.reduced (12.0f, 10.0f).withTrimmedTop (16.0f);
        drawText (g, "NOTE ORDER  (holding C3 E3 G3 B3)", r.reduced (12.0f, 6.0f).removeFromTop (14.0f), 11.0f, colours::inkDim, true, Justification::centredLeft);
        drawText (g, String (arpchoices::shapes()[(size_t) s.shape]) + (s.range > 0 ? "  /  RANGE " + String (s.range) + " x " + String (s.shift) + " st" : String()),
                  r.reduced (12.0f, 6.0f).removeFromTop (14.0f), 11.0f, arpColour, true, Justification::centredRight);
        const float cw = plot.getWidth() / 32.0f;
        const float rh = plot.getHeight() / (float) (hi - lo + 1);
        g.setColour (colours::grid);
        for (int i = 0; i <= 32; i += 4) g.fillRect (plot.getX() + cw * (float) i, plot.getY(), 1.0f, plot.getHeight());
        const float gate = jlimit (0.1f, 1.0f, s.gate);
        for (int i = 0; i < (int) steps.size() && i < 32; ++i)
            for (int p : steps[(size_t) i])
            {
                const auto b = Rectangle<float> (plot.getX() + cw * (float) i + 1.0f, plot.getBottom() - rh * (float) (p - lo + 1), jmax (2.0f, cw * gate - 2.0f), jmax (2.0f, rh - 1.0f));
                g.setColour (i < (int) cycle ? kStep : kStep.withAlpha (0.45f));   // repeats dimmed
                g.fillRect (b);
            }
    }

private:
    PanelContext& ctx;
    std::vector<uint8_t> shown;
};

// ---------------------------------------------------------------------------
// Arp Pattern Editor graph.
// ---------------------------------------------------------------------------
class PatternGraph final : public Component, public Refreshable, public SettableTooltipClient
{
public:
    explicit PatternGraph (PanelContext& c) : ctx (c)
    {
        ctx.add (this);
        setWantsKeyboardFocus (true);
        setTooltip ("Double-click to add or delete a step; drag to move, drag the right edge to resize; lanes: Accent (double-click), Velocity (drag)");
    }
    ~PatternGraph() override { ctx.remove (this); }

    static constexpr int kRulerH = 18, kAccentH = 14, kVelH = 44, kRows = 9;
    double grid = 0.25;
    int topRow = 6;

    const ArpPattern& pat() const { return slotOf (ctx).pattern; }
    Rectangle<float> gridArea() const { return { 34.0f, (float) kRulerH, (float) getWidth() - 34.0f, (float) (getHeight() - kRulerH - kAccentH - kVelH - 4) }; }
    Rectangle<float> accentArea() const { const auto ga = gridArea(); return { ga.getX(), ga.getBottom() + 2.0f, ga.getWidth(), (float) kAccentH }; }
    Rectangle<float> velArea() const { const auto aa = accentArea(); return { aa.getX(), aa.getBottom() + 2.0f, aa.getWidth(), (float) kVelH }; }
    float rowH() const { return gridArea().getHeight() / (float) kRows; }
    double viewLen() const { return jmax (1.0, std::ceil (pat().length / 4.0) * 4.0); }
    float xOf (double beat) const { const auto ga = gridArea(); return ga.getX() + (float) (beat / viewLen()) * ga.getWidth(); }
    double beatAt (float x) const { const auto ga = gridArea(); return jlimit (0.0, viewLen(), (double) ((x - ga.getX()) / ga.getWidth()) * viewLen()); }
    float yOf (int row) const { return gridArea().getY() + (float) (topRow - row) * rowH(); }
    int rowAt (float y) const { return topRow - (int) std::floor ((y - gridArea().getY()) / rowH()); }
    double snap (double b) const { return std::floor (b / grid + 1.0e-6) * grid; }

    Rectangle<float> stepRect (const ArpPatternStep& s) const
    {
        return { xOf (s.start), yOf (s.row) + 1.0f, jmax (3.0f, xOf (s.start + s.length) - xOf (s.start) - 1.0f), rowH() - 2.0f };
    }
    int stepAt (Point<float> p) const
    {
        const auto& st = pat().steps;
        for (int i = (int) st.size() - 1; i >= 0; --i) if (stepRect (st[(size_t) i]).contains (p)) return i;
        return -1;
    }
    int findKey (const ArpPattern& p) const
    {
        for (int i = 0; i < (int) p.steps.size(); ++i)
            if (std::abs (p.steps[(size_t) i].start - keyStart) < 1.0e-6 && p.steps[(size_t) i].row == keyRow) return i;
        return -1;
    }

    void refresh() override
    {
        Node n;
        pat().save (n);
        auto sig = n.toBinary();
        if (sig != shown) { shown = std::move (sig); repaint(); }
    }

    void resized() override { background = {}; }

    void paint (Graphics& g) override
    {
        const auto& p = pat();
        const auto ga = gridArea();
        const auto key = std::make_tuple (viewLen(), grid, topRow, getWidth(), getHeight());
        if (background.isNull() || key != bgKey)
        {
            bgKey = key;
            background = Image (Image::ARGB, jmax (1, getWidth()), jmax (1, getHeight()), true);
            Graphics bg (background);
            bg.setColour (colours::black);
            bg.fillRect (getLocalBounds());
            for (int i = 0; i < kRows; ++i)
            {
                const int row = topRow - i;
                const auto rr = Rectangle<float> (ga.getX(), yOf (row), ga.getWidth(), rowH());
                bg.setColour (row >= 1 ? colours::panel.darker (0.1f) : colours::panel.darker (0.45f));
                bg.fillRect (rr.reduced (0.0f, 0.5f));
                drawText (bg, String (row), { 0.0f, rr.getY(), 30.0f, rr.getHeight() }, 11.0f, row >= 1 ? colours::ink : colours::inkDim, true, Justification::centredRight);
            }
            for (double b = 0.0; b <= viewLen() + 1.0e-6; b += grid)
            {
                const bool bar = std::fmod (b, 4.0) < 1.0e-6, beat = std::fmod (b, 1.0) < 1.0e-6;
                bg.setColour (bar ? colours::inkDim : (beat ? colours::inkFaint : colours::grid));
                bg.fillRect (xOf (b), ga.getY(), 1.0f, ga.getHeight() + kAccentH + kVelH + 4.0f);
                if (beat && b < viewLen() - 1.0e-6)
                {
                    const int barN = (int) (b / 4.0) + 1, beatN = (int) std::fmod (b, 4.0) + 1;
                    drawText (bg, beatN == 1 ? String (barN) : String (barN) + "." + String (beatN), { xOf (b) + 3.0f, 2.0f, 40.0f, 14.0f }, 10.5f, colours::inkDim, true, Justification::centredLeft);
                }
            }
            bg.setColour (colours::panel.darker (0.3f));
            bg.fillRect (accentArea());
            bg.fillRect (velArea());
            drawText (bg, "ACC", { 0.0f, accentArea().getY(), 31.0f, (float) kAccentH }, 9.5f, colours::inkDim, true, Justification::centredRight);
            drawText (bg, "VEL", { 0.0f, velArea().getY(), 31.0f, 14.0f }, 9.5f, colours::inkDim, true, Justification::centredRight);
        }
        g.drawImageAt (background, 0, 0);

        // Pattern end flag.
        const float ex = xOf (p.length);
        g.setColour (colours::black.withAlpha (0.45f));
        g.fillRect (Rectangle<float> (ex, ga.getY(), ga.getRight() - ex, ga.getHeight()));
        g.setColour (kStep.brighter (0.3f));
        g.fillRect (ex - 1.0f, 0.0f, 2.0f, ga.getBottom());
        Path flag;
        flag.addTriangle (ex, 2.0f, ex, 14.0f, ex - 9.0f, 8.0f);
        g.fillPath (flag);

        const int sel = findKey (p);
        for (int i = 0; i < (int) p.steps.size(); ++i)
        {
            const auto& s = p.steps[(size_t) i];
            if (s.row > topRow || s.row <= topRow - kRows) continue;
            const auto r = stepRect (s);
            const Colour c = s.muted ? colours::inkFaint : (i == sel ? orange : kStep);
            g.setColour (c);
            g.fillRect (r);
            g.setColour (c.darker (0.5f));
            g.drawRect (r, 1.0f);
            // Velocity bar inside the step.
            g.setColour (colours::black.withAlpha (0.45f));
            g.fillRect (r.getX() + 2.0f, r.getBottom() - 3.0f, (r.getWidth() - 4.0f) * jlimit (0.0f, 1.0f, s.velocity / 2.0f), 2.0f);
            if (r.getWidth() > 14.0f)
                drawText (g, String (s.row) + (s.octave != 0 ? (s.octave > 0 ? "+" : "") + String (s.octave * 12) : String()), r.reduced (3.0f, 0.0f), 10.5f, colours::black, true, Justification::centredLeft);
            if (s.accent)
            {
                const auto a = accentArea();
                g.setColour (orange);
                g.fillRect (Rectangle<float> (r.getX(), a.getY() + 3.0f, jmin (r.getWidth(), 10.0f), a.getHeight() - 6.0f));
            }
            const auto v = velArea();
            const float y = v.getBottom() - 2.0f - (v.getHeight() - 6.0f) * jlimit (0.0f, 1.0f, s.velocity / 2.0f);
            g.setColour (i == sel ? orange : kStep);
            g.fillRect (r.getX() + 3.0f, y, 1.5f, v.getBottom() - y);
            g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre ({ r.getX() + 3.75f, y }));
        }
        // 100 % line in the velocity lane.
        const auto v = velArea();
        g.setColour (colours::inkFaint);
        g.fillRect (v.getX(), v.getBottom() - 2.0f - (v.getHeight() - 6.0f) * 0.5f, v.getWidth(), 1.0f);
        g.setColour (colours::inkDim);
        g.drawRect (getLocalBounds().toFloat(), 1.0f);
    }

    void editPattern (const String& label, std::function<void (ArpPattern&)> fn)
    {
        seq::edit (ctx, label, [fn] (Patch& p) { auto& s = p.arp.slots[(size_t) p.arp.selected]; fn (s.pattern); s.pattern.sortSteps(); });
    }

    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();
        mode = None;
        const auto pos = e.position;
        if (velArea().contains (pos))
        {
            const int i = nearestStep (pos.x);
            if (i >= 0) { const auto& s = pat().steps[(size_t) i]; keyStart = s.start; keyRow = s.row; mode = Velocity; beginDrag(); mouseDrag (e); }
            return;
        }
        if (pos.y < kRulerH && std::abs (pos.x - xOf (pat().length)) < 10.0f) { mode = Length; beginDrag(); return; }
        if (! gridArea().contains (pos)) return;
        const int i = stepAt (pos);
        if (e.mods.isPopupMenu())
        {
            if (i >= 0) { keyStart = pat().steps[(size_t) i].start; keyRow = pat().steps[(size_t) i].row; repaint(); showContextMenu (*this, e.getPosition(), stepMenu()); }
            return;
        }
        if (i < 0) { keyRow = -999; repaint(); return; }
        const auto& s = pat().steps[(size_t) i];
        keyStart = s.start; keyRow = s.row;
        origStart = s.start; origRow = s.row; origLen = s.length;
        mode = pos.x > stepRect (s).getRight() - 5.0f ? Resize : Move;
        beginDrag();
        repaint();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        const auto pos = e.position;
        if (mode == Velocity)
        {
            const auto v = velArea();
            const float vel = jlimit (0.0f, 2.0f, 2.0f * (v.getBottom() - 2.0f - pos.y) / (v.getHeight() - 6.0f));
            const double ks = keyStart; const int kr = keyRow;
            editPattern ("Step Velocity", [ks, kr, vel] (ArpPattern& p) { for (auto& s : p.steps) if (std::abs (s.start - ks) < 1.0e-6 && s.row == kr) s.velocity = vel; });
        }
        else if (mode == Length)
        {
            const double len = jlimit (grid, 64.0, std::round (beatAt (pos.x) / grid) * grid);
            editPattern ("Pattern Length", [len] (ArpPattern& p) { p.length = len; });
        }
        else if (mode == Move)
        {
            const double dt = std::round ((beatAt (pos.x) - beatAt ((float) e.getMouseDownX())) / grid) * grid;
            const int dr = rowAt (pos.y) - rowAt ((float) e.getMouseDownY());
            const double ns = jlimit (0.0, jmax (0.0, pat().length - grid), origStart + dt);
            const int nr = jlimit (-12, 24, origRow + dr);
            if (std::abs (ns - keyStart) < 1.0e-9 && nr == keyRow) return;
            const double ks = keyStart; const int kr = keyRow;
            editPattern ("Move Step", [ks, kr, ns, nr] (ArpPattern& p) { for (auto& s : p.steps) if (std::abs (s.start - ks) < 1.0e-6 && s.row == kr) { s.start = ns; s.row = nr; break; } });
            keyStart = ns; keyRow = nr;
        }
        else if (mode == Resize)
        {
            const double len = jmax (grid, std::round ((beatAt (pos.x) - origStart) / grid) * grid);
            const double ks = keyStart; const int kr = keyRow;
            editPattern ("Resize Step", [ks, kr, len] (ArpPattern& p) { for (auto& s : p.steps) if (std::abs (s.start - ks) < 1.0e-6 && s.row == kr) s.length = len; });
        }
        refresh();
    }

    void mouseUp (const MouseEvent&) override
    {
        if (mode != None) endDrag (ctx);
        mode = None;
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        const auto pos = e.position;
        if (accentArea().contains (pos))
        {
            const double b = beatAt (pos.x);
            editPattern ("Accent", [b] (ArpPattern& p) { for (auto& s : p.steps) if (b >= s.start && b < s.start + s.length) { s.accent = ! s.accent; break; } });
            return;
        }
        if (! gridArea().contains (pos)) return;
        const int i = stepAt (pos);
        if (i >= 0)
        {
            const auto s = pat().steps[(size_t) i];
            editPattern ("Delete Step", [s] (ArpPattern& p)
            {
                p.steps.erase (std::remove_if (p.steps.begin(), p.steps.end(), [&] (const ArpPatternStep& x) { return std::abs (x.start - s.start) < 1.0e-6 && x.row == s.row; }), p.steps.end());
            });
            keyRow = -999;
            return;
        }
        ArpPatternStep s;
        s.start = snap (beatAt (pos.x));
        s.length = grid;
        s.row = rowAt (pos.y);
        if (s.start >= pat().length) return;
        keyStart = s.start; keyRow = s.row;
        editPattern ("Add Step", [s] (ArpPattern& p) { p.steps.push_back (s); });
    }

    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) override
    {
        topRow = jlimit (-6 + kRows - 1, 24, topRow + (w.deltaY > 0 ? 1 : -1));
        background = {};
        repaint();
    }

    bool keyPressed (const KeyPress& k) override
    {
        const int i = findKey (pat());
        if (i < 0) return false;
        const double ks = keyStart; const int kr = keyRow;
        auto onSel = [ks, kr] (ArpPattern& p, std::function<void (ArpPatternStep&)> f)
        { for (auto& s : p.steps) if (std::abs (s.start - ks) < 1.0e-6 && s.row == kr) { f (s); break; } };
        const int code = k.getKeyCode();
        if (code == KeyPress::deleteKey || code == KeyPress::backspaceKey)
        {
            editPattern ("Delete Step", [ks, kr] (ArpPattern& p) { p.steps.erase (std::remove_if (p.steps.begin(), p.steps.end(), [&] (const ArpPatternStep& x) { return std::abs (x.start - ks) < 1.0e-6 && x.row == kr; }), p.steps.end()); });
            keyRow = -999;
            return true;
        }
        if (code == KeyPress::upKey || code == KeyPress::downKey)
        {
            const int d = code == KeyPress::upKey ? 1 : -1;
            editPattern ("Move Step", [onSel, d] (ArpPattern& p) { onSel (p, [d] (ArpPatternStep& s) { s.row = jlimit (-12, 24, s.row + d); }); });
            keyRow += d;
            return true;
        }
        if (code == KeyPress::leftKey || code == KeyPress::rightKey)
        {
            const double d = code == KeyPress::rightKey ? grid : -grid;
            const double len = pat().length;
            editPattern ("Move Step", [onSel, d, len] (ArpPattern& p) { onSel (p, [d, len] (ArpPatternStep& s) { s.start = jlimit (0.0, len - s.length, s.start + d); }); });
            keyStart = jlimit (0.0, len - pat().steps[(size_t) i].length, keyStart + d);
            return true;
        }
        return false;
    }

    Menu stepMenu()
    {
        Menu m;
        SafePointer<PatternGraph> safe (this);
        const double ks = keyStart; const int kr = keyRow;
        auto onStep = [safe, ks, kr] (const String& label, std::function<void (ArpPatternStep&)> f)
        {
            if (safe != nullptr) safe->editPattern (label, [ks, kr, f] (ArpPattern& p) { for (auto& s : p.steps) if (std::abs (s.start - ks) < 1.0e-6 && s.row == kr) { f (s); break; } });
        };
        const int i = findKey (pat());
        const bool muted = i >= 0 && pat().steps[(size_t) i].muted, acc = i >= 0 && pat().steps[(size_t) i].accent, tie = i >= 0 && pat().steps[(size_t) i].tie;
        m.add ("Delete", [safe, ks, kr]
        {
            if (safe != nullptr) safe->editPattern ("Delete Step", [ks, kr] (ArpPattern& p) { p.steps.erase (std::remove_if (p.steps.begin(), p.steps.end(), [&] (const ArpPatternStep& x) { return std::abs (x.start - ks) < 1.0e-6 && x.row == kr; }), p.steps.end()); });
        }).withShortcut ("Backspace");
        m.separator();
        m.toggle ("Mute", muted, [onSel = onStep] { onSel ("Mute Step", [] (ArpPatternStep& s) { s.muted = ! s.muted; }); });
        m.toggle ("Accent", acc, [onSel = onStep] { onSel ("Accent", [] (ArpPatternStep& s) { s.accent = ! s.accent; }); });
        m.toggle ("Tie", tie, [onSel = onStep] { onSel ("Tie Step", [] (ArpPatternStep& s) { s.tie = ! s.tie; }); });
        Menu oct;
        for (int o = -2; o <= 2; ++o)
            oct.add (o == 0 ? String ("0") : (o > 0 ? "+" : "") + String (o), [onSel = onStep, o] { onSel ("Step Octave", [o] (ArpPatternStep& s) { s.octave = o; }); },
                     i >= 0 && pat().steps[(size_t) i].octave == o);
        m.sub ("Octave", oct);
        return m;
    }

private:
    int nearestStep (float x) const
    {
        int best = -1;
        float bd = 12.0f;
        const auto& st = pat().steps;
        for (int i = 0; i < (int) st.size(); ++i)
        {
            const float d = std::abs (xOf (st[(size_t) i].start) + 3.75f - x);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }

    enum Mode { None, Move, Resize, Velocity, Length };
    PanelContext& ctx;
    Mode mode = None;
    double keyStart = 0.0, origStart = 0.0, origLen = 0.0;
    int keyRow = -999, origRow = 0;
    std::vector<uint8_t> shown;
    Image background;
    std::tuple<double, double, int, int, int> bgKey;
};

// ---------------------------------------------------------------------------
class ArpEditor final : public Component, public Refreshable
{
public:
    ArpEditor (PanelContext& c, std::function<void()> close)
        : ctx (c), onClose (std::move (close)), closeBtn ("X"),
          bank (c, "BANK", 40), launchQuant (c, "LAUNCH QUANT", 92), editAll (c, "EDIT ALL", true),
          shape (c, "SHAPE", 44), patternEdit (c, "EDIT", true), bpm (c, "BPM", true), hz (c, "HZ", true), trip (c, "TRIP", true), dot (c, "DOT", true),
          rate (c, "RATE"),
          rangeShape (c), shift (c, "SHIFT"), range (c, "RANGE"),
          latch (c, "LATCH", true), thru (c, "THRU", true), offset (c, "OFFSET"), repeats (c, "REPEATS"), gate (c, "GATE"), chance (c, "CHANCE"),
          rtLaunch (c, "LAUNCH"), rtRate (c, "RATE"), rtRateField (c), rtNote (c, "NOTE"), rtFirst (c, "FIRST"),
          velOn (c, "VELOCITY"), velRetrig (c, "RETRIG"), decay (c, "DECAY"), target (c, "TARGET"),
          patName (c), patLength (c, "LENGTH:", 70), patMode (c, "MODE:", 70), patTime (c, "TIME:", 70), patStepMode (c, "STEP MODE:", 70),
          patWrap (c, "WRAP:", 70), patPitch (c, "PITCH:", 70), patRange (c, "RANGE:", 70), patGrid (c),
          graph (c), preview (c), slots (c), midiOut (c)
    {
        ctx.add (this);
        closeBtn.setTooltip ("Close the ARP editor");
        closeBtn.onClick = [this] (const ModifierKeys&) { if (onClose) onClose(); };
        addAndMakeVisible (closeBtn);

        // GLOBAL -------------------------------------------------------------
        bank.arrows = true;
        bank.text = [this] { return bankLabel(); };
        bank.menu = [this] { return bankMenu(); };
        bank.onStep = [this] (int d) { stepBank (d); };
        bank.setTooltip ("Arp bank: click for the bank menu, arrows step through banks");
        launchQuant.text = [this] { return String (seqchoices::launchQuants()[(size_t) bankOf (ctx).launchQuant]); };
        launchQuant.onStep = [this] (int d)
        {
            const int v = jlimit (0, (int) seqchoices::launchQuants().size() - 1, bankOf (ctx).launchQuant + d);
            seq::edit (ctx, "Launch Quant", [v] (Patch& p) { p.arp.launchQuant = v; });
        };
        launchQuant.menu = [this]
        {
            auto& proc = ctx.proc;
            return listMenu (seqchoices::launchQuants(), bankOf (ctx).launchQuant, [&proc] (int i) { proc.editPatch ("Launch Quant", [i] (Patch& p) { p.arp.launchQuant = i; }); });
        };
        launchQuant.setTooltip ("LAUNCH QUANT: grid on which a launched arp syncs to the host clock");
        editAll.accent = arpColour;
        editAll.ticked = [this] { return bankOf (ctx).editAll; };
        editAll.onToggle = [this] { ctx.proc.editPatch ("Edit All", [] (Patch& p) { p.arp.editAll = ! p.arp.editAll; }); };
        editAll.setTooltip ("EDIT ALL: edits apply to all 12 slots (hold Alt for a one-off)");

        // PATTERN ------------------------------------------------------------
        shape.arrows = true;
        shape.text = [this] { return String (arpchoices::shapes()[(size_t) slotOf (ctx).shape]); };
        shape.onStep = [this] (int d)
        {
            const int v = jlimit (0, kNumArpShapes - 1, (int) slotOf (ctx).shape + d);
            editSlot ("Arp Shape", [v] (ArpSlot& s) { s.shape = (ArpShape) v; });
        };
        shape.menu = [this] { return shapeMenu(); };
        shape.setTooltip ("SHAPE: the order the arp walks through the held notes");
        patternEdit.accent = arpColour;
        patternEdit.ticked = [this] { return patternOpen; };
        patternEdit.onToggle = [this] { patternOpen = ! patternOpen; ctx.proc.uiState.setProperty ("arpPatternEdit", patternOpen, nullptr); layoutMiddle(); };
        patternEdit.setTooltip ("Open the Arp Pattern Editor");
        patternOpen = ctx.proc.uiState.getProperty ("arpPatternEdit", true);
        for (auto* b : { &bpm, &hz, &trip, &dot }) b->accent = arpColour;
        bpm.ticked = [this] { return ! slotOf (ctx).hzMode; };
        hz.ticked = [this] { return slotOf (ctx).hzMode; };
        bpm.onToggle = [this] { editSlot ("Arp Rate Mode", [] (ArpSlot& s) { s.hzMode = false; }); };
        hz.onToggle = [this] { editSlot ("Arp Rate Mode", [] (ArpSlot& s) { s.hzMode = true; }); };
        auto notPattern = [this] { return slotOf (ctx).shape != ArpShape::Pattern; };
        auto syncActive = [this] { return slotOf (ctx).shape != ArpShape::Pattern && ! slotOf (ctx).hzMode; };
        bpm.active = notPattern; hz.active = notPattern;
        trip.active = syncActive; dot.active = syncActive;
        trip.ticked = [this] { return slotOf (ctx).triplet; };
        dot.ticked = [this] { return slotOf (ctx).dotted; };
        trip.onToggle = [this] { const bool v = ! slotOf (ctx).triplet; editSlot ("Arp Triplet", [v] (ArpSlot& s) { s.triplet = v; }); };
        dot.onToggle = [this] { const bool v = ! slotOf (ctx).dotted; editSlot ("Arp Dotted", [v] (ArpSlot& s) { s.dotted = v; }); };

        rate.get = [this]
        {
            const auto& s = slotOf (ctx);
            if (s.shape == ArpShape::Pattern) return (float) (std::log2 (s.patternRate) + 2.0) / 4.0f;
            if (s.hzMode) return (float) (std::log (s.rateHz / 0.1f) / std::log (500.0f));
            return (float) s.rate / 11.0f;
        };
        rate.set = [this] (float n)
        {
            const auto& s = slotOf (ctx);
            if (s.shape == ArpShape::Pattern)
            {
                const float r = std::pow (2.0f, std::round (n * 8.0f) / 2.0f - 2.0f);
                editSlot ("Arp Rate", [r] (ArpSlot& x) { x.patternRate = r; });
            }
            else if (s.hzMode)
            {
                const float hzv = 0.1f * std::pow (500.0f, n);
                editSlot ("Arp Rate", [hzv] (ArpSlot& x) { x.rateHz = hzv; });
            }
            else
            {
                const int i = jlimit (0, 11, roundToInt (n * 11.0f));
                editSlot ("Arp Rate", [i] (ArpSlot& x) { x.rate = i; });
            }
        };
        rate.format = [this] (float) { return String (slotOf (ctx).rateLabel()); };
        rate.defaultNorm = 7.0f / 11.0f;
        rate.contextMenu = [this] { return settingMenu (slotTitle (ctx, "Rate"), {}, [this] { editSlot ("Reset Rate", [] (ArpSlot& s) { s.rate = 7; s.rateHz = 8.0f; s.patternRate = 1.0f; }); }); };
        rate.parse = [this] (const String& t, float& n)
        {
            const auto& r = choices::syncRates();
            for (int i = 0; i < (int) r.size(); ++i) if (t.trim().equalsIgnoreCase (String (r[(size_t) i]))) { n = (float) i / 11.0f; return true; }
            if (slotOf (ctx).hzMode && t.getFloatValue() > 0.0f) { n = std::log (t.getFloatValue() / 0.1f) / std::log (500.0f); return true; }
            return false;
        };

        // TRANSPOSE ----------------------------------------------------------
        rangeShape.arrows = true;
        rangeShape.text = [this] { return String (arpchoices::rangeShapes()[(size_t) slotOf (ctx).rangeShape]); };
        rangeShape.onStep = [this] (int d)
        {
            const int v = jlimit (0, kNumRangeShapes - 1, slotOf (ctx).rangeShape + d);
            editSlot ("Range Shape", [v] (ArpSlot& s) { s.rangeShape = v; });
        };
        rangeShape.menu = [this] { return rangeShapeMenu(); };
        rangeShape.setTooltip ("Range shape: the order of the transpositions (also RANGE right-click > Shape)");
        setupInt (shift, "Shift", -24, 24, 0, [] (const ArpSlot& s) { return s.shift; }, [] (ArpSlot& s, int v) { s.shift = v; },
                  [] (int v) { return v == 0 ? String ("0") : (v > 0 ? "+" : "") + String (v); });
        shift.setBipolar (true);
        setupInt (range, "Range", 0, 8, 0, [] (const ArpSlot& s) { return s.range; }, [] (ArpSlot& s, int v) { s.range = v; }, [] (int v) { return String (v); });
        range.contextMenu = [this]
        {
            return settingMenu (slotTitle (ctx, "Range"), [this] (Menu& m) { m.sub ("Shape", rangeShapeMenu()); },
                                [this] { editSlot ("Reset Range", [] (ArpSlot& s) { s.range = 0; }); });
        };

        // PLAYBACK -----------------------------------------------------------
        for (auto* b : { &latch, &thru }) b->accent = arpColour;
        latch.ticked = [this] { return slotOf (ctx).latch; };
        latch.onToggle = [this] { const bool v = ! slotOf (ctx).latch; editSlot ("Arp Latch", [v] (ArpSlot& s) { s.latch = v; }); };
        latch.setTooltip ("LATCH: notes keep playing after the keys are released (CC64 also latches)");
        thru.ticked = [this] { return slotOf (ctx).thru; };
        thru.onToggle = [this] { const bool v = ! slotOf (ctx).thru; editSlot ("Arp Thru", [v] (ArpSlot& s) { s.thru = v; }); };
        thru.setTooltip ("THRU: incoming notes also play directly");
        setupInt (offset, "Offset", -16, 16, 0, [] (const ArpSlot& s) { return s.offset; }, [] (ArpSlot& s, int v) { s.offset = v; },
                  [] (int v) { return v == 0 ? String ("0") : (v > 0 ? "+" : "") + String (v); });
        offset.setBipolar (true);
        // REPEATS: 1..64 then inf (stored 0).
        setupInt (repeats, "Repeats", 0, 64, 64,
                  [] (const ArpSlot& s) { return s.repeats == 0 ? 64 : s.repeats - 1; },
                  [] (ArpSlot& s, int v) { s.repeats = v >= 64 ? 0 : v + 1; },
                  [] (int v) { return v >= 64 ? String ("inf") : String (v + 1); });
        setupPercent (gate, "Gate", 0.01f, 2.0f, 1.0f, [] (const ArpSlot& s) { return s.gate; }, [] (ArpSlot& s, float v) { s.gate = v; });
        setupPercent (chance, "Chance", 0.0f, 1.0f, 1.0f, [] (const ArpSlot& s) { return s.chance; }, [] (ArpSlot& s, float v) { s.chance = v; });
        chance.contextMenu = [this]
        {
            return settingMenu (slotTitle (ctx, "Chance"), [this] (Menu& m)
            {
                m.toggle ("Pre", slotOf (ctx).chancePre, [this] { const bool v = ! slotOf (ctx).chancePre; editSlot ("Chance Pre", [v] (ArpSlot& s) { s.chancePre = v; }); });
            }, [this] { editSlot ("Reset Chance", [] (ArpSlot& s) { s.chance = 1.0f; }); });
        };

        // RETRIGGER ----------------------------------------------------------
        for (auto* b : { &rtLaunch, &rtRate, &rtNote, &rtFirst }) b->accent = arpColour;
        rtLaunch.ticked = [this] { return slotOf (ctx).retrigLaunch; };
        rtLaunch.onToggle = [this] { const bool v = ! slotOf (ctx).retrigLaunch; editSlot ("Retrigger Launch", [v] (ArpSlot& s) { s.retrigLaunch = v; }); };
        rtLaunch.setTooltip ("Restart the pattern when the slot is launched");
        rtRate.ticked = [this] { return slotOf (ctx).retrigRate; };
        rtRate.onToggle = [this] { const bool v = ! slotOf (ctx).retrigRate; editSlot ("Retrigger Rate", [v] (ArpSlot& s) { s.retrigRate = v; }); };
        rtRate.setTooltip ("Restart the pattern every <rate>");
        rtRateField.text = [this] { return String (choices::syncRates()[(size_t) jlimit (0, 11, slotOf (ctx).retrigRateIndex)]); };
        rtRateField.active = [this] { return slotOf (ctx).retrigRate; };
        rtRateField.onStep = [this] (int d)
        {
            const int v = jlimit (0, 8, slotOf (ctx).retrigRateIndex - d);   // up = longer
            editSlot ("Retrigger Rate", [v] (ArpSlot& s) { s.retrigRateIndex = v; });
        };
        rtRateField.menu = [this]
        {
            std::vector<std::string> l (choices::syncRates().begin(), choices::syncRates().begin() + 9);
            return listMenu (l, slotOf (ctx).retrigRateIndex, [this] (int i) { editSlot ("Retrigger Rate", [i] (ArpSlot& s) { s.retrigRateIndex = i; }); });
        };
        rtNote.ticked = [this] { return slotOf (ctx).retrigNote; };
        rtNote.onToggle = [this] { const bool v = ! slotOf (ctx).retrigNote; editSlot ("Retrigger Note", [v] (ArpSlot& s) { s.retrigNote = v; }); };
        rtNote.setTooltip ("Restart on any incoming note");
        rtFirst.ticked = [this] { return slotOf (ctx).retrigFirst; };
        rtFirst.active = [this] { return slotOf (ctx).retrigNote; };
        rtFirst.onToggle = [this] { const bool v = ! slotOf (ctx).retrigFirst; editSlot ("Retrigger First", [v] (ArpSlot& s) { s.retrigFirst = v; }); };
        rtFirst.setTooltip ("Only restart on the first note of a new chord");

        // VELOCITY -----------------------------------------------------------
        for (auto* b : { &velOn, &velRetrig }) b->accent = arpColour;
        velOn.fontH = 12.0f;
        velOn.ticked = [this] { return slotOf (ctx).velocityOn; };
        velOn.onToggle = [this] { const bool v = ! slotOf (ctx).velocityOn; editSlot ("Arp Velocity", [v] (ArpSlot& s) { s.velocityOn = v; }); };
        velOn.setTooltip ("VELOCITY: move output velocities toward TARGET each step");
        velRetrig.ticked = [this] { return slotOf (ctx).velocityRetrig; };
        velRetrig.active = [this] { return slotOf (ctx).velocityOn; };
        velRetrig.onToggle = [this] { const bool v = ! slotOf (ctx).velocityRetrig; editSlot ("Velocity Retrig", [v] (ArpSlot& s) { s.velocityRetrig = v; }); };
        velRetrig.setTooltip ("Reset the decay to the played velocities on every retrigger");
        setupPercent (decay, "Decay", 0.0f, 1.0f, 0.25f, [] (const ArpSlot& s) { return s.decay; }, [] (ArpSlot& s, float v) { s.decay = v; });
        setupInt (target, "Target", 0, 127, 0, [] (const ArpSlot& s) { return s.target; }, [] (ArpSlot& s, int v) { s.target = v; }, [] (int v) { return String (v); });
        decay.active = [this] { return slotOf (ctx).velocityOn; };
        target.active = [this] { return slotOf (ctx).velocityOn; };

        // Pattern settings ---------------------------------------------------
        patName.arrows = true;
        patName.text = [this] { return String (slotOf (ctx).pattern.name) + (patternModified() ? "*" : ""); };
        patName.menu = [this] { return patternMenu(); };
        patName.onStep = [this] (int d)
        {
            const auto f = factoryPatterns();
            int cur = -1;
            for (int i = 0; i < (int) f.size(); ++i) if (f[(size_t) i].first.toStdString() == slotOf (ctx).pattern.name) cur = i;
            const int next = jlimit (0, (int) f.size() - 1, cur < 0 ? 0 : cur + d);
            loadPattern (f[(size_t) next].second);
        };
        patLength.text = [this] { return formatLength (slotOf (ctx).pattern.length); };
        patLength.onStep = [this] (int d)
        {
            const double st = lengthStepFor (patLength.downFrac);
            const double v = jlimit (0.25, 64.0, slotOf (ctx).pattern.length + st * d);
            editPattern ("Pattern Length", [v] (ArpPattern& p) { p.length = v; });
        };
        patLength.onDoubleClick = [this]
        {
            if (! ctx.editValue) return;
            ctx.editValue (patLength, formatLength (slotOf (ctx).pattern.length), [this] (const String& t)
            {
                double b;
                if (parseLength (t, b)) editPattern ("Pattern Length", [b] (ArpPattern& p) { p.length = jlimit (0.25, 64.0, b); });
            });
        };
        patLength.setTooltip ("LENGTH: bars . beats . 16ths (drag each segment)");
        auto setupList = [this] (Field& f, const std::vector<std::string>& (*list)(), std::function<int (const ArpPattern&)> get,
                                 std::function<void (ArpPattern&, int)> set, const String& label)
        {
            f.dropArrow = true;
            f.text = [this, list, get] { return String (list()[(size_t) jlimit (0, (int) list().size() - 1, get (slotOf (ctx).pattern))]); };
            f.onStep = [this, list, get, set, label] (int d)
            {
                const int v = jlimit (0, (int) list().size() - 1, get (slotOf (ctx).pattern) + d);
                editPattern (label, [set, v] (ArpPattern& p) { set (p, v); });
            };
            f.menu = [this, list, get, set, label]
            {
                return listMenu (list(), get (slotOf (ctx).pattern), [this, set, label] (int i) { editPattern (label, [set, i] (ArpPattern& p) { set (p, i); }); });
            };
        };
        setupList (patMode, &arpchoices::patternModes, [] (const ArpPattern& p) { return p.mode; }, [] (ArpPattern& p, int v) { p.mode = v; }, "Pattern Mode");
        setupList (patTime, &arpchoices::patternTimes, [] (const ArpPattern& p) { return p.time; }, [] (ArpPattern& p, int v) { p.time = v; }, "Pattern Time");
        patTime.active = [this] { const int m = slotOf (ctx).pattern.mode; return m == 3 || m == 4 || m == 5; };
        patTime.text = [this] { const auto& p = slotOf (ctx).pattern; return (p.mode == 3 || p.mode == 4 || p.mode == 5) ? String (arpchoices::patternTimes()[(size_t) p.time]) : String ("-"); };
        setupList (patStepMode, &arpchoices::stepModes, [] (const ArpPattern& p) { return p.stepMode; }, [] (ArpPattern& p, int v) { p.stepMode = v; }, "Step Mode");
        setupList (patWrap, &arpchoices::wrapModes, [] (const ArpPattern& p) { return p.wrap; }, [] (ArpPattern& p, int v) { p.wrap = v; }, "Wrap Mode");
        patPitch.text = [this] { return String (slotOf (ctx).pattern.wrapPitch); };
        patPitch.onStep = [this] (int d) { const int v = jlimit (0, 24, slotOf (ctx).pattern.wrapPitch + d); editPattern ("Wrap Pitch", [v] (ArpPattern& p) { p.wrapPitch = v; }); };
        patPitch.arrows = true;
        patRange.text = [this] { const int r = slotOf (ctx).pattern.wrapRange; return r == 0 ? String ("ALL") : String (r); };
        patRange.onStep = [this] (int d) { const int v = jlimit (0, 8, slotOf (ctx).pattern.wrapRange + d); editPattern ("Wrap Range", [v] (ArpPattern& p) { p.wrapRange = v; }); };
        patRange.arrows = true;
        patGrid.dropArrow = true;
        patGrid.text = [this] { return gridName (graph.grid); };
        patGrid.onStep = [this] (int d) { setGrid (d > 0 ? graph.grid * 2.0 : graph.grid * 0.5); };
        patGrid.menu = [this]
        {
            Menu m;
            for (double g : { 1.0, 0.5, 0.25, 0.125 })
                m.add (gridName (g), [this, g] { setGrid (g); }, std::abs (graph.grid - g) < 1.0e-9);
            return m;
        };
        patGrid.setTooltip ("Grid size (drag or click)");
        graph.grid = (double) ctx.proc.uiState.getProperty ("arpGrid", 0.25);

        for (Component* comp : std::initializer_list<Component*> { &bank, &launchQuant, &editAll, &shape, &patternEdit, &bpm, &hz, &trip, &dot, &rate,
                                                                   &rangeShape, &shift, &range, &latch, &thru, &offset, &repeats, &gate, &chance,
                                                                   &rtLaunch, &rtRate, &rtRateField, &rtNote, &rtFirst, &velOn, &velRetrig, &decay, &target,
                                                                   &slots, &midiOut })
            addAndMakeVisible (comp);
        for (Component* comp : std::initializer_list<Component*> { &patName, &patLength, &patMode, &patTime, &patStepMode, &patWrap, &patPitch, &patRange, &patGrid, &graph, &preview })
            addChildComponent (comp);
        refresh();
    }

    ~ArpEditor() override { ctx.remove (this); }

    // ---- edits ------------------------------------------------------------
    void editSlot (const String& label, std::function<void (ArpSlot&)> fn)
    {
        const bool alt = altHeld();
        seq::edit (ctx, label, [fn, alt] (Patch& p) { p.arp.applyEdit (p.arp.selected, alt, fn); });
    }
    void editPattern (const String& label, std::function<void (ArpPattern&)> fn)
    {
        seq::edit (ctx, label, [fn] (Patch& p) { auto& s = p.arp.slots[(size_t) p.arp.selected]; fn (s.pattern); s.pattern.sortSteps(); });
    }

    void setupInt (SeqKnob& k, const String& what, int lo, int hi, int def, std::function<int (const ArpSlot&)> get,
                   std::function<void (ArpSlot&, int)> set, std::function<String (int)> fmt)
    {
        const float span = (float) (hi - lo);
        k.setAccent (arpColour);
        k.get = [this, get, lo, span] { return (float) (get (slotOf (ctx)) - lo) / span; };
        k.set = [this, set, lo, span, what] (float n)
        {
            const int v = lo + roundToInt (n * span);
            editSlot ("Arp " + what, [set, v] (ArpSlot& s) { set (s, v); });
        };
        k.format = [fmt, lo, span] (float n) { return fmt (lo + roundToInt (n * span)); };
        k.parse = [lo, hi, span] (const String& t, float& n)
        {
            const String s = t.trim();
            int v = s.equalsIgnoreCase ("inf") ? hi : s.getIntValue();
            if (! s.equalsIgnoreCase ("inf") && ! s.trimCharactersAtStart ("+-").containsOnly ("0123456789")) return false;
            n = (float) (jlimit (lo, hi, v) - lo) / span;
            return true;
        };
        k.defaultNorm = (float) (def - lo) / span;
        k.contextMenu = [this, what, set, def, lo]
        {
            const int d = def;
            return settingMenu (slotTitle (ctx, what), {}, [this, set, d, what, lo]
            {
                (void) lo;
                editSlot ("Reset " + what, [set, d] (ArpSlot& s) { set (s, d); });
            });
        };
    }

    void setupPercent (SeqKnob& k, const String& what, float lo, float hi, float def, std::function<float (const ArpSlot&)> get, std::function<void (ArpSlot&, float)> set)
    {
        k.setAccent (arpColour);
        k.get = [this, get, lo, hi] { return (get (slotOf (ctx)) - lo) / (hi - lo); };
        k.set = [this, set, lo, hi, what] (float n)
        {
            const float v = lo + n * (hi - lo);
            editSlot ("Arp " + what, [set, v] (ArpSlot& s) { set (s, v); });
        };
        k.format = [lo, hi] (float n) { return String (roundToInt ((lo + n * (hi - lo)) * 100.0f)) + "%"; };
        k.parse = [lo, hi] (const String& t, float& n) { n = (t.getFloatValue() / 100.0f - lo) / (hi - lo); return t.trim().isNotEmpty(); };
        k.defaultNorm = (def - lo) / (hi - lo);
        k.contextMenu = [this, what, set, def]
        {
            return settingMenu (slotTitle (ctx, what), {}, [this, set, def, what] { editSlot ("Reset " + what, [set, def] (ArpSlot& s) { set (s, def); }); });
        };
    }

    static String gridName (double g)
    {
        if (g >= 4.0) return "1/1";
        return "1/" + String (roundToInt (4.0 / g));
    }
    void setGrid (double g)
    {
        graph.grid = jlimit (0.125, 1.0, g);
        ctx.proc.uiState.setProperty ("arpGrid", graph.grid, nullptr);
        graph.repaint();
        patGrid.refresh();
    }

    // ---- menus ------------------------------------------------------------
    Menu shapeMenu()
    {
        Menu m;
        const int cur = (int) slotOf (ctx).shape;
        const auto& l = arpchoices::shapes();
        for (int i = 0; i < (int) l.size(); ++i)
        {
            if (i == (int) ArpShape::Pattern) m.separator();
            m.add (l[(size_t) i], [this, i] { editSlot ("Arp Shape", [i] (ArpSlot& s) { s.shape = (ArpShape) i; }); }, i == cur);
        }
        return m;
    }

    Menu rangeShapeMenu()
    {
        return listMenu (arpchoices::rangeShapes(), slotOf (ctx).rangeShape, [this] (int i) { editSlot ("Range Shape", [i] (ArpSlot& s) { s.rangeShape = i; }); });
    }

    String bankLabel() const
    {
        const auto& b = bankOf (ctx);
        ArpBank init; init.init();
        return String (b.name) + (isModified (&ctx.proc, "arp", b.name, signature (b), signature (init)) ? "*" : "");
    }

    void loadBank (ArpBank b, const String& name)
    {
        b.name = name.toStdString();
        const auto keepLock = bankOf (ctx).lockModule;
        b.lockModule = keepLock;
        setBaseline (&ctx.proc, "arp", b.name, signature (b));
        ctx.proc.editPatch ("Load Arp Bank", [b] (Patch& p) { p.arp = b; });
    }

    Menu bankMenu()
    {
        Menu m, factory, user;
        const auto cur = String (bankOf (ctx).name);
        SafePointer<ArpEditor> safe (this);
        for (auto& [name, b] : factoryBanks())
        {
            const auto bankCopy = b; const auto n = name;
            factory.add (name, [safe, bankCopy, n] { if (safe != nullptr) safe->loadBank (bankCopy, n); }, name == cur);
        }
        bool inUser = false;
        for (auto& f : userFiles ("Arp Banks"))
        {
            const auto n = f.getFileNameWithoutExtension();
            inUser |= n == cur;
            user.add (n, [safe, f, n]
            {
                Node node;
                if (safe == nullptr || ! loadNode (f, node)) return;
                ArpBank b;
                b.load (node);
                safe->loadBank (b, n);
            }, n == cur);
        }
        m.sub ("Factory", factory, ! inUser);
        m.sub ("User", user, inUser, ! user.empty());
        m.separator();
        m.add ("Save Arp Bank...", [safe] { if (safe != nullptr) safe->saveBankDialog(); });
        return m;
    }

    void stepBank (int d)
    {
        std::vector<std::pair<String, std::function<ArpBank()>>> all;
        for (auto& [n, b] : factoryBanks()) { const auto bankCopy = b; all.push_back ({ n, [bankCopy] { return bankCopy; } }); }
        for (auto& f : userFiles ("Arp Banks"))
            all.push_back ({ f.getFileNameWithoutExtension(), [f] { Node n; ArpBank b; if (loadNode (f, n)) b.load (n); return b; } });
        int cur = -1;
        for (int i = 0; i < (int) all.size(); ++i) if (all[(size_t) i].first == String (bankOf (ctx).name)) cur = i;
        const int next = cur < 0 ? 0 : (cur + d + (int) all.size()) % (int) all.size();
        loadBank (all[(size_t) next].second(), all[(size_t) next].first);
    }

    void saveBankDialog()
    {
        if (! ctx.editValue) return;
        SafePointer<ArpEditor> safe (this);
        ctx.editValue (bank, String (bankOf (ctx).name == "Init" ? "My Arp Bank" : bankOf (ctx).name), [safe] (const String& t)
        {
            const auto name = t.trim();
            if (safe == nullptr || name.isEmpty()) return;
            auto& proc = safe->ctx.proc;
            proc.editPatch ("Save Arp Bank", [name] (Patch& p) { p.arp.name = name.toStdString(); });
            Node n;
            proc.patch().arp.save (n);
            saveNode (userFileFor ("Arp Banks", name), n);
            setBaseline (&proc, "arp", proc.patch().arp.name, signature (proc.patch().arp));
        });
    }

    bool patternModified() const
    {
        const auto& p = slotOf (ctx).pattern;
        for (auto& [n, f] : factoryPatterns())
            if (n.toStdString() == p.name)
            {
                Node a, b;
                p.save (a);
                f.save (b);
                return a.toBinary() != b.toBinary();
            }
        return false;
    }

    void loadPattern (const ArpPattern& pat) { editPattern ("Load Pattern", [pat] (ArpPattern& p) { p = pat; }); }

    Menu patternMenu()
    {
        Menu m, factory, user;
        const auto cur = String (slotOf (ctx).pattern.name);
        SafePointer<ArpEditor> safe (this);
        for (auto& [name, pat] : factoryPatterns())
        {
            const auto pp = pat;
            factory.add (name, [safe, pp] { if (safe != nullptr) safe->loadPattern (pp); }, name == cur);
        }
        for (auto& f : userFiles ("Arp Patterns"))
        {
            const auto n = f.getFileNameWithoutExtension();
            user.add (n, [safe, f, n]
            {
                Node node;
                if (safe == nullptr || ! loadNode (f, node)) return;
                ArpPattern p;
                p.load (node);
                p.name = n.toStdString();
                safe->loadPattern (p);
            }, n == cur);
        }
        m.sub ("Factory", factory);
        m.sub ("User", user, false, ! user.empty());
        m.separator();
        m.add ("Save Pattern...", [safe]
        {
            if (safe == nullptr || ! safe->ctx.editValue) return;
            safe->ctx.editValue (safe->patName, String (slotOf (safe->ctx).pattern.name), [safe] (const String& t)
            {
                if (safe == nullptr || t.trim().isEmpty()) return;
                const auto name = t.trim().toStdString();
                safe->editPattern ("Save Pattern", [name] (ArpPattern& p) { p.name = name; });
                Node n;
                slotOf (safe->ctx).pattern.save (n);
                saveNode (userFileFor ("Arp Patterns", t.trim()), n);
            });
        });
        m.add ("Rename Pattern...", [safe]
        {
            if (safe == nullptr || ! safe->ctx.editValue) return;
            safe->ctx.editValue (safe->patName, String (slotOf (safe->ctx).pattern.name), [safe] (const String& t)
            {
                if (safe == nullptr || t.trim().isEmpty()) return;
                const auto name = t.trim().toStdString();
                safe->editPattern ("Rename Pattern", [name] (ArpPattern& p) { p.name = name; });
            });
        });
        return m;
    }

    // ---- layout / paint -----------------------------------------------------
    static constexpr int kPaneY = 40, kPaneH = 166, kMidY = 216, kSlotY = 480;
    struct Pane { int x, w; const char* title; };
    static constexpr Pane kPanes[] { { 10, 214, "GLOBAL" }, { 232, 256, "PATTERN" }, { 496, 190, "TRANSPOSE" },
                                     { 694, 318, "PLAYBACK" }, { 1020, 172, "RETRIGGER" }, { 1200, 190, "" } };

    bool showPatternEditor() const { return patternOpen && slotOf (ctx).shape == ArpShape::Pattern; }

    void layoutMiddle()
    {
        const bool pe = showPatternEditor();
        for (Component* c : std::initializer_list<Component*> { &patName, &patLength, &patMode, &patTime, &patStepMode, &patWrap, &patPitch, &patRange, &patGrid, &graph })
            c->setVisible (pe);
        preview.setVisible (! pe);
        patternEdit.setVisible (slotOf (ctx).shape == ArpShape::Pattern);
        const int sx = 242;
        shape.setBounds (patternEdit.isVisible() ? Rectangle<int> (sx + 44, kPaneY + 14, 192, 24) : Rectangle<int> (sx, kPaneY + 14, 236, 24));
        repaint (0, kMidY - 10, getWidth(), kSlotY - kMidY + 10);
    }

    void resized() override
    {
        closeBtn.setBounds (getWidth() - 40, 6, 30, 22);
        const int y0 = kPaneY + 14;
        // GLOBAL
        bank.setBounds (20, y0, 194, 24);
        launchQuant.setBounds (20, y0 + 36, 194, 24);
        editAll.setBounds (20, y0 + 72, 92, 24);
        // PATTERN
        patternEdit.setBounds (242, y0, 40, 24);
        bpm.setBounds (242, y0 + 36, 44, 22);
        hz.setBounds (290, y0 + 36, 44, 22);
        trip.setBounds (242, y0 + 66, 44, 22);
        dot.setBounds (290, y0 + 66, 44, 22);
        rate.setBounds (370, y0 + 32, 96, 92);
        // TRANSPOSE
        rangeShape.setBounds (506, y0, 170, 24);
        shift.setBounds (510, y0 + 40, 76, 84);
        range.setBounds (596, y0 + 40, 76, 84);
        // PLAYBACK
        latch.setBounds (704, y0, 70, 24);
        thru.setBounds (780, y0, 70, 24);
        int kx = 704;
        for (auto* k : { &offset, &repeats, &gate, &chance }) { k->setBounds (kx, y0 + 40, 74, 84); kx += 76; }
        // RETRIGGER
        rtLaunch.setBounds (1030, y0, 120, 22);
        rtRate.setBounds (1030, y0 + 32, 70, 22);
        rtRateField.setBounds (1104, y0 + 32, 76, 22);
        rtNote.setBounds (1030, y0 + 64, 120, 22);
        rtFirst.setBounds (1050, y0 + 92, 120, 22);
        // VELOCITY (header checkbox sits on the pane's top edge)
        velOn.setBounds (1212, kPaneY - 9, 100, 18);
        velRetrig.setBounds (1210, y0, 120, 22);
        decay.setBounds (1210, y0 + 40, 76, 84);
        target.setBounds (1296, y0 + 40, 76, 84);

        // Middle: pattern editor or order preview.
        const int my = kMidY + 14, mh = kSlotY - kMidY - 26;
        patName.setBounds (20, my, 214, 24);
        int fy = my + 30;
        for (auto* f : { &patLength, &patMode, &patTime, &patStepMode, &patWrap, &patPitch, &patRange })
        {
            f->setBounds (f == &patTime || f == &patPitch || f == &patRange ? 34 : 20, fy, f == &patTime || f == &patPitch || f == &patRange ? 200 : 214, 21);
            fy += 25;
        }
        patGrid.setBounds (256, my, 64, 20);
        graph.setBounds (256, my + 24, getWidth() - 266, mh - 24);
        preview.setBounds (20, my, getWidth() - 40, mh);

        slots.setBounds (10, kSlotY + 4, 1180, getHeight() - kSlotY - 12);
        midiOut.setBounds (1200, kSlotY + 4, 190, getHeight() - kSlotY - 12);
        layoutMiddle();
    }

    void paint (Graphics& g) override
    {
        drawEditorBackground (g, getLocalBounds(), "ARP", arpColour);
        drawText (g, "Click a slot to edit it, its bar to launch it. Hold Alt (or EDIT ALL) to change every slot.",
                  { 80.0f, 6.0f, 700.0f, 20.0f }, 11.0f, colours::inkDim, true, Justification::centredLeft);
        for (const auto& p : kPanes)
            drawSection (g, { (float) p.x, (float) kPaneY, (float) p.w, (float) kPaneH }, p.title, colours::ink);
        // VELOCITY pane: its title is the checkbox.
        g.setColour (colours::panel);
        g.fillRect (1208, kPaneY - 9, 100, 18);
        const bool pe = showPatternEditor();
        drawSection (g, { 10.0f, (float) kMidY, (float) getWidth() - 20.0f, (float) (kSlotY - kMidY - 8) }, pe ? "ARP PATTERN EDITOR" : "PREVIEW", pe ? arpColour : colours::ink);
        if (pe)
        {
            g.setColour (colours::inkFaint);
            g.fillRect (246, kMidY + 12, 1, kSlotY - kMidY - 30);
        }
        drawText (g, "SLOT " + String (bankOf (ctx).selected + 1), { (float) getWidth() - 200.0f, 6.0f, 150.0f, 20.0f }, 12.0f, arpColour, true, Justification::centredRight);
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && e.y < 30 && e.x < 80) showContextMenu (*this, e.getPosition(), tabMenu (ctx, false));
    }

    void refresh() override
    {
        const auto& s = slotOf (ctx);
        const int key = (int) s.shape * 1000 + bankOf (ctx).selected * 10 + (patternOpen ? 1 : 0) + (s.hzMode ? 5 : 0);
        if (key != shownKey) { shownKey = key; layoutMiddle(); repaint(); }
    }

private:
    PanelContext& ctx;
    std::function<void()> onClose;
    PanelButton closeBtn;
    Field bank, launchQuant;
    Check editAll;
    Field shape;
    Check patternEdit, bpm, hz, trip, dot;
    SeqKnob rate;
    Field rangeShape;
    SeqKnob shift, range;
    Check latch, thru;
    SeqKnob offset, repeats, gate, chance;
    Check rtLaunch, rtRate;
    Field rtRateField;
    Check rtNote, rtFirst, velOn, velRetrig;
    SeqKnob decay, target;
    Field patName, patLength, patMode, patTime, patStepMode, patWrap, patPitch, patRange, patGrid;
    PatternGraph graph;
    OrderPreview preview;
    SlotRow slots;
    MidiOutBox midiOut;
    bool patternOpen = true;
    int shownKey = -1;
};
} // namespace

std::unique_ptr<Component> createArpEditor (PanelContext& ctx, std::function<void()> onClose)
{
    return std::make_unique<ArpEditor> (ctx, std::move (onClose));
}
} // namespace tincture::ui
