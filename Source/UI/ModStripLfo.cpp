// ModStripLfo.cpp — the LFO section of the modulation strip (spec D §2):
// tabs, the graph editor with its tools and menus, the shape menu, the knob
// row, and the pop-out LFO editor.
#include "ModStrip.h"
#include "../Settings.h"

namespace tincture::ui
{
using namespace juce;

namespace
{
const Colour kLfoColour = PanelContext::sourceColour (ModSource::Lfo1);
constexpr int kHistory = 160;

const char* kToolNames[] { "Point", "Flat", "Ramp Up", "Ramp Down" };

void toolIcon (int tool, Graphics& g, Rectangle<float> r, Colour c)
{
    g.setColour (c);
    const auto m = r.getCentre();
    const float w = jmin (r.getWidth(), r.getHeight()) * 0.42f;
    switch (tool)
    {
        case 0: g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (m)); break;
        case 1: g.drawLine (m.x - w, m.y, m.x + w, m.y, 1.8f); break;
        case 2: g.drawLine (m.x - w, m.y + w * 0.7f, m.x + w, m.y - w * 0.7f, 1.8f); break;
        default: g.drawLine (m.x - w, m.y - w * 0.7f, m.x + w, m.y + w * 0.7f, 1.8f); break;
    }
}

void popoutIcon (Graphics& g, Rectangle<float> r, Colour c)
{
    g.setColour (c);
    const auto b = Rectangle<float> (10.0f, 9.0f).withCentre (r.getCentre().translated (-1.0f, 1.0f));
    g.drawRect (b, 1.2f);
    g.drawLine (b.getCentreX(), b.getCentreY(), b.getRight() + 2.0f, b.getY() - 2.0f, 1.4f);
    g.drawLine (b.getRight() - 2.0f, b.getY() - 2.0f, b.getRight() + 2.0f, b.getY() - 2.0f, 1.4f);
    g.drawLine (b.getRight() + 2.0f, b.getY() - 2.0f, b.getRight() + 2.0f, b.getY() + 2.0f, 1.4f);
}

void gridIcon (Graphics& g, Rectangle<float> r, bool horizontal)
{
    g.setColour (colours::inkDim);
    const auto m = r.getCentre();
    for (int i = -1; i <= 1; ++i)
    {
        if (horizontal) g.drawLine (m.x - 5.0f, m.y + (float) i * 3.5f, m.x + 5.0f, m.y + (float) i * 3.5f, 1.3f);
        else g.drawLine (m.x + (float) i * 3.5f, m.y - 5.0f, m.x + (float) i * 3.5f, m.y + 5.0f, 1.3f);
    }
}

File userShapeFolder() { return Settings::userShapes(); }

Array<File> userShapeFiles()
{
    auto files = userShapeFolder().findChildFiles (File::findFiles, false, "*.tshape");
    files.sort();
    return files;
}

bool loadShapeFile (const File& f, Curve& out)
{
    MemoryBlock mb;
    if (! f.loadFileAsData (mb)) return false;
    Node n;
    if (! Node::fromBinary ((const uint8_t*) mb.getData(), mb.getSize(), n)) return false;
    out.load (n);
    return ! out.points.empty();
}

bool saveShapeFile (const String& name, const Curve& c)
{
    const auto clean = File::createLegalFileName (name.trim());
    if (clean.isEmpty()) return false;
    userShapeFolder().createDirectory();
    Node n ("shape");
    c.save (n);
    const auto bin = n.toBinary();
    return userShapeFolder().getChildFile (clean + ".tshape").replaceWithData (bin.data(), bin.size());
}

void loadShape (TinctureProcessor& proc, int l, const Curve& c, const String& name)
{
    proc.editPatch ("Load LFO Shape", [l, c, name] (Patch& p)
    {
        auto& d = p.lfo[(size_t) l];
        d.shape = c;
        d.shapeName = name.toStdString();
        d.loopback = -1;
        d.pointMods.clear();
        for (auto& b : d.buses) b = LfoBus();       // a new shape has no point modulation
    });
}

// Every LFO setting parameter (for Alt-drag copy).
std::vector<int> lfoParams (int l)
{
    const auto& i = ids().lfo[l];
    return { i.type, i.mode, i.mono, i.direction, i.gridX, i.gridY, i.host, i.bpm, i.rate, i.rateSync, i.trip, i.dot,
             i.swing, i.x10, i.rise, i.delay, i.smooth, i.phase, i.phaseSnap };
}

// The point context menu (§2.6.1).
Menu lfoPointMenu (PanelContext& ctx, int l, CurveEditor& g, int point, bool handle)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().lfo[(size_t) l];
    Component::SafePointer<CurveEditor> ed (&g);
    Menu m;
    m.header ("Shift-Key Function:");
    for (int t = 1; t <= 3; ++t)
        m.add (kToolNames[t], [ed, t, &proc]
        {
            proc.uiState.setProperty ("lfoShiftFn", t, nullptr);
            if (ed) ed->shiftTool = (CurveEditor::Tool) t;
        }, (int) g.shiftTool == t);
    m.separator();

    // Targets: the selection when the clicked point is in it, else the point.
    std::vector<int> targets;
    if (point >= 0)
    {
        if (! handle && g.selection.count (point) > 0) targets.assign (g.selection.begin(), g.selection.end());
        else targets.push_back (point);
    }
    auto busMenu = [&] (int kind)
    {
        Menu buses;
        const int next = d.nextFreeBus();
        for (int b = 0; b < kMaxLfoBuses; ++b)
        {
            const bool used = d.buses[b].source >= 0;
            if (! used && b != next) continue;
            bool subscribed = false;
            for (const auto& pm : d.pointMods)
                if (pm.bus == b && std::find (targets.begin(), targets.end(), pm.point) != targets.end()
                    && ! same (kind == 0 ? pm.dx : kind == 1 ? pm.dy : pm.dc, 0.0f)) subscribed = true;
            auto pick = [&proc, l, b, kind, targets] (ModSource s)
            {
                proc.editPatch ("Modulate LFO Point", [l, b, kind, targets, s] (Patch& p)
                {
                    auto& dd = p.lfo[(size_t) l];
                    dd.buses[b].source = (int) s;
                    if (same (dd.buses[b].amount, 0.0f)) dd.buses[b].amount = 1.0f;
                    for (int pt : targets)
                    {
                        LfoPointMod* pm = nullptr;
                        for (auto& q : dd.pointMods) if (q.point == pt && q.bus == b) pm = &q;
                        if (pm == nullptr) { dd.pointMods.push_back ({ pt, b, 0.0f, 0.0f, 0.0f }); pm = &dd.pointMods.back(); }
                        if (kind == 0) pm->dx = 0.1f;
                        else if (kind == 1) pm->dy = 0.25f;
                        else pm->dc = 0.5f;
                    }
                });
            };
            buses.sub ("LFO Bus " + String (b + 1), sourceMenu (pick, used ? (ModSource) d.buses[b].source : ModSource::None), subscribed);
        }
        return buses;
    };
    if (handle || point < 0)
        m.sub ("Modulate Curves:", busMenu (2), false, point >= 1 && d.nextFreeBus() >= -1);
    else
    {
        m.sub ("Modulate X:", busMenu (0));
        m.sub ("Modulate Y:", busMenu (1));
    }
    m.separator();

    const int loopAt = handle ? point - 1 : point;
    const int n = (int) d.shape.points.size();
    m.add ("Set Loopback Point Here", [&proc, l, loopAt, n]
    {
        proc.editPatch ("Set Loopback Point", [l, loopAt, n] (Patch& p) { p.lfo[(size_t) l].loopback = loopAt >= n - 1 ? -1 : loopAt; });
    }, false, loopAt >= 0);
    m.add ("Remove Loopback Point", [&proc, l] { proc.editPatch ("Remove Loopback Point", [l] (Patch& p) { p.lfo[(size_t) l].loopback = -1; }); },
           false, d.loopback >= 0);
    std::set<int> sel = g.selection;
    bool removable = false;
    for (int i : sel) removable |= i > 0 && i < n - 1;
    m.add ("Remove Selected Points", [&proc, l, sel, ed]
    {
        proc.editPatch ("Remove Points", [l, sel] (Patch& p)
        {
            auto& dd = p.lfo[(size_t) l];
            for (auto it = sel.rbegin(); it != sel.rend(); ++it) dd.shape.remove (*it);
            dd.pointMods.erase (std::remove_if (dd.pointMods.begin(), dd.pointMods.end(), [&] (const LfoPointMod& q) { return q.point >= (int) dd.shape.points.size(); }), dd.pointMods.end());
            if (dd.loopback >= (int) dd.shape.points.size() - 1) dd.loopback = -1;
        });
        if (ed) ed->selection.clear();
    }, false, removable);
    m.add ("Flip Vertical", [&proc, l, sel] { proc.editPatch ("Flip Vertical", [l, sel] (Patch& p) { p.lfo[(size_t) l].shape = CurveEditor::flipVertical (p.lfo[(size_t) l].shape, sel); }); });
    m.add ("Flip Horizontal", [&proc, l, sel] { proc.editPatch ("Flip Horizontal", [l, sel] (Patch& p) { p.lfo[(size_t) l].shape = CurveEditor::flipHorizontal (p.lfo[(size_t) l].shape, sel); }); });
    return m;
}
} // namespace

// ===========================================================================
// Shared LFO helpers
// ===========================================================================
void bindLfoGraph (CurveEditor& g, PanelContext& ctx, const int* lfo)
{
    auto& proc = ctx.proc;
    g.colour = kLfoColour;
    g.shiftTool = (CurveEditor::Tool) jlimit (1, 3, (int) proc.uiState.getProperty ("lfoShiftFn", 1));
    g.getCurve = [&proc, lfo] { return proc.patch().lfo[(size_t) *lfo].shape; };
    g.onEdit = [&proc, lfo] (const Curve& c)
    {
        const int l = *lfo;
        proc.editPatch ({}, [l, c] (Patch& p)
        {
            auto& d = p.lfo[(size_t) l];
            d.shape = c;
            const int n = (int) c.points.size();
            if (d.loopback >= n - 1) d.loopback = -1;
            d.pointMods.erase (std::remove_if (d.pointMods.begin(), d.pointMods.end(), [n] (const LfoPointMod& q) { return q.point >= n; }), d.pointMods.end());
        });
    };
    g.onCommit = [&proc] (const String& label) { proc.commitHistory (label == "Move Point" ? "Edit LFO" : label); };
    CurveEditor* ed = &g;
    g.pointMenu = [&ctx, lfo, ed] (int point, bool handle) { return lfoPointMenu (ctx, *lfo, *ed, point, handle); };
    g.onSetLoopback = [&proc, lfo] (int point)
    {
        const int l = *lfo;
        if (proc.params.geti (ids().lfo[l].mode) != (int) LfoMode::Envelope) return;
        proc.editPatch ("Set Loopback Point", [l, point] (Patch& p)
        {
            auto& d = p.lfo[(size_t) l];
            d.loopback = point >= (int) d.shape.points.size() - 1 ? -1 : point;
        });
    };
    g.onPointDrop = [&proc, lfo, ed] (int point, int axis, ModSource s)
    {
        const int l = *lfo;
        std::vector<int> targets;
        if (ed->selection.count (point) > 0) targets.assign (ed->selection.begin(), ed->selection.end());
        else targets.push_back (point);
        proc.editPatch ("Modulate LFO Point", [l, axis, s, targets] (Patch& p)
        {
            auto& d = p.lfo[(size_t) l];
            const int b = d.nextFreeBus();
            if (b < 0) return;
            d.buses[b].source = (int) s;
            d.buses[b].amount = 1.0f;
            for (int pt : targets)
                d.pointMods.push_back ({ pt, b, axis == 0 ? 0.1f : 0.0f, axis == 1 ? 0.25f : 0.0f, 0.0f });
        });
    };
}

void refreshLfoGraph (CurveEditor& g, PanelContext& ctx, int l, std::deque<float>* history)
{
    auto& proc = ctx.proc;
    const auto& id = ids().lfo[l];
    const auto& d = proc.patch().lfo[(size_t) l];
    g.gridX = proc.params.geti (id.gridX);
    g.gridY = proc.params.geti (id.gridY);
    const auto type = (LfoType) proc.params.geti (id.type);
    const auto& t = proc.telemetry();
    const bool live = t.activeVoices.load() > 0;
    const bool shapeless = type == LfoType::Lorenz || type == LfoType::Rossler || type == LfoType::SampleHold;
    g.editable = ! shapeless;
    if (shapeless && history != nullptr)
    {
        history->push_back (t.lfo[l].load());
        while ((int) history->size() > kHistory) history->pop_front();
        g.trace.assign (history->begin(), history->end());
        g.overlayText = String (choices::lfoTypes()[(size_t) type]).toUpperCase();
    }
    else
    {
        if (history != nullptr) history->clear();
        g.trace.clear();
        g.overlayText = type == LfoType::Path ? "PATH" : "";
    }
    g.playhead = live ? t.lfoPhase[l].load() : -1.0f;
    g.loopback = d.loopback;
    std::vector<CurveEditor::ModBar> bars;
    for (const auto& pm : d.pointMods)
    {
        if (pm.bus < 0 || pm.bus >= kMaxLfoBuses || d.buses[pm.bus].source < 0) continue;
        const float a = d.buses[pm.bus].amount;
        bars.push_back ({ pm.point, pm.dx * a, pm.dy * a, pm.dc * a, PanelContext::sourceColour ((ModSource) d.buses[pm.bus].source) });
    }
    const bool barsChanged = bars.size() != g.modBars.size();
    g.modBars = std::move (bars);
    if (barsChanged) g.repaint();
    g.update();
}

Menu lfoShapeMenu (PanelContext& ctx, int l, Component* saveAnchor)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().lfo[(size_t) l];
    const String current (d.shapeName);
    Menu m;
    m.add ("Default", [&proc, l] { loadShape (proc, l, LfoShapes::defaultShape(), "Default"); }, current == "Default");
    m.separator();
    for (const auto& folder : LfoShapes::factory())
    {
        Menu sub;
        bool has = false;
        for (const auto& s : folder.shapes)
        {
            const String name (s.name);
            const auto curve = s.curve;
            sub.add (name, [&proc, l, curve, name] { loadShape (proc, l, curve, name); }, name == current);
            has |= name == current;
        }
        m.sub (folder.name, sub, has);
    }
    {
        Menu user;
        bool has = false;
        for (const auto& f : userShapeFiles())
        {
            const auto name = f.getFileNameWithoutExtension();
            user.add (name, [&proc, l, f, name] { Curve c; if (loadShapeFile (f, c)) loadShape (proc, l, c, name); }, name == current);
            has |= name == current;
        }
        if (user.empty()) user.add ("(no saved shapes)", [] {}, false, false);
        m.sub ("User", user, has);
    }
    m.separator();
    m.add ("Random (Curved)", [&proc, l]
    {
        const int gx = proc.params.geti (ids().lfo[l].gridX);
        loadShape (proc, l, LfoShapes::randomCurved (gx, (uint32_t) Random::getSystemRandom().nextInt()), "Random (Curved)");
    });
    m.add ("Random (Flat)", [&proc, l]
    {
        const int gx = proc.params.geti (ids().lfo[l].gridX);
        loadShape (proc, l, LfoShapes::randomFlat (gx, (uint32_t) Random::getSystemRandom().nextInt()), "Random (Flat)");
    });
    m.separator();
    for (int o = 0; o < kNumOscs; ++o)
    {
        const bool has = proc.patch().wavetable[(size_t) o].table != nullptr;
        m.add ("Wavetable " + String::charToString ((juce_wchar) ('A' + o)) + " to LFO", [&proc, l, o]
        {
            const auto table = proc.patch().wavetable[(size_t) o].table;
            if (table == nullptr) return;
            const int frames = jmax (1, table->numFrames());
            const int frame = jlimit (0, frames - 1, roundToInt (proc.params.getNorm (ids().osc[o].wtpos) * (float) (frames - 1)));
            std::vector<float> buf ((size_t) Wavetable::kFrameSize);
            table->renderFrame (frame, buf.data());
            loadShape (proc, l, LfoShapes::fromWavetableFrame (buf), "Wavetable " + String::charToString ((juce_wchar) ('A' + o)));
        }, false, has);
    }
    m.separator();
    Component::SafePointer<Component> anchor (saveAnchor);
    PanelContext* pc = &ctx;
    m.add ("Save Shape...", [pc, l, anchor]
    {
        auto& p = pc->proc;
        const auto curve = p.patch().lfo[(size_t) l].shape;
        auto doSave = [&p, l, curve] (const String& name)
        {
            if (saveShapeFile (name, curve))
                p.editPatch ("Save LFO Shape", [l, name] (Patch& pp) { pp.lfo[(size_t) l].shapeName = name.trim().toStdString(); });
        };
        if (pc->editValue && anchor != nullptr)
            pc->editValue (*anchor, String (p.patch().lfo[(size_t) l].shapeName), doSave);
    });
    return m;
}

void stepLfoShape (PanelContext& ctx, int l, int delta)
{
    auto& proc = ctx.proc;
    const String current (proc.patch().lfo[(size_t) l].shapeName);
    // Find the current folder (factory, then User); else step through Basic.
    for (const auto& folder : LfoShapes::factory())
        for (int i = 0; i < (int) folder.shapes.size(); ++i)
            if (String (folder.shapes[(size_t) i].name) == current)
            {
                const int n = (int) folder.shapes.size();
                const auto& s = folder.shapes[(size_t) (((i + delta) % n + n) % n)];
                loadShape (proc, l, s.curve, s.name);
                return;
            }
    const auto files = userShapeFiles();
    for (int i = 0; i < files.size(); ++i)
        if (files[i].getFileNameWithoutExtension() == current)
        {
            const auto& f = files[((i + delta) % files.size() + files.size()) % files.size()];
            Curve c;
            if (loadShapeFile (f, c)) loadShape (proc, l, c, f.getFileNameWithoutExtension());
            return;
        }
    const auto& basic = LfoShapes::factory().front().shapes;
    const auto& s = basic[delta >= 0 ? 0 : basic.size() - 1];
    loadShape (proc, l, s.curve, s.name);
}

void lfoRateExtras (PanelContext& ctx, int l, Menu& m)
{
    auto& proc = ctx.proc;
    const auto& id = ids().lfo[l];
    const int p = proc.params.on (id.bpm) ? id.swing : id.x10;
    m.toggle (proc.params.on (id.bpm) ? "Swing" : "10x", proc.params.on (p), [&proc, p] { proc.setWithUndo (p, proc.params.on (p) ? 0.0f : 1.0f); });
}

// ===========================================================================
// LFO panel
// ===========================================================================
LfoPanel::LfoPanel (PanelContext& c, OscPageHost& h) : ctx (c), host_ (h)
{
    ctx.add (this);
    auto& proc = ctx.proc;
    for (int l = 0; l < kNumLfos; ++l)
    {
        tabs[l] = std::make_unique<SourceTile> (ctx, lfoSource (l), "LFO " + String (l + 1));
        tabs[l]->onClick = [this, l] { select (l); };
        // Alt-drag one LFO tab onto another: copy all settings (§2.1).
        tabs[l]->onTileDrop = [&proc, l] (ModSource from, bool alt)
        {
            const int src = (int) from - (int) ModSource::Lfo1;
            if (! alt || src < 0 || src >= kNumLfos || src == l) return;
            proc.editAll ("Copy LFO", [src, l] (Patch& p, ParamValues& v)
            {
                p.lfo[(size_t) l] = p.lfo[(size_t) src];
                const auto a = lfoParams (src), b = lfoParams (l);
                for (size_t i = 0; i < a.size(); ++i) v[b[i]] = v[a[i]];
            });
        };
        addChildComponent (*tabs[l]);
    }
    const auto& id = ids().lfo[0];
    type = std::make_unique<ParamChoice> (ctx, id.type);
    type->fontH = 11.5f;
    type->label = [] (int i) { return String (choices::lfoTypes()[(size_t) jlimit (0, 4, i)]).replace ("Chaos: ", ""); };
    addAndMakeVisible (*type);
    for (int i = 0; i < 3; ++i)
    {
        modeBtn[i].setFontHeight (10.5f);
        modeBtn[i].setLitColour (kLfoColour);
        modeBtn[i].onClick = [this, i] (const ModifierKeys&) { ctx.proc.setWithUndo (ids().lfo[sel].mode, (float) i); refresh(); };
        addAndMakeVisible (modeBtn[i]);
    }
    modeBtn[0].setTooltip ("FREE: ignores notes, every voice shares one phase");
    modeBtn[1].setTooltip ("RETRIG: phase restarts at PHASE on every note");
    modeBtn[2].setTooltip ("ENVELOPE: plays one cycle and holds (loops from a loopback point)");
    mono = std::make_unique<ParamToggle> (ctx, id.mono, "MONO");
    mono->setFontHeight (10.5f);
    addAndMakeVisible (*mono);

    bindLfoGraph (graph, ctx, &sel);
    addAndMakeVisible (graph);

    popout.icon = popoutIcon;
    popout.setTooltip ("Open the large LFO editor");
    popout.onClick = [this] (const ModifierKeys&) { openEditor(); };
    addAndMakeVisible (popout);
    for (int t = 0; t < 4; ++t)
    {
        toolBtn[t].icon = [t] (Graphics& g, Rectangle<float> r, Colour col) { toolIcon (t, g, r, col); };
        toolBtn[t].setTooltip (String (kToolNames[t]) + " tool");
        toolBtn[t].setLitColour (kLfoColour);
        toolBtn[t].onClick = [this, t] (const ModifierKeys&) { setTool ((CurveEditor::Tool) t); };
        addAndMakeVisible (toolBtn[t]);
    }
    setTool (CurveEditor::Tool::Point);

    shape.fontH = 11.5f;
    shape.getText = [this] { return String (ctx.proc.patch().lfo[(size_t) sel].shapeName); };
    shape.onOpen = [this] { showMenu (shape, lfoShapeMenu (ctx, sel, &shape), { MenuOptions::Place::Below, {}, 170, false }); };
    shape.setTooltip ("Shape presets");
    addAndMakeVisible (shape);
    for (auto* b : { &shapePrev, &shapeNext }) { b->setFontHeight (11.0f); addAndMakeVisible (*b); }
    shapePrev.onClick = [this] (const ModifierKeys&) { stepLfoShape (ctx, sel, -1); };
    shapeNext.onClick = [this] (const ModifierKeys&) { stepLfoShape (ctx, sel, 1); };

    direction = std::make_unique<ParamChoice> (ctx, id.direction);
    direction->fontH = 11.5f;
    direction->label = [] (int i) { return String (choices::lfoDirections()[(size_t) jlimit (0, 2, i)]); };
    addAndMakeVisible (*direction);
    gridY = std::make_unique<ParamField> (ctx, id.gridY);
    gridX = std::make_unique<ParamField> (ctx, id.gridX);
    for (auto* f : { gridY.get(), gridX.get() }) { f->fontH = 12.0f; f->pixelsPerStep = 8.0f; addAndMakeVisible (*f); }

    host = std::make_unique<ParamToggle> (ctx, id.host, "HOST");
    trip = std::make_unique<ParamToggle> (ctx, id.trip, "TRIP");
    dot = std::make_unique<ParamToggle> (ctx, id.dot, "DOT");
    for (auto* t : { host.get(), trip.get(), dot.get() }) { t->setFontHeight (10.5f); addAndMakeVisible (*t); }
    host->setTooltip ("Anchor the phase to the host transport");
    for (auto* b : { &bpmBtn, &hzBtn })
    {
        b->setFontHeight (10.5f);
        b->setLitColour (kLfoColour);
        addAndMakeVisible (*b);
    }
    bpmBtn.onClick = [this] (const ModifierKeys&) { ctx.proc.setWithUndo (ids().lfo[sel].bpm, 1.0f); refresh(); };
    hzBtn.onClick = [this] (const ModifierKeys&) { ctx.proc.setWithUndo (ids().lfo[sel].bpm, 0.0f); refresh(); };

    rate = std::make_unique<ParamKnob> (ctx, id.rateSync, "RATE");
    rateField = std::make_unique<ParamField> (ctx, id.rateSync);
    rateField->fontH = 12.0f;
    rateField->pixelsPerStep = 8.0f;
    rise = std::make_unique<ParamKnob> (ctx, id.rise, "RISE");
    delay = std::make_unique<ParamKnob> (ctx, id.delay, "DELAY");
    smooth = std::make_unique<ParamKnob> (ctx, id.smooth, "SMOOTH");
    phase = std::make_unique<ParamKnob> (ctx, id.phase, "PHASE");
    for (auto* k : { rate.get(), rise.get(), delay.get(), smooth.get(), phase.get() })
    {
        k->small = true;
        k->accent = kLfoColour;
        addAndMakeVisible (*k);
    }
    addAndMakeVisible (*rateField);
    rate->extraMenu = [this] (Menu& m) { lfoRateExtras (ctx, sel, m); };
    rateField->extraMenu = rate->extraMenu;
    phase->extraMenu = [this] (Menu& m)
    {
        auto& p = ctx.proc;
        const int s = ids().lfo[sel].phaseSnap;
        m.toggle ("Snap to Grid", p.params.on (s), [&p, s] { p.setWithUndo (s, p.params.on (s) ? 0.0f : 1.0f); });
    };

    select (jlimit (0, kNumLfos - 1, (int) proc.uiState.getProperty ("modLfo", 0)));
}

LfoPanel::~LfoPanel() { ctx.remove (this); }

int LfoPanel::visibleTabs() const
{
    // LFO 7-10 appear once any of LFO 6-10 has a destination (§2.1).
    const auto& m = ctx.proc.patch().matrix;
    for (int l = 5; l < kNumLfos; ++l)
        if (! m.rowsForSource (lfoSource (l)).empty()) return kNumLfos;
    return sel >= 6 ? kNumLfos : 6;
}

void LfoPanel::setTool (CurveEditor::Tool t)
{
    graph.tool = t;
    for (int i = 0; i < 4; ++i) toolBtn[i].setLit (i == (int) t);
}

void LfoPanel::select (int l)
{
    sel = jlimit (0, kNumLfos - 1, l);
    ctx.proc.uiState.setProperty ("modLfo", sel, nullptr);
    for (int i = 0; i < kNumLfos; ++i) { tabs[i]->selected = i == sel; tabs[i]->repaint(); }
    graph.selection.clear();
    history.clear();
    rebind();
    refresh();
    resized();
    repaint();
}

void LfoPanel::rebind()
{
    const auto& id = ids().lfo[sel];
    type->setParam (id.type);
    mono->setParam (id.mono);
    direction->setParam (id.direction);
    gridY->setParam (id.gridY);
    gridX->setParam (id.gridX);
    host->setParam (id.host);
    trip->setParam (id.trip);
    dot->setParam (id.dot);
    rise->setParam (id.rise);
    delay->setParam (id.delay);
    smooth->setParam (id.smooth);
    phase->setParam (id.phase);

    const bool bpm = ctx.proc.params.on (id.bpm);
    shownBpm = bpm;
    bpmBtn.setLit (bpm);
    hzBtn.setLit (! bpm);
    rate->setParam (bpm ? id.rateSync : id.rate);
    rateField->setParam (bpm ? id.rateSync : id.rate);
    auto& proc = ctx.proc;
    const int l = sel;
    if (bpm)
    {
        auto syncText = [&proc, l] (float plain)
        {
            const auto& names = choices::syncRates();
            String t (names[(size_t) jlimit (0, (int) names.size() - 1, (int) std::lround (plain))]);
            if (proc.params.on (ids().lfo[l].trip)) t << " T";
            if (proc.params.on (ids().lfo[l].dot)) t << " D";
            return t;
        };
        rateField->format = syncText;
        auto* k = rate.get();
        k->format = [k, syncText] (double n) { return syncText (params()[k->param()].fromNorm ((float) n)); };
        rise->format = [rk = rise.get()] (double n) { return beatsText (params()[rk->param()].fromNorm ((float) n)); };
        delay->format = [dk = delay.get()] (double n) { return beatsText (params()[dk->param()].fromNorm ((float) n)); };
    }
    else
    {
        auto hzText = [&proc, l] (float plain)
        {
            const float hz = plain * (proc.params.on (ids().lfo[l].x10) ? 10.0f : 1.0f);
            return String (hz, hz < 10.0f ? 2 : 1) + " Hz";
        };
        rateField->format = hzText;
        auto* k = rate.get();
        k->format = [k, hzText] (double n) { return hzText (params()[k->param()].fromNorm ((float) n)); };
        for (auto* kk : { rise.get(), delay.get() })
            kk->format = [kk] (double n) { const auto& i = params()[kk->param()]; return String (i.format (i.fromNorm ((float) n))); };
    }
    rateField->repaint();
    for (int t = 0; t < kNumLfos; ++t) tabs[t]->refresh();
}

void LfoPanel::openEditor()
{
    if (host_.overlayId() == "lfo-editor") { host_.closeOverlay(); return; }
    OscPageHost* h = &host_;
    auto close = [h]
    {
        // Close after the click that asked for it has finished.
        MessageManager::callAsync ([h] { if (h->overlayId() == "lfo-editor") h->closeOverlay(); });
    };
    host_.showOverlay (std::make_unique<LfoEditorOverlay> (ctx, sel, close), OscPageHost::Area::ModulesAndMods, "lfo-editor");
}

void LfoPanel::refresh()
{
    const auto& id = ids().lfo[sel];
    auto& P = ctx.proc.params;
    if (P.on (id.bpm) != shownBpm) rebind();
    const int mode = P.geti (id.mode);
    for (int i = 0; i < 3; ++i) modeBtn[i].setLit (i == mode);
    const int vis = visibleTabs();
    if (vis != shownTabs) { shownTabs = vis; resized(); }
    shape.refresh();
    rateField->refresh();
    const bool shapeless = graph.editable;   // updated below
    (void) shapeless;
    refreshLfoGraph (graph, ctx, sel, &history);
    for (int t = 0; t < 4; ++t) { toolBtn[t].setEnabled (graph.editable); toolBtn[t].setDimmed (! graph.editable); }
}

void LfoPanel::resized()
{
    const int vis = visibleTabs();
    for (int l = 0; l < kNumLfos; ++l)
    {
        tabs[l]->setVisible (l < vis);
        tabs[l]->setBounds (8 + roundToInt ((float) l * 53.4f), 14, 50, 20);
    }
    type->setBounds (8, 38, 80, 19);
    for (int i = 0; i < 3; ++i) modeBtn[i].setBounds (8, 61 + i * 19, 80, 18);
    mono->setBounds (8, 120, 80, 18);
    graph.setBounds (94, 38, 416, 92);
    popout.setBounds (514, 38, 26, 18);
    for (int t = 0; t < 4; ++t) toolBtn[t].setBounds (514, 58 + t * 18, 26, 18);
    shape.setBounds (94, 134, 150, 19);
    shapePrev.setBounds (246, 134, 19, 19);
    shapeNext.setBounds (267, 134, 19, 19);
    direction->setBounds (292, 134, 96, 19);
    gridY->setBounds (410, 134, 36, 19);
    gridX->setBounds (472, 134, 38, 19);
    host->setBounds (8, 166, 42, 19);
    bpmBtn.setBounds (56, 158, 38, 17);
    hzBtn.setBounds (56, 177, 38, 17);
    rate->setBounds (98, 154, 52, 54);
    rateField->setBounds (152, 167, 66, 19);
    trip->setBounds (224, 158, 40, 17);
    dot->setBounds (224, 177, 40, 17);
    rise->setBounds (272, 154, 62, 54);
    delay->setBounds (338, 154, 62, 54);
    smooth->setBounds (404, 154, 62, 54);
    phase->setBounds (470, 154, 62, 54);
}

void LfoPanel::paint (Graphics& g)
{
    gridIcon (g, { 392.0f, 134.0f, 16.0f, 19.0f }, true);
    gridIcon (g, { 454.0f, 134.0f, 16.0f, 19.0f }, false);
    g.setColour (colours::inkFaint);
    g.drawHorizontalLine (147 + 0, 8.0f, 90.0f);
}

// ===========================================================================
// Pop-out editor
// ===========================================================================
LfoEditorOverlay::LfoEditorOverlay (PanelContext& c, int l, std::function<void()> onClose) : lfo (l), ctx (c)
{
    ctx.add (this);
    setOpaque (true);
    bindLfoGraph (graph, ctx, &lfo);
    graph.padding = 8.0f;
    addAndMakeVisible (graph);
    close.icon = [] (Graphics& g, Rectangle<float> r, Colour col)
    {
        g.setColour (col);
        const auto m = r.getCentre();
        g.drawLine (m.x - 4.0f, m.y - 4.0f, m.x + 4.0f, m.y + 4.0f, 1.6f);
        g.drawLine (m.x + 4.0f, m.y - 4.0f, m.x - 4.0f, m.y + 4.0f, 1.6f);
    };
    close.setTooltip ("Close the LFO editor");
    close.onClick = [onClose] (const ModifierKeys&) { if (onClose) onClose(); };
    addAndMakeVisible (close);
    shape.fontH = 12.0f;
    shape.getText = [this] { return String (ctx.proc.patch().lfo[(size_t) lfo].shapeName); };
    shape.onOpen = [this] { showMenu (shape, lfoShapeMenu (ctx, lfo, &shape), { MenuOptions::Place::Above, {}, 170, false }); };
    addAndMakeVisible (shape);
    for (auto* b : { &shapePrev, &shapeNext }) { b->setFontHeight (12.0f); addAndMakeVisible (*b); }
    shapePrev.onClick = [this] (const ModifierKeys&) { stepLfoShape (ctx, lfo, -1); };
    shapeNext.onClick = [this] (const ModifierKeys&) { stepLfoShape (ctx, lfo, 1); };
    for (int t = 0; t < 4; ++t)
    {
        toolBtn[t].icon = [t] (Graphics& g, Rectangle<float> r, Colour col) { toolIcon (t, g, r, col); };
        toolBtn[t].setTooltip (String (kToolNames[t]) + " tool");
        toolBtn[t].setLitColour (kLfoColour);
        toolBtn[t].onClick = [this, t] (const ModifierKeys&) { setTool ((CurveEditor::Tool) t); };
        addAndMakeVisible (toolBtn[t]);
    }
    setTool (CurveEditor::Tool::Point);
    const auto& id = ids().lfo[l];
    direction = std::make_unique<ParamChoice> (ctx, id.direction);
    direction->fontH = 12.0f;
    direction->label = [] (int i) { return String (choices::lfoDirections()[(size_t) jlimit (0, 2, i)]); };
    addAndMakeVisible (*direction);
    gridY = std::make_unique<ParamField> (ctx, id.gridY);
    gridX = std::make_unique<ParamField> (ctx, id.gridX);
    for (auto* f : { gridY.get(), gridX.get() }) { f->fontH = 12.0f; f->pixelsPerStep = 8.0f; addAndMakeVisible (*f); }
}

LfoEditorOverlay::~LfoEditorOverlay() { ctx.remove (this); }

void LfoEditorOverlay::setTool (CurveEditor::Tool t)
{
    graph.tool = t;
    for (int i = 0; i < 4; ++i) toolBtn[i].setLit (i == (int) t);
}

void LfoEditorOverlay::resized()
{
    auto r = getLocalBounds().reduced (14);
    close.setBounds (r.getRight() - 24, r.getY() + 4, 24, 22);
    r.removeFromTop (30);
    auto bar = r.removeFromBottom (26);
    r.removeFromBottom (8);
    graph.setBounds (r);
    bar.removeFromLeft (4);
    shape.setBounds (bar.removeFromLeft (190));
    bar.removeFromLeft (3);
    shapePrev.setBounds (bar.removeFromLeft (24));
    bar.removeFromLeft (2);
    shapeNext.setBounds (bar.removeFromLeft (24));
    bar.removeFromLeft (24);
    for (int t = 0; t < 4; ++t) { toolBtn[t].setBounds (bar.removeFromLeft (32)); bar.removeFromLeft (2); }
    bar.removeFromLeft (22);
    direction->setBounds (bar.removeFromLeft (120));
    bar.removeFromLeft (26);
    gridY->setBounds (bar.removeFromLeft (44));
    bar.removeFromLeft (26);
    gridX->setBounds (bar.removeFromLeft (44));
}

void LfoEditorOverlay::paint (Graphics& g)
{
    g.fillAll (colours::panel);
    drawSection (g, getLocalBounds().toFloat().reduced (6.0f, 10.0f).withTrimmedTop (-2.0f), "LFO" + String (lfo + 1), kLfoColour);
    if (gridY != nullptr)
    {
        gridIcon (g, gridY->getBounds().toFloat().translated (-22.0f, 0.0f).withWidth (20.0f), true);
        gridIcon (g, gridX->getBounds().toFloat().translated (-22.0f, 0.0f).withWidth (20.0f), false);
    }
    drawText (g, "Double-click: add / remove point   Alt-drag: snap / bend all   Shift: draw steps   Right-click: point menu",
              { 200.0f, 10.0f, (float) getWidth() - 260.0f, 24.0f }, 11.0f, colours::inkDim, false, Justification::centredRight);
}

void LfoEditorOverlay::refresh()
{
    shape.refresh();
    refreshLfoGraph (graph, ctx, lfo, &history);
    for (int t = 0; t < 4; ++t) toolBtn[t].setEnabled (graph.editable);
}
} // namespace tincture::ui
