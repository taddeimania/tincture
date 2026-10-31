// FxModules.cpp — FX edit operations, menus, presets, the curve editor and
// the 16 module editors (see FxModules.h).
#include "FxModules.h"
#include "../Model/FactoryPresets.h"

namespace tincture::ui::fxui
{
using namespace juce;

// ===========================================================================
// Data helpers
// ===========================================================================
Colour typeColour (FxType t) { return Colour (fxInfo (t).colour); }

std::array<float, kFxParams> unitValues (const TinctureProcessor& proc, int unit)
{
    std::array<float, kFxParams> v {};
    if (unit < 0 || unit >= kNumFxUnits) return v;
    for (int k = 0; k < kFxParams; ++k) v[(size_t) k] = proc.get (ids().fx[unit].p[k]);
    return v;
}

void writeUnit (ParamValues& v, int unit, const std::array<float, kFxParams>& p, float mix)
{
    if (unit < 0 || unit >= kNumFxUnits) return;
    for (int k = 0; k < kFxParams; ++k) v[ids().fx[unit].p[k]] = p[(size_t) k];
    v[ids().fx[unit].mix] = mix;
}

bool writeNode (const File& f, const Node& n)
{
    f.getParentDirectory().createDirectory();
    const auto bin = n.toBinary();
    return f.replaceWithData (bin.data(), bin.size());
}

bool readNode (const File& f, Node& n)
{
    MemoryBlock mb;
    if (! f.loadFileAsData (mb)) return false;
    return Node::fromBinary ((const uint8_t*) mb.getData(), mb.getSize(), n);
}

static String safeName (FxType t)
{
    return String (fxInfo (t).name).replaceCharacters ("/", "-");
}

File rackPresetFolder()          { return Settings::userBanks().getChildFile ("FX Racks"); }
File fxPresetFolder (FxType t)   { return Settings::userBanks().getChildFile ("FX Presets").getChildFile (safeName (t)); }
File fxDefaultFile (FxType t)    { return Settings::userBanks().getChildFile ("FX Defaults").getChildFile (safeName (t) + ".tfx"); }

static bool presetValues (const Node& n, FxType t, std::array<float, kFxParams>& p, float& mix)
{
    if (n.integer ("type", -1) != (int) t) return false;
    const auto* f = n.floats ("p");
    if (f == nullptr) return false;
    p = fxDefaults (t);
    for (size_t k = 0; k < f->size() && k < (size_t) kFxParams; ++k) p[k] = jlimit (0.0f, 1.0f, (*f)[k]);
    mix = (float) n.num ("mix", fxDefaultMix (t));
    return true;
}

std::array<float, kFxParams> defaultsFor (FxType t, float& mix)
{
    auto p = fxDefaults (t);
    mix = fxDefaultMix (t);
    Node n;
    const auto f = fxDefaultFile (t);
    if (f.existsAsFile() && readNode (f, n))
    {
        std::array<float, kFxParams> q {};
        float m = mix;
        if (presetValues (n, t, q, m)) { p = q; mix = m; }
    }
    return p;
}

static void signatureOf (const FxModuleSlot& m, String& s)
{
    s << (int) m.type << ":" << m.unit << (m.bypass ? "b" : "") << ":" << m.irIndex << (m.userIr ? "u" : "");
    if (m.isSplitter())
    {
        s << "[";
        for (int b = 0; b < fxInfo (m.type).numBands; ++b)
        {
            s << (m.bandBypass[(size_t) b] ? "B" : "") << "(";
            for (auto& c : m.bands[(size_t) b]) signatureOf (c, s);
            s << ")";
        }
        s << "]";
    }
    s << ";";
}

String layoutSignature (const FxLayout& L, int rack)
{
    String s;
    for (int r = 0; r < kNumFxRacks; ++r)
    {
        if (rack >= 0 && r != rack) continue;
        const auto& R = L.racks[(size_t) r];
        s << r << "{" << String (R.presetName) << (R.locked ? "L" : "") << "|";
        for (auto& m : R.modules) signatureOf (m, s);
        s << "}";
    }
    return s;
}

String listName (const FxList& l)
{
    static const char* racks[] { "MAIN", "BUS 1", "BUS 2" };
    return racks[jlimit (0, 2, l.rack)];
}

static bool unitOfParam (int param, int& unit, int& slot)
{
    static const std::map<int, std::pair<int, int>> table = []
    {
        std::map<int, std::pair<int, int>> m;
        for (int u = 0; u < kNumFxUnits; ++u)
        {
            m[ids().fx[u].mix] = { u, -1 };
            for (int k = 0; k < kFxParams; ++k) m[ids().fx[u].p[k]] = { u, k };
        }
        return m;
    }();
    auto it = table.find (param);
    if (it == table.end()) return false;
    unit = it->second.first;
    slot = it->second.second;
    return true;
}

String paramDisplayName (const TinctureProcessor& proc, int param)
{
    if (param < 0 || param >= params().size()) return "---";
    int unit = -1, slot = -1;
    if (unitOfParam (param, unit, slot))
    {
        FxPath p;
        if (proc.patch().fx.findUnit (unit, p))
        {
            const auto* m = proc.patch().fx.get (p);
            const auto& info = fxInfo (m->type);
            String n = String (info.caps) + " ";
            if (slot < 0) return n + (info.mixName == "MIX" ? "Mix" : String (info.mixName));
            return n + String (info.params[(size_t) slot].name);
        }
    }
    return params()[param].name;
}

static void removeRowsForUnits (Patch& p, const std::vector<int>& units)
{
    if (units.empty()) return;
    p.matrix.removeRowsWhere ([&units] (int dest)
    {
        int u = -1, k = -1;
        return unitOfParam (dest, u, k) && std::find (units.begin(), units.end(), u) != units.end();
    });
}

// ===========================================================================
// Edit operations
// ===========================================================================
int addModule (TinctureProcessor& proc, const FxList& where, FxType t, int at)
{
    {
        FxLayout test = proc.patch().fx;
        if (test.add (where, t, at) < 0) return -1;
    }
    float mix = 1.0f;
    const auto vals = defaultsFor (t, mix);
    int unit = -1;
    proc.editAll ("Add " + String (fxInfo (t).name), [&] (Patch& p, ParamValues& v)
    {
        unit = p.fx.add (where, t, at);
        if (unit >= 0)
        {
            writeUnit (v, unit, vals, mix);
            // A splitter's bands start empty; nothing else to write.
        }
    });
    return unit;
}

void removeModule (TinctureProcessor& proc, const FxPath& path)
{
    if (proc.patch().fx.get (path) == nullptr) return;
    proc.editPatch ("Remove FX Module", [path] (Patch& p)
    {
        const auto freed = p.fx.remove (path);
        removeRowsForUnits (p, freed);
    });
}

bool duplicateModule (TinctureProcessor& proc, const FxPath& from, const FxList& to, int at)
{
    {
        FxLayout test = proc.patch().fx;
        if (test.duplicate (from, to, at).empty()) return false;
    }
    proc.editAll ("Duplicate FX Module", [&] (Patch& p, ParamValues& v)
    {
        const auto pairs = p.fx.duplicate (from, to, at);
        for (auto [a, b] : pairs)
        {
            for (int k = 0; k < kFxParams; ++k) v[ids().fx[b].p[k]] = v[ids().fx[a].p[k]];
            v[ids().fx[b].mix] = v[ids().fx[a].mix];
        }
    });
    return true;
}

bool moveModule (TinctureProcessor& proc, const FxPath& from, const FxList& to, int at)
{
    {
        FxLayout test = proc.patch().fx;
        if (! test.move (from, to, at)) return false;
        if (layoutSignature (test) == layoutSignature (proc.patch().fx)) return true;   // no change
    }
    proc.editPatch ("Move FX Module", [from, to, at] (Patch& p) { p.fx.move (from, to, at); });
    return true;
}

void toggleBypass (TinctureProcessor& proc, const FxPath& path, bool allInRack)
{
    const auto* m = proc.patch().fx.get (path);
    if (m == nullptr) return;
    const bool on = ! m->bypass;
    proc.editPatch (on ? "Bypass FX" : "Enable FX", [path, on, allInRack] (Patch& p)
    {
        if (allInRack) p.fx.setRackBypass (path.list.rack, on);
        else p.fx.setBypass (path, on);
    });
}

void toggleBandBypass (TinctureProcessor& proc, int rack, int splitter, int band)
{
    const auto* m = proc.patch().fx.get (FxPath { FxList { rack, -1, -1 }, splitter });
    if (m == nullptr || band < 0 || band > 2) return;
    const bool on = ! m->bandBypass[(size_t) band];
    proc.editPatch ("Bypass Band", [=] (Patch& p) { p.fx.setBandBypass (rack, splitter, band, on); });
}

Node saveRackNode (const TinctureProcessor& proc, int rack, const String& name)
{
    auto getter = [&proc] (int unit)
    {
        FxUnitValues u;
        u.unit = unit;
        u.mix = proc.get (ids().fx[unit].mix);
        u.p = unitValues (proc, unit);
        return u;
    };
    Node n = proc.patch().fx.saveRack (rack, getter);
    n.set ("name", name.toStdString());
    return n;
}

void loadRackPreset (TinctureProcessor& proc, int rack, const Node& n, const String& name)
{
    {
        FxLayout test = proc.patch().fx;
        std::vector<FxUnitValues> out;
        if (! test.loadRack (rack, n, out)) return;
    }
    proc.editAll ("Load FX Bus", [&] (Patch& p, ParamValues& v)
    {
        std::vector<FxUnitValues> out;
        std::vector<int> freed;
        if (! p.fx.loadRack (rack, n, out, &freed)) return;
        removeRowsForUnits (p, freed);
        for (auto& u : out) writeUnit (v, u.unit, u.p, u.mix);
        p.fx.racks[(size_t) rack].presetName = name.toStdString();
    });
}

Node fxPresetNode (const TinctureProcessor& proc, FxType t, int unit)
{
    Node n ("fxpreset");
    n.set ("type", (double) (int) t);
    n.set ("mix", (double) proc.get (ids().fx[unit].mix));
    const auto v = unitValues (proc, unit);
    n.set ("p", std::vector<float> (v.begin(), v.end()));
    return n;
}

void applyFxPreset (TinctureProcessor& proc, int unit, const Node& n, const String& label)
{
    FxPath path;
    if (! proc.patch().fx.findUnit (unit, path)) return;
    const auto t = proc.patch().fx.get (path)->type;
    std::array<float, kFxParams> p {};
    float mix = 1.0f;
    if (! presetValues (n, t, p, mix)) return;
    proc.editAll (label, [&] (Patch&, ParamValues& v) { writeUnit (v, unit, p, mix); });
}

// ===========================================================================
// Menus
// ===========================================================================
Menu addMenu (TinctureProcessor& proc, const FxList& where, int at)
{
    Menu m;
    const auto& names = fxTypeNames();
    for (int i = 0; i < (int) names.size(); ++i)
    {
        const auto t = (FxType) i;
        const bool ok = ! (where.splitter >= 0 && fxInfo (t).splitter);
        m.add (names[(size_t) i], [&proc, where, t, at] { addModule (proc, where, t, at); }, false, ok)
         .withSwatch (typeColour (t));
    }
    return m;
}

Menu moduleMenu (PanelContext& ctx, const FxPath& path, Component& anchor)
{
    auto& proc = ctx.proc;
    Menu m;
    const auto* slot = proc.patch().fx.get (path);
    if (slot == nullptr) return m;
    const auto t = slot->type;
    const int unit = slot->unit;
    const auto& info = fxInfo (t);

    Menu factory;
    factory.add ("Default", [&proc, t, unit]
    {
        Node n ("fxpreset");
        n.set ("type", (double) (int) t).set ("mix", (double) fxDefaultMix (t));
        const auto d = fxDefaults (t);
        n.set ("p", std::vector<float> (d.begin(), d.end()));
        applyFxPreset (proc, unit, n, "Load FX Preset");
    });
    m.sub ("Factory", factory);
    const auto files = fxPresetFolder (t).findChildFiles (File::findFiles, false, "*.tfx");
    if (! files.isEmpty())
    {
        Menu user;
        auto sorted = files;
        sorted.sort();
        for (auto& f : sorted)
            user.add (f.getFileNameWithoutExtension(), [&proc, unit, f]
            {
                Node n;
                if (readNode (f, n)) applyFxPreset (proc, unit, n, "Load FX Preset");
            });
        m.sub ("User", user);
    }
    m.separator();
    Component::SafePointer<Component> safe (&anchor);
    m.add ("Save FX Preset...", [&ctx, &proc, t, unit, safe]
    {
        if (safe == nullptr || ! ctx.editValue) return;
        ctx.editValue (*safe, String (fxInfo (t).name) + " 1", [&proc, t, unit] (const String& name)
        {
            const auto clean = File::createLegalFileName (name.trim());
            if (clean.isEmpty()) return;
            writeNode (fxPresetFolder (t).getChildFile (clean + ".tfx"), fxPresetNode (proc, t, unit));
        });
    });
    m.add ("Save as Default Preset", [&proc, t, unit] { writeNode (fxDefaultFile (t), fxPresetNode (proc, t, unit)); });
    m.separator();
    const bool canDup = proc.patch().fx.numFreeUnits() >= proc.patch().fx.countModules (*slot);
    m.add ("Duplicate FX Module", [&proc, path] { duplicateModule (proc, path, path.list, path.index + 1); }, false, canDup);
    m.add ("Remove FX Module", [&proc, path] { removeModule (proc, path); });
    m.title (info.caps);
    return m;
}

// ===========================================================================
// BypassButton
// ===========================================================================
void BypassButton::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const float s = jmin (r.getWidth(), r.getHeight());
    const auto c = r.getCentre();
    const Colour col = bypassed ? colours::ledRed : colours::inkDim;
    const float rad = s * 0.32f;
    Path arc;
    arc.addCentredArc (c.x, c.y + 1.0f, rad, rad, 0.0f, 0.75f, MathConstants<float>::twoPi - 0.75f, true);
    g.setColour (col);
    g.strokePath (arc, PathStrokeType (1.7f, PathStrokeType::curved, PathStrokeType::rounded));
    g.drawLine (c.x, c.y - rad - 1.5f, c.x, c.y + 0.5f, 1.7f);
}

// ===========================================================================
// CurveEditor
// ===========================================================================
Point<float> CurveEditor::toScreen (float x, float y) const
{
    const auto p = plot();
    return { p.getX() + x * p.getWidth(), p.getBottom() - y * p.getHeight() };
}

void CurveEditor::paint (Graphics& g)
{
    const auto c = dragging ? work : (get ? get() : Curve());
    const auto p = plot();
    drawLcdBox (g, getLocalBounds().toFloat());
    g.setColour (colours::grid);
    for (int i = 1; i < 4; ++i)
    {
        g.drawVerticalLine ((int) (p.getX() + p.getWidth() * (float) i / 4.0f), p.getY(), p.getBottom());
        g.drawHorizontalLine ((int) (p.getY() + p.getHeight() * (float) i / 4.0f), p.getX(), p.getRight());
    }
    g.setColour (colours::inkFaint);
    g.drawLine (Line<float> (toScreen (0, 0), toScreen (1, 1)), 1.0f);
    Path path;
    const int n = jmax (32, (int) p.getWidth());
    for (int i = 0; i <= n; ++i)
    {
        const float x = (float) i / (float) n;
        const auto pt = toScreen (x, jlimit (0.0f, 1.0f, c.evaluate (x)));
        if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
    }
    g.setColour (colour);
    g.strokePath (path, PathStrokeType (2.0f));
    for (size_t i = 0; i < c.points.size(); ++i)
    {
        const auto& q = c.points[i];
        const auto pt = toScreen (q.x, q.y);
        g.setColour (colours::black);
        g.fillEllipse (Rectangle<float> (10.0f, 10.0f).withCentre (pt));
        g.setColour (colour);
        g.drawEllipse (Rectangle<float> (9.0f, 9.0f).withCentre (pt), 2.0f);
        if (i > 0)
        {
            const float mx = (c.points[i - 1].x + q.x) * 0.5f;
            const auto hp = toScreen (mx, jlimit (0.0f, 1.0f, c.evaluate (mx)));
            g.setColour (colour.withAlpha (0.7f));
            g.fillRect (Rectangle<float> (5.0f, 5.0f).withCentre (hp));
        }
    }
}

void CurveEditor::mouseDown (const MouseEvent& e)
{
    if (! get) return;
    work = get();
    dragPoint = dragSegment = -1;
    dragging = false;
    const auto pos = e.position;
    for (size_t i = 0; i < work.points.size(); ++i)
        if (toScreen (work.points[i].x, work.points[i].y).getDistanceFrom (pos) < 9.0f) { dragPoint = (int) i; break; }
    if (dragPoint < 0 && work.points.size() >= 2)
    {
        const auto p = plot();
        const float x = jlimit (0.0f, 1.0f, (pos.x - p.getX()) / p.getWidth());
        for (size_t i = 1; i < work.points.size(); ++i)
            if (x <= work.points[i].x) { dragSegment = (int) i; break; }
        if (dragSegment < 0) dragSegment = (int) work.points.size() - 1;
        startBend = work.points[(size_t) dragSegment].curve;
        startBends.clear();
        for (auto& q : work.points) startBends.push_back (q.curve);
    }
}

void CurveEditor::mouseDrag (const MouseEvent& e)
{
    if (! get || ! set) return;
    dragging = true;
    const auto p = plot();
    if (dragPoint >= 0)
    {
        auto& q = work.points[(size_t) dragPoint];
        float x = (e.position.x - p.getX()) / p.getWidth();
        const float y = jlimit (0.0f, 1.0f, (p.getBottom() - e.position.y) / p.getHeight());
        const int last = (int) work.points.size() - 1;
        if (dragPoint == 0) x = 0.0f;
        else if (dragPoint == last) x = 1.0f;
        else x = jlimit (work.points[(size_t) dragPoint - 1].x + 0.001f, work.points[(size_t) dragPoint + 1].x - 0.001f, x);
        q.x = x;
        q.y = y;
    }
    else if (dragSegment > 0)
    {
        const float d = (float) -e.getDistanceFromDragStartY() / 80.0f;
        auto bendOf = [&] (int i, float start)
        {
            const float dir = work.points[(size_t) i].y >= work.points[(size_t) i - 1].y ? 1.0f : -1.0f;
            return jlimit (-1.0f, 1.0f, start + d * dir);
        };
        if (e.mods.isAltDown())
            for (int i = 1; i < (int) work.points.size(); ++i) work.points[(size_t) i].curve = bendOf (i, startBends[(size_t) i]);
        else
            work.points[(size_t) dragSegment].curve = bendOf (dragSegment, startBend);
    }
    set (work, false);
    repaint();
}

void CurveEditor::mouseUp (const MouseEvent&)
{
    if (dragging && set) set (work, true);
    dragging = false;
    dragPoint = dragSegment = -1;
    repaint();
}

void CurveEditor::mouseDoubleClick (const MouseEvent& e)
{
    if (! get || ! set) return;
    work = get();
    for (size_t i = 0; i < work.points.size(); ++i)
        if (toScreen (work.points[i].x, work.points[i].y).getDistanceFrom (e.position) < 9.0f)
        {
            if (work.remove ((int) i)) { set (work, true); repaint(); }
            return;
        }
    const auto p = plot();
    work.insert ((e.position.x - p.getX()) / p.getWidth(), (p.getBottom() - e.position.y) / p.getHeight());
    set (work, true);
    repaint();
}

// ===========================================================================
// Module editors
// ===========================================================================
namespace
{
enum class CellKind { Knob, Toggles, Choice, Radio, Field, Display, Label, Bands };
enum class DisplayKind { None, RespEq, RespFilter, RespDelay, RespDist, Transfer, Gr, Ir, RevCut };

struct Cell
{
    CellKind kind;
    std::vector<int> slots;     // Knob: {knob, under (-1: the sync toggle)}; Toggles/Field: stacked
    int width = 58;
    DisplayKind display = DisplayKind::None;
    String text;                // Label text / caption above Toggles
};

Cell K (int s, int under = -1) { Cell c { CellKind::Knob, { s, under }, 54, DisplayKind::None, {} }; return c; }
Cell T (std::vector<int> s, const String& cap = {}, int w = 84) { Cell c { CellKind::Toggles, std::move (s), w, DisplayKind::None, cap }; return c; }
Cell C (int s, int w = 124) { return { CellKind::Choice, { s }, w, DisplayKind::None, {} }; }
Cell R (int s, int w = 84) { return { CellKind::Radio, { s }, w, DisplayKind::None, {} }; }
Cell F (std::vector<int> s, const String& cap = {}, int w = 66) { Cell c { CellKind::Field, std::move (s), w, DisplayKind::None, cap }; return c; }
Cell D (DisplayKind d, int w) { Cell c { CellKind::Display, {}, w, d, {} }; return c; }
Cell L (const String& t, int w = 22) { Cell c { CellKind::Label, {}, w, DisplayKind::None, t }; return c; }
Cell B() { return { CellKind::Bands, {}, 0, DisplayKind::None, {} }; }

std::vector<Cell> cellsFor (FxType t)
{
    switch (t)
    {
        case FxType::Bode:       return { T ({ 9, 10 }, {}, 96), K (0), K (1), K (2), K (3), K (4), K (6), K (7), K (8) };
        case FxType::Chorus:     return { K (0), K (2), K (3), K (4), K (5), K (6, 7) };
        case FxType::Compressor: return { R (0, 84), K (1), K (2), K (3), K (4), K (5), D (DisplayKind::Gr, 72), K (6), K (7), K (8),
                                          L ("BANDS", 14), K (9), K (10), K (11) };
        case FxType::Convolve:   return { D (DisplayKind::Ir, 200), K (0), K (1), T ({ 2 }, {}, 66), K (3), K (5), K (6), K (7), K (8) };
        case FxType::Delay:      return { C (0, 118), F ({ 4, 5 }, "L"), T ({ 3, 2 }, {}, 54), F ({ 6, 7 }, "R"), K (8),
                                          D (DisplayKind::RespDelay, 190), K (9), K (10) };
        case FxType::Distortion: return { C (0, 128), R (1, 58), D (DisplayKind::RespDist, 180), K (3), K (4), K (5), D (DisplayKind::Transfer, 100) };
        case FxType::Equalizer:  return { K (1), K (2), K (3), R (0, 78), D (DisplayKind::RespEq, 300), R (4, 78), K (5), K (6), K (7) };
        case FxType::Filter:     return { C (0, 118), K (1), K (2), K (3), K (4), K (5), D (DisplayKind::RespFilter, 240) };
        case FxType::Flanger:    return { K (0), K (2), K (3), K (4) };
        case FxType::Hyper:      return { L ("HYPER"), K (0), F ({ 1 }, "UNISON", 62), K (2), T ({ 3 }, {}, 70), L ("DIMENSION"), K (4), K (5) };
        case FxType::Phaser:     return { K (0), F ({ 2 }, "POLES", 58), K (3), K (4), K (5), K (6), K (7) };
        case FxType::Reverb:     return { C (0, 96), D (DisplayKind::RevCut, 60), K (1), K (2), K (3), K (4), K (6), K (7), K (8),
                                          K (9), K (10), K (11), K (12), K (13), C (14, 104) };
        case FxType::SplitterLH:
        case FxType::SplitterLMH:
        case FxType::SplitterMS: return { B() };
        case FxType::Utility:    return { T ({ 0, 1 }, "POLARITY INV", 92), K (2), K (3), T ({ 4 }, {}, 92), K (5), K (6), K (7) };
    }
    return {};
}

class ModuleEditor;

// --- Displays --------------------------------------------------------------
class Display : public Component
{
public:
    Display (ModuleEditor& e, DisplayKind k) : ed (e), kind (k) {}
    void paint (Graphics&) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    float gr[3] { 0, 0, 0 };

private:
    struct Handle { int sx, sy; Colour c; };
    std::vector<Handle> handles() const;
    Point<float> handlePos (const Handle&) const;
    float dbTop() const { return kind == DisplayKind::RespEq ? 24.0f : 18.0f; }
    float dbBottom() const { return kind == DisplayKind::RespEq ? -24.0f : -42.0f; }
    void paintResponse (Graphics&, Rectangle<float>);
    void paintIr (Graphics&, Rectangle<float>);

    ModuleEditor& ed;
    DisplayKind kind;
    int active = -1;
    std::vector<int> gestures;
    float driveStart = 0.0f;
    // IR cache
    const void* irKey = nullptr;
    int irIdx = -2;
    Path irPath;
};

// A choice slot as an LCD with a menu (and < > steppers).
class SlotChoice : public ChoiceDisplay
{
public:
    SlotChoice (ModuleEditor& e, int s) : ed (e), slot (s) {}
    void mouseDown (const MouseEvent&) override;
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) override;
    void step (int d);
    ModuleEditor& ed;
    int slot;
};

// A knob that names itself after its slot in the status line.
class SlotKnob : public ParamKnob
{
public:
    SlotKnob (PanelContext& c, int param, const String& cap, std::function<String()> st) : ParamKnob (c, param, cap), status (std::move (st)) {}
    void mouseEnter (const MouseEvent& e) override
    {
        Knob::mouseEnter (e);
        if (ctx.setStatus && status) ctx.setStatus (status());
    }
    std::function<String()> status;
};

class BandPanel;

class ModuleEditor final : public FxModuleEditor
{
public:
    ModuleEditor (PanelContext& c, FxHost& h, const FxPath& p, const FxModuleSlot& s)
        : ctx (c), host (h), fxPath (p), type (s.type), u (s.unit), info (fxInfo (s.type)), cells (cellsFor (s.type))
    {
        ctx.add (this);
        readValues();
        bypassed = s.bypass;
        build (s);
        refresh();
    }
    ~ModuleEditor() override { ctx.remove (this); }

    const FxPath& path() const override { return fxPath; }
    int unit() const override { return u; }

    // Current slot values (normalised).
    float vals[kFxParams] {};
    PanelContext& ctx;
    FxHost& host;
    FxPath fxPath;
    FxType type;
    int u;
    const FxTypeInfo& info;
    std::vector<Cell> cells;

    int pid (int slot) const { return ids().fx[u].p[slot]; }
    const FxParamDesc& desc (int slot) const { return info.params[(size_t) slot]; }
    String fmt (int slot) const { return fmtAt (slot, vals[slot]); }
    String fmtAt (int slot, float n) const
    {
        float v[kFxParams];
        std::copy (vals, vals + kFxParams, v);
        v[slot] = n;
        if (type == FxType::Equalizer && (slot == 2 || slot == 6))
            return String (roundToInt (n * 100.0f));       // Q on the spec's 0-100 scale (default 60)
        return String (fxFormat (type, slot, v));
    }
    const FxModuleSlot* slotData() const { return ctx.proc.patch().fx.get (fxPath); }
    void setSlot (int slot, float norm) { ctx.proc.setWithUndo (pid (slot), jlimit (0.0f, 1.0f, norm)); }
    int index (int slot) const { return desc (slot).index (vals[slot]); }

    void readValues()
    {
        for (int k = 0; k < kFxParams; ++k) vals[k] = ctx.proc.get (pid (k));
    }

    Menu choiceMenu (int slot)
    {
        Menu m;
        const auto& d = desc (slot);
        const int cur = index (slot);
        const int n = d.count();
        auto& proc = ctx.proc;
        const int id = pid (slot);
        for (int i = 0; i < n; ++i)
            m.add (d.choices[(size_t) i], [&proc, id, i, n] { proc.setWithUndo (id, n > 1 ? (float) i / (float) (n - 1) : 0.0f); }, i == cur);
        if (type == FxType::Delay && slot == 0)
        {
            m.separator();
            const int hq = pid (1);
            m.toggle ("High Quality", vals[1] >= 0.5f, [&proc, hq] { proc.setWithUndo (hq, proc.get (hq) >= 0.5f ? 0.0f : 1.0f); });
        }
        return m;
    }

    void refresh() override
    {
        if (! panelVisible (*this)) return;
        float before[kFxParams];
        std::copy (vals, vals + kFxParams, before);
        readValues();
        const bool changed = ! std::equal (vals, vals + kFxParams, before) || first;
        if (const auto* s = slotData())
        {
            if (s->bypass != bypassed) { bypassed = s->bypass; repaint(); }
            bypassBtn.set (s->bypass);
        }
        if (changed)
        {
            first = false;
            updateVariant();
            for (auto& kv : valueText)
            {
                const String t = fmt (kv.first);
                if (t != kv.second.text) { kv.second.text = t; repaint (kv.second.area); }
            }
            for (auto* d : displays) d->repaint();
            updateEnablement();
        }
        // Mix value text.
        const String mt = String (roundToInt (ctx.proc.get (ids().fx[u].mix) * 100.0f)) + "%";
        if (mt != mixText) { mixText = mt; repaint (mixTextArea); }
        if (type == FxType::Compressor)
            for (auto* d : displays)
            {
                float g[3] {};
                ctx.proc.fxEngine().gainReduction (u, g);
                bool diff = false;
                for (int i = 0; i < 3; ++i) diff |= std::abs (g[i] - d->gr[i]) > 0.05f;
                if (diff) { std::copy (g, g + 3, d->gr); d->repaint(); }
            }
        for (auto& c : choiceDisplays) c->refresh();
    }

    void paint (Graphics& g) override;
    void paintOverChildren (Graphics& g) override
    {
        if (bypassed)
        {
            g.setColour (colours::panel.withAlpha (0.45f));
            g.fillRect (getLocalBounds().withTrimmedLeft (30).withTrimmedRight (34).reduced (2, 10));
        }
    }
    void resized() override { layoutCells(); }
    void mouseDown (const MouseEvent& e) override;
    void mouseDrag (const MouseEvent& e) override;
    void mouseUp (const MouseEvent& e) override;

    Rectangle<int> menuArea() const { return { 4, 8, 22, 20 }; }
    Rectangle<int> gripArea() const { return { 0, 30, 28, getHeight() - 40 }; }
    Rectangle<int> tabArea() const
    {
        const float w = GlyphArrangement::getStringWidth (panelFont (12.0f, true), info.caps) + 12.0f;
        return { 44, 0, (int) w, 18 };
    }

private:
    struct CellRt
    {
        Cell def;
        std::vector<Component*> comps;
        Rectangle<int> area;
        bool visible = true;
    };
    struct ValueText { String text; Rectangle<int> area; };

    void build (const FxModuleSlot& s);
    void layoutCells();
    void updateVariant();
    void updateEnablement();
    bool slotVisible (int k) const { return fxSlotVisible (type, k, vals); }

    template <typename T, typename... A> T* make (A&&... a)
    {
        auto c = std::make_unique<T> (std::forward<A> (a)...);
        auto* raw = c.get();
        addAndMakeVisible (*raw);
        owned.push_back (std::move (c));
        return raw;
    }

    std::vector<CellRt> rts;
    std::vector<std::unique_ptr<Component>> owned;
    std::map<int, SlotKnob*> knobs;
    std::map<int, ParamToggle*> toggles;
    std::map<int, ParamField*> fields;
    std::map<int, ValueText> valueText;
    std::vector<Display*> displays;
    std::vector<ChoiceDisplay*> choiceDisplays;
    std::vector<std::pair<int, PanelButton*>> radios;     // slot, button (label index in name)
    std::vector<BandPanel*> bands;
    std::vector<Component*> splitKnobs;
    std::unique_ptr<ParamKnob> mixKnob;
    std::unique_ptr<ParamFader> levelFader;
    PanelButton editA { "EDIT A" }, editB { "EDIT B" };
    BypassButton bypassBtn;
    String mixText, variantSig;
    Rectangle<int> mixTextArea;
    bool bypassed = false, first = true, draggingModule = false;
    friend class Display;
    friend class BandPanel;
};

// --- Splitter band panels --------------------------------------------------
class BandPanel final : public Component, public SettableTooltipClient
{
public:
    BandPanel (ModuleEditor& e, int b) : ed (e), band (b)
    {
        addAndMakeVisible (bypass);
        bypass.onClick = [this] (const ModifierKeys&)
        {
            toggleBandBypass (ed.ctx.proc, ed.fxPath.list.rack, ed.fxPath.index, band);
        };
        bypass.setTooltip ("Bypass this band");
        setTooltip ("Click to show this band's modules; right-click or the arrow to add one");
    }
    void resized() override { bypass.setBounds (getWidth() - 26, getHeight() - 26, 22, 22); }
    void paint (Graphics& g) override
    {
        const auto* s = ed.slotData();
        const bool sel = ed.host.selectedBand (ed.u) == band;
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (sel ? Colour (0xff3a6f6a) : colours::black);
        g.fillRect (r);
        g.setColour (sel ? colours::ink : colours::inkDim);
        g.drawRect (r, 1.5f);
        const auto name = String (ed.info.bandNames[(size_t) band]);
        drawText (g, name, { r.getX() + 8, r.getY() + 6, r.getWidth() - 40, 16 }, 14.0f, colours::ink, true, Justification::centredLeft);
        const int count = s != nullptr ? (int) s->bands[(size_t) band].size() : 0;
        drawText (g, String (count), { r.getRight() - 30, r.getY() + 6, 22, 16 }, 14.0f, colours::lcd, true, Justification::centredRight);
        // ▼ add
        const auto a = addArea().toFloat();
        Path tri;
        tri.addTriangle (a.getX() + 2, a.getY() + 4, a.getRight() - 2, a.getY() + 4, a.getCentreX(), a.getBottom() - 3);
        g.setColour (colours::ink);
        g.fillPath (tri);
        drawText (g, "ADD FX", { a.getRight() + 4, a.getY(), 60, a.getHeight() }, 11.0f, colours::inkDim, true, Justification::centredLeft);
        if (s != nullptr)
        {
            // The band's modules, by name.
            float y = r.getY() + 46;
            for (auto& m : s->bands[(size_t) band])
            {
                if (y > r.getBottom() - 30) { drawText (g, "...", { r.getX() + 8, y, 40, 13 }, 11.0f, colours::inkDim, false, Justification::centredLeft); break; }
                g.setColour (typeColour (m.type));
                g.fillRect (Rectangle<float> (r.getX() + 8, y + 2, 3, 10));
                drawText (g, fxInfo (m.type).caps, { r.getX() + 15, y, r.getWidth() - 50, 13 }, 10.5f,
                          m.bypass ? colours::inkDim : colours::ink, true, Justification::centredLeft);
                y += 14;
            }
        }
        bypass.set (s != nullptr && s->bandBypass[(size_t) band]);
    }
    Rectangle<int> addArea() const { return { 8, 26, 16, 14 }; }
    void mouseDown (const MouseEvent& e) override
    {
        const FxList where { ed.fxPath.list.rack, ed.fxPath.index, band };
        if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), addMenu (ed.ctx.proc, where)); return; }
        ed.host.selectBand (ed.u, band);
        if (addArea().expanded (4).contains (e.getPosition()))
            showMenu (*this, addMenu (ed.ctx.proc, where), MenuOptions { MenuOptions::Place::AtPoint, addArea().getBottomLeft(), 0, false });
    }
    ModuleEditor& ed;
    int band;
    BypassButton bypass;
};

// --- SlotChoice ------------------------------------------------------------
void SlotChoice::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    showMenu (*this, ed.choiceMenu (slot));
}
void SlotChoice::step (int d)
{
    const auto& desc = ed.desc (slot);
    const int n = desc.count();
    if (n <= 1) return;
    const int cur = desc.index (ed.vals[slot]);
    ed.setSlot (slot, (float) ((cur + d + n) % n) / (float) (n - 1));
}
void SlotChoice::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) { step (w.deltaY > 0 ? -1 : 1); }

// --- ModuleEditor ----------------------------------------------------------
void ModuleEditor::build (const FxModuleSlot&)
{
    for (auto& def : cells)
    {
        CellRt rt;
        rt.def = def;
        switch (def.kind)
        {
            case CellKind::Knob:
            {
                const int k = def.slots[0];
                const int typeK = (int) type;
                auto* kn = make<SlotKnob> (ctx, pid (k), String (fxSlotName (type, k, vals)), [this, k]
                {
                    return String (info.caps) + " " + String (desc (k).name) + ": " + fmt (k);
                });
                kn->small = true;
                kn->bipolar = desc (k).bipolar;
                kn->accent = typeColour (type);
                kn->format = [this, k] (double n) { return fmtAt (k, (float) n); };
                kn->setTooltip (desc (k).name);
                // Spec extras in the right-click menu.
                auto addToggle = [this] (Menu& m, const String& name, int s)
                {
                    const int id = pid (s);
                    auto* pr = &ctx.proc;
                    m.toggle (name, pr->get (id) >= 0.5f, [pr, id] { pr->setWithUndo (id, pr->get (id) >= 0.5f ? 0.0f : 1.0f); });
                };
                if (typeK == (int) FxType::Bode && k == 0) kn->extraMenu = [addToggle] (Menu& m) { addToggle (m, "Retrig", 10); };
                if (typeK == (int) FxType::Compressor && k == 2) kn->extraMenu = [addToggle] (Menu& m) { addToggle (m, "Limiter Latency Comp", 12); };
                if (typeK == (int) FxType::Filter && k == 1) kn->extraMenu = [addToggle] (Menu& m) { addToggle (m, "Key Track", 6); };
                if (typeK == (int) FxType::Filter && k == 3) kn->extraMenu = [addToggle] (Menu& m) { addToggle (m, "Clean Mode", 7); };
                if (typeK == (int) FxType::Distortion && k == 3) kn->extraMenu = [addToggle] (Menu& m) { addToggle (m, "Key Track", 6); };
                knobs[k] = kn;
                rt.comps.push_back (kn);
                valueText[k] = {};
                int under = def.slots.size() > 1 ? def.slots[1] : -1;
                if (under < 0 && desc (k).syncToggle >= 0) under = desc (k).syncToggle;
                if (under >= 0)
                {
                    if (desc (under).kind == FxKind::Toggle)
                    {
                        auto* t = make<ParamToggle> (ctx, pid (under), String (desc (under).shortName));
                        t->setFontHeight (10.5f);
                        toggles[under] = t;
                        rt.comps.push_back (t);
                    }
                    else
                    {
                        // A two-state pill (Chorus LPF/HPF): click flips.
                        auto* b = make<PanelButton> (fmt (under));
                        b->setFontHeight (10.5f);
                        b->onClick = [this, under] (const ModifierKeys&) { setSlot (under, index (under) == 0 ? 1.0f : 0.0f); };
                        b->setTooltip ("Click to switch " + String (desc (under).name));
                        radios.push_back ({ under, b });
                        rt.comps.push_back (b);
                    }
                }
                break;
            }
            case CellKind::Toggles:
                for (int s : def.slots)
                {
                    auto* t = make<ParamToggle> (ctx, pid (s), String (desc (s).shortName == "MIN" ? "MIN PHASE" : desc (s).shortName));
                    t->setFontHeight (10.5f);
                    t->setTooltip (desc (s).name);
                    toggles[s] = t;
                    rt.comps.push_back (t);
                }
                break;
            case CellKind::Choice:
            {
                const int s = def.slots[0];
                auto* c = make<SlotChoice> (*this, s);
                c->fontH = 13.0f;
                c->getText = [this, s] { return fmt (s).toUpperCase(); };
                c->setTooltip (desc (s).name);
                choiceDisplays.push_back (c);
                rt.comps.push_back (c);
                auto* l = make<PanelButton> ("<");
                auto* r = make<PanelButton> (">");
                l->onClick = [c] (const ModifierKeys&) { c->step (-1); };
                r->onClick = [c] (const ModifierKeys&) { c->step (1); };
                rt.comps.push_back (l);
                rt.comps.push_back (r);
                break;
            }
            case CellKind::Radio:
            {
                const int s = def.slots[0];
                const auto& d = desc (s);
                for (int i = 0; i < d.count(); ++i)
                {
                    auto* b = make<PanelButton> (String (d.choices[(size_t) i]).toUpperCase());
                    b->setFontHeight (10.5f);
                    const int n = d.count();
                    b->onClick = [this, s, i, n] (const ModifierKeys&) { setSlot (s, (float) i / (float) (n - 1)); };
                    b->setTooltip (d.name);
                    radios.push_back ({ s, b });
                    rt.comps.push_back (b);
                }
                break;
            }
            case CellKind::Field:
                for (size_t i = 0; i < def.slots.size(); ++i)
                {
                    const int s = def.slots[i];
                    auto* f = make<ParamField> (ctx, pid (s), String());
                    f->format = [this, s] (float) { return fmt (s); };
                    f->fontH = 12.5f;
                    f->setTooltip (desc (s).name);
                    fields[s] = f;
                    rt.comps.push_back (f);
                }
                break;
            case CellKind::Display:
            {
                auto* d = make<Display> (*this, def.display);
                displays.push_back (d);
                rt.comps.push_back (d);
                if (def.display == DisplayKind::RespDist)
                {
                    auto* f = make<ParamFader> (ctx, pid (2), false);
                    f->format = [this] (double n) { return fmtAt (2, (float) n); };
                    f->fill = typeColour (type);
                    f->setTooltip ("Filter Type: LP - BP - HP");
                    rt.comps.push_back (f);
                }
                if (def.display == DisplayKind::Ir)
                {
                    auto* l = make<PanelButton> ("<");
                    auto* r = make<PanelButton> (">");
                    auto stepIr = [this] (int delta)
                    {
                        const int n = (int) FxEngine::convolveIrNames().size();
                        const auto* s = slotData();
                        if (s == nullptr || n == 0) return;
                        const int next = ((s->irIndex + delta) % n + n) % n;
                        const auto path = fxPath;
                        ctx.proc.editPatch ("Change IR", [path, next] (Patch& p)
                        {
                            if (auto* m = p.fx.get (path)) { m->irIndex = next; m->userIr.reset(); }
                        });
                    };
                    l->onClick = [stepIr] (const ModifierKeys&) { stepIr (-1); };
                    r->onClick = [stepIr] (const ModifierKeys&) { stepIr (1); };
                    rt.comps.push_back (l);
                    rt.comps.push_back (r);
                }
                if (def.display == DisplayKind::Transfer)
                {
                    // X-Shaper curve editors.
                    addChildComponent (editA);
                    addChildComponent (editB);
                    auto open = [this] (bool b)
                    {
                        const auto path = fxPath;
                        auto& pr = ctx.proc;
                        host.openCurveEditor (String ("X-SHAPER ") + (b ? "B" : "A"), typeColour (type),
                            [&pr, path, b]
                            {
                                const auto* m = pr.patch().fx.get (path);
                                return m == nullptr ? Curve() : (b ? m->shapeB : m->shapeA);
                            },
                            [&pr, path, b] (const Curve& c, bool commit)
                            {
                                pr.editPatch ("", [&] (Patch& p) { if (auto* m = p.fx.get (path)) (b ? m->shapeB : m->shapeA) = c; });
                                if (commit) pr.commitHistory ("Edit X-Shaper");
                            });
                    };
                    editA.onClick = [open] (const ModifierKeys&) { open (false); };
                    editB.onClick = [open] (const ModifierKeys&) { open (true); };
                    editA.setFontHeight (10.5f);
                    editB.setFontHeight (10.5f);
                }
                break;
            }
            case CellKind::Label:
                break;
            case CellKind::Bands:
                for (int b = 0; b < info.numBands; ++b)
                {
                    auto* p = make<BandPanel> (*this, b);
                    bands.push_back (p);
                    rt.comps.push_back (p);
                }
                for (int k = 0; k < kFxParams - 1; ++k)
                    if (desc (k).kind == FxKind::Knob)
                    {
                        auto* kn = make<SlotKnob> (ctx, pid (k), String (desc (k).shortName), [this, k]
                        {
                            return String (info.caps) + " " + String (desc (k).name) + ": " + fmt (k);
                        });
                        kn->small = true;
                        kn->format = [this, k] (double n) { return fmtAt (k, (float) n); };
                        knobs[k] = kn;
                        valueText[k] = {};
                        splitKnobs.push_back (kn);
                        rt.comps.push_back (kn);
                    }
                break;
        }
        rts.push_back (std::move (rt));
    }

    if (info.hasMix)
    {
        mixKnob = std::make_unique<SlotKnob> (ctx, ids().fx[u].mix, String (info.mixName), [this]
        {
            return String (info.caps) + " " + String (info.mixName) + ": " + mixText;
        });
        mixKnob->small = true;
        mixKnob->accent = typeColour (type);
        addAndMakeVisible (*mixKnob);
    }
    levelFader = std::make_unique<ParamFader> (ctx, pid (kFxLevelSlot), true, "LEVEL");
    levelFader->format = [this] (double n) { return fmtAt (kFxLevelSlot, (float) n); };
    levelFader->fill = typeColour (type);
    levelFader->setTooltip ("Output level");
    addAndMakeVisible (*levelFader);
    addAndMakeVisible (bypassBtn);
    bypassBtn.onClick = [this] (const ModifierKeys& m) { toggleBypass (ctx.proc, fxPath, m.isAltDown()); };
    variantSig = "?";
}

void ModuleEditor::updateVariant()
{
    String sig;
    for (int k = 0; k < kFxParams; ++k) sig << (slotVisible (k) ? String (fxSlotName (type, k, vals)) : String ("-")) << ",";
    const bool xshaper = type == FxType::Distortion && index (0) >= fxchoices::XShaper;
    sig << (xshaper ? "X" : "");
    if (sig == variantSig) return;
    variantSig = sig;
    for (auto& [k, kn] : knobs) { kn->caption = String (fxSlotName (type, k, vals)); kn->repaint(); }
    editA.setVisible (xshaper);
    editB.setVisible (xshaper);
    layoutCells();
    repaint();
}

void ModuleEditor::updateEnablement()
{
    // Radio buttons and pills show the current choice.
    for (auto& [s, b] : radios)
    {
        const auto& d = desc (s);
        const String cur = String (d.choices[(size_t) d.index (vals[s])]).toUpperCase();
        if (d.count() == 2 && type == FxType::Chorus) b->setLabel (cur);
        else b->setLit (b->getLabel() == cur);
    }
    auto enable = [this] (int k, bool on)
    {
        if (auto it = knobs.find (k); it != knobs.end()) it->second->setEnabled (on);
        if (auto it = fields.find (k); it != fields.end()) it->second->setEnabled (on);
    };
    if (type == FxType::Compressor)
    {
        const bool multi = index (0) == 1;
        for (int k : { 6, 7, 8, 9, 10, 11 }) enable (k, multi);
    }
    if (type == FxType::Distortion)
    {
        const bool filt = index (1) != 0;
        for (int k : { 3, 4 }) enable (k, filt);
    }
    if (type == FxType::Delay)
    {
        const bool link = vals[3] >= 0.5f;
        enable (6, ! link);
        enable (7, ! link);
    }
    if (type == FxType::Utility) enable (5, vals[4] >= 0.5f);
    if (type == FxType::Equalizer)
    {
        enable (3, index (0) != 2);
        enable (7, index (4) != 2);
    }
}

void ModuleEditor::layoutCells()
{
    const int H = getHeight();
    if (getWidth() <= 0) return;
    const int top = 22, h = H - top - 10;
    int x = 40;
    valueText.clear();
    for (auto& rt : rts)
    {
        const auto& d = rt.def;
        bool vis = true;
        if (d.kind == CellKind::Knob || d.kind == CellKind::Choice) vis = slotVisible (d.slots[0]);
        rt.visible = vis;
        for (auto* c : rt.comps) c->setVisible (vis);
        if (! vis) continue;
        int w = d.width;
        if (d.kind == CellKind::Knob)   // long variant captions ("CHORUS DEPTH") widen their cell
            w = jmax (w, (int) GlyphArrangement::getStringWidth (panelFont (11.0f), String (fxSlotName (type, d.slots[0], vals))) + 4);
        rt.area = { x, top, w, h };
        switch (d.kind)
        {
            case CellKind::Knob:
            {
                const int k = d.slots[0];
                valueText[k] = { {}, Rectangle<int> (x - 4, top - 2, w + 8, 13) };
                rt.comps[0]->setBounds (x, top + 12, w, 60);
                if (rt.comps.size() > 1) rt.comps[1]->setBounds (x + 6, top + 76, w - 12, 18);
                x += w + 4;
                break;
            }
            case CellKind::Toggles:
            {
                int y = top + (d.text.isNotEmpty() ? 22 : 16);
                for (auto* c : rt.comps) { c->setBounds (x + 2, y, w - 4, 22); y += 28; }
                x += w + 8;
                break;
            }
            case CellKind::Choice:
                rt.comps[0]->setBounds (x, top + 16, w, 28);
                rt.comps[1]->setBounds (x, top + 50, 30, 22);
                rt.comps[2]->setBounds (x + 34, top + 50, 30, 22);
                x += w + 10;
                break;
            case CellKind::Radio:
            {
                int y = top + 14;
                const int bh = rt.comps.size() > 2 ? 22 : 26;
                for (auto* c : rt.comps) { c->setBounds (x, y, w, bh); y += bh + 4; }
                x += w + 10;
                break;
            }
            case CellKind::Field:
            {
                int y = top + 18;
                for (size_t i = 0; i < rt.comps.size(); ++i)
                {
                    rt.comps[i]->setBounds (x, y, w, i == 0 ? 30 : 24);
                    y += i == 0 ? 36 : 30;
                }
                x += w + 8;
                break;
            }
            case CellKind::Display:
            {
                auto r = Rectangle<int> (x, top + 2, w, h - 4);
                if (d.display == DisplayKind::RespDist)
                {
                    rt.comps[1]->setBounds (r.removeFromBottom (20));
                    r.removeFromBottom (2);
                }
                if (d.display == DisplayKind::Ir)
                {
                    auto row = r.removeFromBottom (22);
                    r.removeFromBottom (4);
                    rt.comps[1]->setBounds (row.removeFromLeft (30));
                    row.removeFromLeft (4);
                    rt.comps[2]->setBounds (row.removeFromLeft (30));
                }
                if (d.display == DisplayKind::Transfer)
                {
                    auto row = r.removeFromBottom (20);
                    r.removeFromBottom (4);
                    editA.setBounds (row.removeFromLeft (w / 2 - 2));
                    row.removeFromLeft (4);
                    editB.setBounds (row);
                }
                rt.comps[0]->setBounds (r);
                x += w + 10;
                break;
            }
            case CellKind::Label:
                x += w + 6;
                break;
            case CellKind::Bands:
            {
                // Band panels with the split knobs between them.
                const int bw = 170;
                int knobIdx = 0;
                for (size_t b = 0; b < bands.size(); ++b)
                {
                    bands[b]->setBounds (x, top - 4, bw, h + 4);
                    x += bw + 8;
                    if (b + 1 < bands.size() && knobIdx < (int) splitKnobs.size())
                    {
                        auto* kn = splitKnobs[(size_t) knobIdx];
                        int k = -1;
                        for (auto& [kk, kp] : knobs) if (kp == kn) k = kk;
                        kn->setBounds (x, top + 22, 64, 60);
                        if (k >= 0) valueText[k] = { {}, Rectangle<int> (x - 4, top + 8, 72, 13) };
                        x += 72;
                        ++knobIdx;
                    }
                }
                rt.area = { rt.area.getX(), top, x - rt.area.getX(), h };
                break;
            }
        }
    }
    const int W = getWidth();
    if (mixKnob != nullptr)
    {
        mixKnob->setBounds (W - 140, top + 12, 62, 60);
        mixTextArea = { W - 144, top - 2, 70, 13 };
    }
    levelFader->setBounds (W - 72, 14, 34, H - 22);
    bypassBtn.setBounds (W - 30, 10, 24, 24);
    for (auto& kv : valueText) kv.second.text = fmt (kv.first);
    mixText = String (roundToInt (ctx.proc.get (ids().fx[u].mix) * 100.0f)) + "%";
}

void ModuleEditor::paint (Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    const Colour col = typeColour (type);
    g.setColour (colours::panel);
    g.fillRect (b);
    // Left edge column: ▼ (module menu) and a grip; right edge: screws.
    g.setColour (colours::panelEdge);
    g.fillRect (b.withWidth (28.0f));
    g.fillRect (b.withLeft (b.getRight() - 34.0f));
    const auto m = menuArea().toFloat();
    Path tri;
    tri.addTriangle (m.getX() + 3, m.getY() + 5, m.getRight() - 3, m.getY() + 5, m.getCentreX(), m.getBottom() - 3);
    g.setColour (col);
    g.fillPath (tri);
    g.setColour (colours::inkFaint);
    for (int i = 0; i < 4; ++i)
        g.drawHorizontalLine ((int) (b.getCentreY() - 6.0f + (float) i * 4.0f), 8.0f, 20.0f);
    for (float y : { b.getBottom() - 12.0f })
    {
        g.setColour (colours::inkFaint);
        g.drawEllipse (Rectangle<float> (9, 9).withCentre ({ 14.0f, y }), 1.2f);
        g.drawEllipse (Rectangle<float> (9, 9).withCentre ({ b.getRight() - 17.0f, y }), 1.2f);
    }
    drawSection (g, { 32.0f, 9.0f, b.getWidth() - 32.0f - 38.0f, b.getHeight() - 13.0f }, info.caps, col);
    // Captions and values.
    for (auto& rt : rts)
    {
        if (! rt.visible) continue;
        const auto a = rt.area.toFloat();
        const auto& d = rt.def;
        if (d.kind == CellKind::Choice)
            drawText (g, String (fxSlotName (type, d.slots[0], vals)), { a.getX(), a.getY(), a.getWidth(), 14 }, 11.0f, colours::ink, true, Justification::centredLeft);
        if (d.kind == CellKind::Toggles && d.text.isNotEmpty())
            drawText (g, d.text, { a.getX(), a.getY() + 2, a.getWidth(), 14 }, 11.0f, colours::ink, true, Justification::centredLeft);
        if (d.kind == CellKind::Field && d.text.isNotEmpty())
            drawText (g, d.text, { a.getX(), a.getY(), a.getWidth(), 14 }, 11.0f, colours::ink, true);
        if (d.kind == CellKind::Label)
        {
            g.setColour (colours::inkFaint);
            g.drawVerticalLine ((int) a.getX(), a.getY(), a.getBottom());
            Graphics::ScopedSaveState ss (g);
            g.addTransform (AffineTransform::rotation (-MathConstants<float>::halfPi, a.getX() + 12, a.getCentreY()));
            drawText (g, d.text, { a.getX() + 12 - 50, a.getCentreY() - 7, 100, 14 }, 11.0f, colours::inkDim, true);
        }
    }
    for (auto& [k, vt] : valueText)
        if (auto it = knobs.find (k); it != knobs.end() && it->second->isVisible())
            drawText (g, vt.text, vt.area.toFloat(), 10.5f, it->second->isEnabled() ? colours::lcd : colours::lcdDim, true);
    if (mixKnob != nullptr) drawText (g, mixText, mixTextArea.toFloat(), 10.5f, colours::lcd, true);
}

void ModuleEditor::mouseDown (const MouseEvent& e)
{
    draggingModule = false;
    if (menuArea().expanded (3).contains (e.getPosition()) || e.mods.isPopupMenu())
    {
        if (e.mods.isPopupMenu()) showContextMenu (*this, e.getPosition(), moduleMenu (ctx, fxPath, *this));
        else showMenu (*this, moduleMenu (ctx, fxPath, *this), MenuOptions { MenuOptions::Place::AtPoint, menuArea().getBottomLeft(), 0, false });
    }
}

void ModuleEditor::mouseDrag (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    const auto start = e.getMouseDownPosition();
    if (! (gripArea().contains (start) || tabArea().contains (start))) return;
    if (! draggingModule && e.getDistanceFromDragStart() < 4) return;
    draggingModule = true;
    if (auto* hc = dynamic_cast<Component*> (&host))
        host.moduleDragged (fxPath, hc->getLocalPoint (this, e.getPosition()), false, e.mods.isAltDown());
}

void ModuleEditor::mouseUp (const MouseEvent& e)
{
    if (! draggingModule) return;
    draggingModule = false;
    if (auto* hc = dynamic_cast<Component*> (&host))
        host.moduleDragged (fxPath, hc->getLocalPoint (this, e.getPosition()), true, e.mods.isAltDown());
}

// --- Display ---------------------------------------------------------------
std::vector<Display::Handle> Display::handles() const
{
    switch (kind)
    {
        case DisplayKind::RespEq:     return { { 1, 3, colours::node (0) }, { 5, 7, colours::node (1) } };
        case DisplayKind::RespFilter: return { { 1, 2, colours::brand } };
        case DisplayKind::RespDelay:  return { { 9, 10, colours::brand } };
        case DisplayKind::RespDist:   return { { 3, 4, colours::brand } };
        case DisplayKind::None: case DisplayKind::Transfer: case DisplayKind::Gr:
        case DisplayKind::Ir:   case DisplayKind::RevCut:  return {};
    }
    return {};
}

Point<float> Display::handlePos (const Handle& h) const
{
    const auto r = getLocalBounds().toFloat().reduced (2.0f);
    const auto& dx = ed.desc (h.sx);
    const auto& dy = ed.desc (h.sy);
    const float x = r.getX() + freqToNorm (dx.toPlain (ed.vals[h.sx])) * r.getWidth();
    float y;
    if (dy.unit == FxUnitKind::Db)
        y = r.getY() + (dbTop() - dy.toPlain (ed.vals[h.sy])) / (dbTop() - dbBottom()) * r.getHeight();
    else
        y = r.getBottom() - ed.vals[h.sy] * r.getHeight();
    return { x, y };
}

void Display::paintResponse (Graphics& g, Rectangle<float> r)
{
    drawFrequencyGrid (g, r, false, dbTop(), dbBottom(), 12.0f);
    const Colour col = typeColour (ed.type);
    const bool off = kind == DisplayKind::RespDist && ed.index (1) == 0;
    Path p, fill;
    const int n = jmax (16, (int) r.getWidth() / 2);
    const float zeroY = r.getY() + dbTop() / (dbTop() - dbBottom()) * r.getHeight();
    for (int i = 0; i <= n; ++i)
    {
        const float t = (float) i / (float) n;
        const float db = FxEngine::magnitudeDb (ed.type, ed.vals, normToFreq (t), 48000.0);
        const float y = jlimit (r.getY(), r.getBottom(), r.getY() + (dbTop() - db) / (dbTop() - dbBottom()) * r.getHeight());
        const float x = r.getX() + t * r.getWidth();
        if (i == 0) { p.startNewSubPath (x, y); fill.startNewSubPath (x, zeroY); fill.lineTo (x, y); }
        else { p.lineTo (x, y); fill.lineTo (x, y); }
    }
    fill.lineTo (r.getRight(), zeroY);
    fill.closeSubPath();
    g.setColour (col.withAlpha (off ? 0.06f : 0.16f));
    g.fillPath (fill);
    g.setColour (off ? colours::inkFaint : col);
    g.strokePath (p, PathStrokeType (2.0f));
    if (off) drawText (g, "FILTER OFF", r, 11.0f, colours::inkDim, true);
    for (size_t i = 0; i < handles().size(); ++i)
    {
        const auto h = handles()[i];
        const auto pt = handlePos (h);
        g.setColour (colours::black);
        g.fillEllipse (Rectangle<float> (12, 12).withCentre (pt));
        g.setColour (h.c);
        g.drawEllipse (Rectangle<float> (11, 11).withCentre (pt), 2.0f);
    }
}

void Display::paintIr (Graphics& g, Rectangle<float> r)
{
    const auto* s = ed.slotData();
    if (s == nullptr) return;
    std::shared_ptr<const SampleData> ir = s->userIr ? s->userIr : FxEngine::convolveFactoryIr (s->irIndex);
    if (ir.get() != irKey || s->irIndex != irIdx)
    {
        irKey = ir.get();
        irIdx = s->irIndex;
        irPath.clear();
        if (ir != nullptr && ir->frames() > 0)
        {
            const int cols = 160;
            const int frames = ir->frames();
            const float* d = ir->channel (0);
            float peak = 1.0e-6f;
            for (int i = 0; i < frames; ++i) peak = jmax (peak, std::abs (d[i]));
            std::vector<float> hi;
            for (int c = 0; c < cols; ++c)
            {
                const int a = c * frames / cols, b = jmax (a + 1, (c + 1) * frames / cols);
                float m = 0.0f;
                for (int i = a; i < b && i < frames; ++i) m = jmax (m, std::abs (d[i]));
                hi.push_back (m / peak);
            }
            irPath.startNewSubPath (0.0f, 0.0f);
            for (int c = 0; c < cols; ++c) irPath.lineTo ((float) c / (float) (cols - 1), -hi[(size_t) c]);
            for (int c = cols - 1; c >= 0; --c) irPath.lineTo ((float) c / (float) (cols - 1), hi[(size_t) c]);
            irPath.closeSubPath();
        }
    }
    const auto plotR = r.withTrimmedTop (16.0f).reduced (4.0f, 4.0f);
    g.setColour (colours::grid);
    g.drawHorizontalLine ((int) plotR.getCentreY(), plotR.getX(), plotR.getRight());
    if (! irPath.isEmpty())
    {
        g.setColour (typeColour (ed.type).withAlpha (0.85f));
        g.fillPath (irPath, AffineTransform::scale (plotR.getWidth(), plotR.getHeight() * 0.5f).translated (plotR.getX(), plotR.getCentreY()));
    }
    const auto& names = FxEngine::convolveIrNames();
    const String name = s->userIr ? String ("USER IR") : (s->irIndex >= 0 && s->irIndex < (int) names.size() ? String (names[(size_t) s->irIndex]) : String ("---"));
    drawText (g, name.toUpperCase(), { r.getX() + 6, r.getY() + 2, r.getWidth() - 12, 14 }, 11.0f, colours::lcd, true, Justification::centredLeft);
}

void Display::paint (Graphics& g)
{
    const auto b = getLocalBounds().toFloat();
    drawLcdBox (g, b);
    const auto r = b.reduced (2.0f);
    const Colour col = typeColour (ed.type);
    switch (kind)
    {
        case DisplayKind::RespEq: case DisplayKind::RespFilter: case DisplayKind::RespDelay: case DisplayKind::RespDist:
            paintResponse (g, r);
            break;
        case DisplayKind::Transfer:
        {
            g.setColour (colours::grid);
            g.drawHorizontalLine ((int) r.getCentreY(), r.getX(), r.getRight());
            g.drawVerticalLine ((int) r.getCentreX(), r.getY(), r.getBottom());
            const auto* s = ed.slotData();
            Path p;
            for (int i = 0; i <= 64; ++i)
            {
                const float x = -1.0f + 2.0f * (float) i / 64.0f;
                const float y = jlimit (-1.0f, 1.0f, FxEngine::distortionTransfer (ed.vals, s, x));
                const auto pt = Point<float> (r.getX() + (x + 1.0f) * 0.5f * r.getWidth(), r.getCentreY() - y * 0.5f * r.getHeight() * 0.92f);
                if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
            }
            g.setColour (col);
            g.strokePath (p, PathStrokeType (2.0f));
            break;
        }
        case DisplayKind::Gr:
        {
            const bool multi = ed.index (0) == 1;
            const char* names[] { "H", "M", "L" };
            const int nb = multi ? 3 : 1;
            const float lw = r.getWidth() / (float) nb;
            for (int i = 0; i < nb; ++i)
            {
                const int band = multi ? i : 1;
                auto col_ = r.withX (r.getX() + lw * (float) i).withWidth (lw).reduced (5.0f, 4.0f);
                drawText (g, multi ? String (names[i]) : String ("GR"), col_.removeFromBottom (13.0f), 10.5f, colours::inkDim, true);
                g.setColour (colours::socket);
                g.fillRect (col_);
                const float frac = jlimit (0.0f, 1.0f, -gr[band] / 24.0f);
                g.setColour (Colour (0xff5c9cf2));
                g.fillRect (col_.withHeight (col_.getHeight() * frac));
            }
            drawText (g, String (gr[1], 1) + " dB", { r.getX(), r.getY() + 2, r.getWidth(), 12 }, 10.5f, colours::lcd, true);
            break;
        }
        case DisplayKind::Ir:
            paintIr (g, r);
            break;
        case DisplayKind::RevCut:
        {
            const float lo = ed.vals[1], hi = ed.vals[2];
            const auto pr = r.reduced (4.0f, 8.0f).withTrimmedBottom (10.0f);
            Path p;
            const float x0 = pr.getX() + lo * pr.getWidth() * 0.5f;
            const float x1 = pr.getRight() - hi * pr.getWidth() * 0.5f;
            p.startNewSubPath (pr.getX(), pr.getBottom());
            p.lineTo (x0, pr.getBottom());
            p.lineTo (jmin (x0 + 12.0f, x1), pr.getY());
            p.lineTo (jmax (x1 - 12.0f, x0), pr.getY());
            p.lineTo (x1, pr.getBottom());
            p.lineTo (pr.getRight(), pr.getBottom());
            g.setColour (col.withAlpha (0.2f));
            g.fillPath (p);
            g.setColour (col);
            g.strokePath (p, PathStrokeType (1.8f));
            break;
        }
        case DisplayKind::None: break;
    }
}

void Display::mouseDown (const MouseEvent& e)
{
    auto& proc = ed.ctx.proc;
    active = -1;
    gestures.clear();
    if (kind == DisplayKind::Ir)
    {
        const auto* s = ed.slotData();
        if (s == nullptr) return;
        Menu m;
        const auto& names = FxEngine::convolveIrNames();
        const auto path = ed.fxPath;
        for (int i = 0; i < (int) names.size(); ++i)
            m.add (names[(size_t) i], [&proc, path, i]
            {
                proc.editPatch ("Change IR", [path, i] (Patch& p) { if (auto* mm = p.fx.get (path)) { mm->irIndex = i; mm->userIr.reset(); } });
            }, ! s->userIr && s->irIndex == i);
        m.separator();
        m.add ("Load IR...", [&proc, path]
        {
            auto chooser = std::make_shared<FileChooser> ("Load an impulse response", File(), "*.wav;*.aif;*.aiff;*.flac");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [&proc, path, chooser] (const FileChooser& fc)
            {
                const auto f = fc.getResult();
                if (! f.existsAsFile()) return;
                auto data = TinctureProcessor::decodeAudio (f);
                if (data == nullptr) return;
                proc.editPatch ("Load IR", [path, data] (Patch& p) { if (auto* mm = p.fx.get (path)) mm->userIr = data; });
            });
        });
        m.add ("Clear User IR", [&proc, path]
        {
            proc.editPatch ("Clear IR", [path] (Patch& p) { if (auto* mm = p.fx.get (path)) mm->userIr.reset(); });
        }, false, s->userIr != nullptr);
        showMenu (*this, m);
        return;
    }
    if (kind == DisplayKind::Transfer)
    {
        driveStart = ed.vals[5];
        proc.beginGesture (ed.pid (5));
        gestures.push_back (ed.pid (5));
        return;
    }
    const auto hs = handles();
    if (hs.empty()) return;
    float best = 1.0e9f;
    for (size_t i = 0; i < hs.size(); ++i)
    {
        const float d = handlePos (hs[i]).getDistanceFrom (e.position);
        if (d < best) { best = d; active = (int) i; }
    }
    if (hs.size() > 1 && best > 40.0f)
    {
        // EQ: the band on the clicked side.
        active = e.position.x < (float) getWidth() * 0.5f ? 0 : 1;
    }
    const auto h = hs[(size_t) active];
    proc.beginGesture (ed.pid (h.sx));
    proc.beginGesture (ed.pid (h.sy));
    gestures = { ed.pid (h.sx), ed.pid (h.sy) };
    mouseDrag (e);
}

void Display::mouseDrag (const MouseEvent& e)
{
    auto& proc = ed.ctx.proc;
    const auto r = getLocalBounds().toFloat().reduced (2.0f);
    if (kind == DisplayKind::Transfer)
    {
        proc.set (ed.pid (5), jlimit (0.0f, 1.0f, driveStart - (float) e.getDistanceFromDragStartY() / 150.0f));
        return;
    }
    if (active < 0) return;
    const auto h = handles()[(size_t) active];
    const auto& dx = ed.desc (h.sx);
    const auto& dy = ed.desc (h.sy);
    const float tx = jlimit (0.0f, 1.0f, (e.position.x - r.getX()) / r.getWidth());
    proc.set (ed.pid (h.sx), dx.toNorm (jlimit (dx.min, dx.max, normToFreq (tx))));
    const float ty = jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight());
    if (dy.unit == FxUnitKind::Db)
    {
        const float db = dbBottom() + ty * (dbTop() - dbBottom());
        proc.set (ed.pid (h.sy), dy.toNorm (jlimit (dy.min, dy.max, db)));
    }
    else proc.set (ed.pid (h.sy), ty);
    ed.refresh();
}

void Display::mouseUp (const MouseEvent&)
{
    for (int id : gestures) ed.ctx.proc.endGesture (id);
    gestures.clear();
    active = -1;
}
} // namespace

std::unique_ptr<FxModuleEditor> createModuleEditor (PanelContext& ctx, FxHost& host, const FxPath& path, const FxModuleSlot& slot)
{
    return std::make_unique<ModuleEditor> (ctx, host, path, slot);
}
} // namespace tincture::ui::fxui
