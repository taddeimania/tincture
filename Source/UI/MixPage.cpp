// MixPage.cpp — the mixer (spec C §2): 11 channel strips
//   SUB  OSC A  OSC B  OSC C  NOISE | FILTER 1  FILTER 2 | BUS 1  BUS 2 | MAIN  DIRECT
// with enable headers, route dropdowns, filter balance, bus sends, pan, MIX
// (filters), level faders (meters on the buses and MAIN), the ENV 1 gate
// button, and the FX lists of BUS 1 / BUS 2 / MAIN with bypass buttons.
#include "FxModules.h"

namespace tincture::ui
{
using namespace juce;

namespace
{
enum class StripKind { Osc, Filter, Bus, Main, Direct };

const Colour kFilterBlue { 0xff7fb8ff };

struct StripIds
{
    int enable = -1, route = -1, bal = -1, bus1 = -1, bus2 = -1, pan = -1, level = -1, gate = -1, mix = -1;
    int cutoff = -1, res = -1, type = -1;
};

// A small FX list (names + bypass buttons) for a rack.
class FxList final : public Component, public Refreshable
{
public:
    FxList (PanelContext& c, int r) : ctx (c), rack (r) { ctx.add (this); rebuild(); }
    ~FxList() override { ctx.remove (this); }

    void refresh() override
    {
        if (! fxui::panelVisible (*this) || rack < 0) return;
        if (fxui::layoutSignature (ctx.proc.patch().fx, rack) != sig) rebuild();
    }
    void rebuild()
    {
        if (rack < 0) return;
        sig = fxui::layoutSignature (ctx.proc.patch().fx, rack);
        rows.clear();
        buttons.clear();
        const auto& mods = ctx.proc.patch().fx.racks[(size_t) rack].modules;
        for (int i = 0; i < (int) mods.size(); ++i)
        {
            rows.push_back ({ FxPath { tincture::FxList { rack, -1, -1 }, i }, mods[(size_t) i].type, mods[(size_t) i].bypass, 0 });
            const auto& m = mods[(size_t) i];
            if (m.isSplitter())
                for (int b = 0; b < fxInfo (m.type).numBands; ++b)
                    for (int j = 0; j < (int) m.bands[(size_t) b].size(); ++j)
                        rows.push_back ({ FxPath { tincture::FxList { rack, i, b }, j }, m.bands[(size_t) b][(size_t) j].type, m.bands[(size_t) b][(size_t) j].bypass, 1 });
        }
        auto& proc = ctx.proc;
        for (auto& r : rows)
        {
            auto b = std::make_unique<fxui::BypassButton>();
            b->set (r.bypass);
            const auto path = r.path;
            b->onClick = [&proc, path] (const ModifierKeys& m) { fxui::toggleBypass (proc, path, m.isAltDown()); };
            addAndMakeVisible (*b);
            buttons.push_back (std::move (b));
        }
        resized();
        repaint();
    }
    void resized() override
    {
        for (size_t i = 0; i < buttons.size(); ++i) buttons[i]->setBounds (getWidth() - 22, 4 + (int) i * 20, 18, 18);
    }
    void paint (Graphics& g) override
    {
        drawLcdBox (g, getLocalBounds().toFloat());
        for (size_t i = 0; i < rows.size(); ++i)
        {
            const auto& r = rows[i];
            const float y = 4.0f + (float) i * 20.0f;
            if (y + 18.0f > (float) getHeight()) break;
            const float x = 5.0f + (float) r.depth * 8.0f;
            g.setColour (fxui::typeColour (r.type));
            g.fillRect (Rectangle<float> (x, y + 4, 3, 10));
            drawText (g, fxInfo (r.type).caps, { x + 6, y, (float) getWidth() - x - 30, 18 }, 10.5f,
                      r.bypass ? colours::inkDim : colours::ink, true, Justification::centredLeft);
        }
        if (rows.empty() && rack >= 0)
            drawText (g, "NO FX", getLocalBounds().toFloat().withHeight (26.0f), 10.5f, colours::inkFaint, true);
    }
    int rowCount() const { return (int) rows.size(); }

private:
    struct Row { FxPath path; FxType type; bool bypass; int depth; };
    PanelContext& ctx;
    int rack;
    String sig;
    std::vector<Row> rows;
    std::vector<std::unique_ptr<fxui::BypassButton>> buttons;
};

// The filter strip's mini display: an approximate response; drag = cutoff + res.
class MiniFilter final : public Component, public Refreshable
{
public:
    MiniFilter (PanelContext& c, int f) : ctx (c), filt (f) { ctx.add (this); }
    ~MiniFilter() override { ctx.remove (this); }
    
    void refresh() override
    {
        if (! fxui::panelVisible (*this)) return;
        const auto& id = ids().filter[filt];
        const float a = ctx.proc.get (id.cutoff), b = ctx.proc.get (id.res);
        const int t = ctx.proc.params.geti (id.type);
        if (! same (a, cut) || ! same (b, res) || t != type) { cut = a; res = b; type = t; repaint(); }
    }
    void paint (Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat();
        drawLcdBox (g, r);
        const auto& id = ids().filter[filt];
        const auto& ci = params()[id.cutoff];
        const float cn = ci.toNorm (cut);
        const auto pr = r.reduced (3.0f);
        const auto& ch = params()[id.type].choices;
        const String name = String (ch[(size_t) jlimit (0, (int) ch.size() - 1, type)]).toUpperCase();
        const bool high = name.contains ("HIGH") || name.contains ("HP");
        Path p;
        for (int i = 0; i <= 40; ++i)
        {
            const float x = (float) i / 40.0f;
            const float d = (x - cn) * 6.0f;
            float y = high ? (d < 0 ? 0.55f + d * 0.5f : 0.55f) : (d > 0 ? 0.55f - d * 0.5f : 0.55f);
            y += res * 0.35f * std::exp (-d * d * 8.0f);
            const auto pt = Point<float> (pr.getX() + x * pr.getWidth(), pr.getBottom() - jlimit (0.0f, 1.0f, y) * pr.getHeight());
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (kFilterBlue);
        g.strokePath (p, PathStrokeType (1.6f));
        const auto plus = Point<float> (pr.getX() + cn * pr.getWidth(), pr.getBottom() - (0.55f + res * 0.35f) * pr.getHeight());
        g.setColour (colours::brand);
        g.drawLine (plus.x - 4, plus.y, plus.x + 4, plus.y, 1.5f);
        g.drawLine (plus.x, plus.y - 4, plus.x, plus.y + 4, 1.5f);
        drawText (g, name, { r.getX() + 4, r.getY() + 2, r.getWidth() - 8, 11 }, 10.5f, colours::lcdDim, true, Justification::centredLeft);
    }
    void mouseDown (const MouseEvent& e) override
    {
        const auto& id = ids().filter[filt];
        ctx.proc.beginGesture (id.cutoff);
        ctx.proc.beginGesture (id.res);
        mouseDrag (e);
    }
    void mouseDrag (const MouseEvent& e) override
    {
        const auto& id = ids().filter[filt];
        const auto pr = getLocalBounds().toFloat().reduced (3.0f);
        const float x = jlimit (0.0f, 1.0f, (e.position.x - pr.getX()) / pr.getWidth());
        const float y = jlimit (0.0f, 1.0f, (pr.getBottom() - e.position.y) / pr.getHeight());
        ctx.proc.set (id.cutoff, params()[id.cutoff].fromNorm (x));
        ctx.proc.set (id.res, jlimit (0.0f, 1.0f, (y - 0.55f) / 0.35f));
        refresh();
    }
    void mouseUp (const MouseEvent&) override
    {
        const auto& id = ids().filter[filt];
        ctx.proc.endGesture (id.cutoff);
        ctx.proc.endGesture (id.res);
    }

private:
    PanelContext& ctx;
    int filt;
    float cut = -1, res = -1;
    int type = -1;
};

// ---------------------------------------------------------------------------
class Strip final : public Component, public Refreshable
{
public:
    Strip (PanelContext& c, StripKind k, int idx, const String& nm) : ctx (c), kind (k), index (idx), name (nm)
    {
        ctx.add (this);
        auto& t = ids();
        if (kind == StripKind::Osc)
        {
            if (idx < 3) { auto& o = t.osc[idx]; ids_ = { o.enable, o.route, o.filterBal, o.bus1, o.bus2, o.pan, o.level, o.env1Gate }; }
            else if (idx == 3) { auto& s = t.sub; ids_ = { s.enable, s.route, s.filterBal, s.bus1, s.bus2, s.pan, s.level, s.env1Gate }; }
            else { auto& s = t.noise; ids_ = { s.enable, s.route, s.filterBal, s.bus1, s.bus2, s.pan, s.level, s.env1Gate }; }
        }
        else if (kind == StripKind::Filter)
        {
            auto& f = t.filter[idx];
            ids_.enable = f.enable; ids_.route = f.route; ids_.bus1 = f.bus1; ids_.bus2 = f.bus2; ids_.pan = f.chanPan;
            ids_.level = f.level; ids_.gate = f.env1Gate; ids_.mix = f.mix; ids_.cutoff = f.cutoff; ids_.res = f.res; ids_.type = f.type;
        }
        else if (kind == StripKind::Bus) { ids_.level = idx == 0 ? t.global.bus1Level : t.global.bus2Level; ids_.route = idx == 0 ? t.global.bus1Route : t.global.bus2Route; }
        else if (kind == StripKind::Main) ids_.level = t.global.mainLevel;
        else ids_.level = t.global.directLevel;

        if (ids_.route >= 0)
        {
            route = std::make_unique<ParamChoice> (ctx, ids_.route);
            route->fontH = 12.5f;
            route->label = [this] (int i) { return String (params()[ids_.route].choices[(size_t) jlimit (0, (int) params()[ids_.route].choices.size() - 1, i)]).toUpperCase(); };
            route->buildMenu = [this] { return routeMenu(); };
            route->setTooltip ("Where this channel goes");
            addAndMakeVisible (*route);
        }
        if (ids_.bal >= 0)
        {
            bal = std::make_unique<ParamKnob> (ctx, ids_.bal, "FILTER");
            bal->small = true;
            bal->accent = kFilterBlue;
            addAndMakeVisible (*bal);
        }
        if (ids_.bus1 >= 0)
        {
            bus1 = std::make_unique<ParamKnob> (ctx, ids_.bus1, "1");
            bus2 = std::make_unique<ParamKnob> (ctx, ids_.bus2, "2");
            for (auto* knob : { bus1.get(), bus2.get() }) { knob->small = true; addAndMakeVisible (*knob); }
        }
        if (ids_.pan >= 0)
        {
            pan = std::make_unique<ParamKnob> (ctx, ids_.pan, "PAN");
            pan->small = true;
            addAndMakeVisible (*pan);
        }
        if (ids_.mix >= 0)
        {
            mix = std::make_unique<ParamKnob> (ctx, ids_.mix, "MIX");
            mix->small = true;
            mix->accent = kFilterBlue;
            addAndMakeVisible (*mix);
        }
        if (kind == StripKind::Filter)
        {
            mini = std::make_unique<MiniFilter> (ctx, idx);
            addAndMakeVisible (*mini);
        }
        level = std::make_unique<ParamFader> (ctx, ids_.level, true);
        level->fill = kind == StripKind::Osc ? colours::brand : kind == StripKind::Filter ? kFilterBlue : colours::lcd;
        level->setTooltip (String (params()[ids_.level].name));
        addAndMakeVisible (*level);
        if (ids_.gate >= 0)
        {
            gate = std::make_unique<ParamToggle> (ctx, ids_.gate, "ENV 1");
            gate->setFontHeight (10.5f);
            gate->setTooltip ("ON: this channel follows ENV 1. OFF: not gated by ENV 1");
            addAndMakeVisible (*gate);
        }
        if (kind == StripKind::Bus || kind == StripKind::Main || kind == StripKind::Direct)
        {
            fx = std::make_unique<FxList> (ctx, kind == StripKind::Bus ? 1 + idx : (kind == StripKind::Main ? 0 : -1));
            addAndMakeVisible (*fx);
        }
        if (kind == StripKind::Bus || kind == StripKind::Main)
        {
            addAndMakeVisible (meterL);
            addAndMakeVisible (meterR);
        }
        refresh();
    }
    ~Strip() override { ctx.remove (this); }

    bool hasEnable() const { return ids_.enable >= 0; }
    bool enabled() const { return ! hasEnable() || ctx.proc.params.on (ids_.enable); }

    Menu routeMenu()
    {
        auto& proc = ctx.proc;
        const auto& info = params()[ids_.route];
        const int cur = proc.params.geti (ids_.route);
        const int id = ids_.route;
        Menu m;
        for (int i = 0; i < (int) info.choices.size(); ++i)
        {
            bool ok = true;
            // No cycles: F2 can't go to F1 while F1 goes to F2 (and buses likewise).
            if (kind == StripKind::Filter && i == 0) ok = proc.params.geti (ids().filter[1 - index].route) != 0;
            if (kind == StripKind::Bus && i == 2) ok = proc.params.geti (index == 0 ? ids().global.bus2Route : ids().global.bus1Route) != 2;
            m.add (info.choices[(size_t) i], [&proc, id, i] { proc.setWithUndo (id, (float) i); }, i == cur, ok);
        }
        return m;
    }

    void refresh() override
    {
        if (! fxui::panelVisible (*this)) return;
        const bool en = enabled();
        int r = ids_.route >= 0 ? ctx.proc.params.geti (ids_.route) : 1;
        bool fed = true;
        if (kind == StripKind::Filter)
        {
            // Dimmed until a source is routed into it.
            fed = false;
            auto feeds = [&] (int routeId, int balId)
            {
                if (ctx.proc.params.geti (routeId) != 0) return false;
                const float b = ctx.proc.get (balId);
                return index == 0 ? b < 0.999f : b > 0.001f;
            };
            for (int o = 0; o < 3; ++o) fed |= ctx.proc.params.on (ids().osc[o].enable) && feeds (ids().osc[o].route, ids().osc[o].filterBal);
            fed |= ctx.proc.params.on (ids().sub.enable) && feeds (ids().sub.route, ids().sub.filterBal);
            fed |= ctx.proc.params.on (ids().noise.enable) && feeds (ids().noise.route, ids().noise.filterBal);
            fed |= ctx.proc.params.geti (ids().filter[1 - index].route) == 0 && ctx.proc.params.on (ids().filter[1 - index].enable);
        }
        const float alpha = en && fed ? 1.0f : 0.45f;
        if (! same (alpha, getAlpha())) setAlpha (alpha);
        if (en != shownEnabled) { shownEnabled = en; repaint(); }
        if (bal != nullptr) bal->setVisible (r == 0);
        if (gate != nullptr) gate->setVisible (r == 1 || r == 2);
        if (route != nullptr) route->refresh();
        if (meterL.isVisible())
        {
            const int rk = kind == StripKind::Main ? 0 : 1 + index;
            meterL.push (ctx.proc.fxEngine().rackPeak (rk, 0));
            meterR.push (ctx.proc.fxEngine().rackPeak (rk, 1));
        }
        if (bal != nullptr && bal->isVisible())
        {
            const float b = ctx.proc.get (ids_.bal);
            if (! same (b, shownBal)) { shownBal = b; repaint (balArea); }
        }
    }

    Colour tint() const
    {
        switch (kind)
        {
            case StripKind::Osc: return Colour (0xff2c3a33);
            case StripKind::Filter: return Colour (0xff2b3340);
            case StripKind::Main: return Colour (0xff3a3a3d);
            case StripKind::Bus:
            case StripKind::Direct: return Colour (0xff303032);
        }
        return Colour (0xff303032);
    }

    void paint (Graphics& g) override
    {
        const auto b = getLocalBounds().toFloat();
        g.setColour (tint());
        g.fillRect (b);
        g.setColour (colours::ink.withAlpha (0.6f));
        g.drawRect (b, 1.0f);
        // Header.
        const auto h = headerArea().toFloat();
        g.setColour (colours::black);
        g.fillRect (h.reduced (1.0f));
        if (hasEnable())
        {
            drawLed (g, { h.getX() + 13, h.getCentreY() }, 5.0f, kind == StripKind::Osc ? colours::ledGreen : kFilterBlue, enabled());
            drawText (g, name, h.withTrimmedLeft (24), 13.0f, colours::ink, true, Justification::centredLeft);
        }
        else drawText (g, name, h, 13.0f, colours::ink, true);

        if (bal != nullptr && bal->isVisible())
        {
            const float v = ctx.proc.get (ids_.bal);
            const auto a = balArea.toFloat();
            drawText (g, "1", { a.getX(), a.getY(), 12, 12 }, 11.0f, colours::ink, true);
            drawText (g, "2", { a.getRight() - 12, a.getY(), 12, 12 }, 11.0f, colours::ink, true);
            drawLed (g, { a.getX() + 6, a.getY() + 18 }, 3.0f, kFilterBlue, v < 0.999f);
            drawLed (g, { a.getRight() - 6, a.getY() + 18 }, 3.0f, kFilterBlue, v > 0.001f);
        }
        if (bus1 != nullptr)
            drawText (g, "BUS", { 0.0f, (float) bus1->getY() - 14, b.getWidth(), 12 }, 11.0f, colours::inkDim, true);
        if (fx != nullptr && kind != StripKind::Direct)
            drawText (g, "FX", { 0.0f, (float) fx->getY() - 14, b.getWidth(), 12 }, 11.0f, colours::inkDim, true);
        // Fader scale (dB marks).
        const auto& info = params()[ids_.level];
        const auto fr = level->getBounds().toFloat().reduced (0.0f, 8.0f);
        std::vector<float> marks;
        if (kind == StripKind::Osc) marks = { 0, -6, -12, -24, -48 };
        else if (kind == StripKind::Main) marks = { 3, 0, -6, -12, -24, -48 };
        else marks = { 12, 6, 0, -12, -36 };
        for (float db : marks)
        {
            float n;
            if (info.unit == Unit::Db) n = info.toNorm (db);
            else n = info.toNorm (Decibels::decibelsToGain (db));
            if (info.unit == Unit::Db && (db > info.max || db < info.min)) continue;
            const float y = fr.getBottom() - n * fr.getHeight();
            g.setColour (colours::inkDim);
            g.drawHorizontalLine ((int) y, fr.getX() - 6, fr.getX() - 1);
            drawText (g, (db > 0 ? "+" : "") + String ((int) db), { fr.getX() - 34, y - 6, 26, 12 }, 10.5f, colours::inkDim, false, Justification::centredRight);
        }
        drawText (g, String (info.format (ctx.proc.get (ids_.level))), { 0.0f, (float) level->getBottom() + 2, b.getWidth(), 13 }, 10.5f, colours::lcd, true);
    }
    Rectangle<int> headerArea() const { return { 0, 0, getWidth(), 30 }; }

    void mouseDown (const MouseEvent& e) override
    {
        if (hasEnable() && headerArea().contains (e.getPosition()) && ! e.mods.isPopupMenu())
            ctx.proc.setWithUndo (ids_.enable, enabled() ? 0.0f : 1.0f);
    }

    void resized() override
    {
        const int W = getWidth();
        int y = 38;
        if (route != nullptr) route->setBounds (6, y, W - 12, 26);
        y += 34;
        balArea = { 8, y, W - 16, 24 };
        if (bal != nullptr) bal->setBounds (W / 2 - 28, y, 56, 58);
        if (mini != nullptr) mini->setBounds (6, y, W - 12, 58);
        if (fx != nullptr) fx->setBounds (6, kind == StripKind::Main || kind == StripKind::Direct ? 52 : 86, W - 12, kind == StripKind::Main || kind == StripKind::Direct ? 260 : 226);
        y += 82;
        if (bus1 != nullptr)
        {
            bus1->setBounds (W / 2 - 52, y, 50, 52);
            bus2->setBounds (W / 2 + 2, y, 50, 52);
        }
        y += 62;
        if (pan != nullptr && mix != nullptr)
        {
            pan->setBounds (W / 2 - 52, y, 50, 56);
            mix->setBounds (W / 2 + 2, y, 50, 56);
        }
        else if (pan != nullptr) pan->setBounds (W / 2 - 26, y, 52, 56);
        const int fy = 330, fh = getHeight() - fy - 66;
        level->setBounds (W / 2 - 6, fy, 34, fh);
        meterL.setBounds (W / 2 + 32, fy + 8, 6, fh - 16);
        meterR.setBounds (W / 2 + 40, fy + 8, 6, fh - 16);
        if (gate != nullptr) gate->setBounds (8, getHeight() - 32, W - 16, 24);
    }

private:
    PanelContext& ctx;
    StripKind kind;
    int index;
    String name;
    StripIds ids_;
    std::unique_ptr<ParamChoice> route;
    std::unique_ptr<ParamKnob> bal, bus1, bus2, pan, mix;
    std::unique_ptr<ParamFader> level;
    std::unique_ptr<ParamToggle> gate;
    std::unique_ptr<FxList> fx;
    std::unique_ptr<MiniFilter> mini;
    Meter meterL, meterR;
    Rectangle<int> balArea;
    bool shownEnabled = true;
    float shownBal = -1.0f;
};

class MixPage final : public Component
{
public:
    explicit MixPage (PanelContext& ctx)
    {
        struct S { StripKind k; int i; const char* n; };
        const S defs[] { { StripKind::Osc, 3, "SUB" }, { StripKind::Osc, 0, "OSC A" }, { StripKind::Osc, 1, "OSC B" }, { StripKind::Osc, 2, "OSC C" },
                         { StripKind::Osc, 4, "NOISE" }, { StripKind::Filter, 0, "FILTER 1" }, { StripKind::Filter, 1, "FILTER 2" },
                         { StripKind::Bus, 0, "BUS 1" }, { StripKind::Bus, 1, "BUS 2" }, { StripKind::Main, 0, "MAIN" }, { StripKind::Direct, 0, "DIRECT" } };
        for (auto& d : defs)
        {
            strips.push_back (std::make_unique<Strip> (ctx, d.k, d.i, d.n));
            addAndMakeVisible (*strips.back());
        }
    }
    void paint (Graphics& g) override { g.fillAll (colours::panel); }
    void resized() override
    {
        const int w = 114, gap = 4, groupGap = 18;
        int x = (getWidth() - (11 * w + 10 * gap + 3 * (groupGap - gap))) / 2;
        for (int i = 0; i < 11; ++i)
        {
            strips[(size_t) i]->setBounds (x, 14, w, getHeight() - 28);
            x += w + ((i == 4 || i == 6 || i == 8) ? groupGap : gap);
        }
    }

private:
    std::vector<std::unique_ptr<Strip>> strips;
};
} // namespace

std::unique_ptr<juce::Component> createMixPage (PanelContext& ctx)
{
    return std::make_unique<MixPage> (ctx);
}
} // namespace tincture::ui
