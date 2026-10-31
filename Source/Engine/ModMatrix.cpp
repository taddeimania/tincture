// ModMatrix.cpp — sources, rows, compiled matrix and the modulation runtime
// (see ModMatrix.h).
#include "ModMatrix.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tincture
{
// ---------------------------------------------------------------------------
// Sources
// ---------------------------------------------------------------------------
namespace
{
const ModSourceInfo kNone { "none", "---", "---", false, false };

const ModSourceInfo kInfo[kNumModSources] = {
    { "env1", "Env 1", "Env 1", false, true }, { "env2", "Env 2", "Env 2", false, true },
    { "env3", "Env 3", "Env 3", false, true }, { "env4", "Env 4", "Env 4", false, true },
    { "lfo1", "LFO 1", "LFO 1", false, true }, { "lfo2", "LFO 2", "LFO 2", false, true },
    { "lfo3", "LFO 3", "LFO 3", false, true }, { "lfo4", "LFO 4", "LFO 4", false, true },
    { "lfo5", "LFO 5", "LFO 5", false, true }, { "lfo6", "LFO 6", "LFO 6", false, true },
    { "lfo7", "LFO 7", "LFO 7", false, true }, { "lfo8", "LFO 8", "LFO 8", false, true },
    { "lfo9", "LFO 9", "LFO 9", false, true }, { "lfo10", "LFO 10", "LFO 10", false, true },
    { "velo", "Velocity", "Velocity", false, true },
    { "relvelo", "Rel Velo", "Release Velocity", false, true },
    { "note", "Note", "Note", false, true },
    { "rand1", "Rand 1", "Note-On Rand 1", false, true },
    { "rand2", "Rand 2", "Note-On Rand 2", false, true },
    { "randd", "Rand Disc", "Note-On Rand (Discrete)", false, true },
    { "alt1", "Alt 1", "Note-On Alt 1", false, true },
    { "alt2", "Alt 2", "Note-On Alt 2", false, true },
    { "voices", "Voices", "Active Voices", false, false },
    { "vindex", "Voice Idx", "Voice Index", false, true },
    { "vmod1", "Voice Mod 1", "Voice Mod 1", false, true },
    { "vmod2", "Voice Mod 2", "Voice Mod 2", false, true },
    { "fixed", "Fixed", "Fixed", false, false },
    { "oscA", "Osc A", "Osc A", true, true }, { "oscB", "Osc B", "Osc B", true, true },
    { "oscC", "Osc C", "Osc C", true, true }, { "noise", "Noise", "Noise", true, true },
    { "sub", "Sub", "Sub", true, true },
    { "macro1", "Macro 1", "Macro 1", false, false }, { "macro2", "Macro 2", "Macro 2", false, false },
    { "macro3", "Macro 3", "Macro 3", false, false }, { "macro4", "Macro 4", "Macro 4", false, false },
    { "macro5", "Macro 5", "Macro 5", false, false }, { "macro6", "Macro 6", "Macro 6", false, false },
    { "macro7", "Macro 7", "Macro 7", false, false }, { "macro8", "Macro 8", "Macro 8", false, false },
    { "exprX", "Expr X", "Expression X", true, true },
    { "exprY", "Expr Y", "Expression Y", false, true },
    { "exprZ", "Expr Z", "Expression Z", false, true },
    { "filter1", "Filter 1", "Filter 1", true, true }, { "filter2", "Filter 2", "Filter 2", true, true },
    { "wheel", "Mod Wheel", "Mod Wheel", false, false },
    { "at", "Aftertouch", "Aftertouch", false, false },
    { "polyat", "Poly AT", "Poly Aftertch", false, true },
    { "bend", "Pitch Bend", "Pitch Bend", true, false },
};

ModSource range (ModSource first, int count, std::vector<ModSource>& out)
{
    for (int i = 0; i < count; ++i) out.push_back ((ModSource) ((int) first + i));
    return first;
}

bool startsWith (const std::string& s, const char* p) { return s.rfind (p, 0) == 0; }
}

const ModSourceInfo& modSourceInfo (ModSource s) noexcept
{
    const int i = (int) s;
    return i >= 0 && i < kNumModSources ? kInfo[i] : kNone;
}

ModSource modSourceFromId (const std::string& id) noexcept
{
    for (int i = 0; i < kNumModSources; ++i)
        if (id == kInfo[i].id) return (ModSource) i;
    return ModSource::None;
}

const std::vector<ModSourceCategory>& modSourceTree()
{
    static const std::vector<ModSourceCategory> tree = []
    {
        std::vector<ModSourceCategory> t;
        auto cat = [&t] (const char* name, ModSource first, int count)
        {
            ModSourceCategory c { name, {}, false };
            range (first, count, c.sources);
            t.push_back (std::move (c));
        };
        cat ("Envelopes", ModSource::Env1, 4);
        cat ("LFOs", ModSource::Lfo1, 10);
        cat ("Note", ModSource::Velocity, 13);
        cat ("Oscillators", ModSource::OscA, 5);
        cat ("Macros", ModSource::Macro1, 8);
        cat ("Note Expression", ModSource::ExprX, 3);
        cat ("Filters", ModSource::Filter1, 2);
        for (auto s : { ModSource::ModWheel, ModSource::Aftertouch, ModSource::PolyAftertouch, ModSource::PitchBend })
            t.push_back ({ modSourceInfo (s).menuName, { s }, true });
        return t;
    }();
    return tree;
}

void SourceValues::copyGlobalsFrom (const SourceValues& g) noexcept
{
    v[(int) ModSource::ActiveVoices] = g.v[(int) ModSource::ActiveVoices];
    v[(int) ModSource::Fixed] = 1.0f;
    for (int m = 0; m < kNumMacros; ++m) v[(int) ModSource::Macro1 + m] = g.v[(int) ModSource::Macro1 + m];
    v[(int) ModSource::ModWheel] = g.v[(int) ModSource::ModWheel];
    v[(int) ModSource::Aftertouch] = g.v[(int) ModSource::Aftertouch];
    v[(int) ModSource::PitchBend] = g.v[(int) ModSource::PitchBend];
}

float modSimpleBend (float x, float bend) noexcept
{
    x = clamp01 (x);
    if (std::abs (bend - 0.5f) < 1.0e-4f) return x;
    // bend 1 -> x^(1/4) (bows up), bend 0 -> x^4.
    return std::pow (x, std::exp2 ((0.5f - clamp01 (bend)) * 4.0f));
}

// ---------------------------------------------------------------------------
// ModRow
// ---------------------------------------------------------------------------
void ModRow::save (Node& n) const
{
    n.set ("source", modSourceInfo (source).id);
    n.set ("dest", dest >= 0 && dest < params().size() ? params()[dest].id : std::string());
    n.set ("curve", (double) curve);
    n.set ("custom", customCurve ? 1.0 : 0.0);
    n.set ("rise", (double) rise).set ("fall", (double) fall);
    n.set ("amount", (double) amount);
    n.set ("bipolar", bipolar ? 1.0 : 0.0);
    n.set ("aux", modSourceInfo (aux).id);
    n.set ("auxInv", auxInvert ? 1.0 : 0.0);
    n.set ("auxCurve", (double) auxCurve);
    n.set ("auxCustom", auxCustom ? 1.0 : 0.0);
    n.set ("output", (double) output);
    n.set ("bypass", bypass ? 1.0 : 0.0);
    if (customCurve || ! (srcCurve == Curve())) srcCurve.save (n.add ("srcCurve"));
    if (auxCustom || ! (auxCurveData == Curve())) auxCurveData.save (n.add ("auxCurveData"));
}

void ModRow::load (const Node& n)
{
    *this = ModRow();
    source = modSourceFromId (n.str ("source"));
    const std::string d = n.str ("dest");
    dest = d.empty() ? -1 : params().find (d);
    if (dest >= 0 && ! params()[dest].modulatable()) dest = -1;
    curve = clamp01 ((float) n.num ("curve", 0.5));
    customCurve = n.flag ("custom");
    rise = std::max (0.0f, (float) n.num ("rise"));
    fall = std::max (0.0f, (float) n.num ("fall"));
    amount = clampf ((float) n.num ("amount"), -1.0f, 1.0f);
    bipolar = n.flag ("bipolar");
    aux = modSourceFromId (n.str ("aux"));
    auxInvert = n.flag ("auxInv");
    auxCurve = clamp01 ((float) n.num ("auxCurve", 0.5));
    auxCustom = n.flag ("auxCustom");
    output = clamp01 ((float) n.num ("output", 1.0));
    bypass = n.flag ("bypass");
    if (auto* c = n.child ("srcCurve")) srcCurve.load (*c);
    if (auto* c = n.child ("auxCurveData")) auxCurveData.load (*c);
}

bool ModRow::operator== (const ModRow& o) const
{
    return source == o.source && curve == o.curve && customCurve == o.customCurve && srcCurve == o.srcCurve
        && rise == o.rise && fall == o.fall && amount == o.amount && bipolar == o.bipolar && dest == o.dest
        && aux == o.aux && auxInvert == o.auxInvert && auxCurve == o.auxCurve && auxCustom == o.auxCustom
        && auxCurveData == o.auxCurveData && output == o.output && bypass == o.bypass;
}

// ---------------------------------------------------------------------------
// ModMatrixData
// ---------------------------------------------------------------------------
namespace
{
bool validDest (int d) { return d >= 0 && d < params().size() && params()[d].modulatable(); }
}

bool ModMatrixData::defaultBipolar (int dest) { return validDest (dest) && params()[dest].bipolar(); }

int ModMatrixData::add (const ModRow& r)
{
    if ((int) rows.size() >= kMaxModRows) return -1;
    if (r.dest >= 0 && ! validDest (r.dest)) return -1;
    rows.push_back (r);
    return (int) rows.size() - 1;
}

int ModMatrixData::assign (ModSource s, int dest, float amount)
{
    if (const int i = find (s, dest); i >= 0) return i;
    ModRow r;
    r.source = s;
    r.dest = dest;
    r.amount = amount;
    r.bipolar = defaultBipolar (dest);
    return validDest (dest) ? add (r) : -1;
}

bool ModMatrixData::remove (int i)
{
    if (i < 0 || i >= (int) rows.size()) return false;
    rows.erase (rows.begin() + i);
    return true;
}

bool ModMatrixData::move (int from, int to)
{
    const int n = (int) rows.size();
    if (from < 0 || from >= n || to < 0 || to >= n) return false;
    if (from < to) std::rotate (rows.begin() + from, rows.begin() + from + 1, rows.begin() + to + 1);
    else if (from > to) std::rotate (rows.begin() + to, rows.begin() + from, rows.begin() + from + 1);
    return true;
}

void ModMatrixData::sortBySource()
{
    std::stable_sort (rows.begin(), rows.end(), [] (const ModRow& a, const ModRow& b)
    {
        const int ka = a.source == ModSource::None ? 1000 : (int) a.source;
        const int kb = b.source == ModSource::None ? 1000 : (int) b.source;
        return ka < kb;
    });
}

void ModMatrixData::sortByDestination()
{
    std::stable_sort (rows.begin(), rows.end(), [] (const ModRow& a, const ModRow& b)
    {
        const int ka = a.dest < 0 ? 1 << 30 : a.dest;
        const int kb = b.dest < 0 ? 1 << 30 : b.dest;
        return ka < kb;
    });
}

std::vector<int> ModMatrixData::findRows (int dest) const
{
    std::vector<int> r;
    for (int i = 0; i < (int) rows.size(); ++i)
        if (rows[(size_t) i].dest == dest) r.push_back (i);
    return r;
}

std::vector<int> ModMatrixData::rowsForSource (ModSource s) const
{
    std::vector<int> r;
    for (int i = 0; i < (int) rows.size(); ++i)
        if (rows[(size_t) i].source == s && rows[(size_t) i].dest >= 0) r.push_back (i);
    return r;
}

int ModMatrixData::find (ModSource s, int dest) const
{
    for (int i = 0; i < (int) rows.size(); ++i)
        if (rows[(size_t) i].source == s && rows[(size_t) i].dest == dest) return i;
    return -1;
}

void ModMatrixData::removeSource (ModSource s)
{
    rows.erase (std::remove_if (rows.begin(), rows.end(), [s] (const ModRow& r) { return r.source == s; }), rows.end());
}

void ModMatrixData::removeDest (int dest)
{
    rows.erase (std::remove_if (rows.begin(), rows.end(), [dest] (const ModRow& r) { return r.dest == dest; }), rows.end());
}

void ModMatrixData::setSourceBypass (ModSource s, bool b)
{
    for (auto& r : rows)
        if (r.source == s) r.bypass = b;
}

void ModMatrixData::removeRowsWhere (const std::function<bool (int)>& pred)
{
    rows.erase (std::remove_if (rows.begin(), rows.end(), [&pred] (const ModRow& r) { return r.dest >= 0 && pred (r.dest); }), rows.end());
}

void ModMatrixData::removeRowsForGroup (const std::string& group)
{
    removeRowsWhere ([&group] (int d) { return params()[d].group == group; });
}

int ModMatrixData::createVibrato (ParamValues* pv, LfoData* lfos)
{
    if ((int) rows.size() >= kMaxModRows) return -1;
    int l = 0;
    while (l < kNumLfos && ! rowsForSource (lfoSource (l)).empty()) ++l;
    if (l == kNumLfos) return -1;

    ModRow r;
    r.source = lfoSource (l);
    r.dest = ids().global.tuning;
    r.aux = ModSource::ModWheel;
    r.bipolar = true;
    const auto& tun = params()[r.dest];
    r.amount = 0.5f / (tun.max - tun.min);          // +-0.5 semitone
    add (r);

    if (pv != nullptr && lfos != nullptr)
    {
        const auto& id = ids().lfo[l];
        const auto& reg = params();
        auto untouched = [&] (int p) { return (*pv)[p] == reg[p].def; };
        if (untouched (id.rate) && untouched (id.rateSync) && untouched (id.bpm) && untouched (id.type)
            && lfos[l].shape == Curve::triangle())
        {
            (*pv)[id.bpm] = 0.0f;
            (*pv)[id.rate] = 5.0f;
            if (auto* s = LfoShapes::find ("Basic", "Sine")) { lfos[l].shape = s->curve; lfos[l].shapeName = "Sine"; }
        }
    }
    return l;
}

int ModMatrixData::createVeloAmp (ParamValues& pv)
{
    const int amp = ids().global.amp;
    int i = find (ModSource::Velocity, amp);
    if (i < 0)
    {
        ModRow r;
        r.source = ModSource::Velocity;
        r.dest = amp;
        r.amount = 1.0f;
        r.bipolar = false;
        i = add (r);
        if (i < 0) return -1;
    }
    pv[amp] = 0.0f;
    return i;
}

namespace
{
float rowCurve (const ModRow& r, float s)
{
    return r.customCurve ? r.srcCurve.evaluate (s) : modSimpleBend (s, r.curve);
}
}

void ModMatrixData::applyAndDeleteMacros (ParamValues& pv)
{
    const auto& reg = params();
    auto macroValue = [&pv] (ModSource s) { return pv[ids().macro[(int) s - (int) ModSource::Macro1]]; };
    auto isMacro = [] (ModSource s) { return (int) s >= (int) ModSource::Macro1 && (int) s <= (int) ModSource::Macro8; };

    // Sum the offsets per destination first (several macros can share one).
    std::vector<std::pair<int, float>> offsets;
    for (const auto& r : rows)
    {
        if (! isMacro (r.source) || ! r.active() || ! validDest (r.dest)) continue;
        float s = rowCurve (r, macroValue (r.source));
        if (r.bipolar) s = 2.0f * s - 1.0f;
        float a = 1.0f;
        if (r.aux != ModSource::None)
        {
            // Only a macro aux has a defined value here; other aux sources count as full scale.
            if (isMacro (r.aux)) a = r.auxCustom ? r.auxCurveData.evaluate (macroValue (r.aux)) : modSimpleBend (macroValue (r.aux), r.auxCurve);
            if (r.auxInvert) a = 1.0f - a;
        }
        const float m = s * r.amount * a * r.output;
        auto it = std::find_if (offsets.begin(), offsets.end(), [&r] (const auto& o) { return o.first == r.dest; });
        if (it == offsets.end()) offsets.push_back ({ r.dest, m });
        else it->second += m;
    }
    for (const auto& [d, m] : offsets)
    {
        const auto& info = reg[d];
        pv[d] = info.snap (info.fromNorm (clamp01 (info.toNorm (pv[d]) + m)));
    }
    rows.erase (std::remove_if (rows.begin(), rows.end(), [&isMacro] (const ModRow& r) { return isMacro (r.source); }), rows.end());
}

void ModMatrixData::save (Node& n) const
{
    n.set ("locked", locked ? 1.0 : 0.0);
    for (const auto& r : rows) r.save (n.add ("row"));
}

void ModMatrixData::load (const Node& n)
{
    rows.clear();
    locked = n.flag ("locked");
    for (auto* c : n.children ("row"))
    {
        if ((int) rows.size() >= kMaxModRows) break;
        ModRow r;
        r.load (*c);
        rows.push_back (std::move (r));
    }
}

bool ModMatrixData::loadPreset (const Node& n)
{
    if (locked) return false;
    load (n);
    locked = false;
    return true;
}

void ModGraphs::save (Node& n) const
{
    velo.save (n.add ("velo"));
    note.save (n.add ("note"));
    n.set ("veloLegato", veloLegato ? 1.0 : 0.0);
    n.set ("noteLegato", noteLegato ? 1.0 : 0.0);
}

void ModGraphs::load (const Node& n)
{
    *this = ModGraphs();
    if (auto* c = n.child ("velo")) velo.load (*c);
    if (auto* c = n.child ("note")) note.load (*c);
    veloLegato = n.flag ("veloLegato");
    noteLegato = n.flag ("noteLegato");
}

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
void MatrixVoiceState::noteOn (uint32_t seed) noexcept
{
    Rng r (seed * 747796405u + 2891336453u);
    for (float& d : discrete) d = r.uni();
    slewReady = 0;
}

namespace
{
// 2^x, relative error ~2e-5 (for Log-scaled destinations).
inline float fastPow2 (float x) noexcept
{
    x = clampf (x, -126.0f, 126.0f);
    const float fi = std::floor (x);
    const float f = x - fi;
    const float p = 1.0f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
    const int32_t bits = ((int32_t) fi + 127) << 23;
    float scale;
    std::memcpy (&scale, &bits, sizeof (scale));
    return p * scale;
}
}

// ---------------------------------------------------------------------------
// CompiledMatrix
// ---------------------------------------------------------------------------
std::shared_ptr<const CompiledMatrix> CompiledMatrix::build (const ModMatrixData& data, const ModGraphs& graphs)
{
    const auto& reg = params();
    auto c = std::make_shared<CompiledMatrix>();
    c->slotOf.assign ((size_t) reg.size(), (int16_t) -1);
    c->veloLut = BakedCurve (graphs.velo);
    c->noteLut = BakedCurve (graphs.note);
    c->numDataRows = std::min ((int) data.rows.size(), kMaxModRows);
    for (auto& p : c->rowPhase) p = -1;

    auto phaseOf = [&reg] (int d)
    {
        const auto& i = reg[d];
        if (i.perVoice()) return startsWith (i.group, "ENV ") || startsWith (i.group, "LFO ") ? kModulators : kMain;
        return i.group == "MACROS" ? kGlobalMacros : kGlobal;
    };
    auto kindOf = [] (const ParamInfo& i) -> uint8_t
    {
        switch (i.scale)
        {
            case ParamScale::Log:   return kLog;
            case ParamScale::Power: return std::abs (i.skew - 4.0f) < 1.0e-6f ? kPow4 : kPow;
            case ParamScale::Int:
            case ParamScale::Bool:
            case ParamScale::Choice: return kDiscreteKind;
            default:                return kLinear;
        }
    };

    // Destinations per phase, then slots: phase by phase, kind by kind.
    std::vector<int> dests[kNumPhases];
    for (int i = 0; i < c->numDataRows; ++i)
    {
        const auto& r = data.rows[(size_t) i];
        if (! r.active() || ! validDest (r.dest)) continue;
        const int ph = phaseOf (r.dest);
        c->rowPhase[i] = (int8_t) ph;
        if (std::find (dests[ph].begin(), dests[ph].end(), r.dest) == dests[ph].end()) dests[ph].push_back (r.dest);
    }
    int slot = 0;
    for (int ph = 0; ph < kNumPhases; ++ph)
    {
        c->slotBegin[ph] = slot;
        for (int k = 0; k < kNumKinds; ++k)
        {
            c->kindBegin[ph][k] = slot;
            for (int d : dests[ph])
            {
                const auto& info = reg[d];
                if (kindOf (info) != k) continue;
                Slot s { d, (uint8_t) k, info.min, info.max - info.min, info.skew, 0.0f };
                if (k == kLog)
                {
                    s.min = info.min > 0.0f ? info.min : 1.0e-4f;
                    s.log2Ratio = std::log2 (info.max / s.min);
                }
                c->slotInfo.push_back (s);
                c->slotOf[(size_t) d] = (int16_t) slot++;
            }
        }
        c->kindBegin[ph][kNumKinds] = slot;
    }
    c->slotBegin[kNumPhases] = slot;
    c->nSlots = slot;

    // Rows, grouped by phase.
    auto addLut = [&c] (auto&& fn) -> int16_t
    {
        SmallLut l;
        for (int i = 0; i <= SmallLut::kSize; ++i) l.t[i] = fn ((float) i / (float) SmallLut::kSize);
        c->luts.push_back (l);
        return (int16_t) (c->luts.size() - 1);
    };
    for (int ph = 0; ph < kNumPhases; ++ph)
    {
        c->rowBegin[ph] = (int) c->rows.size();
        for (int i = 0; i < c->numDataRows; ++i)
        {
            if (c->rowPhase[i] != ph) continue;
            const auto& r = data.rows[(size_t) i];
            const auto& si = modSourceInfo (r.source);
            Row x {};
            x.slot = c->slotOf[(size_t) r.dest];
            x.index = (int16_t) i;
            x.src = (int8_t) r.source;
            x.aux = (int8_t) r.aux;
            x.k = r.amount * r.output;
            x.lut = x.auxLut = -1;
            if (r.customCurve) x.lut = addLut ([&r] (float v) { return r.srcCurve.evaluate (v); });
            else if (std::abs (r.curve - 0.5f) > 1.0e-4f) x.lut = addLut ([&r] (float v) { return modSimpleBend (v, r.curve); });
            if (r.aux != ModSource::None)
            {
                if (r.auxCustom) x.auxLut = addLut ([&r] (float v) { return r.auxCurveData.evaluate (v); });
                else if (std::abs (r.auxCurve - 0.5f) > 1.0e-4f) x.auxLut = addLut ([&r] (float v) { return modSimpleBend (v, r.auxCurve); });
                if (modSourceInfo (r.aux).bipolar) x.flags |= kAuxBip;
                if (r.auxInvert) x.flags |= kAuxInv;
                if (r.aux == ModSource::NoteOnRandDiscrete) x.flags |= kAuxDiscrete;
                c->usedMask |= 1ull << (int) r.aux;
            }
            if (si.bipolar) x.flags |= kSrcBip;
            else if (r.bipolar) x.flags |= kToBip;
            if (r.source == ModSource::NoteOnRandDiscrete) x.flags |= kDiscrete;
            if (r.rise > 0.0f || r.fall > 0.0f)
            {
                x.flags |= kSlew;
                x.riseRate = r.rise > 0.0f ? 1.0f / r.rise : 1.0e30f;
                x.fallRate = r.fall > 0.0f ? 1.0f / r.fall : 1.0e30f;
            }
            c->usedMask |= 1ull << (int) r.source;
            c->rows.push_back (x);
        }
    }
    c->rowBegin[kNumPhases] = (int) c->rows.size();

    for (int m = 0; m < kNumMacros; ++m)
    {
        c->macroParam[m] = ids().macro[m];
        c->macroSlot[m] = c->slotOf[(size_t) ids().macro[m]];
    }
    return c;
}

int CompiledMatrix::phaseOfSlot (int s) const noexcept
{
    for (int ph = 0; ph < kNumPhases; ++ph)
        if (s < slotBegin[ph + 1]) return ph;
    return -1;
}

void CompiledMatrix::noteOnSources (const NoteInfo& n, uint32_t seed, bool alt, SourceValues& s, MatrixVoiceState& st) const noexcept
{
    s[ModSource::Velocity] = velocity (n.velocity);
    s[ModSource::ReleaseVelocity] = 0.0f;
    s[ModSource::Note] = note ((float) n.note);
    Rng r (seed ^ 0xA5A5F00Du);
    s[ModSource::NoteOnRand1] = r.uni();
    s[ModSource::NoteOnRand2] = r.uni();
    s[ModSource::NoteOnAlt1] = alt ? 1.0f : 0.0f;
    s[ModSource::NoteOnAlt2] = alt ? 0.0f : 1.0f;
    s[ModSource::Fixed] = 1.0f;
    st.noteOn (r.next());
}

void CompiledMatrix::beginBlock (const float* base, MatrixBlock& blk) const noexcept
{
    const auto& reg = params();
    for (int s = 0; s < nSlots; ++s)
    {
        const int p = slotInfo[(size_t) s].param;
        blk.baseNorm[s] = reg[p].toNorm (base[p]);
        blk.globalModded[s] = base[p];
    }
    for (int m = 0; m < kNumMacros; ++m) blk.macro[m] = base[macroParam[m]];
}

void CompiledMatrix::runRows (int phase, const float* src, MatrixVoiceState& st, float seconds) const noexcept
{
    const Row* r = rows.data() + rowBegin[phase];
    const Row* const e = rows.data() + rowBegin[phase + 1];
    for (; r != e; ++r)
    {
        float s = (r->flags & kDiscrete) ? st.discrete[r->index] : src[r->src];
        if (r->lut >= 0)
        {
            const SmallLut& l = luts[(size_t) r->lut];
            s = (r->flags & kSrcBip) ? l ((s + 1.0f) * 0.5f) * 2.0f - 1.0f : l (s);
        }
        if (r->flags & kSlew)
        {
            float& y = st.slew[r->index];
            const uint64_t bit = 1ull << r->index;
            if ((st.slewReady & bit) == 0) { y = s; st.slewReady |= bit; }
            else
            {
                const float up = r->riseRate * seconds, down = r->fallRate * seconds;
                y = s > y ? std::min (s, y + up) : std::max (s, y - down);
            }
            s = y;
        }
        if (r->flags & kToBip) s = 2.0f * s - 1.0f;
        float a = 1.0f;
        if (r->aux >= 0)
        {
            a = (r->flags & kAuxDiscrete) ? st.discrete[r->index] : src[r->aux];
            if (r->flags & kAuxBip) a = (a + 1.0f) * 0.5f;
            if (r->auxLut >= 0) a = luts[(size_t) r->auxLut] (a);
            if (r->flags & kAuxInv) a = 1.0f - a;
        }
        const float m = s * r->k * a;
        st.acc[r->slot] += m;
        st.rowOut[r->index] = m;
    }
}

void CompiledMatrix::finish (int phase, const float* baseNorm, float* acc, float* out) const noexcept
{
    const Slot* si = slotInfo.data();
    const int* kb = kindBegin[phase];
    for (int s = kb[kLinear]; s < kb[kLinear + 1]; ++s)
    {
        const float n = clamp01 (baseNorm[s] + acc[s]);
        acc[s] = 0.0f;
        out[s] = si[s].min + si[s].range * n;
    }
    for (int s = kb[kPow4]; s < kb[kPow4 + 1]; ++s)
    {
        const float n = clamp01 (baseNorm[s] + acc[s]);
        acc[s] = 0.0f;
        const float n2 = n * n;
        out[s] = si[s].min + si[s].range * (n2 * n2);
    }
    for (int s = kb[kPow]; s < kb[kPow + 1]; ++s)
    {
        const float n = clamp01 (baseNorm[s] + acc[s]);
        acc[s] = 0.0f;
        out[s] = si[s].min + si[s].range * (n > 0.0f ? fastPow2 (si[s].skew * std::log2 (n)) : 0.0f);
    }
    for (int s = kb[kLog]; s < kb[kLog + 1]; ++s)
    {
        const float n = clamp01 (baseNorm[s] + acc[s]);
        acc[s] = 0.0f;
        out[s] = si[s].min * fastPow2 (n * si[s].log2Ratio);
    }
    for (int s = kb[kDiscreteKind]; s < kb[kDiscreteKind + 1]; ++s)
    {
        const float n = clamp01 (baseNorm[s] + acc[s]);
        acc[s] = 0.0f;
        out[s] = std::floor (si[s].min + si[s].range * n + 0.5f);
    }
}

void CompiledMatrix::evalGlobal (const SourceValues& src, MatrixVoiceState& st, MatrixBlock& blk, float seconds) const noexcept
{
    SourceValues s = src;
    blk.applyMacros (s);
    runRows (kGlobalMacros, s.v, st, seconds);
    finish (kGlobalMacros, blk.baseNorm, st.acc, blk.globalModded);
    for (int m = 0; m < kNumMacros; ++m)
        if (macroSlot[m] >= 0) blk.macro[m] = blk.globalModded[macroSlot[m]];
    blk.applyMacros (s);
    runRows (kGlobal, s.v, st, seconds);
    finish (kGlobal, blk.baseNorm, st.acc, blk.globalModded);
}

void CompiledMatrix::evalModulators (const SourceValues& src, MatrixVoiceState& st, const MatrixBlock& blk, float* modded, float seconds) const noexcept
{
    runRows (kModulators, src.v, st, seconds);
    finish (kModulators, blk.baseNorm, st.acc, modded);
    for (int s = slotBegin[kGlobalMacros]; s < nSlots; ++s) modded[s] = blk.globalModded[s];
}

void CompiledMatrix::evalMain (const SourceValues& src, MatrixVoiceState& st, const MatrixBlock& blk, float* modded, float seconds) const noexcept
{
    runRows (kMain, src.v, st, seconds);
    finish (kMain, blk.baseNorm, st.acc, modded);
}

void CompiledMatrix::publish (const MatrixVoiceState* newest, const MatrixVoiceState& global, MatrixTelemetry& t) const noexcept
{
    for (int i = 0; i < kMaxModRows; ++i)
    {
        const int ph = i < numDataRows ? rowPhase[i] : -1;
        float v = 0.0f;
        if (ph >= kGlobalMacros) v = global.rowOut[i];
        else if (ph >= 0 && newest != nullptr) v = newest->rowOut[i];
        t.rowOut[i].store (v, std::memory_order_relaxed);
    }
}

// ---------------------------------------------------------------------------
// ModGlobal
// ---------------------------------------------------------------------------
void ModGlobal::reset (uint32_t seed) noexcept
{
    globals = SourceValues();
    globals[ModSource::Fixed] = 1.0f;
    block = MatrixBlock();
    state = MatrixVoiceState();
    scratch = SourceValues();
    for (int l = 0; l < kNumLfos; ++l)
    {
        lfo[l].reset (seed + 7919u * (uint32_t) (l + 1));
        settings[l] = LfoSettings();
        shared[l] = false;
    }
}

void ModGlobal::beginBlock (const CompiledMatrix& m, const ParamValues& base, const ModVoice* newest, float blockSeconds) noexcept
{
    m.beginBlock (base.v.data(), block);
    scratch = newest != nullptr ? newest->src : SourceValues();
    scratch.copyGlobalsFrom (globals);
    m.evalGlobal (scratch, state, block, blockSeconds);
    block.applyMacros (scratch);
}

void ModGlobal::tick (const CompiledMatrix&, const LfoShapeSet& shapes, const ParamView& pv, const Context& ctx, int offset, int n) noexcept
{
    const LfoClock clk = LfoClock::at (ctx, offset);
    const float swing = pv (ids().global.swing);
    for (int l = 0; l < kNumLfos; ++l)
    {
        settings[l] = LfoSettings::read (pv, ids().lfo[l], ctx.bpm, swing);
        shared[l] = settings[l].mono || settings[l].mode == LfoMode::Free;
        if (shared[l]) lfo[l].process (settings[l], shapes.lfo[l], clk, n, scratch.v);
    }
}

void ModGlobal::noteOn (const LfoShapeSet& shapes, const ParamView& pv, const Context& ctx, int offset, bool legato) noexcept
{
    if (legato) return;
    const LfoClock clk = LfoClock::at (ctx, offset);
    const float swing = pv (ids().global.swing);
    for (int l = 0; l < kNumLfos; ++l)
    {
        const LfoSettings s = LfoSettings::read (pv, ids().lfo[l], ctx.bpm, swing);
        if (s.mono) lfo[l].trigger (s, shapes.lfo[l], clk);
    }
}

void ModGlobal::allNotesReleased() noexcept
{
    for (int l = 0; l < kNumLfos; ++l)
        if (settings[l].mono) lfo[l].release();
}

void ModGlobal::publish (const CompiledMatrix& m, const ModVoice* newest, MatrixTelemetry& mt, LfoTelemetry* lfoT) const noexcept
{
    m.publish (newest != nullptr ? &newest->mst : nullptr, state, mt);
    if (lfoT == nullptr) return;
    for (int l = 0; l < kNumLfos; ++l)
    {
        if (shared[l]) lfo[l].publish (lfoT[l]);
        else if (newest != nullptr) newest->lfo[l].publish (lfoT[l]);
    }
}

// ---------------------------------------------------------------------------
// ModVoice
// ---------------------------------------------------------------------------
void ModVoice::reset (uint32_t seed) noexcept
{
    for (auto& e : env) e.reset();
    for (int l = 0; l < kNumLfos; ++l) lfo[l].reset (seed * 31u + (uint32_t) l);
    for (auto& t : envTick) t = EnvTick();
    for (float& v : lfoPrev) v = 0.0f;
    src = SourceValues();
    mst = MatrixVoiceState();
    std::memset (modded, 0, sizeof (modded));
    cur = 0;
    slotsChanged = true;
}

namespace
{
bool lfoUsed (const CompiledMatrix& m, const LfoShapeSet& shapes, int l) noexcept
{
    const ModSource s = lfoSource (l);
    return m.sourceUsed (s) || ((shapes.busSources >> (int) s) & 1u) != 0;
}
}

void ModVoice::noteOn (const CompiledMatrix& m, const LfoShapeSet& shapes, const ModGlobal& g, const NoteInfo& n,
                       const ParamValues& base, const Context& ctx, int offset, uint32_t seed, bool alt, bool fresh) noexcept
{
    m.noteOnSources (n, seed, alt, src, mst);
    src.copyGlobalsFrom (g.globals);
    g.matrixBlock().applyMacros (src);
    if (fresh)
        for (int e = 0; e < kNumEnvs; ++e) src[envSource (e)] = env[e].value();

    // Settle this voice's modulated parameters before reading the trigger settings.
    m.evalModulators (src, mst, g.matrixBlock(), modded[cur], 0.0f);
    m.evalMain (src, mst, g.matrixBlock(), modded[cur], 0.0f);
    std::memcpy (modded[cur ^ 1], modded[cur], sizeof (modded[0]));
    slotsChanged = false;

    const ParamView pv = view (m, base);
    for (int e = 0; e < kNumEnvs; ++e)
    {
        if (fresh) env[e].trigger();
        else env[e].noteOn (n.legato, pv.on (ids().env[e].legatoInvert));
    }
    if (fresh || ! n.legato)
    {
        const LfoClock clk = LfoClock::at (ctx, offset);
        const float swing = pv (ids().global.swing);
        for (int l = 0; l < kNumLfos; ++l)
            lfo[l].trigger (LfoSettings::read (pv, ids().lfo[l], ctx.bpm, swing), shapes.lfo[l], clk);
    }
}

void ModVoice::noteOff (float releaseVelocity) noexcept
{
    src[ModSource::ReleaseVelocity] = releaseVelocity;
    for (auto& e : env) e.noteOff();
    for (auto& l : lfo) l.release();
}

void ModVoice::tick (const CompiledMatrix& m, const LfoShapeSet& shapes, const ModGlobal& g, const ParamValues& base,
                     const Context& ctx, int offset, int n) noexcept
{
    cur ^= 1;
    const float sr = (float) ctx.sampleRate;
    const float sec = (float) n / sr;
    const MatrixBlock& blk = g.matrixBlock();
    src.copyGlobalsFrom (g.globals);
    blk.applyMacros (src);
    for (int l = 0; l < kNumLfos; ++l) lfoPrev[l] = src.v[(int) ModSource::Lfo1 + l];

    // 2. Rows into ENV / LFO parameters (env / LFO sources from the previous tick).
    m.evalModulators (src, mst, blk, modded[cur], sec);
    const ParamView pv = view (m, base);

    // 3. Envelopes and LFOs.
    for (int e = 0; e < kNumEnvs; ++e)
    {
        envTick[e] = env[e].process (EnvSettings::read (pv, ids().env[e], ctx.bpm), n, sr);
        src[envSource (e)] = envTick[e].end;
    }
    const LfoClock clk = LfoClock::at (ctx, offset);
    const float swing = pv (ids().global.swing);
    for (int l = 0; l < kNumLfos; ++l)
    {
        if (! lfoUsed (m, shapes, l)) continue;
        float v;
        if (g.isShared (l))
        {
            const LfoSettings& s = g.sharedSettings (l);
            if (s.mono) { v = g.sharedLfo (l).value(); lfoPrev[l] = g.sharedLfo (l).previous(); }
            else v = lfo[l].follow (LfoSettings::read (pv, ids().lfo[l], ctx.bpm, swing), shapes.lfo[l], g.sharedLfo (l).value(), n, sr);
        }
        else v = lfo[l].process (LfoSettings::read (pv, ids().lfo[l], ctx.bpm, swing), shapes.lfo[l], clk, n, src.v);
        src.v[(int) ModSource::Lfo1 + l] = v;
    }

    // 4. Everything else.
    m.evalMain (src, mst, blk, modded[cur], sec);
    if (slotsChanged)
    {
        std::memcpy (modded[cur ^ 1], modded[cur], sizeof (modded[0]));
        slotsChanged = false;
    }
}
} // namespace tincture
