// OscModulesCommon.cpp — pieces shared by the module panels: icons, the
// module label, routing button and popover, popovers, the pitch row, the
// warp slots and menu, the remap curve editor, the unison and grain-window
// panes, and two patch-data controls.
#include "OscModules.h"

namespace tincture::ui::oscui
{
using namespace juce;

// ===========================================================================
// Icons
// ===========================================================================
void drawIcon (Graphics& g, IconButton::Icon icon, Rectangle<float> r, Colour c)
{
    g.setColour (c);
    const auto cx = r.getCentreX(), cy = r.getCentreY();
    const float s = jmin (r.getWidth(), r.getHeight()) * 0.5f;
    Path p;
    switch (icon)
    {
        case IconButton::Gear:
        {
            for (int i = 0; i < 8; ++i)
            {
                const float a = (float) i * MathConstants<float>::pi / 4.0f;
                p.addLineSegment ({ cx + std::cos (a) * s * 0.45f, cy + std::sin (a) * s * 0.45f,
                                    cx + std::cos (a) * s * 0.85f, cy + std::sin (a) * s * 0.85f }, 2.2f);
            }
            g.fillPath (p);
            g.drawEllipse (Rectangle<float> (s * 1.1f, s * 1.1f).withCentre ({ cx, cy }), 1.6f);
            return;
        }
        case IconButton::Pencil:
        {
            p.startNewSubPath (cx - s * 0.7f, cy + s * 0.7f);
            p.lineTo (cx - s * 0.55f, cy + s * 0.25f);
            p.lineTo (cx + s * 0.35f, cy - s * 0.65f);
            p.lineTo (cx + s * 0.65f, cy - s * 0.35f);
            p.lineTo (cx - s * 0.25f, cy + s * 0.55f);
            p.closeSubPath();
            g.strokePath (p, PathStrokeType (1.3f));
            return;
        }
        case IconButton::Close:
            p.addLineSegment ({ cx - s * 0.5f, cy - s * 0.5f, cx + s * 0.5f, cy + s * 0.5f }, 1.6f);
            p.addLineSegment ({ cx + s * 0.5f, cy - s * 0.5f, cx - s * 0.5f, cy + s * 0.5f }, 1.6f);
            g.fillPath (p);
            return;
        case IconButton::Window:
        {
            p.startNewSubPath (cx - s * 0.8f, cy + s * 0.55f);
            for (int i = 0; i <= 20; ++i)
            {
                const float t = (float) i / 20.0f;
                const float w = 0.5f - 0.5f * std::cos (MathConstants<float>::twoPi * t);
                p.lineTo (cx - s * 0.8f + t * s * 1.6f, cy + s * 0.55f - w * s * 1.1f);
            }
            g.strokePath (p, PathStrokeType (1.5f));
            return;
        }
        case IconButton::Envelope:
            p.startNewSubPath (cx - s * 0.8f, cy + s * 0.55f);
            p.lineTo (cx - s * 0.45f, cy - s * 0.6f);
            p.lineTo (cx, cy - s * 0.05f);
            p.lineTo (cx + s * 0.45f, cy - s * 0.05f);
            p.lineTo (cx + s * 0.8f, cy + s * 0.55f);
            g.strokePath (p, PathStrokeType (1.5f));
            return;
        case IconButton::Keys:
        {
            const auto k = Rectangle<float> (s * 1.7f, s * 1.2f).withCentre ({ cx, cy });
            g.drawRect (k, 1.2f);
            for (int i = 1; i < 4; ++i)
                g.drawVerticalLine ((int) (k.getX() + k.getWidth() * (float) i / 4.0f), k.getY(), k.getBottom());
            for (int i : { 1, 2 })
                g.fillRect (Rectangle<float> (k.getWidth() * 0.12f, k.getHeight() * 0.55f)
                                .withCentre ({ k.getX() + k.getWidth() * (float) i / 4.0f, k.getY() + k.getHeight() * 0.28f }));
            return;
        }
        case IconButton::XFade:
            p.addLineSegment ({ cx - s * 0.7f, cy + s * 0.55f, cx + s * 0.7f, cy - s * 0.55f }, 1.5f);
            p.addLineSegment ({ cx - s * 0.7f, cy - s * 0.55f, cx + s * 0.7f, cy + s * 0.55f }, 1.5f);
            g.fillPath (p);
            return;
        case IconButton::Arrows:
            p.addTriangle (cx - s * 0.45f, cy - s * 0.15f, cx + s * 0.45f, cy - s * 0.15f, cx, cy - s * 0.75f);
            p.addTriangle (cx - s * 0.45f, cy + s * 0.15f, cx + s * 0.45f, cy + s * 0.15f, cx, cy + s * 0.75f);
            g.fillPath (p);
            return;
        case IconButton::ArrowDown:
            p.addTriangle (cx - s * 0.5f, cy - s * 0.25f, cx + s * 0.5f, cy - s * 0.25f, cx, cy + s * 0.4f);
            g.fillPath (p);
            return;
        case IconButton::ArrowUp:
            p.addTriangle (cx - s * 0.5f, cy + s * 0.25f, cx + s * 0.5f, cy + s * 0.25f, cx, cy - s * 0.4f);
            g.fillPath (p);
            return;
        case IconButton::Left:
            p.addTriangle (cx + s * 0.3f, cy - s * 0.45f, cx + s * 0.3f, cy + s * 0.45f, cx - s * 0.35f, cy);
            g.fillPath (p);
            return;
        case IconButton::Right:
            p.addTriangle (cx - s * 0.3f, cy - s * 0.45f, cx - s * 0.3f, cy + s * 0.45f, cx + s * 0.35f, cy);
            g.fillPath (p);
            return;
        case IconButton::Undo:
        case IconButton::Redo:
        {
            const bool redo = icon == IconButton::Redo;
            p.addCentredArc (cx, cy + s * 0.1f, s * 0.5f, s * 0.5f, 0.0f, redo ? -2.4f : 2.4f, redo ? 1.2f : -1.2f, true);
            g.strokePath (p, PathStrokeType (1.4f));
            Path h;
            const float hx = redo ? cx + s * 0.45f : cx - s * 0.45f;
            h.addTriangle (hx - s * 0.3f, cy - s * 0.2f, hx + s * 0.3f, cy - s * 0.2f, hx, cy + s * 0.25f);
            g.fillPath (h);
            return;
        }
    }
}

void IconButton::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = isLit();
    g.setColour (on ? colours::cream : colours::black);
    g.fillRect (r);
    g.setColour (colours::inkDim);
    g.drawRect (r, 1.0f);
    drawIcon (g, icon, r.reduced (2.0f), on ? colours::black : (isEnabled() ? iconColour : colours::inkFaint));
}

// ===========================================================================
// ModuleLabel
// ===========================================================================
ModuleLabel::ModuleLabel (PanelContext& c, ModuleRef r) : ctx (c), ref (r)
{
    ctx.add (this);
    setTooltip ((r.kind == Kind::Filter ? String ("Click the LED to turn ") : String ("Click to turn ")) + r.title()
                + " on or off; drag onto a control to modulate it; right-click for the module menu");
    refresh();
}

ModuleLabel::~ModuleLabel() { ctx.remove (this); }

void ModuleLabel::refresh()
{
    const bool l = ctx.proc.params.on (ref.enableParam());
    if (l != lit) { lit = l; repaint(); }
}

void ModuleLabel::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    drawLed (g, { 9.0f, r.getCentreY() }, 5.0f, ref.kind == Kind::Filter ? filterAccent() : colours::ledGreen, lit);
    drawText (g, ref.title(), r.withTrimmedLeft (18.0f), 12.5f, lit ? colours::ink : colours::inkDim, true, Justification::centredLeft);
}

void ModuleLabel::mouseDown (const MouseEvent& e)
{
    dragStarted = false;
    downOnLed = ledArea().contains (e.getPosition());
    if (e.mods.isPopupMenu())
        showContextMenu (*this, e.getPosition(), moduleMenu (ctx, ref));
}

void ModuleLabel::mouseDrag (const MouseEvent& e)
{
    if (e.mods.isPopupMenu() || dragStarted || e.getDistanceFromDragStart() < 4) return;
    if (auto* dnd = DragAndDropContainer::findParentDragContainerFor (this))
    {
        dragStarted = true;
        dnd->startDragging (modSourceDragDescription (ref.source()), this);
    }
}

void ModuleLabel::mouseUp (const MouseEvent& e)
{
    if (! dragStarted && ! e.mods.isPopupMenu() && getLocalBounds().contains (e.getPosition()) && (! ledOnly || downOnLed))
    {
        const int en = ref.enableParam();
        ctx.proc.setWithUndo (en, ctx.proc.params.on (en) ? 0.0f : 1.0f);
        refresh();
    }
    dragStarted = false;
}

// ===========================================================================
// RouteButton
// ===========================================================================
RouteButton::RouteButton (PanelContext& c, ModuleRef r) : ctx (c), ref (r)
{
    ctx.add (this);
    setFontHeight (11.0f);
    onClick = [this] (const ModifierKeys& m)
    {
        if (m.isPopupMenu()) { showContextMenu (*this, getMouseXYRelative(), controlMenu (ctx, ref.routeParam())); return; }
        if (onOpen) onOpen();
    };
    refresh();
}

RouteButton::~RouteButton() { ctx.remove (this); }

void RouteButton::refresh()
{
    const auto code = routeCode (ctx.proc, ref);
    if (code != getLabel())
    {
        setLabel (code);
        const int r = ctx.proc.params.geti (ref.routeParam());
        setTooltip (ref.title() + " routing: " + String (params()[ref.routeParam()].choices[(size_t) r]) + " (click for routing and bus sends)");
    }
}

// ===========================================================================
// Popover
// ===========================================================================
Popover::Popover (String t) : title (std::move (t))
{
    addAndMakeVisible (closeBtn);
    closeBtn.setTooltip ("Close");
    closeBtn.onClick = [this] (const ModifierKeys&)
    {
        SafePointer<Popover> safe (this);
        MessageManager::callAsync ([safe] { if (safe != nullptr && safe->onClose) safe->onClose(); });
    };
}

void Popover::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    g.setColour (colours::black);
    g.fillRect (r);
    g.setColour (colours::ink.withAlpha (0.85f));
    g.drawRect (r, 1.0f);
    if (title.isNotEmpty())
    {
        const auto f = panelFont (11.0f, true);
        const float w = GlyphArrangement::getStringWidth (f, title) + 10.0f;
        const auto tab = Rectangle<float> (4.0f, 4.0f, w, 15.0f);
        g.setColour (colours::ink);
        g.fillRect (tab);
        g.setColour (colours::black);
        g.setFont (f);
        g.drawText (title, tab, Justification::centred, false);
    }
}

void Popover::resized()
{
    closeBtn.setBounds (getWidth() - 20, 3, 17, 17);
    layout (getLocalBounds().reduced (6).withTrimmedTop (16));
}

// ===========================================================================
// Routing popover
// ===========================================================================
namespace
{
class RoutingPopover final : public Popover, public Refreshable
{
public:
    RoutingPopover (PanelContext& c, ModuleRef r) : Popover ("ROUTING"), ctx (c), ref (r),
        routeChoice (c, r.routeParam()),
        bus1 (c, r.bus1Param(), "BUS 1"), bus2 (c, r.bus2Param(), "BUS 2")
    {
        ctx.add (this);
        routeChoice.fontH = 12.0f;
        routeChoice.setTooltip ("Where " + r.title() + " goes");
        addAndMakeVisible (routeChoice);
        if (r.balanceParam() >= 0)
        {
            bal = std::make_unique<ParamKnob> (c, r.balanceParam(), "F1 - F2");
            bal->small = true;
            bal->accent = filterAccent();
            addChildComponent (*bal);
        }
        for (auto* k : { &bus1, &bus2 }) { k->small = true; addAndMakeVisible (*k); }
        refresh();
    }
    ~RoutingPopover() override { ctx.remove (this); }

    void refresh() override
    {
        if (bal != nullptr)
        {
            const bool show = ctx.proc.params.geti (ref.routeParam()) == 0;
            if (show != bal->isVisible()) { bal->setVisible (show); resized(); }
        }
    }

    void layout (Rectangle<int> b) override
    {
        routeChoice.setBounds (b.removeFromTop (22).withSizeKeepingCentre (jmin (b.getWidth(), 120), 22));
        b.removeFromTop (4);
        std::vector<Component*> ks;
        if (bal != nullptr && bal->isVisible()) ks.push_back (bal.get());
        ks.push_back (&bus1);
        ks.push_back (&bus2);
        if (b.getWidth() >= 140)
        {
            const int w = 60, h = 62;
            int x = b.getCentreX() - w * (int) ks.size() / 2;
            for (auto* k : ks) { k->setBounds (x, b.getY(), w, h); x += w; }
        }
        else
            for (auto* k : ks) k->setBounds (b.removeFromTop (58).withSizeKeepingCentre (52, 58));
    }

private:
    PanelContext& ctx;
    ModuleRef ref;
    ParamChoice routeChoice;
    std::unique_ptr<ParamKnob> bal;
    ParamKnob bus1, bus2;
};
}

std::unique_ptr<Popover> createRoutingPopover (PanelContext& ctx, ModuleRef ref)
{
    return std::make_unique<RoutingPopover> (ctx, ref);
}

// ===========================================================================
// ModulePanel
// ===========================================================================
ModulePanel::ModulePanel (PanelContext& c, ModuleRef r) : ctx (c), ref (r), label (c, r), route (c, r)
{
    ctx.add (this);
    addAndMakeVisible (label);
    addAndMakeVisible (route);
    label.ledOnly = r.kind == Kind::Filter;
}

ModulePanel::~ModulePanel()
{
    pop.reset();
    ctx.remove (this);
}

void ModulePanel::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (0.75f);
    g.setColour (colours::ink.withAlpha (0.85f));
    g.drawRect (r, 1.5f);
    g.setColour (colours::inkFaint);
    g.drawHorizontalLine (32, r.getX() + 4.0f, r.getRight() - 4.0f);
}

void ModulePanel::paintOverChildren (Graphics& g)
{
    if (dimmed)
    {
        g.setColour (colours::panel.withAlpha (0.45f));
        g.fillRect (getLocalBounds().reduced (2).withTrimmedTop (32));
    }
}

void ModulePanel::refresh()
{
    const bool d = ! ctx.proc.params.on (ref.enableParam());
    if (d != dimmed) { dimmed = d; repaint(); }
}

void ModulePanel::togglePopover (const String& id, Rectangle<int> area, std::function<std::unique_ptr<Popover>()> make)
{
    if (pop != nullptr && popId == id) { closePopover(); return; }
    closePopover();
    pop = make();
    if (pop == nullptr) return;
    popId = id;
    pop->onClose = [this] { closePopover(); };
    addAndMakeVisible (*pop);
    pop->setBounds (area);
    pop->toFront (false);
}

void ModulePanel::closePopover()
{
    if (pop == nullptr) return;
    pop.reset();
    popId.clear();
    repaint();
}

// ===========================================================================
// PitchRow
// ===========================================================================
PitchRow::PitchRow (PanelContext& c, int o) : ctx (c), osc (o)
{
    const auto& id = ids().osc[osc];
    oct = std::make_unique<ParamField> (c, id.oct, "OCT");
    sem = std::make_unique<ParamField> (c, id.sem, "SEM");
    fin = std::make_unique<ParamField> (c, id.fine, "FIN");
    crs = std::make_unique<ParamField> (c, id.coarse, "CRS");
    auto modeMenu = [this] (Menu& m)
    {
        auto& proc = ctx.proc;
        const int p = ids().osc[osc].pitchMode;
        const int cur = proc.params.geti (p);
        Menu sub;
        const auto& names = choices::pitchModes();
        for (int i = 0; i < (int) names.size(); ++i)
            sub.add (names[(size_t) i], [&proc, p, i] { proc.setWithUndo (p, (float) i); }, i == cur);
        m.sub ("Mode", sub);
    };
    oct->extraMenu = modeMenu;
    sem->extraMenu = modeMenu;
    oct->format = [] (float v) { return String (roundToInt (v)); };
    fin->format = [] (float v) { return String (roundToInt (v)); };
    crs->format = [] (float v) { return std::abs (v) < 0.005f ? String ("-") : String (v, 2); };
    for (auto* f : { oct.get(), sem.get(), fin.get(), crs.get() })
    {
        f->fontH = 12.0f;
        addAndMakeVisible (*f);
    }
    ctx.add (this);
    refresh();
}

PitchRow::~PitchRow() { ctx.remove (this); }

void PitchRow::resized()
{
    const int w = (getWidth() - 9) / 4;
    int x = 0;
    for (auto* f : { oct.get(), sem.get(), fin.get(), crs.get() })
    {
        f->setBounds (x, 0, w, getHeight());
        x += w + 3;
    }
}

void PitchRow::refresh()
{
    const auto& id = ids().osc[osc];
    const int mode = ctx.proc.params.geti (id.pitchMode);
    if (mode == shownMode) return;
    shownMode = mode;
    if (mode == 2)
    {
        sem->setParam (id.ratio);
        sem->caption = "RAT";
        sem->format = [] (float v) { return String (v, v < 10.0f ? 2 : 1); };
    }
    else
    {
        sem->setParam (id.sem);
        sem->caption = mode == 1 ? "HRM" : "SEM";
        if (mode == 1)
            sem->format = [] (float v) { const int s = roundToInt (v); return s >= 0 ? "x" + String (s + 1) : "1/" + String (1 - s); };
        else
            sem->format = [] (float v) { return String (roundToInt (v)); };
    }
    sem->repaint();
}

// ===========================================================================
// Warp menu and slots
// ===========================================================================
namespace
{
// Mode indices by category for one oscillator (other oscillators only).
struct WarpCats
{
    std::vector<int> alt, filter, dist, fm, pd, am, rm;
};

WarpCats warpCats (int osc)
{
    WarpCats c;
    using namespace warp;
    for (int m = BendPlus; m <= OddEven; ++m) c.alt.push_back (m);
    c.filter = { Lpf, Hpf };
    for (int m = DistFirst; m <= DistLast; ++m) c.dist.push_back (m);
    for (int k = 0; k < 7; ++k)
    {
        if (k == osc) continue;    // FM/PD/AM/RM (A) is not offered on OSC A
        c.fm.push_back (FmFirst + k);
        c.pd.push_back (PdFirst + k);
        c.am.push_back (AmFirst + k);
        c.rm.push_back (RmFirst + k);
    }
    c.pd.push_back (PdSelf);
    return c;
}

// Phase warps only exist in the wavetable oscillator; sample modes use the post warps.
// Choose a warp mode. Bend +/-, Asym +/- and Mirror are centred at 50 %:
// picking one with the amount at 0 sets it to 50 % in the same undo step,
// so the sound doesn't jump to a full bend.
void setWarpMode (TinctureProcessor& proc, int osc, int slot, int mode)
{
    proc.editAll ("Warp Mode", [osc, slot, mode] (Patch&, ParamValues& v)
    {
        const auto& id = ids().osc[osc];
        v[slot == 0 ? id.warp1Mode : id.warp2Mode] = (float) mode;
        const int amt = slot == 0 ? id.warp1 : id.warp2;
        const bool centred = mode == warp::BendPlusMinus || mode == warp::AsymPlusMinus || mode == warp::Mirror;
        if (centred && v[amt] <= 0.0f) v[amt] = 0.5f;
    });
}

bool warpAvailable (int mode, int oscType)
{
    return oscType == 0 || mode == 0 || warp::isPost (mode);
}

std::vector<int> warpOrder (int osc, int oscType)
{
    const auto c = warpCats (osc);
    std::vector<int> l { 0, warp::Sync };
    for (const auto* v : { &c.alt, &c.filter, &c.dist, &c.fm, &c.pd, &c.am, &c.rm })
        l.insert (l.end(), v->begin(), v->end());
    l.erase (std::remove_if (l.begin(), l.end(), [oscType] (int m) { return ! warpAvailable (m, oscType); }), l.end());
    return l;
}
}

String warpModeShort (int mode)
{
    const auto& n = choices::warpModes();
    return String (n[(size_t) jlimit (0, (int) n.size() - 1, mode)]).toUpperCase();
}

Menu warpMenu (PanelContext& ctx, int osc, int slot)
{
    auto& proc = ctx.proc;
    const auto& id = ids().osc[osc];
    const int p = slot == 0 ? id.warp1Mode : id.warp2Mode;
    const int cur = proc.params.geti (p);
    const int type = proc.params.geti (id.type);
    const auto& names = choices::warpModes();
    const auto cats = warpCats (osc);

    auto item = [&] (Menu& m, int mode)
    {
        m.add (names[(size_t) mode], [&proc, osc, slot, mode] { setWarpMode (proc, osc, slot, mode); }, mode == cur, warpAvailable (mode, type));
    };
    auto category = [&] (Menu& m, const char* name, const std::vector<int>& modes, std::function<void (Menu&)> extra = {})
    {
        Menu sub;
        bool has = false, any = false;
        for (int mode : modes) { item (sub, mode); has |= mode == cur; any |= warpAvailable (mode, type); }
        if (extra) extra (sub);
        m.sub (name, sub, has, any);
    };

    Menu m;
    item (m, 0);
    item (m, warp::Sync);
    category (m, "Alt Warp", cats.alt);
    category (m, "Filter", cats.filter);
    category (m, "Distortion", cats.dist);
    category (m, "FM", cats.fm, [&proc, &id, type] (Menu& sub)
    {
        sub.separator();
        const int ft = proc.params.geti (id.fmType);
        const auto& fmNames = choices::fmTypes();
        const int fp = id.fmType;
        for (int i = 0; i < (int) fmNames.size(); ++i)
            sub.add (fmNames[(size_t) i], [&proc, fp, i] { proc.setWithUndo (fp, (float) i); }, i == ft, type == 0);
    });
    category (m, "PD", cats.pd);
    category (m, "AM", cats.am);
    category (m, "RM", cats.rm);
    m.separator();
    m.add ("Swap A/B", [&proc, osc]
    {
        proc.editAll ("Swap Warps", [osc] (Patch& pt, ParamValues& v)
        {
            const auto& i = ids().osc[osc];
            std::swap (v[i.warp1Mode], v[i.warp2Mode]);
            std::swap (v[i.warp1], v[i.warp2]);
            std::swap (v[i.uniWarp1], v[i.uniWarp2]);
            std::swap (pt.wavetable[(size_t) osc].remap[0], pt.wavetable[(size_t) osc].remap[1]);
            pt.wavetable[(size_t) osc].bakeRemaps();
        });
    });
    return m;
}

WarpModeField::WarpModeField (PanelContext& c, int o, int s) : ctx (c), osc (o), slot (s)
{
    ctx.add (this);
    for (auto* b : { &prev, &next }) addAndMakeVisible (*b);
    prev.onClick = [this] (const ModifierKeys&) { step (-1); };
    next.onClick = [this] (const ModifierKeys&) { step (1); };
    prev.setTooltip ("Previous warp mode");
    next.setTooltip ("Next warp mode");
    refresh();
}

WarpModeField::~WarpModeField() { ctx.remove (this); }

void WarpModeField::refresh()
{
    const auto& id = ids().osc[osc];
    const int m = ctx.proc.params.geti (slot == 0 ? id.warp1Mode : id.warp2Mode);
    if (m != shown)
    {
        shown = m;
        setTooltip ("Warp " + String (slot + 1) + ": " + String (choices::warpModes()[(size_t) m]) + " (click for the warp menu)");
        repaint();
    }
}

void WarpModeField::resized()
{
    prev.setBounds (0, 0, 15, getHeight());
    next.setBounds (getWidth() - 15, 0, 15, getHeight());
}

void WarpModeField::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (16.0f, 0.0f);
    drawLcdBox (g, r);
    drawText (g, warpModeShort (shown), r.reduced (2.0f, 0.0f), 11.0f, shown == 0 ? colours::lcdDim : colours::lcd, true);
}

void WarpModeField::mouseDown (const MouseEvent& e)
{
    const int p = slot == 0 ? ids().osc[osc].warp1Mode : ids().osc[osc].warp2Mode;
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), controlMenu (ctx, p)); return; }
    showMenu (*this, warpMenu (ctx, osc, slot));
}

void WarpModeField::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) { step (w.deltaY > 0 ? -1 : 1); }

void WarpModeField::step (int d)
{
    const auto& id = ids().osc[osc];
    const int p = slot == 0 ? id.warp1Mode : id.warp2Mode;
    const auto order = warpOrder (osc, ctx.proc.params.geti (id.type));
    const int cur = ctx.proc.params.geti (p);
    auto it = std::find (order.begin(), order.end(), cur);
    int i = it == order.end() ? 0 : (int) (it - order.begin());
    i = (i + d + (int) order.size()) % (int) order.size();
    (void) p;
    setWarpMode (ctx.proc, osc, slot, order[(size_t) i]);
    refresh();
}

WarpSection::WarpSection (PanelContext& c, int o, bool comp) : ctx (c), osc (o), compact (comp)
{
    const auto& id = ids().osc[osc];
    k1 = std::make_unique<ParamKnob> (c, id.warp1, "WARP 1");
    k2 = std::make_unique<ParamKnob> (c, id.warp2, "WARP 2");
    for (auto* k : { k1.get(), k2.get() }) { k->small = true; k->accent = colours::brand; addAndMakeVisible (*k); }
    m1 = std::make_unique<WarpModeField> (c, osc, 0);
    m2 = std::make_unique<WarpModeField> (c, osc, 1);
    addAndMakeVisible (*m1);
    addAndMakeVisible (*m2);
    var = std::make_unique<ParamFader> (c, id.warpVar, false);
    var->setTooltip ("Warp Var: hard sync (left) to soft sync (right)");
    addChildComponent (*var);
    for (auto* p : { &pen1, &pen2 })
    {
        addChildComponent (*p);
        p->setTooltip ("Edit the remap curve");
    }
    pen1.onClick = [this] (const ModifierKeys&) { if (onEditRemap) onEditRemap (0); };
    pen2.onClick = [this] (const ModifierKeys&) { if (onEditRemap) onEditRemap (1); };
    ctx.add (this);
    refresh();
}

WarpSection::~WarpSection() { ctx.remove (this); }

void WarpSection::refresh()
{
    const auto& id = ids().osc[osc];
    const int a = ctx.proc.params.geti (id.warp1Mode), b = ctx.proc.params.geti (id.warp2Mode);
    const int key = a * 100 + b;
    if (key == shownModes) return;
    shownModes = key;
    auto isRemap = [] (int m) { return m == warp::Remap1 || m == warp::Remap2 || m == warp::Remap4; };
    var->setVisible (a == warp::Sync || b == warp::Sync);
    pen1.setVisible (isRemap (a));
    pen2.setVisible (isRemap (b));
    repaint();
}

void WarpSection::resized()
{
    const int w = getWidth();
    const int kw = 42;
    k1->setBounds (0, 2, kw, 54);
    k2->setBounds (w - kw, 2, kw, 54);
    const int fx = kw + 2, fw = w - 2 * kw - 4;
    m1->setBounds (fx, 4, fw, 20);
    m2->setBounds (fx, 28, fw, 20);
    var->setBounds (fx + 26, 66, fw - 26, 16);
    pen1.setBounds (kw / 2 - 10, 60, 20, 18);
    pen2.setBounds (w - kw / 2 - 10, 60, 20, 18);
    (void) compact;
}

void WarpSection::paint (Graphics& g)
{
    const int w = getWidth();
    const auto cap = Rectangle<float> (44.0f, 50.0f, (float) w - 88.0f, 13.0f);
    g.setColour (colours::inkDim);
    const float y = cap.getCentreY();
    const float tw = 40.0f;
    g.drawHorizontalLine ((int) y, cap.getX() + 2.0f, cap.getCentreX() - tw * 0.5f - 3.0f);
    g.drawHorizontalLine ((int) y, cap.getCentreX() + tw * 0.5f + 3.0f, cap.getRight() - 2.0f);
    drawText (g, "WARP", cap.withSizeKeepingCentre (tw, 13.0f), 10.5f, colours::ink, true);
    if (var->isVisible())
        drawText (g, "VAR", { 44.0f, 66.0f, 26.0f, 16.0f }, 10.5f, colours::inkDim, true, Justification::centredLeft);
}

// ===========================================================================
// Remap curve editor
// ===========================================================================
namespace
{
class RemapEditor final : public Popover, public Refreshable
{
public:
    RemapEditor (PanelContext& c, int o, int s) : Popover ("REMAP " + String (s + 1)), ctx (c), osc (o), slot (s)
    {
        ctx.add (this);
        setRepaintsOnMouseActivity (false);
    }
    ~RemapEditor() override { ctx.remove (this); }

    void refresh() override
    {
        const auto& cur = ctx.proc.patch().wavetable[(size_t) osc].remap[slot];
        if (! (cur == shown)) { shown = cur; repaint(); }
    }

    Rectangle<float> graph() const { return getLocalBounds().toFloat().reduced (8.0f).withTrimmedTop (16.0f); }
    Point<float> toScreen (float x, float y) const { auto r = graph(); return { r.getX() + x * r.getWidth(), r.getBottom() - y * r.getHeight() }; }
    Point<float> fromScreen (Point<float> p) const
    {
        auto r = graph();
        return { jlimit (0.0f, 1.0f, (p.x - r.getX()) / r.getWidth()), jlimit (0.0f, 1.0f, (r.getBottom() - p.y) / r.getHeight()) };
    }

    void paint (Graphics& g) override
    {
        Popover::paint (g);
        const auto r = graph();
        g.setColour (colours::grid);
        for (int i = 1; i < 4; ++i)
        {
            g.drawVerticalLine ((int) (r.getX() + r.getWidth() * (float) i / 4.0f), r.getY(), r.getBottom());
            g.drawHorizontalLine ((int) (r.getY() + r.getHeight() * (float) i / 4.0f), r.getX(), r.getRight());
        }
        g.drawRect (r, 1.0f);
        g.setColour (colours::inkFaint);
        g.drawLine ({ r.getBottomLeft(), r.getTopRight() }, 1.0f);
        Path p;
        for (int i = 0; i <= 200; ++i)
        {
            const float x = (float) i / 200.0f;
            const auto pt = toScreen (x, shown.evaluate (x));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (colours::brand);
        g.strokePath (p, PathStrokeType (1.8f));
        for (int i = 0; i < (int) shown.points.size(); ++i)
        {
            const auto pt = toScreen (shown.points[(size_t) i].x, shown.points[(size_t) i].y);
            g.setColour (i == dragIndex ? colours::cream : colours::brand);
            g.fillEllipse (Rectangle<float> (7.0f, 7.0f).withCentre (pt));
        }
        drawText (g, "double-click: add / remove", r.withTop (r.getBottom() - 14.0f), 10.5f, colours::inkFaint, false, Justification::centredRight);
    }

    int pointAt (Point<float> pos) const
    {
        for (int i = 0; i < (int) shown.points.size(); ++i)
            if (toScreen (shown.points[(size_t) i].x, shown.points[(size_t) i].y).getDistanceFrom (pos) < 7.0f) return i;
        return -1;
    }

    void edit (const String& label, std::function<void (Curve&)> fn)
    {
        const int o = osc, s = slot;
        ctx.proc.editPatch (label, [o, s, fn] (Patch& p)
        {
            auto& w = p.wavetable[(size_t) o];
            fn (w.remap[s]);
            w.remap[s].sortAndClamp();
            w.bakeRemaps();
        });
        refresh();
    }

    void mouseDown (const MouseEvent& e) override
    {
        dragIndex = pointAt (e.position);
        moved = false;
        repaint();
    }

    void mouseDrag (const MouseEvent& e) override
    {
        if (dragIndex < 0) return;
        const auto v = fromScreen (e.position);
        const int i = dragIndex;
        const int n = (int) shown.points.size();
        edit ({}, [v, i, n] (Curve& c)
        {
            auto& pt = c.points[(size_t) i];
            if (i > 0 && i < n - 1)
                pt.x = jlimit (c.points[(size_t) i - 1].x + 0.001f, c.points[(size_t) i + 1].x - 0.001f, v.x);
            pt.y = v.y;
        });
        moved = true;
    }

    void mouseUp (const MouseEvent&) override
    {
        if (moved) ctx.proc.commitHistory ("Edit Remap");
        dragIndex = -1;
        moved = false;
        repaint();
    }

    void mouseDoubleClick (const MouseEvent& e) override
    {
        const int i = pointAt (e.position);
        if (i >= 0) { edit ("Remove Remap Point", [i] (Curve& c) { c.remove (i); }); return; }
        const auto v = fromScreen (e.position);
        edit ("Add Remap Point", [v] (Curve& c) { c.insert (v.x, v.y); });
    }

private:
    PanelContext& ctx;
    int osc, slot;
    Curve shown;
    int dragIndex = -1;
    bool moved = false;
};

// ---------------------------------------------------------------------------
class UnisonPopover final : public Popover
{
public:
    UnisonPopover (PanelContext& c, int osc, int type) : Popover ("UNISON")
    {
        const auto& id = ids().osc[osc];
        auto choice = [&] (int p, const char* cap)
        {
            auto ch = std::make_unique<ParamChoice> (c, p);
            ch->caption = cap;
            ch->captionLeft = true;
            ch->fontH = 11.5f;
            addAndMakeVisible (*ch);
            rows.push_back ({ ch.get(), true });
            owned.push_back (std::move (ch));
        };
        auto field = [&] (int p, const char* cap, std::function<String (float)> fmt = {})
        {
            auto f = std::make_unique<ParamField> (c, p, cap);
            f->fontH = 11.5f;
            if (fmt) f->format = std::move (fmt);
            addAndMakeVisible (*f);
            rows.push_back ({ f.get(), false });
            owned.push_back (std::move (f));
        };
        auto pct = [] (float v) { return String (roundToInt (v * 100.0f)); };
        auto st = [] (float v) { return String (v, v < 10.0f ? 1 : 0); };
        choice (id.uniMode, "MODE");
        choice (id.uniStack, "STACK");
        if (auto* stack = dynamic_cast<ParamChoice*> (owned.back().get()))
            stack->label = [] (int i) { static const char* n[] { "OFF", "12", "12+7", "C-12", "C-24" }; return String (n[jlimit (0, 4, i)]); };
        if (type == 3 || type == 4)
        {
            field (id.detune, type == 3 ? "DTUNE" : "DETUNE", [] (float v) { return String (v, 2); });
            field (id.blend, "BLEND", pct);
        }
        field (id.uniWidth, "WIDTH", pct);
        field (id.uniRange, "RANGE", st);
        if (type == 0) field (id.uniWtpos, "WT POS", pct);
        if (type == 2 || type == 3 || type == 4)
        {
            field (id.uniStart, "START", pct);
            field (id.uniSpan, "SPAN", pct);
        }
        if (type != 3)
        {
            field (id.uniWarp1, type == 1 ? "WARP 1" : "WARP A", pct);
            field (id.uniWarp2, type == 1 ? "WARP 2" : "WARP B", pct);
        }
        if (type == 3)
        {
            auto sp = std::make_unique<PatchChoice> (c);
            sp->caption = "SPAWN";
            sp->captionLeft = true;
            sp->fontH = 11.5f;
            auto& proc = c.proc;
            const int o = osc;
            static const char* names[] { "Together", "Even", "Exp", "Random" };
            sp->getText = [&proc, o] { return String (names[jlimit (0, 3, proc.patch().sample[(size_t) o].spawnPattern)]).toUpperCase(); };
            sp->setTooltip ("Spawn pattern of the unison grain voices");
            sp->buildMenu = [&proc, o]
            {
                Menu m;
                const int cur = proc.patch().sample[(size_t) o].spawnPattern;
                for (int i = 0; i < 4; ++i)
                    m.add (names[i], [&proc, o, i] { proc.editPatch ("Spawn Pattern", [o, i] (Patch& p) { p.sample[(size_t) o].spawnPattern = i; }); }, i == cur);
                return m;
            };
            sp->onStep = [&proc, o] (int d)
            {
                const int cur = proc.patch().sample[(size_t) o].spawnPattern;
                const int n = (cur + d + 4) % 4;
                proc.editPatch ("Spawn Pattern", [o, n] (Patch& p) { p.sample[(size_t) o].spawnPattern = n; });
            };
            addAndMakeVisible (*sp);
            spawn = sp.get();
            owned.push_back (std::move (sp));
        }
    }

    void layout (Rectangle<int> b) override
    {
        // Two columns of rows; the dropdowns take the first row.
        const int colW = (b.getWidth() - 6) / 2;
        const int rh = 19, gap = 3;
        int i = 0, y = b.getY();
        for (auto& r : rows)
        {
            const int col = i % 2;
            r.c->setBounds (b.getX() + col * (colW + 6), y, colW, rh);
            if (col == 1) y += rh + gap;
            ++i;
        }
        if (i % 2 == 1) y += rh + gap;
        if (spawn != nullptr) spawn->setBounds (b.getX(), y, b.getWidth(), rh);
    }

private:
    struct Row { Component* c; bool choice; };
    std::vector<Row> rows;
    std::vector<std::unique_ptr<Component>> owned;
    Component* spawn = nullptr;
};

// ---------------------------------------------------------------------------
class WindowPopover final : public Popover, public Refreshable
{
public:
    WindowPopover (PanelContext& c, int o) : Popover ("GRAIN WINDOW"), ctx (c), osc (o)
    {
        const auto& id = ids().osc[osc];
        auto pct = [] (float v) { return String (roundToInt (v * 100.0f)); };
        auto field = [&] (int p, const char* cap)
        {
            auto f = std::make_unique<ParamField> (c, p, cap);
            f->fontH = 11.5f;
            f->format = pct;
            addAndMakeVisible (*f);
            fields.push_back (std::move (f));
        };
        field (id.winAmt, "AMT");
        field (id.winAmtRand, "RAND");
        field (id.winSkew, "SKEW");
        field (id.winSkewRand, "RAND");
        shape = std::make_unique<ParamChoice> (c, id.winShape);
        shape->caption = "SHAPE";
        shape->captionLeft = true;
        shape->fontH = 11.5f;
        addAndMakeVisible (*shape);
        ctx.add (this);
    }
    ~WindowPopover() override { ctx.remove (this); }

    void refresh() override
    {
        const auto& id = ids().osc[osc];
        const float k = ctx.proc.params.get (id.winAmt) * 7.0f + ctx.proc.params.get (id.winSkew) * 13.0f + ctx.proc.params.get (id.winShape) * 17.0f;
        if (! same (k, key)) { key = k; repaint(); }
    }

    void layout (Rectangle<int> b) override
    {
        preview = b.removeFromLeft (b.getWidth() * 2 / 5).toFloat().reduced (2.0f);
        b.removeFromLeft (6);
        const int rh = 19, gap = 4;
        const int colW = (b.getWidth() - 4) / 2;
        for (int i = 0; i < 4; ++i)
            fields[(size_t) i]->setBounds (b.getX() + (i % 2) * (colW + 4), b.getY() + (i / 2) * (rh + gap), colW, rh);
        shape->setBounds (b.getX(), b.getY() + 2 * (rh + gap), b.getWidth(), rh);
    }

    void paint (Graphics& g) override
    {
        Popover::paint (g);
        const auto& id = ids().osc[osc];
        const int sh = ctx.proc.params.geti (id.winShape);
        const float amt = ctx.proc.params.get (id.winAmt), skew = ctx.proc.params.get (id.winSkew);
        g.setColour (colours::grid);
        g.drawRect (preview, 1.0f);
        Path p;
        const float peak = 0.5f + 0.45f * skew;
        for (int i = 0; i <= 100; ++i)
        {
            const float t = (float) i / 100.0f;
            // Skew moves the peak: map t so that `peak` lands on 0.5.
            const float u = t < peak ? 0.5f * t / jmax (0.001f, peak) : 0.5f + 0.5f * (t - peak) / jmax (0.001f, 1.0f - peak);
            const float w = 1.0f + (SampleOsc::windowShape (sh, u) - 1.0f) * amt;
            const auto pt = Point<float> (preview.getX() + t * preview.getWidth(), preview.getBottom() - 2.0f - w * (preview.getHeight() - 6.0f));
            if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
        }
        g.setColour (loopBlue());
        g.strokePath (p, PathStrokeType (1.6f));
    }

private:
    PanelContext& ctx;
    int osc;
    float key = -1.0f;
    Rectangle<float> preview;
    std::vector<std::unique_ptr<ParamField>> fields;
    std::unique_ptr<ParamChoice> shape;
};
} // namespace

std::unique_ptr<Popover> createRemapEditor (PanelContext& ctx, int osc, int slot) { return std::make_unique<RemapEditor> (ctx, osc, slot); }
std::unique_ptr<Popover> createUnisonPopover (PanelContext& ctx, int osc, int type) { return std::make_unique<UnisonPopover> (ctx, osc, type); }
std::unique_ptr<Popover> createWindowPopover (PanelContext& ctx, int osc) { return std::make_unique<WindowPopover> (ctx, osc); }

// ===========================================================================
// PatchChoice / PatchField
// ===========================================================================
PatchChoice::PatchChoice (PanelContext& c) : ctx (c) { ctx.add (this); }
PatchChoice::~PatchChoice() { ctx.remove (this); }

void PatchChoice::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! buildMenu) return;
    showMenu (*this, buildMenu());
}

void PatchChoice::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    if (onStep) onStep (w.deltaY > 0 ? -1 : 1);
    refresh();
}

PatchField::PatchField (PanelContext& c, String cap) : caption (std::move (cap)), ctx (c) { ctx.add (this); }
PatchField::~PatchField() { ctx.remove (this); }

void PatchField::refresh()
{
    const float v = get ? get() : 0.0f;
    if (! same (v, shown)) { shown = v; repaint(); }
}

void PatchField::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    if (caption.isNotEmpty())
    {
        const float w = jmin (r.getWidth() * 0.55f, (float) GlyphArrangement::getStringWidth (panelFont (10.5f), caption) + 8.0f);
        drawText (g, caption, r.removeFromLeft (w), 10.5f, colours::inkDim, true, Justification::centredLeft);
    }
    r = r.reduced (0.5f);
    drawLcdBox (g, r);
    const float v = get ? get() : 0.0f;
    drawText (g, format ? format (v) : String (roundToInt (v * 100.0f)), r.reduced (3.0f, 0.0f), 12.0f, isEnabled() ? colours::lcd : colours::lcdDim, true);
}

void PatchField::mouseDown (const MouseEvent&)
{
    startV = get ? get() : 0.0f;
    dragging = false;
}

void PatchField::mouseDrag (const MouseEvent& e)
{
    if (! set) return;
    dragging = true;
    const float dy = (float) -e.getDistanceFromDragStartY() + (float) e.getDistanceFromDragStartX() * 0.5f;
    set (jlimit (0.0f, 1.0f, startV + dy / (e.mods.isShiftDown() ? 1000.0f : 200.0f)), false);
    refresh();
}

void PatchField::mouseUp (const MouseEvent&)
{
    if (dragging && set) set (get ? get() : 0.0f, true);
    dragging = false;
}
} // namespace tincture::ui::oscui
