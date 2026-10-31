// OscModulesOther.cpp — the SUB, NOISE and FILTER 1 / FILTER 2 panels.
#include "OscModules.h"

namespace tincture::ui::oscui
{
using namespace juce;

namespace
{
ParamKnob& addKnob (Component& parent, std::vector<std::unique_ptr<Component>>& owned, PanelContext& ctx, int p, const String& cap,
                    Rectangle<int> r, Colour accent)
{
    auto k = std::make_unique<ParamKnob> (ctx, p, cap);
    k->small = true;
    k->accent = accent;
    k->setBounds (r);
    parent.addAndMakeVisible (*k);
    auto& ref = *k;
    owned.push_back (std::move (k));
    return ref;
}

// ===========================================================================
// SUB
// ===========================================================================
class ShapeButton final : public PanelButton, public Refreshable
{
public:
    ShapeButton (PanelContext& c, int s) : ctx (c), shape (s)
    {
        ctx.add (this);
        setTooltip (String (choices::subShapes()[(size_t) s]));
        onClick = [this] (const ModifierKeys& m)
        {
            const int p = ids().sub.shape;
            if (m.isPopupMenu()) { showContextMenu (*this, getMouseXYRelative(), controlMenu (ctx, p)); return; }
            ctx.proc.setWithUndo (p, (float) shape);
            refresh();
        };
        refresh();
    }
    ~ShapeButton() override { ctx.remove (this); }
    void refresh() override { setLit (ctx.proc.params.geti (ids().sub.shape) == shape); }
    void paint (Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (isLit() ? colours::cream : colours::black);
        g.fillRect (r);
        g.setColour (colours::inkDim);
        g.drawRect (r, 1.0f);
        const auto in = r.reduced (5.0f, 6.0f);
        Path p;
        for (int i = 0; i <= 48; ++i)
        {
            const float t = (float) i / 48.0f;
            const Point<float> pt (in.getX() + t * in.getWidth(), in.getCentreY() - SubOsc::sample (shape, jmin (t, 0.9999f)) * in.getHeight() * 0.5f);
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (isLit() ? colours::black : colours::brand);
        g.strokePath (p, PathStrokeType (1.4f));
    }
private:
    PanelContext& ctx;
    int shape;
};

class SubPanel final : public ModulePanel
{
public:
    explicit SubPanel (PanelContext& c) : ModulePanel (c, { Kind::Sub, 0 })
    {
        const auto& id = ids().sub;
        auto field = [&] (int p, const char* cap, Rectangle<int> r, std::function<String (float)> fmt)
        {
            auto f = std::make_unique<ParamField> (c, p, cap);
            f->fontH = 12.0f;
            f->format = std::move (fmt);
            f->setBounds (r);
            addAndMakeVisible (*f);
            auto* added = f.get();
            owned.push_back (std::move (f));
            return added;
        };
        field (id.oct, "OCT", { 6, 40, 80, 20 }, [] (float v) { return String (roundToInt (v)); });
        field (id.coarse, "CRS", { 6, 64, 80, 20 }, [] (float v) { return std::abs (v) < 0.005f ? String ("-") : String (v, 1); });
        for (int s = 0; s < 6; ++s)
        {
            auto b = std::make_unique<ShapeButton> (c, s);
            b->setBounds (6 + (s % 2) * 41, 92 + (s / 2) * 30, 39, 28);
            addAndMakeVisible (*b);
            owned.push_back (std::move (b));
        }
        auto* ph = field (id.phase, "PHASE", { 6, 186, 80, 20 }, [] (float v) { return String (roundToInt (v)) + String::fromUTF8 ("\xc2\xb0"); });
        auto& proc = c.proc;
        ph->extraMenu = [&proc] (Menu& m)
        {
            const int p = ids().sub.contiguous;
            m.toggle ("Contiguous", proc.params.on (p), [&proc, p] { proc.setWithUndo (p, proc.params.on (p) ? 0.0f : 1.0f); });
        };
        addKnob (*this, owned, c, id.pan, "PAN", { 16, 222, 60, 62 }, colours::brand);
        addKnob (*this, owned, c, id.level, "LEVEL", { 16, 296, 60, 62 }, colours::brand);
        route.onOpen = [this] { togglePopover ("route", { 2, 34, getWidth() - 4, 220 }, [this] { return createRoutingPopover (ctx, ref); }); };
        refresh();
    }
    ~SubPanel() override { pop.reset(); }
    void resized() override
    {
        label.setBounds (5, 6, 52, 22);
        route.setBounds (getWidth() - 34, 6, 28, 22);
    }
private:
    std::vector<std::unique_ptr<Component>> owned;
};

// ===========================================================================
// NOISE
// ===========================================================================
struct NoiseItem { std::string category, name; int colour; };

std::vector<NoiseItem> noiseList()
{
    std::vector<NoiseItem> l;
    for (const auto& c : FactoryNoises::categories())
        for (const auto& n : FactoryNoises::names (c)) l.push_back ({ c, n, 0 });
    for (int i = 1; i < (int) choices::noiseColours().size(); ++i) l.push_back ({ "Color", choices::noiseColours()[(size_t) i], i });
    return l;
}

void setNoise (TinctureProcessor& proc, const NoiseItem& it)
{
    if (it.colour > 0) { proc.setWithUndo (ids().noise.colour, (float) it.colour); return; }
    auto s = FactoryNoises::get (it.category, it.name);
    if (s == nullptr) return;
    proc.editAll ("Load Noise", [s] (Patch& p, ParamValues& v) { p.noise.sample = s; v[ids().noise.colour] = 0.0f; });
}

class NoiseDisplay final : public Component, public Refreshable, public FileDragAndDropTarget, public SettableTooltipClient
{
public:
    explicit NoiseDisplay (PanelContext& c) : ctx (c)
    {
        ctx.add (this);
        setTooltip ("Drop an audio file here to use it as the noise");
    }
    ~NoiseDisplay() override { ctx.remove (this); }
    void refresh() override
    {
        const auto* s = ctx.proc.patch().noise.sample.get();
        const int col = ctx.proc.params.geti (ids().noise.colour);
        const float k = ctx.proc.params.get (ids().noise.start) + ctx.proc.params.get (ids().noise.tilt) * 3.0f;
        if (s != shownSample || col != shownColour || ! same (k, shownKey)) { shownSample = s; shownColour = col; shownKey = k; path = {}; repaint(); }
    }
    void paint (Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        drawLcdBox (g, r);
        const auto in = r.reduced (3.0f);
        if (shownColour > 0)
        {
            // The colour's spectral slope with the tilt filter, on a log axis.
            const float slope[] { 0.0f, 0.0f, -3.0f, -6.0f, 0.0f };
            const float tilt = ctx.proc.params.get (ids().noise.tilt);
            Path p;
            for (int i = 0; i <= 40; ++i)
            {
                const float t = (float) i / 40.0f;
                const float oct = t * 10.0f - 5.0f;
                const float db = slope[shownColour] * oct + tilt * 6.0f * oct * 0.5f;
                const Point<float> pt (in.getX() + t * in.getWidth(), in.getCentreY() - jlimit (-1.0f, 1.0f, db / 36.0f) * in.getHeight() * 0.5f);
                if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
            }
            g.setColour (colours::brand);
            g.strokePath (p, PathStrokeType (1.5f));
            drawText (g, String (choices::noiseColours()[(size_t) shownColour]).toUpperCase(), in.withTop (in.getBottom() - 12.0f), 10.5f, colours::lcdDim, true, Justification::bottomRight);
            return;
        }
        if (shownSample == nullptr || shownSample->frames() == 0) { drawText (g, "NO SAMPLE", in, 11.0f, colours::lcdDim); return; }
        if (path.isEmpty())
        {
            const int n = shownSample->frames();
            for (int x = 0; x < (int) in.getWidth(); ++x)
            {
                const int a = (int) ((float) x / in.getWidth() * (float) n), b = jmin (n, a + jmax (1, n / (int) in.getWidth()));
                float pk = 0.0f;
                for (int i = a; i < b; i += jmax (1, (b - a) / 32)) pk = jmax (pk, std::abs (shownSample->left[(size_t) i]));
                path.addRectangle (in.getX() + (float) x, in.getCentreY() - pk * in.getHeight() * 0.5f, 1.0f, jmax (1.0f, pk * in.getHeight()));
            }
        }
        g.setColour (colours::brand.withAlpha (0.8f));
        g.fillPath (path);
        g.setColour (colours::ledYellow);
        g.drawVerticalLine ((int) (in.getX() + in.getWidth() * ctx.proc.params.get (ids().noise.start)), in.getY(), in.getBottom());
    }
    bool isInterestedInFileDrag (const StringArray& files) override { for (auto& f : files) if (isAudioFile (f)) return true; return false; }
    void filesDropped (const StringArray& files, int, int) override { for (auto& f : files) if (isAudioFile (f)) { ctx.proc.loadNoiseFile (File (f)); return; } }
private:
    PanelContext& ctx;
    const SampleData* shownSample = nullptr;
    int shownColour = -1;
    float shownKey = -1.0f;
    Path path;
};

class NoisePanel final : public ModulePanel
{
public:
    explicit NoisePanel (PanelContext& c) : ModulePanel (c, { Kind::Noise, 0 }), name (c), display (c)
    {
        const auto& id = ids().noise;
        auto& proc = c.proc;
        name.fontH = 11.5f;
        name.setTooltip ("Noise: click for the library; < > step");
        name.getText = [&proc]
        {
            const int col = proc.params.geti (ids().noise.colour);
            if (col > 0) return String (choices::noiseColours()[(size_t) col]);
            const auto& s = proc.patch().noise.sample;
            return s != nullptr ? String (s->name) : String ("-");
        };
        name.buildMenu = [this] { return noiseMenu(); };
        name.onStep = [&proc] (int d) { step (proc, d); };
        addAndMakeVisible (name);
        for (auto* b : { &prev, &next }) addAndMakeVisible (*b);
        prev.onClick = [&proc] (const ModifierKeys&) { step (proc, -1); };
        next.onClick = [&proc] (const ModifierKeys&) { step (proc, 1); };
        prev.setTooltip ("Previous noise");
        next.setTooltip ("Next noise");
        for (auto* b : { &loopBtn, &oneShotBtn }) { b->setFontHeight (10.5f); addAndMakeVisible (*b); }
        loopBtn.setTooltip ("Loop the noise sample");
        oneShotBtn.setTooltip ("Play the noise sample once");
        loopBtn.onClick = [&proc] (const ModifierKeys&) { proc.setWithUndo (ids().noise.oneShot, 0.0f); };
        oneShotBtn.onClick = [&proc] (const ModifierKeys&) { proc.setWithUndo (ids().noise.oneShot, 1.0f); };
        addAndMakeVisible (display);

        auto field = [&] (int p, const char* cap)
        {
            auto f = std::make_unique<ParamField> (c, p, cap);
            f->fontH = 12.0f;
            f->format = [] (float v) { return String (roundToInt (v * 100.0f)); };
            addAndMakeVisible (*f);
            return f;
        };
        start = field (id.start, "START");
        rand = field (id.rand, "RAND");
        stereo = std::make_unique<ParamKnob> (c, id.stereo, "STEREO");
        filter = std::make_unique<ParamKnob> (c, id.tilt, "FILTER");
        for (auto* k : { stereo.get(), filter.get() }) { k->small = true; k->accent = colours::brand; addChildComponent (*k); }
        addKnob (*this, owned, c, id.fine, "FINE", { 8, 236, 64, 60 }, colours::brand);
        addKnob (*this, owned, c, id.pan, "PAN", { 78, 236, 64, 60 }, colours::brand);
        addKnob (*this, owned, c, id.pitch, "PITCH", { 8, 300, 64, 60 }, colours::brand);
        addKnob (*this, owned, c, id.level, "LEVEL", { 78, 300, 64, 60 }, colours::brand);
        keytrack = std::make_unique<ParamToggle> (c, id.pitchTrack, "KEYTRACK");
        keytrack->setFontHeight (10.5f);
        keytrack->setTooltip ("Noise pitch follows the keyboard");
        addAndMakeVisible (*keytrack);
        route.onOpen = [this] { togglePopover ("route", { 4, 34, getWidth() - 8, 160 }, [this] { return createRoutingPopover (ctx, ref); }); };
        refresh();
    }
    ~NoisePanel() override { pop.reset(); }

    void resized() override
    {
        label.setBounds (5, 6, 84, 22);
        route.setBounds (getWidth() - 40, 6, 34, 22);
        name.setBounds (6, 38, 90, 20);
        prev.setBounds (98, 38, 22, 20);
        next.setBounds (122, 38, 22, 20);
        loopBtn.setBounds (6, 62, 68, 18);
        oneShotBtn.setBounds (76, 62, 68, 18);
        display.setBounds (6, 84, 138, 66);
        start->setBounds (6, 156, 138, 20);
        rand->setBounds (6, 180, 138, 20);
        stereo->setBounds (8, 152, 64, 54);
        filter->setBounds (78, 152, 64, 54);
        keytrack->setBounds (6, 210, 138, 20);
    }

    void refresh() override
    {
        ModulePanel::refresh();
        const int col = ctx.proc.params.geti (ids().noise.colour);
        const bool colour = col > 0;
        if (colour != shownColour)
        {
            shownColour = colour;
            showAll (! colour, loopBtn, oneShotBtn, *start, *rand);
            showAll (colour, *stereo, *filter);
        }
        const bool one = ctx.proc.params.on (ids().noise.oneShot);
        loopBtn.setLit (! one);
        oneShotBtn.setLit (one);
        name.refresh();
    }

    static void step (TinctureProcessor& proc, int d)
    {
        const auto l = noiseList();
        if (l.empty()) return;
        const int col = proc.params.geti (ids().noise.colour);
        const auto& s = proc.patch().noise.sample;
        int i = -1;
        for (int k = 0; k < (int) l.size(); ++k)
        {
            const auto& it = l[(size_t) k];
            if (col > 0 ? it.colour == col : (s != nullptr && it.colour == 0 && it.name == s->name && it.category == s->category)) i = k;
        }
        i = i < 0 ? 0 : (i + d + (int) l.size()) % (int) l.size();
        setNoise (proc, l[(size_t) i]);
    }

    Menu noiseMenu()
    {
        auto& proc = ctx.proc;
        const int col = proc.params.geti (ids().noise.colour);
        const auto s = proc.patch().noise.sample;
        Menu m;
        for (const auto& c : FactoryNoises::categories())
        {
            Menu sub;
            bool has = false;
            for (const auto& n : FactoryNoises::names (c))
            {
                const bool cur = col == 0 && s != nullptr && s->category == c && s->name == n;
                sub.add (n, [&proc, c, n] { setNoise (proc, { c, n, 0 }); }, cur);
                has |= cur;
            }
            m.sub (c, sub, has);
        }
        m.separator();
        Menu colour;
        for (int i = 1; i < (int) choices::noiseColours().size(); ++i)
            colour.add (choices::noiseColours()[(size_t) i], [&proc, i] { proc.setWithUndo (ids().noise.colour, (float) i); }, col == i);
        m.sub ("Color", colour, col > 0);
        m.separator();
        const String path = s != nullptr ? String (s->path) : String();
        m.add ("Show Sample in Finder", [path] { File (path).revealToUser(); }, false, col == 0 && path.isNotEmpty());
        m.add ("Load Sample...", [&proc]
        {
            static std::unique_ptr<FileChooser> fc;
            fc = std::make_unique<FileChooser> ("Load a noise sample", File(), "*.wav;*.aif;*.aiff;*.flac");
            fc->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [&proc] (const FileChooser& f)
            {
                if (f.getResult().existsAsFile()) proc.loadNoiseFile (f.getResult());
            });
        });
        if (s != nullptr && ! FactoryNoises::isFactory (*s))
            m.toggle ("Embed in Preset", s->embedded, [&proc]
            {
                const auto& cur = proc.patch().noise.sample;
                if (cur == nullptr) return;
                auto copy = std::make_shared<SampleData> (*cur);
                copy->embedded = ! cur->embedded;
                proc.editPatch ("Embed in Preset", [copy] (Patch& p) { p.noise.sample = copy; });
            });
        return m;
    }

private:
    PatchChoice name;
    IconButton prev { IconButton::Left }, next { IconButton::Right };
    PanelButton loopBtn { "LOOP" }, oneShotBtn { "ONE-SHOT" };
    NoiseDisplay display;
    std::unique_ptr<ParamField> start, rand;
    std::unique_ptr<ParamKnob> stereo, filter;
    std::unique_ptr<ParamToggle> keytrack;
    std::vector<std::unique_ptr<Component>> owned;
    int shownColour = -1;
};

// ===========================================================================
// FILTER
// ===========================================================================
// S / A / B / C / N: feeds this filter (route = Filter with a share for it).
class SourceSwitch final : public PanelButton, public Refreshable
{
public:
    SourceSwitch (PanelContext& c, int f, ModuleRef src, const String& text) : PanelButton (text), ctx (c), filter (f), source (src)
    {
        ctx.add (this);
        setFontHeight (11.0f);
        setLitColour (filterAccent());
        setTooltip ("Send " + src.title() + " through Filter " + String (f + 1));
        onClick = [this] (const ModifierKeys&) { toggle(); };
        refresh();
    }
    ~SourceSwitch() override { ctx.remove (this); }

    static float share (const TinctureProcessor& proc, ModuleRef src, int filter)
    {
        if (proc.params.geti (src.routeParam()) != 0) return 0.0f;
        const float b = proc.params.get (src.balanceParam());
        return filter == 0 ? 1.0f - b : b;
    }

    void refresh() override { setLit (share (ctx.proc, source, filter) > 0.001f); }

    void toggle()
    {
        auto& proc = ctx.proc;
        const int rp = source.routeParam(), bp = source.balanceParam();
        const bool feeds = share (proc, source, filter) > 0.001f;
        const float mine = filter == 0 ? 0.0f : 1.0f, other = 1.0f - mine;
        const int f = filter;
        proc.editAll ("Filter Routing", [rp, bp, feeds, mine, other, f] (Patch&, ParamValues& v)
        {
            if (feeds)
            {
                const float b = v[bp];
                const bool only = f == 0 ? b <= 0.001f : b >= 0.999f;
                if (only) v[rp] = 1.0f;       // Main
                else v[bp] = other;           // leave it to the other filter
            }
            else if ((int) std::lround (v[rp]) != 0) { v[rp] = 0.0f; v[bp] = mine; }
            else v[bp] = 0.5f;               // it fed the other filter: now both
        });
        refresh();
    }

private:
    PanelContext& ctx;
    int filter;
    ModuleRef source;
};

class FilterDisplay final : public Component, public Refreshable, public SettableTooltipClient
{
public:
    FilterDisplay (PanelContext& c, int f) : ctx (c), filter (f)
    {
        ctx.add (this);
        setTooltip ("Drag: cutoff (sideways) and resonance (up/down). Right-click: display mode");
    }
    ~FilterDisplay() override { ctx.remove (this); }

    String modeKey() const { return "filterDisplay" + String (filter + 1); }
    int mode() const { return (int) ctx.proc.uiState.getProperty (modeKey(), 0); }

    void refresh() override
    {
        const auto& id = ids().filter[filter];
        auto& pr = ctx.proc;
        const float k[] { pr.params.get (id.type), pr.params.get (id.cutoff), pr.params.get (id.res), pr.params.get (id.drive),
                          pr.params.get (id.var), (float) mode(), pr.params.get (id.enable) };
        if (! std::equal (std::begin (k), std::end (k), key))
        {
            std::copy (std::begin (k), std::end (k), key);
            rebuild();
            repaint();
        }
    }

    void resized() override { rebuild(); }

    void rebuild()
    {
        curve.clear();
        fillPath.clear();
        const auto r = getLocalBounds().toFloat().reduced (2.0f);
        const auto& id = ids().filter[filter];
        auto& pr = ctx.proc;
        const int type = pr.params.geti (id.type);
        const float cut = pr.params.get (id.cutoff), res = pr.params.get (id.res), drv = pr.params.get (id.drive), var = pr.params.get (id.var);
        const int n = jmax (32, (int) r.getWidth());
        for (int i = 0; i <= n; ++i)
        {
            const float t = (float) i / (float) n;
            const float hz = normToFreq (t);
            const float db = jlimit (-60.0f, 30.0f, FilterVoice::magnitudeDb (type, cut, res, drv, var, hz, 48000.0));
            const Point<float> p (r.getX() + t * r.getWidth(), yOfDb (db, r));
            if (i == 0) { curve.startNewSubPath (p); fillPath.startNewSubPath (r.getX(), r.getBottom()); fillPath.lineTo (p); }
            else { curve.lineTo (p); fillPath.lineTo (p); }
        }
        fillPath.lineTo (r.getRight(), r.getBottom());
        fillPath.closeSubPath();
    }

    static float yOfDb (float db, Rectangle<float> r) { return r.getY() + (24.0f - db) / 72.0f * r.getHeight(); }

    void paint (Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        drawLcdBox (g, r);
        const auto in = r.reduced (2.0f);
        g.setColour (colours::grid);
        for (float hz : { 100.0f, 1000.0f, 10000.0f })
            g.drawVerticalLine ((int) (in.getX() + freqToNorm (hz) * in.getWidth()), in.getY(), in.getBottom());
        g.drawHorizontalLine ((int) yOfDb (0.0f, in), in.getX(), in.getRight());
        const bool on = ctx.proc.params.on (ids().filter[filter].enable);
        const auto c = on ? filterAccent() : colours::inkFaint;
        g.setColour (c.withAlpha (0.18f));
        g.fillPath (fillPath);
        g.setColour (c);
        g.strokePath (curve, PathStrokeType (1.6f));
        if (mode() == 2) drawText (g, "Phase", in.reduced (3.0f).removeFromTop (12.0f), 10.5f, colours::ledGreen, true, Justification::topRight);
        if (dragging)
        {
            const auto& id = ids().filter[filter];
            const float x = in.getX() + freqToNorm (ctx.proc.params.get (id.cutoff)) * in.getWidth();
            const float y = in.getBottom() - ctx.proc.params.get (id.res) * in.getHeight();
            g.setColour (colours::ledGreen);
            g.drawLine (x - 5.0f, y, x + 5.0f, y, 1.5f);
            g.drawLine (x, y - 5.0f, x, y + 5.0f, 1.5f);
        }
    }

    void mouseDown (const MouseEvent& e) override
    {
        auto& proc = ctx.proc;
        if (e.mods.isPopupMenu())
        {
            Menu m;
            m.header ("Display Mode");
            const char* names[] { "Frequency Response", "Frequency Response & FFT", "Phase Response & FFT" };
            const int cur = mode();
            const auto k = modeKey();
            for (int i = 0; i < 3; ++i)
                m.add (names[i], [&proc, k, i] { proc.uiState.setProperty (k, i, nullptr); }, cur == i);
            showContextMenu (*this, e.getPosition(), m);
            return;
        }
        if (e.mods.isAltDown()) { proc.uiState.setProperty (modeKey(), (mode() + 1) % 3, nullptr); refresh(); return; }
        const auto& id = ids().filter[filter];
        proc.beginGesture (id.cutoff);
        proc.beginGesture (id.res);
        dragging = true;
        mouseDrag (e);
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (! dragging) return;
        const auto in = getLocalBounds().toFloat().reduced (2.0f);
        const auto& id = ids().filter[filter];
        ctx.proc.set (id.cutoff, normToFreq (jlimit (0.0f, 1.0f, (e.position.x - in.getX()) / in.getWidth())));
        ctx.proc.set (id.res, jlimit (0.0f, 1.0f, (in.getBottom() - e.position.y) / in.getHeight()));
        refresh();
    }

    void mouseUp (const MouseEvent&) override
    {
        if (! dragging) return;
        const auto& id = ids().filter[filter];
        ctx.proc.endGesture (id.cutoff);
        ctx.proc.endGesture (id.res);
        dragging = false;
        repaint();
    }

private:
    PanelContext& ctx;
    int filter;
    float key[7] {};
    Path curve, fillPath;
    bool dragging = false;
};

class FilterPanel final : public ModulePanel
{
public:
    FilterPanel (PanelContext& c, int f) : ModulePanel (c, { Kind::Filter, f }), filter (f), type (c, ids().filter[f].type), display (c, f)
    {
        const auto& id = ids().filter[f];
        auto& proc = c.proc;
        type.fontH = 12.0f;
        type.setTooltip ("Filter type");
        type.buildMenu = [&proc, f] { return typeMenu (proc, f); };
        addAndMakeVisible (type);
        for (auto* b : { &prev, &next }) addAndMakeVisible (*b);
        prev.setTooltip ("Previous filter type");
        next.setTooltip ("Next filter type");
        prev.onClick = [this] (const ModifierKeys&) { type.step (-1); };
        next.onClick = [this] (const ModifierKeys&) { type.step (1); };
        addAndMakeVisible (display);
        const ModuleRef srcs[] { { Kind::Sub, 0 }, { Kind::Osc, 0 }, { Kind::Osc, 1 }, { Kind::Osc, 2 }, { Kind::Noise, 0 } };
        const char* names[] { "S", "A", "B", "C", "N" };
        for (int i = 0; i < 5; ++i)
        {
            auto s = std::make_unique<SourceSwitch> (c, f, srcs[i], names[i]);
            s->setBounds (6 + i * 27, 176, 25, 20);
            addAndMakeVisible (*s);
            owned.push_back (std::move (s));
        }
        keytrack = std::make_unique<ParamToggle> (c, id.keytrack, "KEY");
        keytrack->setFontHeight (10.5f);
        keytrack->setLitColour (filterAccent());
        keytrack->setTooltip ("Keytrack: the cutoff follows the note");
        addAndMakeVisible (*keytrack);
        const auto acc = filterAccent();
        addKnob (*this, owned, c, id.cutoff, "CUTOFF", { 4, 202, 50, 60 }, acc);
        addKnob (*this, owned, c, id.res, "RES", { 54, 202, 50, 60 }, acc);
        auto& drive = addKnob (*this, owned, c, id.drive, "DRIVE", { 4, 268, 50, 60 }, acc);
        drive.extraMenu = [&proc, f] (Menu& m)
        {
            const int p = ids().filter[f].cleanDrive;
            m.toggle ("Clean Mode", proc.params.on (p), [&proc, p] { proc.setWithUndo (p, proc.params.on (p) ? 0.0f : 1.0f); });
        };
        var = &addKnob (*this, owned, c, id.var, "FAT", { 54, 268, 50, 60 }, acc);
        addKnob (*this, owned, c, id.pan, "PAN", { 108, 202, 42, 60 }, acc);
        addKnob (*this, owned, c, id.mix, "MIX", { 108, 268, 42, 60 }, acc);
        auto lv = std::make_unique<ParamFader> (c, id.level, true, "LEVEL");
        lv->fill = acc;
        lv->setBounds (152, 200, 36, 130);
        addAndMakeVisible (*lv);
        owned.push_back (std::move (lv));
        route.onOpen = [this] { togglePopover ("route", { 6, 34, getWidth() - 12, 140 }, [this] { return createRoutingPopover (ctx, ref); }); };
        refresh();
    }
    ~FilterPanel() override { pop.reset(); }

    static Menu typeMenu (TinctureProcessor& proc, int f)
    {
        const int p = ids().filter[f].type;
        const int cur = proc.params.geti (p);
        const auto& names = choices::filterTypes();
        Menu m;
        for (const auto& cat : choices::filterCategories())
        {
            Menu sub;
            for (int i = cat.first; i < cat.first + cat.count; ++i)
                sub.add (names[(size_t) i], [&proc, p, i] { proc.setWithUndo (p, (float) i); }, i == cur);
            m.sub (cat.name, sub, cur >= cat.first && cur < cat.first + cat.count);
        }
        return m;
    }

    void resized() override
    {
        label.setBounds (5, 6, 110, 22);
        route.setBounds (getWidth() - 40, 6, 34, 22);
        type.setBounds (6, 38, 128, 20);
        prev.setBounds (136, 38, 24, 20);
        next.setBounds (162, 38, 24, 20);
        display.setBounds (6, 62, getWidth() - 12, 108);
        keytrack->setBounds (142, 176, 44, 20);
    }

    void paint (Graphics& g) override
    {
        ModulePanel::paint (g);
        g.setColour (colours::inkFaint);
        g.drawVerticalLine (106, 206.0f, 326.0f);
    }

    void refresh() override
    {
        ModulePanel::refresh();
        const int t = ctx.proc.params.geti (ids().filter[filter].type);
        if (t != shownType)
        {
            shownType = t;
            const String cap (choices::filterVarLabel (t));
            var->caption = cap.isEmpty() ? String ("---") : cap;
            var->setEnabled (FilterVoice::hasVar (t));
            var->repaint();
        }
    }

private:
    int filter;
    ParamChoice type;
    IconButton prev { IconButton::Left }, next { IconButton::Right };
    FilterDisplay display;
    std::unique_ptr<ParamToggle> keytrack;
    ParamKnob* var = nullptr;
    std::vector<std::unique_ptr<Component>> owned;
    int shownType = -1;
};
} // namespace

std::unique_ptr<Component> createSubPanel (PanelContext& ctx) { return std::make_unique<SubPanel> (ctx); }
std::unique_ptr<Component> createNoisePanel (PanelContext& ctx) { return std::make_unique<NoisePanel> (ctx); }
std::unique_ptr<Component> createFilterPanel (PanelContext& ctx, int f) { return std::make_unique<FilterPanel> (ctx, f); }
} // namespace tincture::ui::oscui
