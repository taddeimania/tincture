// FxLayout.cpp — FX rack patch data: editing operations, unit allocation,
// rack presets and save/load.
#include "FxLayout.h"

#include <algorithm>

namespace tincture
{
namespace
{
bool typeFromName (const std::string& s, FxType& out)
{
    const auto& names = fxTypeNames();
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == s) { out = (FxType) i; return true; }
    return false;
}

void walk (const std::vector<FxModuleSlot>& l, const FxList& where,
           const std::function<void (const FxModuleSlot&, const FxPath&)>& fn)
{
    for (size_t i = 0; i < l.size(); ++i)
    {
        fn (l[i], FxPath { where, (int) i });
        if (where.splitter < 0 && l[i].isSplitter())
            for (int b = 0; b < fxInfo (l[i].type).numBands; ++b)
                walk (l[i].bands[(size_t) b], FxList { where.rack, (int) i, b }, fn);
    }
}

void assignUnits (FxModuleSlot& m, std::vector<int>& freeUnits, std::vector<std::pair<int, int>>* pairs)
{
    const int u = freeUnits.front();
    freeUnits.erase (freeUnits.begin());
    if (pairs) pairs->push_back ({ m.unit, u });
    m.unit = u;
    for (auto& b : m.bands)
        for (auto& c : b)
            assignUnits (c, freeUnits, pairs);
}
} // namespace

// ---------------------------------------------------------------------------
// FxModuleSlot
// ---------------------------------------------------------------------------
FxModuleSlot FxModuleSlot::make (FxType t)
{
    FxModuleSlot m;
    m.type = t;
    if (t == FxType::Distortion)
    {
        m.shapeA = Curve();                                // identity
        m.shapeB.points = { { 0.0f, 0.0f, 0.0f, false }, { 0.2f, 0.7f, 0.4f, false }, { 0.55f, 0.35f, 0.0f, false },
                            { 1.0f, 1.0f, -0.3f, false } }; // a folding shape
    }
    return m;
}

void FxModuleSlot::save (Node& n, bool withUnits, const ValueGetter* values) const
{
    n.set ("type", fxInfo (type).name);
    if (withUnits) n.set ("unit", (double) unit);
    n.set ("bypass", bypass ? 1.0 : 0.0).set ("expanded", expanded ? 1.0 : 0.0);
    if (values && unit >= 0)
    {
        const FxUnitValues v = (*values) (unit);
        std::vector<float> f { v.mix };
        f.insert (f.end(), v.p.begin(), v.p.end());
        n.set ("values", std::move (f));
    }
    if (type == FxType::Distortion)
    {
        Node a ("shapeA"), b ("shapeB");
        shapeA.save (a);
        shapeB.save (b);
        n.add (std::move (a));
        n.add (std::move (b));
    }
    if (type == FxType::Convolve)
    {
        n.set ("ir", (double) irIndex);
        if (userIr)
        {
            Node s ("userIr");
            userIr->save (s);
            n.add (std::move (s));
        }
    }
    if (isSplitter())
    {
        n.set ("selBand", (double) selectedBand);
        for (int b = 0; b < fxInfo (type).numBands; ++b)
        {
            Node bn ("band");
            bn.set ("index", (double) b).set ("bypass", bandBypass[(size_t) b] ? 1.0 : 0.0);
            for (auto& c : bands[(size_t) b])
            {
                Node cn ("fx");
                c.save (cn, withUnits, values);
                bn.add (std::move (cn));
            }
            n.add (std::move (bn));
        }
    }
}

bool FxModuleSlot::load (const Node& n, FxModuleSlot& out, bool withUnits, bool allowSplitter, std::vector<FxUnitValues>* values)
{
    FxType t = FxType::Utility;
    if (! typeFromName (n.str ("type"), t)) return false;
    if (fxInfo (t).splitter && ! allowSplitter) return false;
    out = make (t);
    out.unit = withUnits ? n.integer ("unit", -1) : -1;
    if (withUnits && (out.unit < 0 || out.unit >= kNumFxUnits)) return false;
    out.bypass = n.flag ("bypass");
    out.expanded = n.flag ("expanded", true);
    if (values)
    {
        FxUnitValues v;
        const auto d = fxDefaults (t);
        std::copy (d.begin(), d.end(), v.p.begin());
        v.mix = fxDefaultMix (t);
        if (auto* f = n.floats ("values"))
        {
            if (! f->empty()) v.mix = clamp01 ((*f)[0]);
            for (size_t k = 1; k < f->size() && k <= (size_t) kFxParams; ++k) v.p[k - 1] = clamp01 ((*f)[k]);
        }
        values->push_back (v);
    }
    if (t == FxType::Distortion)
    {
        if (auto* a = n.child ("shapeA")) out.shapeA.load (*a);
        if (auto* b = n.child ("shapeB")) out.shapeB.load (*b);
    }
    if (t == FxType::Convolve)
    {
        out.irIndex = std::max (0, n.integer ("ir", 0));
        if (auto* s = n.child ("userIr")) out.userIr = SampleData::load (*s);
    }
    if (fxInfo (t).splitter)
    {
        out.selectedBand = std::clamp (n.integer ("selBand", 0), 0, fxInfo (t).numBands - 1);
        for (auto* bn : n.children ("band"))
        {
            const int b = bn->integer ("index", -1);
            if (b < 0 || b >= fxInfo (t).numBands) continue;
            out.bandBypass[(size_t) b] = bn->flag ("bypass");
            for (auto* cn : bn->children ("fx"))
            {
                FxModuleSlot c;
                if (load (*cn, c, withUnits, false, values))
                    out.bands[(size_t) b].push_back (std::move (c));
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// FxLayout: lookup
// ---------------------------------------------------------------------------
std::vector<FxModuleSlot>* FxLayout::list (const FxList& l)
{
    return const_cast<std::vector<FxModuleSlot>*> (static_cast<const FxLayout*> (this)->list (l));
}

const std::vector<FxModuleSlot>* FxLayout::list (const FxList& l) const
{
    if (l.rack < 0 || l.rack >= kNumFxRacks) return nullptr;
    const auto& top = racks[(size_t) l.rack].modules;
    if (l.splitter < 0) return &top;
    if (l.splitter >= (int) top.size()) return nullptr;
    const auto& s = top[(size_t) l.splitter];
    if (! s.isSplitter() || l.band < 0 || l.band >= fxInfo (s.type).numBands) return nullptr;
    return &s.bands[(size_t) l.band];
}

FxModuleSlot* FxLayout::get (const FxPath& p)
{
    auto* l = list (p.list);
    return l && p.index >= 0 && p.index < (int) l->size() ? &(*l)[(size_t) p.index] : nullptr;
}

const FxModuleSlot* FxLayout::get (const FxPath& p) const
{
    auto* l = list (p.list);
    return l && p.index >= 0 && p.index < (int) l->size() ? &(*l)[(size_t) p.index] : nullptr;
}

void FxLayout::forEach (const std::function<void (const FxModuleSlot&, const FxPath&)>& fn) const
{
    for (int r = 0; r < kNumFxRacks; ++r)
        walk (racks[(size_t) r].modules, FxList { r, -1, -1 }, fn);
}

bool FxLayout::findUnit (int unit, FxPath& out) const
{
    bool found = false;
    forEach ([&] (const FxModuleSlot& m, const FxPath& p) { if (! found && m.unit == unit) { out = p; found = true; } });
    return found;
}

std::array<bool, kNumFxUnits> FxLayout::usedUnits() const
{
    std::array<bool, kNumFxUnits> u {};
    forEach ([&] (const FxModuleSlot& m, const FxPath&) { if (m.unit >= 0 && m.unit < kNumFxUnits) u[(size_t) m.unit] = true; });
    return u;
}

int FxLayout::numFreeUnits() const
{
    const auto u = usedUnits();
    return (int) std::count (u.begin(), u.end(), false);
}

int FxLayout::countModules (const FxModuleSlot& m) const
{
    int c = 1;
    for (auto& b : m.bands) for (auto& x : b) c += countModules (x);
    return c;
}

int FxLayout::allocUnit() const
{
    const auto u = usedUnits();
    for (int i = 0; i < kNumFxUnits; ++i) if (! u[(size_t) i]) return i;
    return -1;
}

void FxLayout::collectUnits (const FxModuleSlot& m, std::vector<int>& out)
{
    if (m.unit >= 0) out.push_back (m.unit);
    for (auto& b : m.bands) for (auto& c : b) collectUnits (c, out);
}

// ---------------------------------------------------------------------------
// FxLayout: editing
// ---------------------------------------------------------------------------
int FxLayout::add (const FxList& where, FxType t, int at)
{
    auto* l = list (where);
    if (! l || (where.splitter >= 0 && fxInfo (t).splitter)) return -1;
    const int u = allocUnit();
    if (u < 0) return -1;
    auto m = FxModuleSlot::make (t);
    m.unit = u;
    if (at < 0 || at > (int) l->size()) at = (int) l->size();
    l->insert (l->begin() + at, std::move (m));
    markCustom (where.rack);
    return u;
}

std::vector<int> FxLayout::remove (const FxPath& p)
{
    std::vector<int> freed;
    auto* l = list (p.list);
    if (! l || p.index < 0 || p.index >= (int) l->size()) return freed;
    collectUnits ((*l)[(size_t) p.index], freed);
    l->erase (l->begin() + p.index);
    markCustom (p.list.rack);
    return freed;
}

bool FxLayout::move (const FxPath& from, const FxList& to, int at)
{
    auto* src = list (from.list);
    auto* dst = list (to);
    if (! src || ! dst || from.index < 0 || from.index >= (int) src->size()) return false;
    const auto& m = (*src)[(size_t) from.index];
    if (to.splitter >= 0 && m.isSplitter()) return false;                  // no splitter in a band
    FxList dest = to;
    const bool fromTop = from.list.splitter < 0;
    if (fromTop && to.splitter >= 0 && to.rack == from.list.rack)
    {
        if (to.splitter == from.index) return false;                       // into its own band
        if (from.index < to.splitter) --dest.splitter;                     // removal shifts the splitter
    }
    if (at < 0 || at > (int) dst->size()) at = (int) dst->size();
    const bool sameList = src == dst;
    if (sameList && at > from.index) --at;

    FxModuleSlot moving = std::move ((*src)[(size_t) from.index]);
    src->erase (src->begin() + from.index);
    dst = list (dest);
    if (! dst) return false;                                                // can't happen after the checks
    at = std::clamp (at, 0, (int) dst->size());
    dst->insert (dst->begin() + at, std::move (moving));
    markCustom (from.list.rack);
    markCustom (to.rack);
    return true;
}

std::vector<std::pair<int, int>> FxLayout::duplicate (const FxPath& from, const FxList& to, int at)
{
    std::vector<std::pair<int, int>> pairs;
    const auto* m = get (from);
    auto* dst = list (to);
    if (! m || ! dst || (to.splitter >= 0 && m->isSplitter())) return pairs;
    if (countModules (*m) > numFreeUnits()) return pairs;
    FxModuleSlot copy = *m;
    std::vector<int> freeUnits;
    const auto used = usedUnits();
    for (int i = 0; i < kNumFxUnits; ++i) if (! used[(size_t) i]) freeUnits.push_back (i);
    assignUnits (copy, freeUnits, &pairs);
    if (at < 0 || at > (int) dst->size()) at = (int) dst->size();
    dst->insert (dst->begin() + at, std::move (copy));
    markCustom (to.rack);
    return pairs;
}

bool FxLayout::setBypass (const FxPath& p, bool on)
{
    auto* m = get (p);
    if (! m) return false;
    m->bypass = on;
    return true;
}

void FxLayout::setRackBypass (int rack, bool on)
{
    if (rack < 0 || rack >= kNumFxRacks) return;
    for (auto& m : racks[(size_t) rack].modules)
    {
        m.bypass = on;
        for (auto& b : m.bands) for (auto& c : b) c.bypass = on;
    }
}

bool FxLayout::setBandBypass (int rack, int splitter, int band, bool on)
{
    auto* m = get (FxPath { FxList { rack, -1, -1 }, splitter });
    if (! m || ! m->isSplitter() || band < 0 || band >= fxInfo (m->type).numBands) return false;
    m->bandBypass[(size_t) band] = on;
    return true;
}

std::vector<int> FxLayout::clearRack (int rack)
{
    std::vector<int> freed;
    if (rack < 0 || rack >= kNumFxRacks) return freed;
    for (auto& m : racks[(size_t) rack].modules) collectUnits (m, freed);
    racks[(size_t) rack].modules.clear();
    racks[(size_t) rack].presetName = "Init";
    return freed;
}

// ---------------------------------------------------------------------------
// Rack presets
// ---------------------------------------------------------------------------
Node FxLayout::saveRack (int rack, const ValueGetter& values) const
{
    Node n ("fxrack");
    if (rack < 0 || rack >= kNumFxRacks) return n;
    n.set ("name", racks[(size_t) rack].presetName);
    for (auto& m : racks[(size_t) rack].modules)
    {
        Node c ("fx");
        m.save (c, false, &values);
        n.add (std::move (c));
    }
    return n;
}

bool FxLayout::loadRack (int rack, const Node& n, std::vector<FxUnitValues>& out, std::vector<int>* freed)
{
    if (rack < 0 || rack >= kNumFxRacks) return false;
    std::vector<FxModuleSlot> mods;
    std::vector<FxUnitValues> vals;
    for (auto* c : n.children ("fx"))
    {
        FxModuleSlot m;
        std::vector<FxUnitValues> v;
        if (FxModuleSlot::load (*c, m, false, true, &v))
        {
            mods.push_back (std::move (m));
            vals.insert (vals.end(), v.begin(), v.end());
        }
    }
    // Units available once this rack's current modules are gone.
    std::vector<int> old;
    for (auto& m : racks[(size_t) rack].modules) collectUnits (m, old);
    auto used = usedUnits();
    for (int u : old) used[(size_t) u] = false;
    std::vector<int> freeUnits;
    for (int i = 0; i < kNumFxUnits; ++i) if (! used[(size_t) i]) freeUnits.push_back (i);
    if (vals.size() > freeUnits.size()) return false;

    std::vector<std::pair<int, int>> pairs;
    for (auto& m : mods) assignUnits (m, freeUnits, &pairs);
    for (size_t i = 0; i < vals.size(); ++i) vals[i].unit = pairs[i].second;     // same depth-first order

    racks[(size_t) rack].modules = std::move (mods);
    racks[(size_t) rack].presetName = n.str ("name", "Custom");
    out = std::move (vals);
    if (freed) *freed = std::move (old);
    return true;
}

// ---------------------------------------------------------------------------
// Whole layout
// ---------------------------------------------------------------------------
void FxLayout::save (Node& n) const
{
    for (int r = 0; r < kNumFxRacks; ++r)
    {
        Node rn ("rack");
        rn.set ("index", (double) r).set ("name", racks[(size_t) r].presetName)
          .set ("locked", racks[(size_t) r].locked ? 1.0 : 0.0);
        for (auto& m : racks[(size_t) r].modules)
        {
            Node c ("fx");
            m.save (c, true);
            rn.add (std::move (c));
        }
        n.add (std::move (rn));
    }
}

bool FxLayout::load (const Node& n, bool respectLocks, std::vector<FxUnitRemap>* remap)
{
    FxLayout fresh;
    std::array<bool, kNumFxUnits> taken {};
    // Kept (locked) racks first: their units stay.
    for (int r = 0; r < kNumFxRacks; ++r)
    {
        fresh.racks[(size_t) r].locked = racks[(size_t) r].locked;
        if (respectLocks && racks[(size_t) r].locked)
        {
            fresh.racks[(size_t) r] = racks[(size_t) r];
            std::vector<int> u;
            for (auto& m : racks[(size_t) r].modules) collectUnits (m, u);
            for (int x : u) taken[(size_t) x] = true;
        }
    }
    std::vector<std::pair<FxModuleSlot*, int>> clashes;   // modules needing a new unit
    std::function<void (FxModuleSlot&)> claim = [&] (FxModuleSlot& m)
    {
        if (m.unit < 0 || m.unit >= kNumFxUnits || taken[(size_t) m.unit]) clashes.push_back ({ &m, m.unit });
        else taken[(size_t) m.unit] = true;
        for (auto& b : m.bands) for (auto& c : b) claim (c);
    };
    for (auto* rn : n.children ("rack"))
    {
        const int r = rn->integer ("index", -1);
        if (r < 0 || r >= kNumFxRacks || (respectLocks && racks[(size_t) r].locked)) continue;
        auto& dst = fresh.racks[(size_t) r];
        dst.presetName = rn->str ("name", "Init");
        if (! respectLocks) dst.locked = rn->flag ("locked");
        for (auto* c : rn->children ("fx"))
        {
            FxModuleSlot m;
            if (FxModuleSlot::load (*c, m, true, true)) dst.modules.push_back (std::move (m));
        }
    }
    for (int r = 0; r < kNumFxRacks; ++r)
        if (! (respectLocks && racks[(size_t) r].locked))       // kept racks already hold their units
            for (auto& m : fresh.racks[(size_t) r].modules)
                claim (m);
    // Re-home clashing modules; drop them when the pool is exhausted.
    for (auto& [m, from] : clashes)
    {
        int u = -1;
        for (int i = 0; i < kNumFxUnits && u < 0; ++i) if (! taken[(size_t) i]) u = i;
        m->unit = u;
        if (u >= 0)
        {
            taken[(size_t) u] = true;
            if (remap) remap->push_back ({ from, u });
        }
    }
    // Remove modules left without a unit (and their children).
    std::function<void (std::vector<FxModuleSlot>&)> prune = [&] (std::vector<FxModuleSlot>& l)
    {
        l.erase (std::remove_if (l.begin(), l.end(), [] (const FxModuleSlot& m) { return m.unit < 0; }), l.end());
        for (auto& m : l) for (auto& b : m.bands) prune (b);
    };
    for (auto& rk : fresh.racks) prune (rk.modules);
    *this = std::move (fresh);
    return true;
}
} // namespace tincture
