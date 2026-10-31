// OscModulesOsc.cpp — OSC A / B / C: the header (type, content browser,
// pitch row) and one body per oscillator type: WAVETABLE (display with warp
// preview / 3D frames, phase strip, WT POS, unison, warps), SAMPLE, GRANULAR,
// SPECTRAL and MULTISAMPLE.
#include "OscModules.h"

namespace tincture::ui::oscui
{
using namespace juce;

namespace
{
std::unique_ptr<FileChooser>& chooser()
{
    static std::unique_ptr<FileChooser> c;
    return c;
}

void browseForFile (const String& title, const String& patterns, std::function<void (const File&)> done)
{
    chooser() = std::make_unique<FileChooser> (title, File(), patterns);
    chooser()->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [done] (const FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f.existsAsFile()) done (f);
    });
}

String oscLetter (int osc) { return String::charToString ((juce_wchar) ('A' + osc)); }

// ---------------------------------------------------------------------------
// Wavetable helpers
// ---------------------------------------------------------------------------
struct TableRef { std::string category, name; };

std::vector<TableRef> allTables()
{
    std::vector<TableRef> l;
    for (const auto& c : FactoryTables::categories())
        for (const auto& n : FactoryTables::names (c)) l.push_back ({ c, n });
    return l;
}

void setTable (TinctureProcessor& proc, int osc, const std::string& cat, const std::string& name)
{
    auto t = FactoryTables::get (cat, name);
    if (t == nullptr) return;
    proc.editPatch ("Load Wavetable", [osc, t] (Patch& p) { p.wavetable[(size_t) osc].table = t; });
}

void stepTable (TinctureProcessor& proc, int osc, int d)
{
    const auto list = allTables();
    if (list.empty()) return;
    const auto& t = proc.patch().wavetable[(size_t) osc].table;
    int i = -1;
    if (t != nullptr)
        for (int k = 0; k < (int) list.size(); ++k)
            if (list[(size_t) k].category == t->category && list[(size_t) k].name == t->name) i = k;
    i = i < 0 ? 0 : (i + d + (int) list.size()) % (int) list.size();
    setTable (proc, osc, list[(size_t) i].category, list[(size_t) i].name);
}

// ---------------------------------------------------------------------------
// The warp preview for the 2D display (mirrors WavetableOsc's phase warps).
// ---------------------------------------------------------------------------
float biasFromBipolar (float s) { return 1.0f / (0.5f - 0.45f * jlimit (-1.0f, 1.0f, s)) - 2.0f; }
float bendHalves (float x, float a) { const bool lo = x < 0.5f; const float u = lo ? 2.0f * x : 2.0f - 2.0f * x; const float y = 0.5f * u / (a * (1.0f - u) + 1.0f); return lo ? y : 1.0f - y; }
float asymWarp (float x, float a) { return x / (a * (1.0f - x) + 1.0f); }
float wrap01 (float x) { return x - std::floor (x); }

float readFrame (const std::vector<float>& f, float ph)
{
    const float x = wrap01 (ph) * (float) Wavetable::kFrameSize;
    const int j = (int) x & (Wavetable::kFrameSize - 1);
    const float fr = x - std::floor (x);
    return f[(size_t) j] + (f[(size_t) ((j + 1) & (Wavetable::kFrameSize - 1))] - f[(size_t) j]) * fr;
}

float remapSegments (float ph, const BakedCurve& lut, float amt, int segments)
{
    const float fs = (float) segments;
    const float q = ph * fs;
    const int k = jmin ((int) q, segments - 1);
    const float u = q - (float) k;
    if (k & 1) { const float v = 1.0f - u; return ((float) k + 1.0f - (v + (lut (v) - v) * amt)) / fs; }
    return ((float) k + u + (lut (u) - u) * amt) / fs;
}

// ---------------------------------------------------------------------------
// Waveform display (WAVETABLE)
// ---------------------------------------------------------------------------
class WavetableDisplay final : public Component, public Refreshable, public FileDragAndDropTarget, public SettableTooltipClient
{
public:
    WavetableDisplay (PanelContext& c, int o) : ctx (c), osc (o)
    {
        ctx.add (this);
        addAndMakeVisible (pencil);
        pencil.setTooltip ("Open the wavetable editor");
        pencil.iconColour = colours::inkDim;
        pencil.onClick = [this] (const ModifierKeys&) { if (onEdit) onEdit(); };
        setTooltip ("Click: 2D / 3D view. Drop an audio file here to import it as a wavetable");
        refresh();
    }
    ~WavetableDisplay() override { ctx.remove (this); }

    std::function<void()> onEdit;

    bool is3d() const { return (bool) ctx.proc.uiState.getProperty ("osc3d" + oscLetter (osc), false); }

    void resized() override { pencil.setBounds (getWidth() - 21, 3, 18, 18); image3d = {}; rebuild(); }

    void refresh() override
    {
        auto& proc = ctx.proc;
        const auto& id = ids().osc[osc];
        const auto& data = proc.patch().wavetable[(size_t) osc];
        const bool live = proc.telemetry().activeVoices.load() > 0;
        Key k;
        k.table = data.table.get();
        k.pos = live ? proc.telemetry().wtPos[osc].load() : proc.params.get (id.wtpos);
        k.smooth = proc.params.on (id.wtSmooth);
        k.m1 = proc.params.geti (id.warp1Mode); k.a1 = proc.params.get (id.warp1);
        k.m2 = proc.params.geti (id.warp2Mode); k.a2 = proc.params.get (id.warp2);
        k.var = proc.params.get (id.warpVar);
        k.r0 = data.remap[0]; k.r1 = data.remap[1];
        k.d3 = is3d();
        const float ph = proc.params.get (id.phase);
        if (! same (ph, shownPhase))
        {
            if (shownPhase >= 0.0f) phaseFlash = 24;
            shownPhase = ph;
            repaint();
        }
        if (phaseFlash > 0 && --phaseFlash == 0) repaint();
        if (! (k == key))
        {
            if (k.table != key.table) image3d = {};
            key = k;
            rebuild();
            repaint();
        }
    }

    void paint (Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        drawLcdBox (g, r);
        const auto* t = key.table;
        if (t == nullptr) { drawText (g, "NO TABLE", r, 12.0f, colours::lcdDim); return; }
        if (key.d3) paint3d (g, r.reduced (4.0f));
        else
        {
            const auto in = r.reduced (4.0f, 6.0f);
            g.setColour (colours::grid);
            g.drawHorizontalLine ((int) in.getCentreY(), in.getX(), in.getRight());
            g.setColour (colours::brand.withAlpha (0.12f));
            g.fillPath (fill);
            g.setColour (colours::brand);
            g.strokePath (wave, PathStrokeType (1.6f));
            if (phaseFlash > 0)
            {
                const float x = in.getX() + in.getWidth() * shownPhase / 360.0f;
                g.setColour (colours::ledYellow);
                g.drawVerticalLine ((int) x, in.getY(), in.getBottom());
            }
        }
        const int n = t->numFrames();
        const int frame = n <= 1 ? 1 : roundToInt (key.pos * (float) (n - 1)) + 1;
        drawText (g, String (frame) + " / " + String (n), r.reduced (6.0f, 3.0f).removeFromBottom (13.0f), 10.5f, colours::lcdDim, true, Justification::bottomLeft);
    }

    void mouseUp (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu() || e.getDistanceFromDragStart() > 3) return;
        ctx.proc.uiState.setProperty ("osc3d" + oscLetter (osc), ! is3d(), nullptr);
        refresh();
    }

    bool isInterestedInFileDrag (const StringArray& files) override
    {
        for (auto& f : files) if (isAudioFile (f)) return true;
        return false;
    }

    void filesDropped (const StringArray& files, int, int) override
    {
        for (auto& f : files)
            if (isAudioFile (f)) { ctx.proc.loadWavetableFile (osc, File (f)); return; }
    }

private:
    struct Key
    {
        const Wavetable* table = nullptr;
        float pos = -1.0f, a1 = 0, a2 = 0, var = 0;
        int m1 = 0, m2 = 0;
        bool smooth = true, d3 = false;
        Curve r0, r1;
        bool operator== (const Key& o) const
        {
            return table == o.table && same (pos, o.pos) && same (a1, o.a1) && same (a2, o.a2) && same (var, o.var) && m1 == o.m1 && m2 == o.m2
                && smooth == o.smooth && d3 == o.d3 && r0 == o.r0 && r1 == o.r1;
        }
    };

    void rebuild()
    {
        wave.clear();
        fill.clear();
        const auto* t = key.table;
        if (t == nullptr || key.d3) return;
        std::vector<float> a ((size_t) Wavetable::kFrameSize), b ((size_t) Wavetable::kFrameSize);
        const int n = t->numFrames();
        float l = n <= 1 ? 0.0f : jlimit (0.0f, 1.0f, key.pos) * (float) (n - 1);
        if (! key.smooth) l = std::round (l);
        const int f0 = (int) l, f1 = jmin (f0 + 1, n - 1);
        t->renderFrame (f0, a.data());
        if (f1 != f0 && l > (float) f0)
        {
            t->renderFrame (f1, b.data());
            const float fr = l - (float) f0;
            for (size_t i = 0; i < a.size(); ++i) a[i] += (b[i] - a[i]) * fr;
        }
        const auto& data = ctx.proc.patch().wavetable[(size_t) osc];
        const int modes[2] { key.m1, key.m2 };
        const float amts[2] { key.a1, key.a2 };

        const auto in = getLocalBounds().toFloat().reduced (4.0f, 6.0f);
        const int pts = jmax (64, (int) in.getWidth() * 2);
        std::vector<float> ys ((size_t) pts + 1);
        float peak = 1.0f;
        for (int i = 0; i <= pts; ++i)
        {
            float x = (float) i / (float) pts;
            x = jmin (x, 0.99999f);
            float ph = x, gain = 1.0f, oddG = 1.0f, evenG = 1.0f;
            for (int s = 0; s < 2; ++s)
            {
                const int m = modes[s];
                const float w = amts[s];
                using namespace warp;
                switch (m)
                {
                    case Sync:
                        if (key.var > 0.001f) { const float d = jmin (jmin (ph, 1.0f - ph) * 2.0f / key.var, 1.0f); gain *= d * d * (3.0f - 2.0f * d); }
                        ph = wrap01 (ph * std::exp2 (4.0f * w));
                        break;
                    case BendPlus:      if (w > 0) ph = bendHalves (ph, biasFromBipolar (w)); break;
                    case BendMinus:     if (w > 0) ph = bendHalves (ph, biasFromBipolar (-w)); break;
                    case BendPlusMinus: ph = bendHalves (ph, biasFromBipolar (2.0f * w - 1.0f)); break;
                    case Pwm:           ph = jmin (ph / (1.0f - 0.95f * w), 1.0f); break;
                    case AsymPlus:      if (w > 0) ph = asymWarp (ph, biasFromBipolar (w)); break;
                    case AsymMinus:     if (w > 0) ph = asymWarp (ph, biasFromBipolar (-w)); break;
                    case AsymPlusMinus: ph = asymWarp (ph, biasFromBipolar (2.0f * w - 1.0f)); break;
                    case Flip:          if (w > 0 && ph >= 1.0f - w) gain = -gain; break;
                    case Mirror:        { const float u = ph < 0.5f ? 2.0f * ph : 2.0f - 2.0f * ph; ph = u / (biasFromBipolar (2.0f * w - 1.0f) * (1.0f - u) + 1.0f); break; }
                    case Remap1:        ph += (data.remapLut[s] (ph) - ph) * w; break;
                    case Remap2:        ph = remapSegments (ph, data.remapLut[s], w, 2); break;
                    case Remap3:        { const float sn = std::sin (ph * MathConstants<float>::halfPi); ph += (sn * sn - ph) * w; break; }
                    case Remap4:        ph = remapSegments (ph, data.remapLut[s], w, 4); break;
                    case Quantize:      if (w > 0) { const float st = std::exp2 (2.0f + 8.0f * (1.0f - w)); ph = std::floor (ph * st) / st; } break;
                    case OddEven:       oddG *= jmin (1.0f, 2.0f - 2.0f * w); evenG *= jmin (1.0f, 2.0f * w); break;
                    default: break;
                }
            }
            float y = readFrame (a, ph);
            if (! same (oddG, 1.0f) || ! same (evenG, 1.0f))
            {
                const float y2 = readFrame (a, ph + 0.5f);
                y = 0.5f * oddG * (y - y2) + 0.5f * evenG * (y + y2);
            }
            y *= gain;
            for (int s = 0; s < 2; ++s)
                if (warp::kindOf (modes[s]) == warp::Kind::Distortion) y = warp::shape (modes[s], amts[s], y);
            ys[(size_t) i] = y;
            peak = jmax (peak, std::abs (y));
        }
        const float sc = 0.5f * in.getHeight() / peak;
        for (int i = 0; i <= pts; ++i)
        {
            const Point<float> p (in.getX() + in.getWidth() * (float) i / (float) pts, in.getCentreY() - ys[(size_t) i] * sc);
            if (i == 0) { wave.startNewSubPath (p); fill.startNewSubPath (in.getX(), in.getCentreY()); }
            else wave.lineTo (p);
            fill.lineTo (p);
        }
        fill.lineTo (in.getRight(), in.getCentreY());
        fill.closeSubPath();
    }

    void paint3d (Graphics& g, Rectangle<float> r)
    {
        const auto* t = key.table;
        const int n = t->numFrames();
        const int shown = jmin (n, 48);
        const float dx = r.getWidth() * 0.28f, dy = r.getHeight() * 0.42f;
        const float fw = r.getWidth() - dx, fh = r.getHeight() - dy;
        auto origin = [&] (int k) { const float u = shown <= 1 ? 0.0f : (float) k / (float) (shown - 1); return Point<float> (r.getX() + u * dx, r.getBottom() - fh * 0.5f - u * dy); };
        auto frameAt = [&] (int k) { return shown <= 1 ? 0 : roundToInt ((float) k / (float) (shown - 1) * (float) (n - 1)); };
        std::vector<float> buf ((size_t) Wavetable::kFrameSize);
        auto pathOf = [&] (int frame, Point<float> o)
        {
            t->renderFrame (frame, buf.data());
            Path p;
            for (int i = 0; i <= 64; ++i)
            {
                const float y = buf[(size_t) jmin (Wavetable::kFrameSize - 1, i * 32)];
                const Point<float> pt (o.x + fw * (float) i / 64.0f, o.y - y * fh * 0.45f);
                if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
            }
            return p;
        };
        if (image3d.isNull() || image3d.getWidth() != getWidth())
        {
            image3d = Image (Image::ARGB, getWidth(), getHeight(), true);
            Graphics ig (image3d);
            for (int k = shown - 1; k >= 0; --k)
            {
                const int f = frameAt (k);
                ig.setColour (t->isSourceFrame (f) ? colours::brand.withAlpha (0.75f) : colours::inkFaint);
                ig.strokePath (pathOf (f, origin (k)), PathStrokeType (1.0f));
            }
        }
        g.drawImageAt (image3d, 0, 0);
        const float u = jlimit (0.0f, 1.0f, key.pos);
        const Point<float> o (r.getX() + u * dx, r.getBottom() - fh * 0.5f - u * dy);
        g.setColour (colours::lcd);
        g.strokePath (pathOf (n <= 1 ? 0 : roundToInt (u * (float) (n - 1)), o), PathStrokeType (1.8f));
    }

    PanelContext& ctx;
    int osc;
    IconButton pencil { IconButton::Pencil };
    Key key;
    Path wave, fill;
    Image image3d;
    float shownPhase = -1.0f;
    int phaseFlash = 0;
};

// ---------------------------------------------------------------------------
// UNISON: an LCD count that warms up with the voice count (CPU warning).
// ---------------------------------------------------------------------------
class UnisonField final : public ParamField
{
public:
    UnisonField (PanelContext& c, int p) : ParamField (c, p, "UNISON") { captionLeft = false; fontH = 13.0f; }
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        drawText (g, caption, r.removeFromBottom (13.0f), 10.5f, colours::ink, true);
        r = r.reduced (0.5f);
        drawLcdBox (g, r);
        const int n = ctx.proc.params.geti (paramId);
        const Colour c = n <= 4 ? colours::lcd : n <= 8 ? colours::ledYellow : n <= 12 ? Colour (0xffff8a3a) : colours::ledRed;
        drawText (g, String (n), r, fontH, c, true);
        // up/down hint
        Path p;
        const float x = r.getRight() - 7.0f, y = r.getCentreY();
        p.addTriangle (x - 3.0f, y - 2.0f, x + 3.0f, y - 2.0f, x, y - 6.0f);
        p.addTriangle (x - 3.0f, y + 2.0f, x + 3.0f, y + 2.0f, x, y + 6.0f);
        g.setColour (colours::inkFaint);
        g.fillPath (p);
    }
};

// ---------------------------------------------------------------------------
// Spectral filter thumbnail (CUT -- [FILTER] -- MIX).
// ---------------------------------------------------------------------------
class SpecFilterThumb final : public Component, public Refreshable, public SettableTooltipClient
{
public:
    SpecFilterThumb (PanelContext& c, int o) : ctx (c), osc (o)
    {
        ctx.add (this);
        setTooltip ("Spectral filter: click to edit the curve, right-click for wavetable filters");
    }
    ~SpecFilterThumb() override { ctx.remove (this); }
    std::function<void()> onEdit;

    void refresh() override
    {
        const auto& d = ctx.proc.patch().sample[(size_t) osc];
        const float cut = ctx.proc.params.get (ids().osc[osc].specCut);
        if (! (d.specFilter == curve) || d.specFilterMask != mask || ! same (cut, shownCut))
        {
            curve = d.specFilter;
            mask = d.specFilterMask;
            shownCut = cut;
            repaint();
        }
    }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        drawText (g, "FILTER", r.removeFromBottom (13.0f), 10.5f, colours::ink, true);
        drawLcdBox (g, r);
        const auto in = r.reduced (3.0f);
        Path p;
        for (int i = 0; i <= 60; ++i)
        {
            const float x = (float) i / 60.0f;
            const float xx = jlimit (0.0f, 1.0f, x - shownCut);
            float y;
            if (mask.size() > 1)
            {
                const float fx = xx * (float) (mask.size() - 1);
                const int i0 = jmin ((int) fx, (int) mask.size() - 2);
                y = 0.5f * (mask[(size_t) i0] + (mask[(size_t) i0 + 1] - mask[(size_t) i0]) * (fx - (float) i0));
            }
            else y = curve.evaluate (xx);
            const Point<float> pt (in.getX() + x * in.getWidth(), in.getBottom() - y * in.getHeight());
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (colours::brand);
        g.strokePath (p, PathStrokeType (1.4f));
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), filterMenu()); return; }
        if (onEdit) onEdit();
    }

    Menu filterMenu()
    {
        auto& proc = ctx.proc;
        const int o = osc;
        const auto& d = proc.patch().sample[(size_t) o];
        const std::string cur = d.specFilterTable;
        Menu m;
        m.add ("Curve Filter", [&proc, o] { proc.editPatch ("Curve Filter", [o] (Patch& p) { p.sample[(size_t) o].specFilterTable.clear(); p.sample[(size_t) o].specFilterMask.clear(); }); },
               cur.empty());
        m.separator();
        for (const auto& c : FactoryTables::categories())
        {
            Menu sub;
            bool has = false;
            for (const auto& n : FactoryTables::names (c))
            {
                const std::string key = c + "/" + n;
                sub.add (n, [&proc, o, c, n] { setTableFilter (proc, o, c, n); }, key == cur);
                has |= key == cur;
            }
            m.sub (c, sub, has);
        }
        return m;
    }

    // A wavetable as the spectral mask: harmonic magnitudes on the log x axis.
    static void setTableFilter (TinctureProcessor& proc, int o, const std::string& cat, const std::string& name)
    {
        auto t = FactoryTables::get (cat, name);
        if (t == nullptr) return;
        std::vector<float> re ((size_t) Wavetable::kFrameSize), im ((size_t) Wavetable::kFrameSize, 0.0f);
        t->renderFrame (0, re.data());
        fft (re.data(), im.data(), Wavetable::kFrameSize, false);
        std::vector<float> m (256);
        float peak = 1e-9f;
        for (int i = 0; i < 256; ++i)
        {
            const int h = jlimit (1, 1023, roundToInt (std::pow (1024.0f, (float) i / 255.0f)));
            m[(size_t) i] = std::sqrt (re[(size_t) h] * re[(size_t) h] + im[(size_t) h] * im[(size_t) h]);
            peak = jmax (peak, m[(size_t) i]);
        }
        for (auto& v : m) v = std::sqrt (v / peak);    // gentler than raw magnitude
        const std::string key = cat + "/" + name;
        proc.editPatch ("Wavetable Filter", [o, m, key] (Patch& p) { p.sample[(size_t) o].specFilterTable = key; p.sample[(size_t) o].specFilterMask = m; });
    }

    static void stepTableFilter (TinctureProcessor& proc, int o, int d)
    {
        const auto list = allTables();
        const auto cur = proc.patch().sample[(size_t) o].specFilterTable;
        int i = -1;
        for (int k = 0; k < (int) list.size(); ++k)
            if (list[(size_t) k].category + "/" + list[(size_t) k].name == cur) i = k;
        i = i < 0 ? 0 : (i + d + (int) list.size()) % (int) list.size();
        setTableFilter (proc, o, list[(size_t) i].category, list[(size_t) i].name);
    }

private:
    PanelContext& ctx;
    int osc;
    Curve curve;
    std::vector<float> mask;
    float shownCut = -1.0f;
};

// Spectral filter editor: the curve popover with a preset folder and GRID.
class SpecFilterEditor final : public Popover, public Refreshable
{
public:
    SpecFilterEditor (PanelContext& c, int o) : Popover ("SPECTRAL " + oscLetter (o) + " FILTER EDIT"), ctx (c), osc (o)
    {
        ctx.add (this);
        addAndMakeVisible (folder);
        folder.setTooltip ("Filter shapes");
        folder.setFontHeight (10.5f);
        folder.onClick = [this] (const ModifierKeys&) { showMenu (folder, presetMenu()); };
        refresh();
    }
    ~SpecFilterEditor() override { ctx.remove (this); }

    void refresh() override
    {
        const auto& cur = ctx.proc.patch().sample[(size_t) osc].specFilter;
        if (! (cur == shown)) { shown = cur; repaint(); }
    }

    void layout (Rectangle<int> b) override
    {
        auto foot = b.removeFromBottom (18);
        folder.setBounds (foot.removeFromLeft (54));
        gridArea = foot.removeFromRight (64).toFloat();
        graph = b.toFloat().reduced (2.0f, 4.0f);
    }

    Point<float> toScreen (float x, float y) const { return { graph.getX() + x * graph.getWidth(), graph.getBottom() - y * graph.getHeight() }; }
    Point<float> fromScreen (Point<float> p) const
    {
        return { jlimit (0.0f, 1.0f, (p.x - graph.getX()) / graph.getWidth()), jlimit (0.0f, 1.0f, (graph.getBottom() - p.y) / graph.getHeight()) };
    }

    void paint (Graphics& g) override
    {
        Popover::paint (g);
        g.setColour (colours::grid);
        for (int i = 1; i < grid; ++i)
        {
            g.drawVerticalLine ((int) (graph.getX() + graph.getWidth() * (float) i / (float) grid), graph.getY(), graph.getBottom());
            g.drawHorizontalLine ((int) (graph.getY() + graph.getHeight() * (float) i / (float) grid), graph.getX(), graph.getRight());
        }
        g.drawRect (graph, 1.0f);
        Path p;
        for (int i = 0; i <= 160; ++i)
        {
            const float x = (float) i / 160.0f;
            const auto pt = toScreen (x, shown.evaluate (x));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (colours::brand);
        g.strokePath (p, PathStrokeType (1.6f));
        for (auto& pt : shown.points)
        {
            g.setColour (colours::brand);
            g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre (toScreen (pt.x, pt.y)));
        }
        drawLcdBox (g, gridArea);
        drawText (g, "GRID: " + String (grid), gridArea, 10.5f, colours::lcd, true);
    }

    int pointAt (Point<float> pos) const
    {
        for (int i = 0; i < (int) shown.points.size(); ++i)
            if (toScreen (shown.points[(size_t) i].x, shown.points[(size_t) i].y).getDistanceFrom (pos) < 7.0f) return i;
        return -1;
    }

    void edit (const String& label, std::function<void (Curve&)> fn)
    {
        const int o = osc;
        ctx.proc.editPatch (label, [o, fn] (Patch& p)
        {
            auto& d = p.sample[(size_t) o];
            fn (d.specFilter);
            d.specFilter.sortAndClamp();
            d.specLut = std::make_shared<const BakedCurve> (d.specFilter);
        });
        refresh();
    }

    void mouseDown (const MouseEvent& e) override
    {
        if (gridArea.contains (e.position)) { gridStart = grid; gridDrag = true; return; }
        gridDrag = false;
        drag = pointAt (e.position);
        moved = false;
    }
    void mouseDrag (const MouseEvent& e) override
    {
        if (gridDrag) { grid = jlimit (1, 32, gridStart - e.getDistanceFromDragStartY() / 6); repaint(); return; }
        if (drag < 0) return;
        auto v = fromScreen (e.position);
        if (e.mods.isAltDown()) { v.x = std::round (v.x * (float) grid) / (float) grid; v.y = std::round (v.y * (float) grid) / (float) grid; }
        const int i = drag;
        edit ({}, [v, i] (Curve& c)
        {
            const int n = (int) c.points.size();
            auto& pt = c.points[(size_t) i];
            if (i > 0 && i < n - 1) pt.x = jlimit (c.points[(size_t) i - 1].x + 0.001f, c.points[(size_t) i + 1].x - 0.001f, v.x);
            pt.y = v.y;
        });
        moved = true;
    }
    void mouseUp (const MouseEvent&) override
    {
        if (moved) ctx.proc.commitHistory ("Edit Spectral Filter");
        drag = -1;
        moved = gridDrag = false;
    }
    void mouseDoubleClick (const MouseEvent& e) override
    {
        if (! graph.contains (e.position)) return;
        const int i = pointAt (e.position);
        if (i >= 0) { edit ("Remove Filter Point", [i] (Curve& c) { c.remove (i); }); return; }
        const auto v = fromScreen (e.position);
        edit ("Add Filter Point", [v] (Curve& c) { c.insert (v.x, v.y); });
    }

    Menu presetMenu()
    {
        Menu m;
        SafePointer<SpecFilterEditor> safe (this);
        auto set = [safe] (const String& label, Curve c)
        {
            if (safe != nullptr) safe->edit (label, [c] (Curve& d) { d = c; });
        };
        m.add ("Default", [set] { set ("Default Filter", Curve::line (0.5f, 0.5f)); });
        m.separator();
        Menu f;
        auto shape = [] (std::initializer_list<std::pair<float, float>> pts)
        {
            Curve c;
            c.points.clear();
            for (auto& [x, y] : pts) c.points.push_back ({ x, y, 0.0f, false });
            return c;
        };
        f.add ("Low Pass", [set, shape] { set ("Filter Shape", shape ({ { 0.0f, 0.5f }, { 0.45f, 0.5f }, { 0.6f, 0.0f }, { 1.0f, 0.0f } })); });
        f.add ("High Pass", [set, shape] { set ("Filter Shape", shape ({ { 0.0f, 0.0f }, { 0.35f, 0.0f }, { 0.5f, 0.5f }, { 1.0f, 0.5f } })); });
        f.add ("Band Pass", [set, shape] { set ("Filter Shape", shape ({ { 0.0f, 0.0f }, { 0.35f, 0.0f }, { 0.5f, 0.7f }, { 0.65f, 0.0f }, { 1.0f, 0.0f } })); });
        f.add ("Notch", [set, shape] { set ("Filter Shape", shape ({ { 0.0f, 0.5f }, { 0.4f, 0.5f }, { 0.5f, 0.0f }, { 0.6f, 0.5f }, { 1.0f, 0.5f } })); });
        f.add ("Tilt Up", [set] { set ("Filter Shape", Curve::line (0.2f, 0.8f)); });
        f.add ("Tilt Down", [set] { set ("Filter Shape", Curve::line (0.8f, 0.2f)); });
        f.add ("Comb", [set, shape] { set ("Filter Shape", shape ({ { 0.0f, 0.5f }, { 0.125f, 0.1f }, { 0.25f, 0.5f }, { 0.375f, 0.1f }, { 0.5f, 0.5f },
                                                                     { 0.625f, 0.1f }, { 0.75f, 0.5f }, { 0.875f, 0.1f }, { 1.0f, 0.5f } })); });
        m.sub ("Factory", f);
        // User shapes saved this session (in the panel state).
        auto& ui = ctx.proc.uiState;
        auto user = ui.getChildWithName ("specShapes");
        if (user.isValid() && user.getNumChildren() > 0)
        {
            Menu u;
            for (int i = 0; i < user.getNumChildren(); ++i)
            {
                const auto node = user.getChild (i);
                const auto name = node.getProperty ("name").toString();
                const auto pts = StringArray::fromTokens (node.getProperty ("points").toString(), ";", "");
                Curve c;
                c.points.clear();
                for (auto& p : pts)
                {
                    const auto xy = StringArray::fromTokens (p, ",", "");
                    if (xy.size() >= 2) c.points.push_back ({ xy[0].getFloatValue(), xy[1].getFloatValue(), xy.size() > 2 ? xy[2].getFloatValue() : 0.0f, false });
                }
                if (c.points.size() >= 2) u.add (name, [set, c] { set ("Filter Shape", c); });
            }
            m.sub ("User", u);
        }
        m.separator();
        auto* proc = &ctx.proc;
        const int o = osc;
        m.add ("Save Shape...", [proc, o]
        {
            auto& uiState = proc->uiState;
            auto list = uiState.getOrCreateChildWithName ("specShapes", nullptr);
            String pts;
            for (auto& p : proc->patch().sample[(size_t) o].specFilter.points)
                pts << String (p.x, 4) << "," << String (p.y, 4) << "," << String (p.curve, 3) << ";";
            ValueTree node ("shape");
            node.setProperty ("name", "Shape " + String (list.getNumChildren() + 1), nullptr);
            node.setProperty ("points", pts, nullptr);
            list.appendChild (node, nullptr);
        });
        return m;
    }

private:
    PanelContext& ctx;
    int osc;
    Curve shown;
    PanelButton folder { "SHAPES" };
    Rectangle<float> graph, gridArea;
    int grid = 8, gridStart = 8, drag = -1;
    bool moved = false, gridDrag = false;
};

// Granular WARP panel (the WARP button's popover).
class WarpPopover final : public Popover
{
public:
    WarpPopover (PanelContext& c, int osc) : Popover ("WARP"), section (c, osc, true) { addAndMakeVisible (section); }
    void layout (Rectangle<int> b) override { section.setBounds (b.withSizeKeepingCentre (jmin (b.getWidth(), 200), jmin (b.getHeight(), 86))); }
    WarpSection section;
};

// Multisample envelope pane (DAHDSR; patch data).
class EnvelopePopover final : public Popover, public Refreshable
{
public:
    EnvelopePopover (PanelContext& c, int o) : Popover ("ENVELOPE"), ctx (c), osc (o)
    {
        ctx.add (this);
        addAndMakeVisible (overrideBtn);
        overrideBtn.setFontHeight (10.5f);
        overrideBtn.setTooltip ("Override the instrument's own envelopes");
        auto& proc = ctx.proc;
        overrideBtn.onClick = [&proc, o] (const ModifierKeys&)
        {
            proc.editPatch ("Envelope Override", [o] (Patch& p) { auto& d = p.sample[(size_t) o]; d.envOverride = ! d.envOverride; });
        };
        const char* caps[] { "DELAY", "A", "H", "D", "S", "R" };
        for (int i = 0; i < 6; ++i)
        {
            auto k = std::make_unique<Knob>();
            k->small = true;
            k->caption = caps[i];
            k->setRange (0.0, 1.0);
            k->setSliderStyle (Slider::RotaryVerticalDrag);
            k->setTextBoxStyle (Slider::NoTextBox, true, 0, 0);
            k->format = [i] (double v) { return i == 4 ? String (roundToInt (v * 100.0)) + " %" : timeText (toTime ((float) v)); };
            k->setPopupDisplayEnabled (true, true, nullptr, 900);
            k->onValueChange = [this, i, kp = k.get()]
            {
                if (syncing) return;
                const float v = (float) kp->getValue();
                const int oo = osc;
                ctx.proc.editPatch ({}, [oo, i, v] (Patch& p)
                {
                    auto& e = p.sample[(size_t) oo].env;
                    float* f[] { &e.delay, &e.attack, &e.hold, &e.decay, &e.sustain, &e.release };
                    *f[i] = i == 4 ? v : toTime (v);
                });
            };
            k->onDragEnd = [this] { ctx.proc.commitHistory ("Envelope"); };
            addAndMakeVisible (*k);
            knobs.push_back (std::move (k));
        }
        refresh();
    }
    ~EnvelopePopover() override { ctx.remove (this); }

    static float toTime (float n) { return 32.0f * n * n * n; }
    static float fromTime (float t) { return std::cbrt (jmax (0.0f, t) / 32.0f); }
    static String timeText (float s) { return s < 1.0f ? String (roundToInt (s * 1000.0f)) + " ms" : String (s, 2) + " s"; }

    void refresh() override
    {
        const auto& d = ctx.proc.patch().sample[(size_t) osc];
        if (d.env == env && d.envOverride == over) return;
        env = d.env;
        over = d.envOverride;
        overrideBtn.setLit (over);
        syncing = true;
        const float v[] { fromTime (env.delay), fromTime (env.attack), fromTime (env.hold), fromTime (env.decay), env.sustain, fromTime (env.release) };
        for (int i = 0; i < 6; ++i)
        {
            knobs[(size_t) i]->setValue (v[i], dontSendNotification);
            knobs[(size_t) i]->setEnabled (over);
        }
        syncing = false;
        repaint();
    }

    void layout (Rectangle<int> b) override
    {
        overrideBtn.setBounds (getWidth() - 96, 3, 72, 17);
        auto row = b.removeFromBottom (44);
        const int w = row.getWidth() / 6;
        for (auto& k : knobs) k->setBounds (row.removeFromLeft (w));
        graph = b.toFloat().reduced (2.0f);
    }

    void paint (Graphics& g) override
    {
        Popover::paint (g);
        g.setColour (colours::grid);
        g.drawRect (graph, 1.0f);
        const float total = jmax (0.05f, env.delay + env.attack + env.hold + env.decay + 0.25f + env.release);
        auto X = [&] (float t) { return graph.getX() + graph.getWidth() * t / total; };
        auto Y = [&] (float l) { return graph.getBottom() - 2.0f - l * (graph.getHeight() - 4.0f); };
        Path p;
        float t = 0.0f;
        p.startNewSubPath (X (0), Y (0));
        t += env.delay; p.lineTo (X (t), Y (0));
        t += env.attack; p.lineTo (X (t), Y (1));
        t += env.hold; p.lineTo (X (t), Y (1));
        t += env.decay; p.lineTo (X (t), Y (env.sustain));
        t += 0.25f; p.lineTo (X (t), Y (env.sustain));
        t += env.release; p.lineTo (X (t), Y (0));
        g.setColour (over ? colours::brand : colours::inkFaint);
        g.strokePath (p, PathStrokeType (1.6f));
        if (! over) drawText (g, "SFZ ENVELOPES", graph, 11.0f, colours::inkDim);
    }

private:
    PanelContext& ctx;
    int osc;
    PanelButton overrideBtn { "OVERRIDE" };
    std::vector<std::unique_ptr<Knob>> knobs;
    SampleEnvSettings env;
    bool over = false, syncing = false;
    Rectangle<float> graph;
};

// ---------------------------------------------------------------------------
// The oscillator panel.
// ---------------------------------------------------------------------------
class OscPanel;

class OscBody final : public Component
{
public:
    OscBody (OscPanel& p, int type);
    void paint (Graphics&) override;
    void paintOverChildren (Graphics&) override;

private:
    ParamKnob& knob (int param, const String& cap, Rectangle<int> r, bool accent = true)
    {
        auto k = std::make_unique<ParamKnob> (ctx, param, cap);
        k->small = true;
        if (accent) k->accent = colours::brand;
        k->setBounds (r);
        addAndMakeVisible (*k);
        auto& ref = *k;
        owned.push_back (std::move (k));
        return ref;
    }
    template <typename T> T& own (std::unique_ptr<T> c, Rectangle<int> r)
    {
        c->setBounds (r);
        addAndMakeVisible (*c);
        auto& ref = *c;
        owned.push_back (std::move (c));
        return ref;
    }
    void unisonRow (int x, int y);
    void warpRow (int y);
    void buildWavetable();
    void buildSample();
    void buildGranular();
    void buildSpectral();
    void buildMulti();

    OscPanel& panel;
    PanelContext& ctx;
    int osc, type;
    std::vector<std::unique_ptr<Component>> owned;
    std::vector<std::pair<Rectangle<float>, String>> captions;
    std::vector<int> dividers;
};

class OscPanel final : public ModulePanel
{
public:
    OscPanel (PanelContext& c, OscPageHost& h, int o)
        : ModulePanel (c, { Kind::Osc, o }), host (h), osc (o), typeChoice (c, ids().osc[o].type), nameChoice (c), pitch (c, o)
    {
        typeChoice.fontH = 12.0f;
        typeChoice.setTooltip ("Oscillator type");
        typeChoice.buildMenu = [this] { return typeMenu(); };
        addAndMakeVisible (typeChoice);
        nameChoice.fontH = 12.0f;
        nameChoice.getText = [this] { return contentName(); };
        nameChoice.buildMenu = [this] { return contentMenu(); };
        nameChoice.onStep = [this] (int d) { stepContent (d); };
        nameChoice.setTooltip ("Click for the library; < > step");
        addAndMakeVisible (nameChoice);
        for (auto* b : { &prevBtn, &nextBtn }) addAndMakeVisible (*b);
        prevBtn.setTooltip ("Previous");
        nextBtn.setTooltip ("Next");
        prevBtn.onClick = [this] (const ModifierKeys&) { stepContent (-1); };
        nextBtn.onClick = [this] (const ModifierKeys&) { stepContent (1); };
        addAndMakeVisible (pitch);
        route.onOpen = [this] { togglePopover ("route", { 6, 34, 234, 160 }, [this] { return createRoutingPopover (ctx, ref); }); };
        refresh();
    }

    ~OscPanel() override { pop.reset(); body.reset(); }

    void resized() override
    {
        label.setBounds (6, 6, 76, 22);
        typeChoice.setBounds (84, 6, 120, 22);
        route.setBounds (getWidth() - 40, 6, 34, 22);
        nameChoice.setBounds (6, 36, getWidth() - 68, 20);
        prevBtn.setBounds (getWidth() - 60, 36, 26, 20);
        nextBtn.setBounds (getWidth() - 32, 36, 26, 20);
        pitch.setBounds (6, 60, getWidth() - 12, 20);
        if (body != nullptr) body->setBounds (0, kBodyY, getWidth(), getHeight() - kBodyY);
    }

    void refresh() override
    {
        ModulePanel::refresh();
        const int t = ctx.proc.params.geti (ids().osc[osc].type);
        if (t != shownType)
        {
            shownType = t;
            closePopover();
            body.reset();
            body = std::make_unique<OscBody> (*this, t);
            addAndMakeVisible (*body);
            resized();
            if (pop != nullptr) pop->toFront (false);
        }
        nameChoice.refresh();
    }

    // Body-relative rectangle -> panel rectangle (popovers live in the panel).
    Rectangle<int> fromBody (Rectangle<int> r) const { return r.translated (0, kBodyY); }

    Menu typeMenu()
    {
        auto& proc = ctx.proc;
        const int o = osc;
        const int cur = proc.params.geti (ids().osc[o].type);
        Menu m;
        const auto& names = choices::oscTypes();
        for (int i = 0; i < (int) names.size(); ++i)
            m.add (names[(size_t) i], [&proc, o, i] { setType (proc, o, i); }, i == cur);
        return m;
    }

    static void setType (TinctureProcessor& proc, int o, int t)
    {
        proc.editAll ("Oscillator Type", [o, t] (Patch& p, ParamValues& v)
        {
            v[ids().osc[o].type] = (float) t;
            auto& d = p.sample[(size_t) o];
            if (t >= 1)
            {
                const bool needSpectral = t == 4 && d.spectral == nullptr && d.sample != nullptr;
                d.type = t;
                if (needSpectral) d.rebuild (true);
            }
        });
    }

    String contentName() const
    {
        const int t = ctx.proc.params.geti (ids().osc[osc].type);
        if (t == 0)
        {
            const auto& tb = ctx.proc.patch().wavetable[(size_t) osc].table;
            return tb != nullptr ? String (tb->name) : String ("-");
        }
        return sampleName (ctx.proc, osc, t);
    }

    Menu contentMenu()
    {
        const int t = ctx.proc.params.geti (ids().osc[osc].type);
        if (t != 0) return sampleBrowserMenu (ctx, osc, t, nameChoice);
        auto& proc = ctx.proc;
        const int o = osc;
        const auto& tb = proc.patch().wavetable[(size_t) o].table;
        Menu m;
        for (const auto& c : FactoryTables::categories())
        {
            Menu sub;
            bool has = false;
            for (const auto& n : FactoryTables::names (c))
            {
                const bool cur = tb != nullptr && tb->category == c && tb->name == n;
                sub.add (n, [&proc, o, c, n] { setTable (proc, o, c, n); }, cur);
                has |= cur;
            }
            m.sub (c, sub, has);
        }
        m.separator();
        m.add ("Load Wavetable...", [&proc, o]
        {
            browseForFile ("Load a wavetable", "*.wav;*.aif;*.aiff;*.flac", [&proc, o] (const File& f) { proc.loadWavetableFile (o, f); });
        });
        SafePointer<OscPanel> safe (this);
        m.add ("Edit...", [safe] { if (safe != nullptr) safe->openEditor(); });
        return m;
    }

    void stepContent (int d)
    {
        const int t = ctx.proc.params.geti (ids().osc[osc].type);
        if (t == 0) stepTable (ctx.proc, osc, d);
        else stepSample (ctx, osc, d);
        nameChoice.refresh();
    }

    void openEditor()
    {
        const String id = "wtedit" + oscLetter (osc);
        if (host.overlayId() == id) { host.closeOverlay(); return; }
        auto* h = &host;
        host.showOverlay (createWavetableEditor (ctx, osc, [h] { h->closeOverlay(); }), OscPageHost::Area::Modules, id);
    }

    static constexpr int kBodyY = 84;
    OscPageHost& host;
    int osc;

private:
    ParamChoice typeChoice;
    PatchChoice nameChoice;
    IconButton prevBtn { IconButton::Left }, nextBtn { IconButton::Right };
    PitchRow pitch;
    std::unique_ptr<OscBody> body;
    int shownType = -1;
};

// ---------------------------------------------------------------------------
// Bodies. Body coordinates: x 0..246, y 0..296 (panel y 84..380).
// Display (6, 0, 234, 110), strip y 112..132, knob rows below.
// ---------------------------------------------------------------------------
OscBody::OscBody (OscPanel& p, int t) : panel (p), ctx (p.ctx), osc (p.osc), type (t)
{
    switch (type)
    {
        case 0: buildWavetable(); break;
        case 1: buildMulti(); break;
        case 2: buildSample(); break;
        case 3: buildGranular(); break;
        default: buildSpectral(); break;
    }
}

void OscBody::paint (Graphics& g)
{
    g.setColour (colours::inkFaint);
    for (int x : dividers) g.drawVerticalLine (x, 140.0f, 284.0f);
    for (auto& [r, t] : captions) drawText (g, t, r, 10.5f, colours::ink, true);
}

void OscBody::paintOverChildren (Graphics&) {}

void OscBody::unisonRow (int x, int y)
{
    const auto& id = ids().osc[osc];
    auto& gear = own (std::make_unique<IconButton> (IconButton::Gear), { x, y + 4, 18, 18 });
    gear.setTooltip ("Unison settings");
    auto* pp = &panel;
    const int o = osc, tp = type;
    gear.onClick = [pp, o, tp] (const ModifierKeys&)
    {
        pp->togglePopover ("unison", pp->fromBody ({ 6, 0, 234, 132 }), [pp, o, tp] { return createUnisonPopover (pp->ctx, o, tp); });
    };
    own (std::make_unique<UnisonField> (ctx, id.unison), { x + 20, y + 2, 40, 36 });
}

void OscBody::warpRow (int y)
{
    auto& w = own (std::make_unique<WarpSection> (ctx, osc), { 6, y, 192, 86 });
    auto* pp = &panel;
    const int o = osc;
    w.onEditRemap = [pp, o] (int slot)
    {
        pp->togglePopover ("remap" + String (slot), pp->fromBody ({ 6, 0, 234, 132 }), [pp, o, slot] { return createRemapEditor (pp->ctx, o, slot); });
    };
    knob (ids().osc[osc].level, "LEVEL", { 200, y + 2, 42, 54 });
    dividers.push_back (199);
}

void OscBody::buildWavetable()
{
    const auto& id = ids().osc[osc];
    auto& disp = own (std::make_unique<WavetableDisplay> (ctx, osc), { 6, 0, 234, 110 });
    auto* pp = &panel;
    disp.onEdit = [pp] { pp->openEditor(); };

    // Strip: frame, PHASE, RAND, phase memory.
    auto& frame = own (std::make_unique<ParamField> (ctx, id.wtpos), { 6, 112, 34, 20 });
    auto& proc = ctx.proc;
    const int o = osc;
    frame.fontH = 12.0f;
    frame.format = [&proc, o] (float v)
    {
        const auto& t = proc.patch().wavetable[(size_t) o].table;
        const int n = t != nullptr ? t->numFrames() : 1;
        return String (n <= 1 ? 1 : roundToInt (v * (float) (n - 1)) + 1);
    };
    auto& ph = own (std::make_unique<ParamField> (ctx, id.phase, "PHASE"), { 44, 112, 72, 20 });
    ph.fontH = 12.0f;
    ph.format = [] (float v) { return String (roundToInt (v)) + String::fromUTF8 ("\xc2\xb0"); };
    ph.extraMenu = [&proc, o] (Menu& m)
    {
        const int p = ids().osc[o].phaseLegato;
        m.toggle ("Phase Legato", proc.params.on (p), [&proc, p] { proc.setWithUndo (p, proc.params.on (p) ? 0.0f : 1.0f); });
    };
    auto& rnd = own (std::make_unique<ParamField> (ctx, id.rand, "RAND"), { 120, 112, 60, 20 });
    rnd.fontH = 12.0f;
    rnd.format = [] (float v) { return String (roundToInt (v * 100.0f)); };
    auto& mem = own (std::make_unique<ParamChoice> (ctx, id.phaseMem), { 184, 112, 56, 20 });
    mem.fontH = 10.5f;
    mem.label = [] (int i) { static const char* s[] { "ALL", "CONT", "PER" }; return String (s[jlimit (0, 2, i)]); };
    mem.setTooltip ("Phase memory: All Voices / Contiguous / Per Voice");

    // Row 1: WT POS, UNISON, DETUNE, BLEND | PAN.
    auto& wt = knob (id.wtpos, "WT POS", { 4, 138, 44, 54 });
    wt.extraMenu = [&proc, o] (Menu& m)
    {
        const int p = ids().osc[o].wtSmooth;
        m.toggle ("Smooth Interpolation", proc.params.on (p), [&proc, p] { proc.setWithUndo (p, proc.params.on (p) ? 0.0f : 1.0f); });
    };
    unisonRow (50, 140);
    knob (id.detune, "DETUNE", { 112, 138, 44, 54 });
    knob (id.blend, "BLEND", { 156, 138, 42, 54 });
    knob (id.pan, "PAN", { 200, 138, 42, 54 });
    warpRow (200);
}

void OscBody::buildSample()
{
    const auto& id = ids().osc[osc];
    own (std::make_unique<SampleDisplay> (ctx, osc, 2), { 6, 0, 234, 110 });
    own (std::make_unique<XFadeButton> (ctx, osc), { 6, 112, 22, 20 });
    auto& proc = ctx.proc;
    const int o = osc;
    auto& loop = own (std::make_unique<PatchChoice> (ctx), { 32, 112, 104, 20 });
    loop.fontH = 11.5f;
    loop.getText = [&proc, o] { return loopFace (proc, o); };
    auto* c = &ctx;
    loop.buildMenu = [c, o] { return loopMenu (*c, o, 2); };
    loop.setTooltip ("Loop mode");
    auto& ls = own (std::make_unique<ParamField> (ctx, id.loopStart, "LS"), { 140, 112, 48, 20 });
    auto& le = own (std::make_unique<ParamField> (ctx, id.loopEnd, "LE"), { 192, 112, 48, 20 });
    for (auto* f : { &ls, &le }) { f->fontH = 12.0f; f->format = [] (float v) { return String (roundToInt (v * 100.0f)); }; }

    auto& scan = knob (id.scan, "SCAN", { 4, 138, 44, 54 });
    scan.extraMenu = [c, o] (Menu& m) { addScanItems (m, *c, o, 2); };
    unisonRow (50, 140);
    knob (id.detune, "DETUNE", { 112, 138, 44, 54 });
    knob (id.blend, "BLEND", { 156, 138, 42, 54 });
    knob (id.pan, "PAN", { 200, 138, 42, 54 });
    warpRow (200);
}

void OscBody::buildGranular()
{
    const auto& id = ids().osc[osc];
    own (std::make_unique<SampleDisplay> (ctx, osc, 3), { 6, 0, 234, 110 });
    own (std::make_unique<XFadeButton> (ctx, osc), { 6, 112, 22, 20 });
    auto& win = own (std::make_unique<IconButton> (IconButton::Window), { 30, 112, 22, 20 });
    win.iconColour = loopBlue();
    win.setTooltip ("Grain window");
    auto* pp = &panel;
    const int o = osc;
    win.onClick = [pp, o] (const ModifierKeys&)
    {
        pp->togglePopover ("window", pp->fromBody ({ 6, 0, 234, 110 }), [pp, o] { return createWindowPopover (pp->ctx, o); });
    };
    auto& proc = ctx.proc;
    auto& loop = own (std::make_unique<PatchChoice> (ctx), { 56, 112, 104, 20 });
    loop.fontH = 11.5f;
    loop.getText = [&proc, o] { return loopFace (proc, o); };
    auto* c = &ctx;
    loop.buildMenu = [c, o] { return loopMenu (*c, o, 3); };
    loop.setTooltip ("Loop mode");
    auto& gear = own (std::make_unique<IconButton> (IconButton::Gear), { 164, 112, 20, 20 });
    gear.setTooltip ("Unison settings");
    gear.onClick = [pp, o] (const ModifierKeys&)
    {
        pp->togglePopover ("unison", pp->fromBody ({ 6, 136, 234, 150 }), [pp, o] { return createUnisonPopover (pp->ctx, o, 3); });
    };
    auto& uni = own (std::make_unique<ParamField> (ctx, id.unison, "UNI"), { 186, 112, 54, 20 });
    uni.fontH = 12.0f;

    // Row 1: WARP button, SCAN, DENS, LENGTH, PAN, LEVEL. Row 2: per-grain randomisation.
    auto& warpBtn = own (std::make_unique<IconButton> (IconButton::ArrowDown), { 8, 150, 32, 22 });
    warpBtn.setTooltip ("Show the warp slots");
    auto* wb = &warpBtn;
    warpBtn.onClick = [pp, o, wb] (const ModifierKeys&)
    {
        pp->togglePopover ("warp", pp->fromBody ({ 6, 136, 234, 124 }), [pp, o] { return std::make_unique<WarpPopover> (pp->ctx, o); });
        const bool open = pp->popoverId() == "warp";
        wb->icon = open ? IconButton::ArrowUp : IconButton::ArrowDown;
        wb->repaint();
    };
    captions.push_back ({ { 4.0f, 176.0f, 40.0f, 14.0f }, "WARP" });
    const int xs[] { 4, 44, 84, 124, 164, 202 };
    auto& scan = knob (id.scan, "SCAN", { xs[1], 138, 40, 54 });
    scan.extraMenu = [c, o] (Menu& m) { addScanItems (m, *c, o, 3); };
    auto& dens = knob (id.density, "DENS", { xs[2], 138, 40, 54 });
    dens.extraMenu = [c, o] (Menu& m) { addDensityItems (m, *c, o); };
    auto& len = knob (id.grainLen, "LENGTH", { xs[3], 138, 40, 54 });
    len.extraMenu = [c, o] (Menu& m) { addLengthItems (m, *c, o); };
    knob (id.pan, "PAN", { xs[4], 138, 40, 54 });
    knob (id.level, "LEVEL", { xs[5], 138, 40, 54 });
    const int y2 = 206;
    knob (id.grOffset, "OFFSET", { xs[0], y2, 40, 54 }, false);
    auto& dir = knob (id.grDir, "DIR", { xs[1], y2, 40, 54 }, false);
    dir.extraMenu = [c, o] (Menu& m) { addDirItems (m, *c, o); };
    knob (id.grPitch, "PITCH", { xs[2], y2, 40, 54 }, false);
    knob (id.grLenRand, "RAND", { xs[3], y2, 40, 54 }, false);
    knob (id.grPanRand, "RAND", { xs[4], y2, 40, 54 }, false);
    knob (id.grGainRand, "RAND", { xs[5], y2, 40, 54 }, false);
    captions.push_back ({ { 4.0f, 264.0f, 238.0f, 14.0f }, "PER-GRAIN RANDOM" });
}

void OscBody::buildSpectral()
{
    const auto& id = ids().osc[osc];
    own (std::make_unique<SampleDisplay> (ctx, osc, 4), { 6, 0, 234, 110 });
    own (std::make_unique<XFadeButton> (ctx, osc), { 6, 112, 22, 20 });
    auto& proc = ctx.proc;
    const int o = osc;
    auto* c = &ctx;
    auto* pp = &panel;
    auto& loop = own (std::make_unique<PatchChoice> (ctx), { 32, 112, 110, 20 });
    loop.fontH = 11.5f;
    loop.getText = [&proc, o] { return loopFace (proc, o); };
    loop.buildMenu = [c, o] { return loopMenu (*c, o, 4); };
    loop.setTooltip ("Loop mode");
    auto& gear = own (std::make_unique<IconButton> (IconButton::Gear), { 146, 112, 20, 20 });
    gear.setTooltip ("Unison settings");
    gear.onClick = [pp, o] (const ModifierKeys&)
    {
        pp->togglePopover ("unison", pp->fromBody ({ 6, 136, 234, 150 }), [pp, o] { return createUnisonPopover (pp->ctx, o, 4); });
    };
    auto& uni = own (std::make_unique<ParamField> (ctx, id.unison, "UNISON"), { 170, 112, 70, 20 });
    uni.fontH = 12.0f;

    auto& scan = knob (id.scan, "SCAN", { 4, 138, 44, 54 });
    scan.extraMenu = [c, o] (Menu& m) { addScanItems (m, *c, o, 4); };
    knob (id.specCut, "CUT", { 48, 138, 40, 54 });
    auto& thumb = own (std::make_unique<SpecFilterThumb> (ctx, o), { 92, 140, 62, 48 });
    thumb.onEdit = [pp, o]
    {
        pp->togglePopover ("specfilter", pp->fromBody ({ 6, 0, 234, 132 }), [pp, o] { return std::make_unique<SpecFilterEditor> (pp->ctx, o); });
    };
    auto& fp = own (std::make_unique<IconButton> (IconButton::Left), { 92, 189, 30, 12 });
    auto& fn = own (std::make_unique<IconButton> (IconButton::Right), { 124, 189, 30, 12 });
    fp.setTooltip ("Previous wavetable filter");
    fn.setTooltip ("Next wavetable filter");
    fp.onClick = [&proc, o] (const ModifierKeys&) { SpecFilterThumb::stepTableFilter (proc, o, -1); };
    fn.onClick = [&proc, o] (const ModifierKeys&) { SpecFilterThumb::stepTableFilter (proc, o, 1); };
    knob (id.specMix, "MIX", { 156, 138, 42, 54 });
    knob (id.pan, "PAN", { 200, 138, 42, 54 });
    warpRow (204);
}

void OscBody::buildMulti()
{
    const auto& id = ids().osc[osc];
    own (std::make_unique<ZoneMapDisplay> (ctx, osc), { 6, 0, 234, 110 });
    auto& proc = ctx.proc;
    const int o = osc;
    auto* pp = &panel;
    auto& env = own (std::make_unique<IconButton> (IconButton::Envelope), { 6, 112, 24, 20 });
    env.setTooltip ("Envelope override pane");
    auto* eb = &env;
    env.onClick = [pp, o, eb] (const ModifierKeys&)
    {
        pp->togglePopover ("env", pp->fromBody ({ 6, 0, 234, 110 }), [pp, o] { return std::make_unique<EnvelopePopover> (pp->ctx, o); });
        eb->setLit (pp->popoverId() == "env");
    };
    auto& vt = own (std::make_unique<PanelButton> ("VEL"), { 34, 112, 34, 20 });
    vt.setFontHeight (10.5f);
    vt.setTooltip ("Velocity tracking on/off");
    vt.setLit (proc.patch().sample[(size_t) o].velTrack);
    auto* vtp = &vt;
    vt.onClick = [&proc, o, vtp] (const ModifierKeys&)
    {
        proc.editPatch ("Velocity Tracking", [o] (Patch& p) { p.sample[(size_t) o].velTrack = ! p.sample[(size_t) o].velTrack; });
        vtp->setLit (proc.patch().sample[(size_t) o].velTrack);
    };
    auto& vamt = own (std::make_unique<PatchField> (ctx, ""), { 70, 112, 48, 20 });
    vamt.setTooltip ("Velocity tracking amount");
    vamt.get = [&proc, o] { return proc.patch().sample[(size_t) o].velTrackAmount; };
    vamt.set = [&proc, o] (float v, bool commit)
    {
        if (commit) { proc.commitHistory ("Velocity Tracking Amount"); return; }
        proc.editPatch ({}, [o, v] (Patch& p) { p.sample[(size_t) o].velTrackAmount = v; });
    };
    auto& rnd = own (std::make_unique<PatchField> (ctx, "PH RAND"), { 124, 112, 116, 20 });
    rnd.setTooltip ("Random start offset per note");
    rnd.get = [&proc, o] { return proc.patch().sample[(size_t) o].phaseRand; };
    rnd.set = [&proc, o] (float v, bool commit)
    {
        if (commit) { proc.commitHistory ("Phase Random"); return; }
        proc.editPatch ({}, [o, v] (Patch& p) { p.sample[(size_t) o].phaseRand = v; });
    };

    knob (id.timbre, "TIMBRE", { 4, 138, 44, 54 });
    unisonRow (50, 140);
    knob (id.detune, "DETUNE", { 112, 138, 44, 54 });
    knob (id.blend, "BLEND", { 156, 138, 42, 54 });
    knob (id.pan, "PAN", { 200, 138, 42, 54 });
    warpRow (200);
}
} // namespace

std::unique_ptr<Component> createOscPanel (PanelContext& ctx, OscPageHost& host, int osc)
{
    return std::make_unique<OscPanel> (ctx, host, osc);
}
} // namespace tincture::ui::oscui
