// ClipEditor.cpp — the CLIP module (spec D 9): GLOBAL, CLIP SETTINGS and
// RECORD panes, the piano roll (ruler with start/end flags and the
// start-offset marker, note grid with keyboard, velocity lane), the 12 clip
// slots and the MIDI OUT box. Clips are patch data (ClipBank).
#include "SeqWidgets.h"
#include "../Engine/Scales.h"

#include <map>

namespace tincture::ui
{
using namespace juce;
using namespace seq;

namespace
{
const Colour kNote { 0xff3fc0a0 };

const ClipBank& bankOf (PanelContext& c) { return c.proc.patch().clips; }
const Clip& clipOf (PanelContext& c) { const auto& b = bankOf (c); return b.clips[(size_t) b.selected]; }

String shortcut (const String& key)
{
   #if JUCE_MAC
    return "Cmd-" + key;
   #else
    return "Ctrl-" + key;
   #endif
}

struct NoteClipboard { std::vector<ClipNote> notes; bool range = false; double rangeLength = 0.0; };
std::map<const void*, NoteClipboard>& noteClipboard() { static std::map<const void*, NoteClipboard> m; return m; }
std::map<const void*, Clip>& clipClipboard() { static std::map<const void*, Clip> m; return m; }

std::vector<uint8_t> signature (const ClipBank& b)
{
    ClipBank c = b;
    c.selected = 0; c.editAll = false; c.lockModule = false; c.triggerOctave = 0; c.previewClip = -1; c.metronome = false; c.recordMode = 0;
    c.grid = 0.25; c.snap = true;
    Node n;
    c.save (n);
    return n.toBinary();
}

// ---------------------------------------------------------------------------
// Factory content (generated).
// ---------------------------------------------------------------------------
Clip chordClip (std::initializer_list<std::initializer_list<int>> chords, double each, const char* name)
{
    Clip c;
    c.name = name;
    double t = 0.0;
    for (auto& ch : chords)
    {
        for (int p : ch) { ClipNote n; n.start = t; n.length = each * 0.95; n.pitch = p; n.velocity = 96; c.notes.push_back (n); }
        t += each;
    }
    c.length = std::max (4.0, t);
    c.sort();
    return c;
}

Clip patternClip (int pitch, std::initializer_list<int> hits, double step, int vel, const char* name)
{
    Clip c;
    c.name = name;
    int i = 0;
    for (int h : hits)
    {
        if (h) { ClipNote n; n.start = i * step; n.length = step * 0.5; n.pitch = pitch; n.velocity = h > 1 ? 120 : vel; c.notes.push_back (n); }
        ++i;
    }
    c.length = std::max (1.0, i * step);
    c.sort();
    return c;
}

std::vector<std::pair<String, Clip>> factoryClips()
{
    return {
        { "Bass Pulse", patternClip (36, { 2, 0, 1, 1, 2, 0, 1, 0, 2, 0, 1, 1, 2, 1, 0, 1 }, 0.25, 90, "Bass Pulse") },
        { "Four Chords", chordClip ({ { 60, 64, 67 }, { 55, 59, 62 }, { 57, 60, 64 }, { 53, 57, 60 } }, 4.0, "Four Chords") },
        { "Init", Clip() },
    };
}

struct FactoryBank { String folder, name; std::function<ClipBank()> make; };
std::vector<FactoryBank> factoryBanks()
{
    return {
        { "Chords", "Basic Triads", []
        {
            ClipBank b; b.init(); b.name = "Basic Triads";
            const int roots[] { 60, 62, 64, 65, 67, 69, 71, 72, 57, 59, 55, 53 };
            for (int i = 0; i < 12; ++i)
            {
                const int r = roots[i];
                const bool minor = i == 1 || i == 2 || i == 5 || i == 8;
                b.clips[(size_t) i] = chordClip ({ { r, r + (minor ? 3 : 4), r + 7 } }, 4.0, "");
            }
            return b;
        } },
        { "Drums", "Four On The Floor", []
        {
            ClipBank b; b.init(); b.name = "Four On The Floor";
            b.clips[0] = patternClip (36, { 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0, 2, 0, 0, 0 }, 0.25, 100, "Kick");
            b.clips[1] = patternClip (38, { 0, 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 2, 0, 0, 1 }, 0.25, 90, "Snare");
            b.clips[2] = patternClip (42, { 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 2, 1 }, 0.25, 70, "Hats");
            return b;
        } },
        { "Guitar", "Strum Eighths", []
        {
            ClipBank b; b.init(); b.name = "Strum Eighths";
            Clip c;
            for (int i = 0; i < 8; ++i)
                for (int k = 0; k < 5; ++k)
                {
                    const int pitches[] { 52, 59, 64, 67, 71 };
                    ClipNote n; n.start = i * 0.5 + k * 0.02; n.length = 0.4; n.pitch = pitches[k]; n.velocity = i % 2 ? 80 : 110;
                    c.notes.push_back (n);
                }
            c.sort();
            b.clips[0] = c;
            return b;
        } },
        { {}, "Init", [] { ClipBank b; b.init(); return b; } },
    };
}

// ---------------------------------------------------------------------------
// The piano roll: ruler, keyboard + note grid, velocity lane.
// ---------------------------------------------------------------------------
class PianoRoll final : public Component, public Refreshable, public SettableTooltipClient
{
public:
    static constexpr int kKeysW = 46, kRulerH = 32, kLaneH = 58;

    explicit PianoRoll (PanelContext& c) : ctx (c)
    {
        ctx.add (this);
        setWantsKeyboardFocus (true);
        setTooltip ("Double-click: add/delete a note. Drag notes to move, their right edge to resize; drag empty space to select. Wheel scrolls (Shift: across, Alt/Cmd: zoom)");
        rebuildRows();
        scrollToPitch (76);
    }
    ~PianoRoll() override { ctx.remove (this); stopAudition(); }

    // ---- view ------------------------------------------------------------
    double viewStart = 0.0, viewLen = 4.0;
    float rowH = 12.0f;
    int scrollRow = 0;
    int foldMode = 0;                      // 0 off, 1 fold, 2 in-scale + used
    bool audition = false;
    std::vector<int> rows;                 // pitches, top to bottom
    double pastePos = 0.0;
    bool hasRange = false;
    double range0 = 0.0, range1 = 0.0;
    std::function<void()> onViewChanged;

    Rectangle<float> gridArea() const { return { (float) kKeysW, (float) kRulerH, (float) getWidth() - kKeysW, (float) (getHeight() - kRulerH - kLaneH - 4) }; }
    Rectangle<float> laneArea() const { const auto g = gridArea(); return { g.getX(), g.getBottom() + 4.0f, g.getWidth(), (float) kLaneH }; }
    Rectangle<float> rulerArea() const { return { (float) kKeysW, 0.0f, (float) getWidth() - kKeysW, (float) kRulerH }; }
    float xOf (double b) const { const auto g = gridArea(); return g.getX() + (float) ((b - viewStart) / viewLen) * g.getWidth(); }
    double beatAt (float x) const { const auto g = gridArea(); return viewStart + (double) ((x - g.getX()) / g.getWidth()) * viewLen; }
    int visibleRows() const { return (int) std::ceil (gridArea().getHeight() / rowH); }
    float yOfIndex (int i) const { return gridArea().getY() + (float) (i - scrollRow) * rowH; }
    int indexOfPitch (int p) const { for (int i = 0; i < (int) rows.size(); ++i) if (rows[(size_t) i] == p) return i; return -1; }
    int pitchAt (float y) const
    {
        const int i = scrollRow + (int) std::floor ((y - gridArea().getY()) / rowH);
        return rows.empty() ? 60 : rows[(size_t) jlimit (0, (int) rows.size() - 1, i)];
    }

    double grid() const
    {
        if (ctx.proc.uiState.getProperty ("clipAutoGrid", false))
        {
            const bool trip = isTriplet (bankOf (ctx).grid);
            double g = 4.0;
            const double pxPerBeat = gridArea().getWidth() / viewLen;
            while (g > 1.0 / 32.0 && g * 0.5 * pxPerBeat >= 14.0) g *= 0.5;
            return trip ? g * 2.0 / 3.0 : g;
        }
        return bankOf (ctx).grid;
    }
    static bool isTriplet (double g) { const double l = std::log2 (g * 1.5); return std::abs (l - std::round (l)) < 1.0e-6; }
    double snap (double b) const { return bankOf (ctx).snap ? std::floor (b / grid() + 1.0e-6) * grid() : b; }
    double snapRound (double b) const { return bankOf (ctx).snap ? std::round (b / grid()) * grid() : b; }

    void rebuildRows()
    {
        const int key = scales::keyFromParam (ctx.proc.get (ids().global.key));
        const int sc = ctx.proc.params.geti (ids().global.scale);
        bool used[128] {};
        clipOf (ctx).usedPitches (used);
        rows.clear();
        for (int p = 127; p >= 0; --p)
        {
            bool show = true;
            if (foldMode == 1) show = key >= 0 ? scales::inScale (p, key, sc) : used[p];
            if (foldMode == 2) show = used[p] || (key >= 0 && scales::inScale (p, key, sc));
            if (show) rows.push_back (p);
        }
        if (rows.empty()) for (int p = 127; p >= 0; --p) rows.push_back (p);
        if (foldMode != 0) rowH = jmax (rowH, 14.0f);
        clampScroll();
        background = {};
        repaint();
    }

    void clampScroll() { scrollRow = jlimit (0, jmax (0, (int) rows.size() - visibleRows()), scrollRow); }
    void scrollToPitch (int p)
    {
        const int i = indexOfPitch (p);
        if (i >= 0) scrollRow = i;
        clampScroll();
    }

    void zoomToFit()
    {
        double t0, t1; int lo, hi;
        const auto& c = clipOf (ctx);
        if (! c.noteBounds (t0, t1, lo, hi)) { t0 = 0.0; t1 = c.length; lo = 60; hi = 72; }
        viewStart = 0.0;
        viewLen = jmax (1.0, std::ceil (jmax (t1, c.length) / 4.0) * 4.0);
        const int span = jmax (12, hi - lo + 3);
        rowH = jlimit (6.0f, 24.0f, gridArea().getHeight() / (float) span);
        clampScroll();
        scrollToPitch (jmin (127, hi + 1));
        const int iLo = indexOfPitch (jmax (0, lo - 1));
        if (iLo >= 0 && iLo >= scrollRow + visibleRows()) scrollRow = iLo - visibleRows() + 1;
        clampScroll();
        viewChanged();
    }

    void viewChanged() { background = {}; repaint(); if (onViewChanged) onViewChanged(); }

    // ---- refresh -----------------------------------------------------------
    void refresh() override
    {
        Node n;
        const auto& c = clipOf (ctx);
        c.save (n);
        auto sig = n.toBinary();
        int sel = 0;
        for (auto& x : c.notes) sel = sel * 31 + (x.selected ? 1 : 0);
        sig.push_back ((uint8_t) sel);
        sig.push_back ((uint8_t) bankOf (ctx).selected);
        const int key = ctx.proc.params.geti (ids().global.key), sc = ctx.proc.params.geti (ids().global.scale);
        const auto gkey = std::make_tuple (key, sc, bankOf (ctx).grid, bankOf (ctx).snap);
        if (gkey != shownScale)
        {
            shownScale = gkey;
            if (foldMode != 0) rebuildRows();
            background = {};
            repaint();
        }
        if (sig != shown)
        {
            const bool slotChanged = bankOf (ctx).selected != shownSlot;
            shown = std::move (sig);
            shownSlot = bankOf (ctx).selected;
            if (slotChanged && foldMode != 0) rebuildRows();
            repaint();
        }
        // Playhead: repaint only the strips it moved through.
        const auto& t = ctx.proc.seqTelemetry();
        const int slot = bankOf (ctx).selected;
        float ph = ((t.clipPlaying.load() >> slot) & 1u) ? t.clipPlayhead[slot].load() : -1.0f;
        if (t.recordingSlot.load() == slot) ph = t.recordPosition.load();
        if (! same (ph, playhead))
        {
            auto strip = [this] (float b) { return b < 0.0f ? Rectangle<int>() : Rectangle<int> ((int) xOf (b) - 2, 0, 5, getHeight()); };
            repaint (strip (playhead));
            playhead = ph;
            repaint (strip (playhead));
        }
    }

    void resized() override { clampScroll(); background = {}; }

    // ---- paint -------------------------------------------------------------
    void paintBackground()
    {
        background = Image (Image::ARGB, jmax (1, getWidth()), jmax (1, getHeight()), true);
        Graphics g (background);
        g.fillAll (colours::black);
        const auto ga = gridArea();
        const int key = scales::keyFromParam (ctx.proc.get (ids().global.key));
        const int sc = ctx.proc.params.geti (ids().global.scale);
        const int n = visibleRows();
        for (int i = 0; i <= n; ++i)
        {
            const int idx = scrollRow + i;
            if (idx >= (int) rows.size()) break;
            const int p = rows[(size_t) idx];
            const float y = yOfIndex (idx);
            Colour c = scales::inScale (p, key, sc) ? Colour (0xff2b2c30) : Colour (0xff1c1c1f);
            if (key >= 0 && scales::isRoot (p, key)) c = Colour (0xff24382f);
            if (foldMode == 2 && key >= 0 && scales::inScale (p, key, sc) && ! scales::isRoot (p, key)) c = Colour (0xff25303f);
            g.setColour (c);
            g.fillRect (ga.getX(), y, ga.getWidth(), rowH);
            g.setColour (colours::black);
            g.fillRect (ga.getX(), y + rowH - 1.0f, ga.getWidth(), 1.0f);
            // Keyboard.
            const int k = p % 12;
            const bool black = k == 1 || k == 3 || k == 6 || k == 8 || k == 10;
            const auto kr = Rectangle<float> (0.0f, y, (float) kKeysW - 2.0f, rowH);
            g.setColour (black ? colours::black.brighter (0.05f) : colours::cream);
            g.fillRect (kr.reduced (0.0f, 0.5f));
            if (k == 0 || foldMode != 0)
                drawText (g, noteName (p), kr.withTrimmedRight (3.0f), jmin (10.5f, rowH - 1.0f), black ? colours::ink : colours::panel, k == 0, Justification::centredRight);
        }
        // Vertical lines.
        const double gr = grid();
        const double first = std::floor (viewStart / gr) * gr;
        const float pxPerBeat = ga.getWidth() / (float) viewLen;
        for (double b = first; b <= viewStart + viewLen + 1.0e-6; b += gr)
        {
            if (b < viewStart - 1.0e-6) continue;
            const bool bar = std::abs (std::fmod (b + 1.0e-9, 4.0)) < 1.0e-6, beat = std::abs (std::fmod (b + 1.0e-9, 1.0)) < 1.0e-6;
            if (! bar && ! beat && gr * pxPerBeat < 5.0f) continue;
            g.setColour (bar ? colours::inkFaint.brighter (0.2f) : (beat ? colours::inkFaint : colours::grid));
            g.fillRect (xOf (b), ga.getY(), 1.0f, ga.getHeight());
            g.setColour (colours::grid);
            g.fillRect (xOf (b), laneArea().getY(), 1.0f, (float) kLaneH);
        }
        // Ruler labels.
        g.setColour (colours::panel.darker (0.3f));
        g.fillRect (rulerArea());
        const double labelStep = pxPerBeat >= 30.0f ? 1.0 : (pxPerBeat >= 8.0f ? 4.0 : 8.0);
        for (double b = std::ceil (viewStart / labelStep) * labelStep; b < viewStart + viewLen; b += labelStep)
        {
            const int barN = (int) (b / 4.0) + 1, beatN = (int) std::fmod (b, 4.0) + 1;
            g.setColour (colours::inkDim);
            g.fillRect (xOf (b), 0.0f, 1.0f, 13.0f);
            drawText (g, beatN == 1 ? String (barN) : String (barN) + "." + String (beatN), { xOf (b) + 3.0f, 1.0f, 40.0f, 14.0f }, 10.5f,
                      beatN == 1 ? colours::ink : colours::inkDim, beatN == 1, Justification::centredLeft);
        }
        // Lane.
        const auto la = laneArea();
        g.setColour (colours::panel.darker (0.35f));
        g.fillRect (Rectangle<float> (0.0f, la.getY(), (float) kKeysW - 2.0f, la.getHeight()));
        drawText (g, "VELO", { 0.0f, la.getY() + 2.0f, (float) kKeysW - 4.0f, 14.0f }, 10.0f, colours::inkDim, true, Justification::centredRight);
        drawText (g, "127", { 0.0f, la.getY() + 16.0f, (float) kKeysW - 4.0f, 12.0f }, 9.5f, colours::inkFaint, true, Justification::centredRight);
        drawText (g, "1", { 0.0f, la.getBottom() - 13.0f, (float) kKeysW - 4.0f, 12.0f }, 9.5f, colours::inkFaint, true, Justification::centredRight);
        g.setColour (colours::grid);
        g.drawRect (la, 1.0f);
    }

    Rectangle<float> noteRect (const ClipNote& n) const
    {
        const int idx = indexOfPitch (n.pitch);
        if (idx < 0) return {};
        return { xOf (n.start), yOfIndex (idx), jmax (3.0f, xOf (n.end()) - xOf (n.start) - 1.0f), rowH - 1.0f };
    }

    void paint (Graphics& g) override
    {
        if (background.isNull()) paintBackground();
        g.drawImageAt (background, 0, 0);
        const auto ga = gridArea();
        const auto& c = clipOf (ctx);

        g.saveState();
        g.reduceClipRegion (ga.toNearestInt());
        // Outside the loop: shaded.
        g.setColour (colours::black.withAlpha (0.5f));
        if (xOf (c.loopStart) > ga.getX()) g.fillRect (Rectangle<float> (ga.getX(), ga.getY(), xOf (c.loopStart) - ga.getX(), ga.getHeight()));
        if (xOf (c.length) < ga.getRight()) g.fillRect (Rectangle<float> (xOf (c.length), ga.getY(), ga.getRight() - xOf (c.length), ga.getHeight()));
        if (hasRange)
        {
            g.setColour (colours::ink.withAlpha (0.12f));
            g.fillRect (Rectangle<float> (xOf (range0), ga.getY(), xOf (range1) - xOf (range0), ga.getHeight()));
        }
        const float px = (float) (gridArea().getWidth() / viewLen);
        for (const auto& n : c.notes)
        {
            const auto r = noteRect (n);
            if (r.isEmpty() || r.getRight() < ga.getX() || r.getX() > ga.getRight()) continue;
            const bool last = std::abs (n.start - lastStart) < 1.0e-6 && n.pitch == lastPitch;
            Colour col = n.take != 0 ? takeRed : (n.muted ? colours::inkFaint : ((n.selected || last) ? orange : kNote));
            g.setColour (col);
            g.fillRect (r);
            g.setColour (col.darker (0.6f));
            g.drawRect (r, 1.0f);
            // Velocity line.
            g.setColour (colours::black.withAlpha (0.5f));
            g.fillRect (r.getX() + 2.0f, r.getBottom() - 3.0f, (r.getWidth() - 4.0f) * (float) n.velocity / 127.0f, 1.5f);
            if (r.getWidth() > 26.0f && rowH >= 10.0f)
                drawText (g, noteName (n.pitch), r.reduced (3.0f, 0.0f).withTrimmedBottom (2.0f), jmin (10.5f, rowH - 1.0f), colours::black, true, Justification::centredLeft);
        }
        if (marquee)
        {
            const auto m = Rectangle<float> (marqueeFrom, marqueeTo);
            g.setColour (colours::ink.withAlpha (0.1f));
            g.fillRect (m);
            g.setColour (colours::ink.withAlpha (0.7f));
            g.drawRect (m, 1.0f);
        }
        g.restoreState();

        // Ruler: flags and the start-offset marker.
        const auto ra = rulerArea();
        g.saveState();
        g.reduceClipRegion (Rectangle<int> (kKeysW - 6, 0, getWidth() - kKeysW + 6, getHeight()).getIntersection (getLocalBounds()));
        const Colour flag { 0xff5ad07a };
        auto drawFlag = [&] (double b, bool left)
        {
            const float x = xOf (b);
            g.setColour (flag);
            g.fillRect (x - (left ? 0.0f : 1.0f), 14.0f, 1.5f, ga.getBottom() - 14.0f);
            Path p;
            if (left) p.addTriangle (x, 14.0f, x, 23.0f, x + 8.0f, 18.5f);
            else p.addTriangle (x, 14.0f, x, 23.0f, x - 8.0f, 18.5f);
            g.fillPath (p);
        };
        drawFlag (c.loopStart, true);
        drawFlag (c.length, false);
        {
            const float x = xOf (c.startOffset);
            Path p;
            p.addTriangle (x - 5.0f, ra.getBottom() - 9.0f, x + 5.0f, ra.getBottom() - 9.0f, x, ra.getBottom() - 1.0f);
            g.setColour (flag.brighter (0.3f));
            g.fillPath (p);
        }
        // Paste position.
        g.setColour (colours::lcd.withAlpha (0.6f));
        g.fillRect (xOf (pastePos), ra.getBottom() - 3.0f, 1.0f, 3.0f);
        // Playhead.
        if (playhead >= 0.0f)
        {
            g.setColour (colours::lcd);
            g.fillRect (xOf (playhead), 0.0f, 1.5f, laneArea().getBottom());
        }
        g.restoreState();

        // Velocity lane.
        const auto la = laneArea();
        g.saveState();
        g.reduceClipRegion (la.toNearestInt());
        for (const auto& n : c.notes)
        {
            const float x = xOf (n.start) + 2.0f;
            if (x < la.getX() || x > la.getRight()) continue;
            const float y = la.getBottom() - 3.0f - (la.getHeight() - 8.0f) * (float) n.velocity / 127.0f;
            const Colour col = n.muted ? colours::inkFaint : (n.selected ? orange : kNote);
            g.setColour (col);
            g.fillRect (x, y, 1.5f, la.getBottom() - y);
            g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre ({ x + 0.75f, y }));
        }
        g.restoreState();
        (void) px;
        g.setColour (colours::inkDim);
        g.drawRect (getLocalBounds().toFloat(), 1.0f);
    }

    // ---- editing -------------------------------------------------------------
    void editClip (const String& label, std::function<void (Clip&)> fn)
    {
        seq::edit (ctx, label, [fn] (Patch& p) { auto& c = p.clips.clips[(size_t) p.clips.selected]; fn (c); c.sort(); });
    }
    void select (std::function<void (Clip&)> fn)
    {
        ctx.proc.editPatch ({}, [fn] (Patch& p) { fn (p.clips.clips[(size_t) p.clips.selected]); });
    }

    int noteAtPoint (Point<float> p) const
    {
        const auto& ns = clipOf (ctx).notes;
        for (int i = (int) ns.size() - 1; i >= 0; --i) if (noteRect (ns[(size_t) i]).contains (p)) return i;
        return -1;
    }

    void auditionNote (int pitch, int vel)
    {
        if (! audition) return;
        stopAudition();
        ctx.proc.uiNoteOn (pitch, (float) vel / 127.0f);
        auditioning = pitch;
    }
    void stopAudition()
    {
        if (auditioning >= 0) ctx.proc.uiNoteOff (auditioning);
        auditioning = -1;
    }

    void mouseDown (const MouseEvent& e) override
    {
        grabKeyboardFocus();
        mode = None;
        const auto pos = e.position;
        const auto& c = clipOf (ctx);

        if (e.mods.isCommandDown() && e.mods.isAltDown()) { mode = Pan; panStart = { (float) viewStart, (float) scrollRow }; return; }
        if (e.mods.isShiftDown() && e.mods.isAltDown()) { mode = Zoom; panStart = { (float) viewLen, rowH }; return; }

        if (pos.x < kKeysW && pos.y > kRulerH && pos.y < gridArea().getBottom())
        {
            const bool was = audition;
            audition = true;
            auditionNote (pitchAt (pos.y), 100);
            audition = was;
            mode = Keys;
            return;
        }
        if (rulerArea().contains (pos))
        {
            const float xo = xOf (c.startOffset), xs = xOf (c.loopStart), xe = xOf (c.length);
            if (e.mods.isPopupMenu())
            {
                if (pos.y > kRulerH - 11 && std::abs (pos.x - xo) < 8.0f) showContextMenu (*this, e.getPosition(), offsetMenu());
                return;
            }
            if (pos.y > kRulerH - 11 && std::abs (pos.x - xo) < 7.0f) mode = Offset;
            else if (std::abs (pos.x - xe) < 9.0f) mode = EndFlag;
            else if (std::abs (pos.x - xs) < 9.0f) mode = StartFlag;
            else { pastePos = jmax (0.0, snapRound (beatAt (pos.x))); repaint(); return; }
            beginDrag();
            return;
        }
        if (laneArea().contains (pos))
        {
            laneNote = nearestNote (pos.x);
            if (laneNote >= 0) { beginDrag(); mode = Lane; mouseDrag (e); }
            return;
        }
        if (! gridArea().contains (pos)) return;
        const int i = noteAtPoint (pos);
        if (e.mods.isPopupMenu())
        {
            if (i >= 0 && ! c.notes[(size_t) i].selected) { const int idx = i; select ([idx] (Clip& cl) { cl.selectAll (false); cl.notes[(size_t) idx].selected = true; }); }
            showContextMenu (*this, e.getPosition(), noteMenu());
            return;
        }
        if (i < 0)
        {
            mode = Marquee;
            marqueeFrom = marqueeTo = pos;
            return;
        }
        const auto n = c.notes[(size_t) i];
        if (e.mods.isShiftDown())
        {
            const int idx = i;
            select ([idx] (Clip& cl) { cl.notes[(size_t) idx].selected = ! cl.notes[(size_t) idx].selected; });
            return;
        }
        if (! n.selected) { const int idx = i; select ([idx] (Clip& cl) { cl.selectAll (false); cl.notes[(size_t) idx].selected = true; }); }
        hasRange = false;
        original = clipOf (ctx).notes;
        mode = pos.x > noteRect (n).getRight() - 5.0f ? Resize : Move;
        anchorBeat = n.start;
        anchorEnd = n.end();
        auditionNote (n.pitch, n.velocity);
        beginDrag();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        const auto pos = e.position;
        switch (mode)
        {
            case Pan:
            {
                const auto ga = gridArea();
                viewStart = jmax (0.0, (double) panStart.x - (double) e.getDistanceFromDragStartX() / ga.getWidth() * viewLen);
                scrollRow = (int) panStart.y - (int) ((float) e.getDistanceFromDragStartY() / rowH);
                clampScroll();
                viewChanged();
                return;
            }
            case Zoom:
            {
                viewLen = jlimit (0.25, 256.0, (double) panStart.x * std::pow (2.0, -e.getDistanceFromDragStartX() / 120.0));
                rowH = jlimit (5.0f, 30.0f, panStart.y * std::pow (2.0f, (float) -e.getDistanceFromDragStartY() / 120.0f));
                clampScroll();
                viewChanged();
                return;
            }
            case Keys:
            {
                const int p = pitchAt (pos.y);
                if (p != auditioning) { const bool was = audition; audition = true; auditionNote (p, 100); audition = was; }
                return;
            }
            case Marquee:
                marqueeTo = pos;
                marquee = true;
                repaint();
                return;
            case Lane:
            {
                const auto la = laneArea();
                const int v = jlimit (1, 127, roundToInt ((la.getBottom() - 3.0f - pos.y) / (la.getHeight() - 8.0f) * 127.0f));
                const int idx = laneNote;
                editClip ("Note Velocity", [idx, v] (Clip& cl)
                {
                    if (idx >= (int) cl.notes.size()) return;
                    const bool sel = cl.notes[(size_t) idx].selected;
                    for (int k = 0; k < (int) cl.notes.size(); ++k)
                        if (k == idx || (sel && cl.notes[(size_t) k].selected)) cl.notes[(size_t) k].velocity = v;
                });
                return;
            }
            case Offset: case EndFlag: case StartFlag:
            {
                const double b = jmax (0.0, snapRound (beatAt (pos.x)));
                const auto m = mode;
                const double minLen = jmax (grid(), 1.0 / 16.0);
                editClip (m == Offset ? "Clip Offset" : "Clip Length", [b, m, minLen] (Clip& cl)
                {
                    if (m == EndFlag) cl.length = jlimit (cl.loopStart + minLen, 256.0, b);
                    else if (m == StartFlag) cl.loopStart = jlimit (0.0, cl.length - minLen, b);
                    else cl.startOffset = jlimit (cl.loopStart, cl.length - 1.0e-3, b);
                });
                return;
            }
            case Move:
            {
                double dt = beatAt (pos.x) - beatAt ((float) e.getMouseDownX());
                dt = bankOf (ctx).snap ? snapRound (anchorBeat + dt) - anchorBeat : dt;
                double minStart = 1.0e9;
                for (auto& n : original) if (n.selected) minStart = jmin (minStart, n.start);
                dt = jmax (dt, -minStart);
                const int downIdx = scrollRow + (int) std::floor (((float) e.getMouseDownY() - gridArea().getY()) / rowH);
                const int nowIdx = scrollRow + (int) std::floor ((pos.y - gridArea().getY()) / rowH);
                int dp = 0;
                if (foldMode == 0) dp = downIdx - nowIdx;
                else
                {
                    const int p0 = rows[(size_t) jlimit (0, (int) rows.size() - 1, downIdx)], p1 = rows[(size_t) jlimit (0, (int) rows.size() - 1, nowIdx)];
                    dp = p1 - p0;
                }
                if (std::abs (dt - lastDt) < 1.0e-9 && dp == lastDp) return;
                lastDt = dt; lastDp = dp;
                const auto orig = original;
                editClip ("Move Notes", [orig, dt, dp] (Clip& cl) { cl.notes = orig; cl.moveSelected (dt, dp); });
                for (auto& n : orig) if (n.selected && dp != 0) { auditionNote (jlimit (0, 127, n.pitch + dp), n.velocity); break; }
                return;
            }
            case Resize:
            {
                double end = beatAt (pos.x);
                end = bankOf (ctx).snap ? snapRound (end) : end;
                const double d = end - anchorEnd;
                const double minLen = bankOf (ctx).snap ? grid() : 1.0 / 64.0;
                const auto orig = original;
                editClip ("Resize Notes", [orig, d, minLen] (Clip& cl) { cl.notes = orig; cl.resizeSelected (d, minLen); });
                return;
            }
            case None: default: return;
        }
    }

    void mouseUp (const MouseEvent& e) override
    {
        stopAudition();
        if (mode == Marquee)
        {
            const auto m = Rectangle<float> (marqueeFrom, marqueeTo);
            marquee = false;
            if (m.getWidth() < 3.0f && m.getHeight() < 3.0f)
            {
                // A click on empty grid: deselect, place the timeline.
                hasRange = false;
                pastePos = jmax (0.0, snap (beatAt (m.getX())));
                select ([] (Clip& cl) { cl.selectAll (false); });
            }
            else
            {
                const double t0 = beatAt (m.getX()), t1 = beatAt (m.getRight());
                const int p0 = pitchAt (m.getBottom()), p1 = pitchAt (m.getY());
                bool any = false;
                for (auto& n : clipOf (ctx).notes) any |= n.start < t1 && n.end() > t0 && n.pitch >= p0 && n.pitch <= p1;
                if (any)
                {
                    hasRange = false;
                    const bool add = e.mods.isShiftDown();
                    select ([t0, t1, p0, p1, add] (Clip& cl) { cl.selectRect (t0, t1, p0, p1, add); });
                }
                else
                {
                    hasRange = true;
                    range0 = jmax (0.0, snap (t0));
                    range1 = jmax (range0 + grid(), snapRound (t1));
                    pastePos = range0;
                    select ([] (Clip& cl) { cl.selectAll (false); });
                }
            }
            repaint();
        }
        else if (mode != None && mode != Pan && mode != Zoom && mode != Keys)
            endDrag (ctx);
        if (mode == Move || mode == Resize)
        {
            double t0, t1;
            if (clipOf (ctx).selectionRange (t0, t1)) pastePos = t1;
        }
        mode = None;
        lastDt = 1.0e9; lastDp = 0;
    }

    void mouseMove (const MouseEvent& e) override
    {
        const int i = noteAtPoint (e.position);
        if (i >= 0 && e.position.x > noteRect (clipOf (ctx).notes[(size_t) i]).getRight() - 5.0f) setMouseCursor (MouseCursor::LeftRightResizeCursor);
        else if (rulerArea().contains (e.position)) setMouseCursor (MouseCursor::PointingHandCursor);
        else setMouseCursor (MouseCursor::NormalCursor);
        if (mode == Marquee) marquee = true;
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        const auto pos = e.position;
        if (! gridArea().contains (pos) || e.mods.isPopupMenu()) return;
        const int i = noteAtPoint (pos);
        if (i >= 0)
        {
            const auto n = clipOf (ctx).notes[(size_t) i];
            editClip ("Delete Note", [n] (Clip& cl) { cl.removeNoteAt (n.start + 1.0e-6, n.pitch); });
            return;
        }
        ClipNote n;
        n.start = jmax (0.0, snap (beatAt (pos.x)));
        n.length = grid();
        n.pitch = pitchAt (pos.y);
        n.velocity = 100;
        n.selected = true;
        lastStart = n.start; lastPitch = n.pitch;
        editClip ("Add Note", [n] (Clip& cl) { cl.selectAll (false); cl.addNote (n); if (n.end() > cl.length) cl.length = std::ceil (n.end() / 4.0) * 4.0; });
        auditionNote (n.pitch, n.velocity);
        stopAudition();
    }

    void mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w) override
    {
        const float d = ! same (w.deltaY, 0.0f) ? w.deltaY : w.deltaX;
        if (same (d, 0.0f)) return;
        const bool onRuler = e.position.y < kRulerH;
        if (onRuler || e.mods.isCommandDown())
        {
            const double anchor = beatAt (e.position.x);
            const double nl = jlimit (0.25, 256.0, viewLen * (d > 0 ? 0.8 : 1.25));
            viewStart = jmax (0.0, anchor - (anchor - viewStart) * nl / viewLen);
            viewLen = nl;
        }
        else if (e.mods.isAltDown()) rowH = jlimit (5.0f, 30.0f, rowH * (d > 0 ? 1.15f : 0.87f));
        else if (e.mods.isShiftDown() || same (w.deltaY, 0.0f)) viewStart = jmax (0.0, viewStart - (double) d * viewLen * 0.25);
        else scrollRow -= d > 0 ? 3 : -3;
        clampScroll();
        viewChanged();
    }

    // ---- commands ------------------------------------------------------------
    void doCut()
    {
        auto& cb = noteClipboard()[&ctx.proc];
        if (hasRange)
        {
            const double a = range0, b = range1;
            cb = { clipOf (ctx).copyRange (a, b), true, b - a };
            editClip ("Cut", [a, b] (Clip& cl) { cl.cutRange (a, b); });
        }
        else
        {
            cb = { clipOf (ctx).copySelected(), false, 0.0 };
            editClip ("Cut", [] (Clip& cl) { cl.cutSelected(); });
        }
    }
    void doCopy()
    {
        auto& cb = noteClipboard()[&ctx.proc];
        if (hasRange) cb = { clipOf (ctx).copyRange (range0, range1), true, range1 - range0 };
        else cb = { clipOf (ctx).copySelected(), false, 0.0 };
    }
    void doPaste()
    {
        auto it = noteClipboard().find (&ctx.proc);
        if (it == noteClipboard().end()) return;
        const auto cb = it->second;
        const double at = pastePos;
        editClip ("Paste", [cb, at] (Clip& cl)
        {
            cl.selectAll (false);
            if (cb.range) cl.pasteRange (cb.notes, at, cb.rangeLength);
            else cl.pasteNotes (cb.notes, at);
        });
        double t0, t1;
        if (clipOf (ctx).selectionRange (t0, t1)) pastePos = t1;
        else if (cb.range) pastePos = at + cb.rangeLength;
    }
    void doDuplicate()
    {
        editClip ("Duplicate", [] (Clip& cl) { cl.duplicateSelected(); });
        double t0, t1;
        if (clipOf (ctx).selectionRange (t0, t1)) pastePos = t1;
    }

    void setGrid (double g, const String& label = "Grid")
    {
        g = jlimit (1.0 / 48.0, 8.0, g);
        ctx.proc.editPatch ({}, [g] (Patch& p) { p.clips.grid = g; });
        (void) label;
        background = {};
        repaint();
    }
    void toggleTriplets() { const double g = bankOf (ctx).grid; setGrid (isTriplet (g) ? g * 1.5 : g * 2.0 / 3.0); }
    void toggleSnap() { ctx.proc.editPatch ({}, [] (Patch& p) { p.clips.snap = ! p.clips.snap; }); }
    void toggleAutoGrid()
    {
        auto& s = ctx.proc.uiState;
        s.setProperty ("clipAutoGrid", ! (bool) s.getProperty ("clipAutoGrid", false), nullptr);
        background = {};
        repaint();
    }

    Menu gridMenu()
    {
        Menu m;
        SafePointer<PianoRoll> safe (this);
        const double g = bankOf (ctx).grid;
        m.add ("Double Grid", [safe, g] { if (safe != nullptr) safe->setGrid (g * 2.0); }).withShortcut (shortcut ("1"));
        m.add ("Half Grid", [safe, g] { if (safe != nullptr) safe->setGrid (g * 0.5); }).withShortcut (shortcut ("2"));
        m.add ("Triplets", [safe] { if (safe != nullptr) safe->toggleTriplets(); }, isTriplet (g)).withShortcut (shortcut ("3"));
        m.add ("Disable Snap", [safe] { if (safe != nullptr) safe->toggleSnap(); }, ! bankOf (ctx).snap).withShortcut (shortcut ("4"));
        m.add ("Auto Adjust Grid", [safe] { if (safe != nullptr) safe->toggleAutoGrid(); }, (bool) ctx.proc.uiState.getProperty ("clipAutoGrid", false)).withShortcut (shortcut ("5"));
        return m;
    }

    Menu noteMenu()
    {
        Menu m;
        SafePointer<PianoRoll> safe (this);
        const bool any = clipOf (ctx).numSelected() > 0 || hasRange;
        const bool sel = clipOf (ctx).numSelected() > 0;
        const bool canPaste = noteClipboard().count (&ctx.proc) > 0;
        auto act = [safe] (std::function<void (PianoRoll&)> f) { return [safe, f] { if (safe != nullptr) f (*safe); }; };
        auto clipAct = [safe] (const String& label, std::function<void (Clip&)> f) { return [safe, label, f] { if (safe != nullptr) safe->editClip (label, f); }; };
        const double gr = grid();
        const int key = scales::keyFromParam (ctx.proc.get (ids().global.key));
        const int sc = ctx.proc.params.geti (ids().global.scale);
        m.add ("Cut", act ([] (PianoRoll& r) { r.doCut(); }), false, any).withShortcut (shortcut ("X"));
        m.add ("Copy", act ([] (PianoRoll& r) { r.doCopy(); }), false, any).withShortcut (shortcut ("C"));
        m.add ("Paste", act ([] (PianoRoll& r) { r.doPaste(); }), false, canPaste).withShortcut (shortcut ("V"));
        m.add ("Duplicate", act ([] (PianoRoll& r) { r.doDuplicate(); }), false, sel).withShortcut (shortcut ("D"));
        m.add ("Delete", clipAct ("Delete Notes", [] (Clip& c) { c.deleteSelected(); }), false, sel).withShortcut ("Backspace");
        m.separator();
        m.add ("Chop", clipAct ("Chop", [gr] (Clip& c) { c.chopSelected (gr); }), false, sel).withShortcut (shortcut ("U"));
        m.add ("Conform to Scale", clipAct ("Conform to Scale", [key, sc] (Clip& c) { c.conformSelected (key, sc); }), false, sel && key >= 0).withShortcut (shortcut ("K"));
        m.add ("Legato", clipAct ("Legato", [] (Clip& c) { c.legatoSelected(); }), false, sel).withShortcut (shortcut ("L"));
        m.add ("Mute", clipAct ("Mute", [] (Clip& c) { c.toggleMuteSelected(); }), false, sel).withShortcut ("0");
        m.add ("Quantize", clipAct ("Quantize", [gr] (Clip& c) { c.quantiseSelected (gr); }), false, sel).withShortcut (shortcut ("Q"));
        m.add ("Reverse", clipAct ("Reverse", [] (Clip& c) { c.reverseSelected(); }), false, sel).withShortcut (shortcut ("R"));
        m.add ("Scale Time 50%", clipAct ("Scale Time 50%", [] (Clip& c) { c.scaleTimeSelected (0.5); }), false, sel).withShortcut ("/");
        m.add ("Scale Time 200%", clipAct ("Scale Time 200%", [] (Clip& c) { c.scaleTimeSelected (2.0); }), false, sel).withShortcut ("*");
        m.separator();
        m.add ("Double Entire clip", clipAct ("Double Clip", [] (Clip& c) { c.doubleClip(); })).withShortcut (shortcut ("E"));
        m.add ("Scale Entire clip 50%", clipAct ("Scale Clip 50%", [] (Clip& c) { c.scaleClip (0.5); }));
        m.add ("Scale Entire clip 200%", clipAct ("Scale Clip 200%", [] (Clip& c) { c.scaleClip (2.0); }));
        m.separator();
        m.add ("Select All", act ([] (PianoRoll& r) { r.select ([] (Clip& c) { c.selectAll(); }); r.hasRange = false; }), false, ! clipOf (ctx).notes.empty())
         .withShortcut (shortcut ("A"));
        return m;
    }

    Menu offsetMenu()
    {
        Menu m;
        m.header ("Clip " + String (bankOf (ctx).selected + 1) + " Offset");
        SafePointer<PianoRoll> safe (this);
        m.add ("Wrap Clip to Begin Here", [safe] { if (safe != nullptr) safe->editClip ("Wrap Clip", [] (Clip& c) { c.wrapToStartOffset(); }); });
        Menu q = listMenu (seqchoices::offsetQuants(), clipOf (ctx).offsetQuant, [safe] (int i)
        {
            if (safe != nullptr) safe->editClip ("Offset Quantize", [i] (Clip& c) { c.offsetQuant = i; });
        });
        m.sub ("Quantize", q);
        m.separator();
        m.sub ("Mod Source", Menu(), false, false);
        m.add ("Bypass Modulator", [] {}, false, false);
        m.add ("Remove Modulator", [] {}, false, false);
        m.add ("Remove All Modulators", [] {}, false, false);
        return m;
    }

    bool keyPressed (const KeyPress& k) override
    {
        const auto mods = k.getModifiers();
        const int code = k.getKeyCode();
        const double gr = grid();
        const bool sel = clipOf (ctx).numSelected() > 0;
        if (mods.isCommandDown())
        {
            switch (code)
            {
                case 'X': doCut(); return true;
                case 'C': doCopy(); return true;
                case 'V': doPaste(); return true;
                case 'D': doDuplicate(); return true;
                case 'U': editClip ("Chop", [gr] (Clip& c) { c.chopSelected (gr); }); return true;
                case 'K': { const int key = scales::keyFromParam (ctx.proc.get (ids().global.key)); const int sc = ctx.proc.params.geti (ids().global.scale);
                            if (key >= 0) editClip ("Conform to Scale", [key, sc] (Clip& c) { c.conformSelected (key, sc); });
                            return true; }
                case 'L': editClip ("Legato", [] (Clip& c) { c.legatoSelected(); }); return true;
                case 'Q': editClip ("Quantize", [gr] (Clip& c) { c.quantiseSelected (gr); }); return true;
                case 'R': editClip ("Reverse", [] (Clip& c) { c.reverseSelected(); }); return true;
                case 'E': editClip ("Double Clip", [] (Clip& c) { c.doubleClip(); }); return true;
                case 'A': select ([] (Clip& c) { c.selectAll(); }); hasRange = false; return true;
                case '1': setGrid (bankOf (ctx).grid * 2.0); return true;
                case '2': setGrid (bankOf (ctx).grid * 0.5); return true;
                case '3': toggleTriplets(); return true;
                case '4': toggleSnap(); return true;
                case '5': toggleAutoGrid(); return true;
                default: break;
            }
            if (code != KeyPress::upKey && code != KeyPress::downKey) return false;
        }
        if (code == KeyPress::backspaceKey || code == KeyPress::deleteKey) { editClip ("Delete Notes", [] (Clip& c) { c.deleteSelected(); }); return true; }
        if (! sel) return false;
        if (k.getTextCharacter() == '0') { editClip ("Mute", [] (Clip& c) { c.toggleMuteSelected(); }); return true; }
        if (k.getTextCharacter() == '/') { editClip ("Scale Time 50%", [] (Clip& c) { c.scaleTimeSelected (0.5); }); return true; }
        if (k.getTextCharacter() == '*') { editClip ("Scale Time 200%", [] (Clip& c) { c.scaleTimeSelected (2.0); }); return true; }
        if (code == KeyPress::upKey || code == KeyPress::downKey)
        {
            const int d = (code == KeyPress::upKey ? 1 : -1) * (mods.isShiftDown() ? 12 : 1);
            editClip ("Transpose Notes", [d] (Clip& c) { c.moveSelected (0.0, d); });
            return true;
        }
        if (code == KeyPress::leftKey || code == KeyPress::rightKey)
        {
            const double d = code == KeyPress::rightKey ? gr : -gr;
            if (mods.isShiftDown()) editClip ("Resize Notes", [d, gr] (Clip& c) { c.resizeSelected (d, gr); });
            else editClip ("Move Notes", [d] (Clip& c) { c.moveSelected (d, 0); });
            return true;
        }
        return false;
    }

    double lastStart = -1.0;
    int lastPitch = -1;

private:
    int nearestNote (float x) const
    {
        int best = -1;
        float bd = 10.0f;
        const auto& ns = clipOf (ctx).notes;
        for (int i = 0; i < (int) ns.size(); ++i)
        {
            const float d = std::abs (xOf (ns[(size_t) i].start) + 2.75f - x);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }

    enum Mode { None, Move, Resize, Marquee, Lane, Offset, EndFlag, StartFlag, Pan, Zoom, Keys };
    PanelContext& ctx;
    Mode mode = None;
    bool marquee = false;
    Point<float> marqueeFrom, marqueeTo, panStart;
    std::vector<ClipNote> original;
    double anchorBeat = 0.0, anchorEnd = 0.0, lastDt = 1.0e9;
    int lastDp = 0, laneNote = -1, auditioning = -1;
    float playhead = -1.0f;
    std::vector<uint8_t> shown;
    int shownSlot = -1;
    std::tuple<int, int, double, bool> shownScale { -1, -1, 0.0, false };
    Image background;
};

// ---------------------------------------------------------------------------
// The 12 clip slots.
// ---------------------------------------------------------------------------
class ClipSlots final : public Component, public Refreshable
{
public:
    explicit ClipSlots (PanelContext& c) : ctx (c) { ctx.add (this); }
    ~ClipSlots() override { ctx.remove (this); }

    Rectangle<float> slotRect (int i) const
    {
        const float gap = 6.0f, w = ((float) getWidth() - gap * 11.0f) / 12.0f;
        return { (w + gap) * (float) i, 0.0f, w, (float) getHeight() };
    }
    Rectangle<float> playButton (int i) const { return slotRect (i).removeFromBottom (24.0f).reduced (3.0f); }
    int slotAt (Point<float> p) const { for (int i = 0; i < 12; ++i) if (slotRect (i).contains (p)) return i; return -1; }

    void refresh() override
    {
        const auto& t = ctx.proc.seqTelemetry();
        const uint32_t playing = t.clipPlaying.load(), pending = t.clipPending.load();
        Node n;
        bankOf (ctx).save (n);
        auto sig = n.toBinary();
        if (sig != shown || playing != shownPlaying || pending != shownPending)
        {
            shown = std::move (sig); shownPlaying = playing; shownPending = pending;
            repaint();
        }
    }

    void paint (Graphics& g) override
    {
        const auto& b = bankOf (ctx);
        for (int i = 0; i < 12; ++i)
        {
            const auto r = slotRect (i);
            const auto& c = b.clips[(size_t) i];
            const bool sel = b.selected == i, playing = (shownPlaying >> i) & 1u, pending = (shownPending >> i) & 1u;
            g.setColour (colours::black);
            g.fillRect (r);
            g.setColour (sel ? clipColour : colours::inkDim);
            g.drawRect (r, sel ? 2.0f : 1.0f);
            auto head = r.withHeight (18.0f).reduced (1.0f);
            if (sel) { g.setColour (clipColour); g.fillRect (head); }
            drawText (g, String (b.clipName (i)), head.reduced (5.0f, 0.0f), 11.5f, sel ? colours::black : colours::ink, true, Justification::centredLeft);
            if (b.previewClip == i) drawText (g, "P", head.reduced (5.0f, 0.0f), 10.5f, sel ? colours::black : clipColour, true, Justification::centredRight);
            // Mini preview.
            auto pv = r.withTrimmedTop (21.0f).withTrimmedBottom (27.0f).reduced (5.0f, 2.0f);
            g.setColour (colours::panel.darker (0.4f));
            g.fillRect (pv);
            double t0, t1; int lo, hi;
            if (c.noteBounds (t0, t1, lo, hi))
            {
                lo -= 1; hi += 1;
                const double len = jmax (c.length, t1);
                g.setColour (sel ? clipColour : kNote.withAlpha (0.8f));
                for (const auto& n : c.notes)
                {
                    const float x = pv.getX() + (float) (n.start / len) * pv.getWidth();
                    const float w = jmax (1.5f, (float) (n.length / len) * pv.getWidth());
                    const float y = pv.getBottom() - (float) (n.pitch - lo + 1) / (float) (hi - lo + 1) * pv.getHeight();
                    g.fillRect (x, y, w, jmax (1.5f, pv.getHeight() / (float) (hi - lo + 1)));
                }
            }
            else
                drawText (g, "EMPTY", pv, 10.0f, colours::inkFaint, true);
            // Play / stop.
            const auto pb = playButton (i);
            g.setColour (playing ? clipColour : (pending ? clipColour.withAlpha (0.35f) : colours::panel));
            g.fillRect (pb);
            g.setColour (playing || sel ? clipColour : colours::inkDim);
            g.drawRect (pb, 1.0f);
            const auto icon = pb.withSizeKeepingCentre (14.0f, 14.0f);
            if (playing) { g.setColour (colours::black); g.fillRect (icon.reduced (2.5f)); }
            else drawArrow (g, icon, 1, sel ? colours::ink : colours::inkDim);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        const int i = slotAt (e.position);
        if (i < 0) return;
        if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), slotMenu (i)); return; }
        ctx.proc.editPatch ({}, [i] (Patch& p) { p.clips.selected = i; });
        if (playButton (i).contains (e.position)) ctx.proc.post ({ SeqCommand::ClipToggle, (int8_t) i });
        refresh();
    }

    Menu slotMenu (int i)
    {
        auto& proc = ctx.proc;
        SafePointer<ClipSlots> safe (this);
        Menu m;
        m.add ("Rename Clip", [safe, i] { if (safe != nullptr) safe->rename (i); });
        m.add ("Copy Clip", [&proc, i] { clipClipboard()[&proc] = proc.patch().clips.copyClip (i); });
        m.add ("Paste Clip", [&proc, i]
        {
            auto it = clipClipboard().find (&proc);
            if (it == clipClipboard().end()) return;
            const Clip c = it->second;
            proc.editPatch ("Paste Clip", [i, c] (Patch& p) { p.clips.pasteClip (i, c); });
        }, false, clipClipboard().count (&proc) > 0);
        m.add ("Erase Clip", [&proc, i] { proc.editPatch ("Erase Clip", [i] (Patch& p) { p.clips.eraseClip (i); }); });
        m.add ("Set as Preview Clip", [&proc, i] { proc.editPatch ("Set as Preview Clip", [i] (Patch& p) { p.clips.setPreviewClip (i); }); },
               bankOf (ctx).previewClip == i);
        return m;
    }

    void rename (int i)
    {
        if (! ctx.editValue) return;
        auto& proc = ctx.proc;
        renameTarget.setBounds (slotRect (i).withHeight (22.0f).toNearestInt());
        addChildComponent (renameTarget);
        ctx.editValue (renameTarget, String (bankOf (ctx).clipName (i)), [&proc, i] (const String& t)
        {
            const auto name = t.trim().toStdString();
            proc.editPatch ("Rename Clip", [i, name] (Patch& p) { p.clips.renameClip (i, name); });
        });
    }

private:
    PanelContext& ctx;
    Component renameTarget;
    std::vector<uint8_t> shown;
    uint32_t shownPlaying = 0xffffffff, shownPending = 0;
};

// ---------------------------------------------------------------------------
class ClipEditor final : public Component, public Refreshable
{
public:
    ClipEditor (PanelContext& c, std::function<void()> close)
        : ctx (c), onClose (std::move (close)), closeBtn ("X"),
          bank (c, "BANK", 40), mono (c, "MONO", true), poly (c, "POLY", true), editAll (c, "EDIT ALL", true),
          clipName (c, "CLIP:", 64), length (c, "LENGTH:", 64), kbSpan (c, "KB SPAN:", 64), trans (c, "TRANS:", 64), mode (c, "MODE:", 64),
          time (c, "TIME:", 50), rate (c, "RATE:", 64), bpm (c, "BPM"), trip (c, "TRIP", true), dot (c, "DOT", true),
          launchQuant (c, "LAUNCH Q:", 64), retrig (c, "RETRIG"), veloTrig (c, "VELO TRIG"), noteGate (c, "NOTE GATE"),
          record (c, "RECORD", true), commit (c, "+", true), discard (c, "UNDO", true), done (c, "DONE", true),
          overdub (c, "OVERDUB", true), extend (c, "EXTEND", true), metronome (c, "METRONOME"),
          gridField (c), fold (c, "FOLD", true), fit (c, "FIT", true), listen (c, "PREVIEW", true),
          roll (c), slots (c), midiOut (c)
    {
        ctx.add (this);
        closeBtn.setTooltip ("Close the CLIP editor");
        closeBtn.onClick = [this] (const ModifierKeys&) { if (onClose) onClose(); };
        addAndMakeVisible (closeBtn);

        // GLOBAL
        bank.arrows = true;
        bank.text = [this] { return bankLabel(); };
        bank.menu = [this] { return bankMenu(); };
        bank.onStep = [this] (int d) { stepBank (d); };
        bank.setTooltip ("Clip bank: click for the bank menu, arrows step through banks");
        for (auto* b : { &mono, &poly, &editAll, &trip, &dot, &record, &commit, &discard, &done, &overdub, &extend, &fold, &fit, &listen }) b->accent = clipColour;
        for (auto* b : { &bpm, &retrig, &veloTrig, &noteGate, &metronome }) b->accent = clipColour;
        mono.ticked = [this] { return bankOf (ctx).triggerMode == 0; };
        poly.ticked = [this] { return bankOf (ctx).triggerMode == 1; };
        mono.onToggle = [this] { ctx.proc.editPatch ("Trigger Mode", [] (Patch& p) { p.clips.triggerMode = 0; }); };
        poly.onToggle = [this] { ctx.proc.editPatch ("Trigger Mode", [] (Patch& p) { p.clips.triggerMode = 1; }); };
        mono.setTooltip ("MONO: one clip plays at a time");
        poly.setTooltip ("POLY: several clips can play at once");
        editAll.ticked = [this] { return bankOf (ctx).editAll; };
        editAll.onToggle = [this] { ctx.proc.editPatch ("Edit All", [] (Patch& p) { p.clips.editAll = ! p.clips.editAll; }); };
        editAll.setTooltip ("EDIT ALL: clip-setting edits apply to all 12 clips (hold Alt for a one-off)");

        // CLIP SETTINGS
        clipName.arrows = true;
        clipName.text = [this] { return String (bankOf (ctx).clipName (bankOf (ctx).selected)); };
        clipName.menu = [this] { return clipMenu(); };
        clipName.onStep = [this] (int d)
        {
            const int v = (bankOf (ctx).selected + d + 12) % 12;
            ctx.proc.editPatch ({}, [v] (Patch& p) { p.clips.selected = v; });
        };
        clipName.setTooltip ("The selected clip: click to load, save or rename; arrows select the previous/next clip");
        length.text = [this] { return formatLength (clipOf (ctx).loopLength()); };
        length.onStep = [this] (int d)
        {
            const double step = lengthStepFor (length.downFrac);
            editClip ("Clip Length", [step, d] (Clip& clip) { clip.length = jlimit (clip.loopStart + 0.25, clip.loopStart + 256.0, clip.length + step * d); clip.startOffset = jmin (clip.startOffset, clip.length - 0.001); });
        };
        length.onDoubleClick = [this]
        {
            if (! ctx.editValue) return;
            ctx.editValue (length, formatLength (clipOf (ctx).loopLength()), [this] (const String& t)
            {
                double b;
                if (parseLength (t, b)) editClip ("Clip Length", [b] (Clip& clip) { clip.length = clip.loopStart + jlimit (0.25, 256.0, b); });
            });
        };
        length.setTooltip ("LENGTH: bars . beats . 16ths (drag a segment, double-click to type)");
        setupList (kbSpan, &seqchoices::kbSpans, [] (const ClipSettings& s) { return s.kbSpan; }, [] (ClipSettings& s, int v) { s.kbSpan = v; }, "KB Span");
        kbSpan.setTooltip ("KB SPAN: Mono/Poly play the clip transposed by the incoming note; Offset starts it at a position per key");
        trans.arrows = true;
        trans.text = [this] { const int t = clipOf (ctx).settings.transpose; return t == 0 ? String ("0") : (t > 0 ? "+" : "") + String (t); };
        trans.onStep = [this] (int d)
        {
            const int v = jlimit (-48, 48, clipOf (ctx).settings.transpose + d);
            editSettings ("Clip Transpose", [v] (ClipSettings& s) { s.transpose = v; });
        };
        setupList (mode, &seqchoices::clipModes, [] (const ClipSettings& s) { return s.mode; }, [] (ClipSettings& s, int v) { s.mode = v; }, "Clip Mode");
        setupList (time, &seqchoices::clipTimes, [] (const ClipSettings& s) { return s.time; }, [] (ClipSettings& s, int v) { s.time = v; }, "Clip Time");
        time.active = [this] { const int m = clipOf (ctx).settings.mode; return m >= 1 && m <= 4; };
        time.text = [this] { const auto& s = clipOf (ctx).settings; return (s.mode >= 1 && s.mode <= 4) ? String (seqchoices::clipTimes()[(size_t) s.time]) : String ("-"); };
        setupList (rate, &seqchoices::clipRates, [] (const ClipSettings& s) { return s.rate; }, [] (ClipSettings& s, int v) { s.rate = v; }, "Clip Rate");
        rate.dropArrow = false;
        rate.arrows = true;
        bpm.ticked = [this] { return clipOf (ctx).settings.bpm; };
        bpm.onToggle = [this] { const bool v = ! clipOf (ctx).settings.bpm; editSettings ("Clip BPM", [v] (ClipSettings& s) { s.bpm = v; }); };
        bpm.setTooltip ("BPM: follow the host tempo (off: free tempo)");
        trip.ticked = [this] { return clipOf (ctx).settings.triplet; };
        trip.onToggle = [this] { const bool v = ! clipOf (ctx).settings.triplet; editSettings ("Clip Triplet", [v] (ClipSettings& s) { s.triplet = v; }); };
        dot.ticked = [this] { return clipOf (ctx).settings.dotted; };
        dot.onToggle = [this] { const bool v = ! clipOf (ctx).settings.dotted; editSettings ("Clip Dotted", [v] (ClipSettings& s) { s.dotted = v; }); };
        setupList (launchQuant, &seqchoices::launchQuants, [] (const ClipSettings& s) { return s.launchQuant; }, [] (ClipSettings& s, int v) { s.launchQuant = v; }, "Launch Quant");
        launchQuant.setTooltip ("LAUNCH QUANT: grid on which a launched clip starts");
        retrig.ticked = [this] { return clipOf (ctx).settings.retrig; };
        retrig.onToggle = [this] { const bool v = ! clipOf (ctx).settings.retrig; editSettings ("Clip Retrig", [v] (ClipSettings& s) { s.retrig = v; }); };
        retrig.setTooltip ("RETRIG: restart from the start when triggered (off: follow the host clock)");
        veloTrig.ticked = [this] { return clipOf (ctx).settings.veloTrig; };
        veloTrig.onToggle = [this] { const bool v = ! clipOf (ctx).settings.veloTrig; editSettings ("Velo Trig", [v] (ClipSettings& s) { s.veloTrig = v; }); };
        veloTrig.setTooltip ("VELO TRIG: scale note velocities by the triggering note's velocity");
        noteGate.ticked = [this] { return clipOf (ctx).settings.noteGate; };
        noteGate.onToggle = [this] { const bool v = ! clipOf (ctx).settings.noteGate; editSettings ("Note Gate", [v] (ClipSettings& s) { s.noteGate = v; }); };
        noteGate.setTooltip ("NOTE GATE: a MIDI-triggered clip stops on note-off");

        // RECORD
        record.fontH = 12.0f;
        record.ticked = [] { return false; };
        record.onToggle = [this] { ctx.proc.post ({ SeqCommand::RecordStart, (int8_t) bankOf (ctx).selected }); };
        record.setTooltip ("Record into the selected clip (it plays while recording)");
        commit.ticked = [] { return false; };
        commit.onToggle = [this] { const int s = recSlot(); ctx.proc.editPatch ("Commit Take", [s] (Patch& p) { p.clips.clips[(size_t) s].commitTakes(); }); };
        commit.setTooltip ("Commit the latest take");
        discard.ticked = [] { return false; };
        discard.onToggle = [this] { const int s = recSlot(); ctx.proc.editPatch ("Discard Take", [s] (Patch& p) { p.clips.clips[(size_t) s].discardTakes(); }); };
        discard.setTooltip ("Discard the uncommitted take");
        done.ticked = [] { return true; };
        done.onToggle = [this]
        {
            const int s = recSlot();
            ctx.proc.post ({ SeqCommand::RecordStop, (int8_t) s });
            ctx.proc.editPatch ("Record", [s] (Patch& p) { p.clips.clips[(size_t) s].commitTakes(); });
        };
        done.setTooltip ("Commit everything and stop recording (the clip keeps playing)");
        overdub.ticked = [this] { return bankOf (ctx).recordMode == 0; };
        extend.ticked = [this] { return bankOf (ctx).recordMode == 1; };
        overdub.onToggle = [this] { ctx.proc.editPatch ("Record Mode", [] (Patch& p) { p.clips.recordMode = 0; }); };
        extend.onToggle = [this] { ctx.proc.editPatch ("Record Mode", [] (Patch& p) { p.clips.recordMode = 1; }); };
        overdub.setTooltip ("OVERDUB: loop between the flags, adding notes on every pass");
        extend.setTooltip ("EXTEND: the clip grows while recording");
        metronome.ticked = [this] { return bankOf (ctx).metronome; };
        metronome.onToggle = [this] { ctx.proc.editPatch ("Metronome", [] (Patch& p) { p.clips.metronome = ! p.clips.metronome; }); };

        // Piano roll header.
        gridField.dropArrow = true;
        gridField.text = [this] { return gridName(); };
        gridField.onStep = [this] (int d) { roll.setGrid (d > 0 ? bankOf (ctx).grid * 2.0 : bankOf (ctx).grid * 0.5); };
        gridField.menu = [this] { return roll.gridMenu(); };
        gridField.contextMenu = [this] { return roll.gridMenu(); };
        gridField.setTooltip ("Grid: drag to change, click or right-click for the grid menu");
        fold.ticked = [this] { return roll.foldMode != 0; };
        fold.onToggle = [this]
        {
            const bool cmd = ModifierKeys::currentModifiers.isCommandDown();
            roll.foldMode = roll.foldMode != 0 ? 0 : (cmd ? 2 : 1);
            roll.rebuildRows();
            if (roll.foldMode != 0) roll.zoomToFit();
        };
        fold.setTooltip ("FOLD: show only used (or in-scale) rows; Cmd-click: in-scale rows plus used rows");
        fit.ticked = [] { return false; };
        fit.onToggle = [this] { roll.zoomToFit(); };
        fit.setTooltip ("Zoom to fit the notes");
        roll.audition = ctx.proc.uiState.getProperty ("clipAudition", false);
        listen.ticked = [this] { return roll.audition; };
        listen.onToggle = [this] { roll.audition = ! roll.audition; ctx.proc.uiState.setProperty ("clipAudition", roll.audition, nullptr); };
        listen.setTooltip ("Note preview: clicking a note plays it");

        for (Component* comp : std::initializer_list<Component*> { &bank, &mono, &poly, &editAll, &clipName, &length, &kbSpan, &trans, &mode, &time, &rate, &bpm, &trip, &dot,
                                                                   &launchQuant, &retrig, &veloTrig, &noteGate, &overdub, &extend, &metronome,
                                                                   &gridField, &fold, &fit, &listen, &roll, &slots, &midiOut })
            addAndMakeVisible (comp);
        for (Component* comp : std::initializer_list<Component*> { &record, &commit, &discard, &done }) addChildComponent (comp);
        refresh();
    }

    ~ClipEditor() override { ctx.remove (this); }

    int recSlot() const { const int s = ctx.proc.seqTelemetry().recordingSlot.load(); return s >= 0 ? s : bankOf (ctx).selected; }

    String gridName() const
    {
        const double g = bankOf (ctx).grid;
        const bool triplet = PianoRoll::isTriplet (g);
        const double base = triplet ? g * 1.5 : g;
        String s = base >= 4.0 ? String (roundToInt (base / 4.0)) + " bar" : "1/" + String (roundToInt (4.0 / base));
        if (triplet) s << "T";
        if (! bankOf (ctx).snap) s = "(" + s + ")";
        return s;
    }

    void editClip (const String& label, std::function<void (Clip&)> fn)
    {
        seq::edit (ctx, label, [fn] (Patch& p) { fn (p.clips.clips[(size_t) p.clips.selected]); });
    }
    void editSettings (const String& label, std::function<void (ClipSettings&)> fn)
    {
        const bool alt = altHeld();
        seq::edit (ctx, label, [fn, alt] (Patch& p) { p.clips.applySettings (p.clips.selected, alt, fn); });
    }

    void setupList (Field& f, const std::vector<std::string>& (*list)(), std::function<int (const ClipSettings&)> get,
                    std::function<void (ClipSettings&, int)> set, const String& label)
    {
        f.dropArrow = true;
        f.text = [this, list, get] { return String (list()[(size_t) jlimit (0, (int) list().size() - 1, get (clipOf (ctx).settings))]); };
        f.onStep = [this, list, get, set, label] (int d)
        {
            const int v = jlimit (0, (int) list().size() - 1, get (clipOf (ctx).settings) + d);
            editSettings (label, [set, v] (ClipSettings& s) { set (s, v); });
        };
        f.menu = [this, list, get, set, label]
        {
            return listMenu (list(), get (clipOf (ctx).settings), [this, set, label] (int i) { editSettings (label, [set, i] (ClipSettings& s) { set (s, i); }); });
        };
    }

    // ---- banks and clips -----------------------------------------------------
    String bankLabel() const
    {
        const auto& b = bankOf (ctx);
        ClipBank init; init.init();
        return String (b.name) + (isModified (&ctx.proc, "clip", b.name, signature (b), signature (init)) ? "*" : "");
    }

    void loadBank (ClipBank b, const String& name)
    {
        b.name = name.toStdString();
        b.lockModule = bankOf (ctx).lockModule;
        b.grid = bankOf (ctx).grid;
        b.snap = bankOf (ctx).snap;
        setBaseline (&ctx.proc, "clip", b.name, signature (b));
        ctx.proc.editPatch ("Load Clip Bank", [b] (Patch& p) { p.clips = b; });
        roll.rebuildRows();
    }

    Menu bankMenu()
    {
        Menu m, factory, user;
        const auto cur = String (bankOf (ctx).name);
        SafePointer<ClipEditor> safe (this);
        std::map<String, Menu> folders;
        std::vector<String> folderOrder;
        bool inFactory = false;
        for (auto& fb : factoryBanks())
        {
            auto make = fb.make; auto name = fb.name;
            auto act = [safe, make, name] { if (safe != nullptr) safe->loadBank (make(), name); };
            inFactory |= name == cur;
            if (fb.folder.isEmpty()) continue;
            if (! folders.count (fb.folder)) folderOrder.push_back (fb.folder);
            folders[fb.folder].add (name, act, name == cur);
        }
        for (auto& f : folderOrder)
        {
            bool ticked = false;
            for (auto& it : folders[f].items()) ticked |= it.ticked;
            factory.sub (f, folders[f], ticked);
        }
        for (auto& fb : factoryBanks())
            if (fb.folder.isEmpty())
            {
                auto make = fb.make; auto name = fb.name;
                factory.add (name, [safe, make, name] { if (safe != nullptr) safe->loadBank (make(), name); }, name == cur);
            }
        bool inUser = false;
        for (auto& f : userFiles ("Clip Banks"))
        {
            const auto n = f.getFileNameWithoutExtension();
            inUser |= n == cur;
            user.add (n, [safe, f, n]
            {
                Node node;
                if (safe == nullptr || ! loadNode (f, node)) return;
                ClipBank b;
                b.load (node);
                safe->loadBank (b, n);
            }, n == cur);
        }
        m.sub ("Factory", factory, inFactory && ! inUser);
        if (! user.empty()) m.sub ("User", user, inUser);
        m.separator();
        m.add ("Save Clip Bank...", [safe] { if (safe != nullptr) safe->saveBankDialog(); });
        return m;
    }

    void stepBank (int d)
    {
        std::vector<std::pair<String, std::function<ClipBank()>>> all;
        for (auto& fb : factoryBanks()) all.push_back ({ fb.name, fb.make });
        for (auto& f : userFiles ("Clip Banks"))
            all.push_back ({ f.getFileNameWithoutExtension(), [f] { Node n; ClipBank b; if (loadNode (f, n)) b.load (n); return b; } });
        int cur = -1;
        for (int i = 0; i < (int) all.size(); ++i) if (all[(size_t) i].first == String (bankOf (ctx).name)) cur = i;
        const int next = cur < 0 ? 0 : (cur + d + (int) all.size()) % (int) all.size();
        loadBank (all[(size_t) next].second(), all[(size_t) next].first);
    }

    void saveBankDialog()
    {
        if (! ctx.editValue) return;
        SafePointer<ClipEditor> safe (this);
        ctx.editValue (bank, String (bankOf (ctx).name == "Init" ? "My Clip Bank" : bankOf (ctx).name), [safe] (const String& t)
        {
            const auto name = t.trim();
            if (safe == nullptr || name.isEmpty()) return;
            auto& proc = safe->ctx.proc;
            proc.editPatch ("Save Clip Bank", [name] (Patch& p) { p.clips.name = name.toStdString(); });
            Node n;
            proc.patch().clips.save (n);
            saveNode (userFileFor ("Clip Banks", name), n);
            setBaseline (&proc, "clip", proc.patch().clips.name, signature (proc.patch().clips));
        });
    }

    void loadClip (Clip c, const String& name)
    {
        c.name = name == "Init" ? std::string() : name.toStdString();
        editClip ("Load Clip", [c] (Clip& x) { x = c; });
        roll.rebuildRows();
        roll.zoomToFit();
    }

    Menu clipMenu()
    {
        Menu m, factory, user;
        SafePointer<ClipEditor> safe (this);
        for (auto& [name, clip] : factoryClips())
        {
            const auto c = clip; const auto n = name;
            factory.add (name, [safe, c, n] { if (safe != nullptr) safe->loadClip (c, n); });
        }
        for (auto& f : userFiles ("Clips"))
        {
            const auto n = f.getFileNameWithoutExtension();
            user.add (n, [safe, f, n]
            {
                Node node;
                if (safe == nullptr || ! loadNode (f, node)) return;
                Clip c;
                c.load (node);
                safe->loadClip (c, n);
            });
        }
        m.sub ("Factory", factory);
        m.sub ("User", user, false, ! user.empty());
        m.separator();
        m.add ("Save Clip...", [safe]
        {
            if (safe == nullptr || ! safe->ctx.editValue) return;
            auto& c = safe->ctx;
            c.editValue (safe->clipName, String (bankOf (c).clipName (bankOf (c).selected)), [safe] (const String& t)
            {
                if (safe == nullptr || t.trim().isEmpty()) return;
                const auto name = t.trim().toStdString();
                safe->editClip ("Save Clip", [name] (Clip& x) { x.name = name; });
                Node n;
                clipOf (safe->ctx).save (n);
                saveNode (userFileFor ("Clips", t.trim()), n);
            });
        });
        m.add ("Rename Clip", [safe]
        {
            if (safe == nullptr || ! safe->ctx.editValue) return;
            auto& c = safe->ctx;
            c.editValue (safe->clipName, String (bankOf (c).clipName (bankOf (c).selected)), [safe] (const String& t)
            {
                if (safe == nullptr) return;
                const auto name = t.trim().toStdString();
                safe->editClip ("Rename Clip", [name] (Clip& x) { x.name = name; });
            });
        });
        return m;
    }

    // ---- layout ----------------------------------------------------------------
    static constexpr int kColX = 10, kColW = 232, kRollX = 252, kTopY = 40, kSlotY = 482;

    void resized() override
    {
        closeBtn.setBounds (getWidth() - 40, 6, 30, 22);
        // GLOBAL 40..136
        bank.setBounds (20, 54, 212, 24);
        mono.setBounds (20, 86, 56, 22);
        poly.setBounds (78, 86, 56, 22);
        editAll.setBounds (146, 86, 86, 22);
        // CLIP SETTINGS 146..470
        int y = 160;
        const int step = 24;
        clipName.setBounds (20, y, 212, 22); y += step + 2;
        length.setBounds (20, y, 212, 21); y += step;
        kbSpan.setBounds (20, y, 212, 21); y += step;
        trans.setBounds (20, y, 212, 21); y += step;
        mode.setBounds (20, y, 212, 21); y += step;
        time.setBounds (34, y, 198, 21); y += step;
        rate.setBounds (20, y, 212, 21); y += step;
        bpm.setBounds (34, y, 70, 21);
        trip.setBounds (108, y, 58, 21);
        dot.setBounds (170, y, 58, 21); y += step;
        launchQuant.setBounds (20, y, 212, 21); y += step + 2;
        retrig.setBounds (20, y, 100, 20);
        veloTrig.setBounds (122, y, 110, 20); y += step - 2;
        noteGate.setBounds (20, y, 110, 20);
        // RECORD 480..626
        record.setBounds (20, 494, 212, 26);
        commit.setBounds (20, 494, 50, 26);
        discard.setBounds (74, 494, 70, 26);
        done.setBounds (148, 494, 84, 26);
        overdub.setBounds (20, 528, 104, 22);
        extend.setBounds (128, 528, 104, 22);
        metronome.setBounds (20, 558, 140, 20);
        // Piano roll
        gridField.setBounds (kRollX, kTopY, 76, 22);
        fold.setBounds (kRollX + 84, kTopY, 56, 22);
        fit.setBounds (kRollX + 144, kTopY, 46, 22);
        listen.setBounds (kRollX + 194, kTopY, 76, 22);
        roll.setBounds (kRollX, kTopY + 28, getWidth() - kRollX - 10, kSlotY - kTopY - 36);
        slots.setBounds (kRollX, kSlotY, 1190 - kRollX, getHeight() - kSlotY - 8);
        midiOut.setBounds (1200, kSlotY, getWidth() - 1210, getHeight() - kSlotY - 8);
    }

    void paint (Graphics& g) override
    {
        drawEditorBackground (g, getLocalBounds(), "CLIP", clipColour);
        drawText (g, "Double-click the grid to add notes. Click a slot to edit it, its bar to launch it.",
                  { 80.0f, 6.0f, 700.0f, 20.0f }, 11.0f, colours::inkDim, true, Justification::centredLeft);
        drawSection (g, { (float) kColX, (float) kTopY, (float) kColW, 98.0f }, "GLOBAL");
        drawSection (g, { (float) kColX, 148.0f, (float) kColW, 324.0f }, "CLIP SETTINGS");
        drawSection (g, { (float) kColX, 482.0f, (float) kColW, (float) getHeight() - 490.0f }, "RECORD");
        drawText (g, "TRIGGER", { 20.0f, 70.0f, 120.0f, 14.0f }, 10.0f, colours::inkDim, true, Justification::centredLeft);
        const auto& b = bankOf (ctx);
        String info = "CLIP " + String (b.selected + 1) + "  |  " + String ((int) clipOf (ctx).notes.size()) + " notes";
        if (b.editAll) info << "  |  EDIT ALL";
        drawText (g, info, { (float) getWidth() - 360.0f, 6.0f, 310.0f, 20.0f }, 12.0f, clipColour, true, Justification::centredRight);
        if (recording)
        {
            g.setColour (takeRed);
            g.fillEllipse (164.0f, 564.0f, 8.0f, 8.0f);
            drawText (g, "REC " + String (ctx.proc.seqTelemetry().recordingSlot.load() + 1), { 176.0f, 558.0f, 60.0f, 20.0f }, 11.0f, takeRed, true, Justification::centredLeft);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() && e.y < 30 && e.x < 80) showContextMenu (*this, e.getPosition(), tabMenu (ctx, true));
    }

    void refresh() override
    {
        const bool rec = ctx.proc.seqTelemetry().recordingSlot.load() >= 0;
        if (rec != recording)
        {
            recording = rec;
            record.setVisible (! rec);
            for (auto* c : { &commit, &discard, &done }) c->setVisible (rec);
            repaint (150, 550, 90, 36);
        }
        if (! rec && ! record.isVisible()) record.setVisible (true);
        const int key = bankOf (ctx).selected * 4096 + (int) clipOf (ctx).notes.size() + (bankOf (ctx).editAll ? 2048 : 0);
        if (key != shownKey) { shownKey = key; repaint (getWidth() - 360, 6, 310, 20); }
    }

    PianoRoll& pianoRoll() { return roll; }

private:
    PanelContext& ctx;
    std::function<void()> onClose;
    PanelButton closeBtn;
    Field bank;
    Check mono, poly, editAll;
    Field clipName, length, kbSpan, trans, mode, time, rate;
    Check bpm, trip, dot;
    Field launchQuant;
    Check retrig, veloTrig, noteGate;
    Check record, commit, discard, done, overdub, extend, metronome;
    Field gridField;
    Check fold, fit, listen;
    PianoRoll roll;
    ClipSlots slots;
    MidiOutBox midiOut;
    bool recording = true;
    int shownKey = -1;
};
} // namespace

std::unique_ptr<Component> createClipEditor (PanelContext& ctx, std::function<void()> onClose)
{
    return std::make_unique<ClipEditor> (ctx, std::move (onClose));
}
} // namespace tincture::ui
