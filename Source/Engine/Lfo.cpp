// Lfo.cpp — LFO 1-10, shape baking and the shape library (see Lfo.h).
#include "Lfo.h"
#include "ModMatrix.h"

#include <cmath>

namespace tincture
{
// ---------------------------------------------------------------------------
// LfoData
// ---------------------------------------------------------------------------
int LfoData::nextFreeBus() const noexcept
{
    for (int b = 0; b < kMaxLfoBuses; ++b)
        if (buses[b].source < 0) return b;
    return -1;
}

namespace
{
void savePoints (Node& n, const char* key, const std::vector<CurvePoint>& pts)
{
    std::vector<float> d;
    d.reserve (pts.size() * 4);
    for (const auto& p : pts) { d.push_back (p.x); d.push_back (p.y); d.push_back (p.curve); d.push_back (p.flat ? 1.0f : 0.0f); }
    n.set (key, std::move (d));
}

void loadPoints (const Node& n, const char* key, std::vector<CurvePoint>& pts)
{
    pts.clear();
    if (auto* d = n.floats (key); d != nullptr && d->size() % 4 == 0)
        for (size_t i = 0; i + 3 < d->size(); i += 4)
            pts.push_back ({ clamp01 ((*d)[i]), clamp01 ((*d)[i + 1]), clampf ((*d)[i + 2], -1.0f, 1.0f), (*d)[i + 3] > 0.5f });
}
}

void LfoData::save (Node& n) const
{
    shape.save (n.add ("shape"));
    if (! path.empty()) savePoints (n, "path", path);
    n.set ("loopback", (double) loopback);
    n.set ("name", shapeName);
    for (int b = 0; b < kMaxLfoBuses; ++b)
    {
        if (buses[b].source < 0) continue;
        auto& c = n.add ("bus");
        c.set ("index", (double) b);
        c.set ("source", modSourceInfo ((ModSource) buses[b].source).id);
        c.set ("amount", (double) buses[b].amount);
    }
    for (const auto& m : pointMods)
    {
        auto& c = n.add ("pmod");
        c.set ("point", (double) m.point).set ("bus", (double) m.bus);
        c.set ("dx", (double) m.dx).set ("dy", (double) m.dy).set ("dc", (double) m.dc);
    }
}

void LfoData::load (const Node& n)
{
    *this = LfoData();
    if (auto* c = n.child ("shape")) shape.load (*c);
    loadPoints (n, "path", path);
    loopback = n.integer ("loopback", -1);
    if (loopback >= (int) shape.points.size()) loopback = -1;
    shapeName = n.str ("name", "Default");
    for (auto* c : n.children ("bus"))
    {
        const int b = c->integer ("index", -1);
        if (b < 0 || b >= kMaxLfoBuses) continue;
        buses[b].source = (int) modSourceFromId (c->str ("source"));
        buses[b].amount = (float) c->num ("amount", 1.0);
    }
    for (auto* c : n.children ("pmod"))
    {
        LfoPointMod m;
        m.point = c->integer ("point"); m.bus = c->integer ("bus");
        m.dx = (float) c->num ("dx"); m.dy = (float) c->num ("dy"); m.dc = (float) c->num ("dc");
        if (m.point >= 0 && m.point < (int) shape.points.size() && m.bus >= 0 && m.bus < kMaxLfoBuses)
            pointMods.push_back (m);
    }
}

bool LfoData::operator== (const LfoData& o) const
{
    if (! (shape == o.shape) || loopback != o.loopback || shapeName != o.shapeName || path.size() != o.path.size()
        || pointMods.size() != o.pointMods.size())
        return false;
    for (size_t i = 0; i < path.size(); ++i)
        if (path[i].x != o.path[i].x || path[i].y != o.path[i].y || path[i].curve != o.path[i].curve || path[i].flat != o.path[i].flat)
            return false;
    for (int b = 0; b < kMaxLfoBuses; ++b)
        if (buses[b].source != o.buses[b].source || buses[b].amount != o.buses[b].amount) return false;
    for (size_t i = 0; i < pointMods.size(); ++i)
    {
        const auto& a = pointMods[i];
        const auto& c = o.pointMods[i];
        if (a.point != c.point || a.bus != c.bus || a.dx != c.dx || a.dy != c.dy || a.dc != c.dc) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// BakedLfo
// ---------------------------------------------------------------------------
namespace
{
// Bakes a path (points in drawing order) into y-by-arc-length.
void bakePath (const std::vector<CurvePoint>& pts, std::array<float, Curve::kLutSize + 1>& lut)
{
    constexpr int kSub = 32;
    std::vector<float> len, ys;
    len.reserve (pts.size() * kSub + 1);
    ys.reserve (pts.size() * kSub + 1);
    len.push_back (0.0f);
    ys.push_back (pts.front().y);
    float total = 0.0f, px = pts.front().x, py = pts.front().y;
    for (size_t i = 1; i < pts.size(); ++i)
    {
        const auto& a = pts[i - 1];
        const auto& b = pts[i];
        for (int k = 1; k <= kSub; ++k)
        {
            const float t = (float) k / (float) kSub;
            const float xx = a.x + (b.x - a.x) * t;
            const float yy = b.flat ? (k == kSub ? b.y : a.y) : a.y + (b.y - a.y) * Curve::bend (t, b.curve);
            total += std::sqrt ((xx - px) * (xx - px) + (yy - py) * (yy - py));
            len.push_back (total);
            ys.push_back (yy);
            px = xx; py = yy;
        }
    }
    if (total <= 1.0e-6f) { lut.fill (pts.front().y); return; }
    size_t j = 1;
    for (int i = 0; i <= Curve::kLutSize; ++i)
    {
        const float target = total * (float) i / (float) Curve::kLutSize;
        while (j < len.size() - 1 && len[j] < target) ++j;
        const float seg = len[j] - len[j - 1];
        const float f = seg > 1.0e-9f ? clamp01 ((target - len[j - 1]) / seg) : 1.0f;
        lut[(size_t) i] = ys[j - 1] + (ys[j] - ys[j - 1]) * f;
    }
}

// Curve::evaluate over a plain point array.
float evaluatePoints (const CurvePoint* p, int count, float x) noexcept
{
    if (x <= p[0].x) return p[0].y;
    if (x >= p[count - 1].x) return p[count - 1].y;
    int i = 1;
    while (i < count - 1 && p[i].x <= x) ++i;
    const auto& a = p[i - 1];
    const auto& b = p[i];
    if (b.flat) return a.y;
    const float w = b.x - a.x;
    if (w <= 1.0e-7f) return b.y;
    return a.y + (b.y - a.y) * Curve::bend ((x - a.x) / w, b.curve);
}
}

BakedLfo BakedLfo::build (const LfoData& d)
{
    BakedLfo b;
    d.shape.bake (b.shape.lut);
    bakePath (d.path.size() >= 2 ? d.path : d.shape.points, b.path.lut);
    b.first = d.shape.points.empty() ? 0.0f : d.shape.points.front().y;
    const int np = (int) d.shape.points.size();
    b.loopX = (d.loopback >= 0 && d.loopback < np - 1) ? d.shape.points[(size_t) d.loopback].x : -1.0f;
    if (b.loopX >= 0.999f) b.loopX = -1.0f;
    bool anyBus = false;
    for (int i = 0; i < kMaxLfoBuses; ++i) { b.buses[i] = d.buses[i]; anyBus = anyBus || d.buses[i].source >= 0; }
    if (anyBus && ! d.pointMods.empty() && np <= kMaxLfoPoints)
    {
        b.points = d.shape.points;
        for (const auto& m : d.pointMods)
            if (m.point >= 0 && m.point < np && m.bus >= 0 && m.bus < kMaxLfoBuses && d.buses[m.bus].source >= 0)
                b.mods.push_back (m);
    }
    return b;
}

float BakedLfo::evaluate (float xx, const float* src) const noexcept
{
    if (mods.empty() || src == nullptr) return shape (xx);
    float bus[kMaxLfoBuses];
    bool any = false;
    for (int i = 0; i < kMaxLfoBuses; ++i)
    {
        bus[i] = buses[i].source >= 0 ? src[buses[i].source] * buses[i].amount : 0.0f;
        any = any || bus[i] != 0.0f;
    }
    if (! any) return shape (xx);

    CurvePoint p[kMaxLfoPoints];
    const int n = (int) points.size();
    std::copy (points.begin(), points.end(), p);
    for (const auto& m : mods)
    {
        const float v = bus[m.bus];
        p[m.point].x += m.dx * v;
        p[m.point].y += m.dy * v;
        p[m.point].curve += m.dc * v;
    }
    // Keep the end points' x, keep x increasing, clamp the rest.
    p[0].x = points.front().x;
    p[n - 1].x = points.back().x;
    for (int i = 0; i < n; ++i)
    {
        if (i > 0 && i < n - 1) p[i].x = clampf (p[i].x, p[i - 1].x, 1.0f);
        p[i].y = clamp01 (p[i].y);
        p[i].curve = clampf (p[i].curve, -1.0f, 1.0f);
    }
    return evaluatePoints (p, n, xx);
}

std::shared_ptr<const LfoShapeSet> LfoShapeSet::build (const LfoData* data)
{
    auto s = std::make_shared<LfoShapeSet>();
    for (int i = 0; i < kNumLfos; ++i)
    {
        s->lfo[i] = BakedLfo::build (data[i]);
        if (! s->lfo[i].mods.empty())
            for (const auto& b : s->lfo[i].buses)
                if (b.source >= 0) s->busSources |= 1ull << b.source;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
double lfoSyncBeats (int syncIndex, bool trip, bool dot) noexcept
{
    double b = syncRateBeats (syncIndex);
    if (trip) b *= 2.0 / 3.0;
    if (dot) b *= 1.5;
    return b;
}

LfoSettings LfoSettings::read (const ParamView& p, const LfoIds& id, double bpm, float swing) noexcept
{
    LfoSettings s;
    s.type = (LfoType) std::clamp (p.integer (id.type), 0, 4);
    s.mode = (LfoMode) std::clamp (p.integer (id.mode), 0, 2);
    s.direction = (LfoDirection) std::clamp (p.integer (id.direction), 0, 2);
    s.gridX = std::max (1, p.integer (id.gridX));
    s.mono = p.on (id.mono);
    s.host = p.on (id.host);
    s.bpm = p.on (id.bpm);
    if (bpm < 1.0) bpm = 1.0;
    if (s.bpm)
    {
        s.cycleBeats = lfoSyncBeats (p.integer (id.rateSync), p.on (id.trip), p.on (id.dot));
        s.hz = (float) (bpm / 60.0 / s.cycleBeats);
        s.swing = p.on (id.swing);
        s.swingAmount = clamp01 (swing);
    }
    else
    {
        s.hz = p (id.rate) * (p.on (id.x10) ? 10.0f : 1.0f);
        s.cycleBeats = bpm / 60.0 / (double) std::max (s.hz, 1.0e-4f);
    }
    // RISE / DELAY follow the BPM / HZ switch: beats in BPM mode.
    const float k = s.bpm ? (float) (60.0 / bpm) : 1.0f;
    s.rise = p (id.rise) * k;
    s.delay = p (id.delay) * k;
    s.smooth = clamp01 (p (id.smooth));
    float ph = p (id.phase) * (1.0f / 360.0f);
    ph -= std::floor (ph);
    if (p.on (id.phaseSnap)) ph = std::round (ph * (float) s.gridX) / (float) s.gridX;
    s.phase = ph >= 1.0f ? 0.0f : ph;
    return s;
}

// ---------------------------------------------------------------------------
// Lfo
// ---------------------------------------------------------------------------
void Lfo::reset (uint32_t seed) noexcept
{
    *this = Lfo();
    rng = Rng (seed * 2654435761u + 12345u);
    chaosInit();
}

void Lfo::chaosInit() noexcept
{
    // A point near the attractor, nudged so instances differ.
    cx = -5.5f + rng.bi(); cy = -7.2f + rng.bi(); cz = 18.0f + rng.bi();
}

void Lfo::trigger (const LfoSettings& s, const BakedLfo&, const LfoClock& clk) noexcept
{
    t = 0.0;
    released = false;
    done = false;
    fresh = true;
    if (s.mode != LfoMode::Free)
    {
        pos = 0.0;
        envPos = s.phase;
        trigPpq = clk.ppq;
        shStep = -1;
        if (s.type == LfoType::Rossler) { cx = 1.0f + rng.bi(); cy = 1.0f + rng.bi(); cz = 0.5f; }
        else if (s.type == LfoType::Lorenz) chaosInit();
    }
}

float Lfo::chaosAdvance (LfoType type, double cycles) noexcept
{
    // Time units per cycle chosen for about one orbit per cycle.
    const float total = (float) cycles * (type == LfoType::Lorenz ? 0.8f : 6.0f);
    const int steps = std::clamp ((int) std::ceil (total / 0.004f), 1, 64);
    const float h = total / (float) steps;
    if (type == LfoType::Lorenz)
    {
        for (int i = 0; i < steps; ++i)
        {
            const float dx = 10.0f * (cy - cx), dy = cx * (28.0f - cz) - cy, dz = cx * cy - (8.0f / 3.0f) * cz;
            cx += h * dx; cy += h * dy; cz += h * dz;
        }
        if (! std::isfinite (cx + cy + cz) || std::abs (cx) > 100.0f) chaosInit();
        return clamp01 ((cx + 20.0f) * (1.0f / 40.0f));
    }
    for (int i = 0; i < steps; ++i)
    {
        const float dx = -cy - cz, dy = cx + 0.2f * cy, dz = 0.2f + cz * (cx - 5.7f);
        cx += h * dx; cy += h * dy; cz += h * dz;
    }
    if (! std::isfinite (cx + cy + cz) || std::abs (cx) > 100.0f || std::abs (cz) > 200.0f) { cx = 1.0f; cy = 1.0f; cz = 0.5f; }
    return clamp01 ((cx + 11.0f) * (1.0f / 23.0f));
}

float Lfo::riseDelay (const LfoSettings& s, float first, float v) const noexcept
{
    if (t < (double) s.delay) return first;
    if (s.rise > 0.0f)
    {
        const float w = (float) ((t - (double) s.delay) / (double) s.rise);
        if (w < 1.0f) return first + (v - first) * w;
    }
    return v;
}

float Lfo::process (const LfoSettings& s, const BakedLfo& b, const LfoClock& clk, int n, const float* src) noexcept
{
    const double dt = (double) n / (double) clk.sampleRate;
    const double t0 = t;
    t += dt;
    const bool retrigLike = s.mode != LfoMode::Free;

    // Running time this tick: frozen during DELAY for RETRIG / ENVELOPE.
    const double run = retrigLike ? std::max (0.0, t - std::max (t0, (double) s.delay)) : dt;
    double adv = run * (double) s.hz;
    const bool anchored = s.host && clk.playing;

    if (s.mode == LfoMode::Envelope)
    {
        if (! done)
        {
            envPos += adv;
            const double end = s.direction == LfoDirection::PingPong ? 2.0 : 1.0;
            const bool loop = b.loopX >= 0.0f && s.direction != LfoDirection::PingPong && ! released;
            if (loop && envPos >= 1.0)
            {
                const double span = std::max (1.0e-6, 1.0 - (double) b.loopX);
                envPos = (double) b.loopX + std::fmod (envPos - 1.0, span);
            }
            else if (envPos >= end) { envPos = end; done = true; }
        }
        const float e = (float) envPos;
        x = s.direction == LfoDirection::Forward ? std::min (e, 1.0f)
          : s.direction == LfoDirection::Reverse ? 1.0f - std::min (e, 1.0f)
          : (e < 1.0f ? e : 2.0f - e);
    }
    else
    {
        if (anchored && s.bpm)
        {
            const double ppqEnd = clk.ppq + (double) n * clk.bpm / (60.0 * (double) clk.sampleRate);
            double beats = ppqEnd;
            if (retrigLike) beats = std::max (0.0, ppqEnd - trigPpq - (double) s.delay * clk.bpm / 60.0);
            const double next = std::fmod (beats / s.cycleBeats, 2.0);
            adv = next - pos;     // for chaos integration
            if (adv < 0.0) adv += 2.0;
            pos = next;
        }
        else
        {
            if (anchored && ! s.bpm && s.mode == LfoMode::Free && ! wasPlaying)
                pos = std::fmod (clk.ppq * 60.0 / std::max (clk.bpm, 1.0) * (double) s.hz, 2.0);
            pos += adv;
            if (pos >= 2.0) pos = std::fmod (pos, 2.0);
        }

        double p = pos;
        if (s.bpm && s.swing && s.swingAmount > 0.0f)
        {
            const double d2 = 2.0 * (0.5 + 0.25 * (double) s.swingAmount);    // first cycle's share of the pair
            p = p < d2 ? p / d2 : 1.0 + (p - d2) / (2.0 - d2);
        }
        p += (double) s.phase;
        double f = p - std::floor (p);
        if (s.direction == LfoDirection::Reverse) f = 1.0 - f;
        else if (s.direction == LfoDirection::PingPong)
        {
            const double q = std::fmod (p, 2.0);
            f = q < 1.0 ? q : 2.0 - q;
        }
        x = (float) f;
    }
    wasPlaying = clk.playing;

    float v;
    switch (s.type)
    {
        case LfoType::Path:    v = b.path (x); break;
        case LfoType::Lorenz:
        case LfoType::Rossler: v = chaosAdvance (s.type, adv); break;
        case LfoType::SampleHold:
        {
            const int step = std::min (s.gridX - 1, (int) (x * (float) s.gridX));
            if (step != shStep) { shStep = step; shValue = rng.uni(); }
            v = shValue;
            break;
        }
        default:               v = b.evaluate (x, src); break;
    }

    v = riseDelay (s, b.first, v);

    if (fresh) { smoothed = v; fresh = false; }
    else if (s.smooth > 1.0e-4f)
    {
        const double tau = (double) s.smooth * 0.25 / std::max ((double) s.hz, 1.0e-3);
        smoothed += (v - smoothed) * (float) (1.0 - std::exp (-dt / tau));
    }
    else smoothed = v;

    prevOut = out;
    out = clamp01 (smoothed);
    return out;
}

float Lfo::follow (const LfoSettings& s, const BakedLfo& b, float shared, int n, float sampleRate) noexcept
{
    t += (double) n / (double) sampleRate;
    prevOut = out;
    out = clamp01 (riseDelay (s, b.first, shared));
    return out;
}

// ---------------------------------------------------------------------------
// Shape library
// ---------------------------------------------------------------------------
namespace LfoShapes
{
namespace
{
Curve make (std::initializer_list<CurvePoint> pts)
{
    Curve c;
    c.points.assign (pts.begin(), pts.end());
    return c;
}

template <typename F>
Curve sampled (F f, int count)
{
    std::vector<float> y ((size_t) count);
    for (int i = 0; i < count; ++i) y[(size_t) i] = clamp01 (f ((float) i / (float) count));
    return fromSamples (y, true, 0.002f, 128);
}

// 16-step gate pattern: 'x' = a gate of 3/4 step, '.' = rest.
Curve gates (const char* pattern)
{
    Curve c;
    c.points.clear();
    const int n = (int) std::strlen (pattern);
    const float w = 1.0f / (float) n;
    c.points.push_back ({ 0.0f, pattern[0] == 'x' ? 1.0f : 0.0f, 0.0f, false });
    for (int i = 0; i < n; ++i)
    {
        if (pattern[i] != 'x') continue;
        const float x0 = (float) i * w;
        if (x0 > 0.0f) c.points.push_back ({ x0, 1.0f, 0.0f, true });               // step up
        c.points.push_back ({ x0 + 0.75f * w, 0.0f, 0.0f, true });                  // hold 1, step down
    }
    c.points.push_back ({ 1.0f, pattern[0] == 'x' ? 1.0f : 0.0f, 0.0f, true });
    return c;
}

// Sidechain pump(s): `count` dips per cycle, each recovering over `rec` of its slot.
Curve pumps (int count, float floor, float hold, float rec, float bendAmt)
{
    Curve c;
    c.points.clear();
    const float w = 1.0f / (float) count;
    for (int i = 0; i < count; ++i)
    {
        const float x0 = (float) i * w;
        c.points.push_back ({ x0, floor, 0.0f, i > 0 });
        if (hold > 0.0f) c.points.push_back ({ x0 + hold * w, floor, 0.0f, false });
        c.points.push_back ({ x0 + (hold + rec) * w, 1.0f, bendAmt, false });
    }
    c.points.push_back ({ 1.0f, 1.0f, 0.0f, false });
    return c;
}
}

Curve defaultShape() { return Curve::triangle(); }

const std::vector<Folder>& factory()
{
    static const std::vector<Folder> lib = []
    {
        std::vector<Folder> f;
        f.push_back ({ "Basic", {
            { "Sine",       sampled ([] (float x) { return 0.5f - 0.5f * std::cos (kTwoPi * x); }, 512) },
            { "Triangle",   Curve::triangle() },
            { "Saw Up",     make ({ { 0, 0, 0, false }, { 1, 1, 0, false } }) },
            { "Saw Down",   make ({ { 0, 1, 0, false }, { 1, 0, 0, false } }) },
            { "Square",     make ({ { 0, 1, 0, false }, { 0.5f, 0, 0, true }, { 1, 1, 0, true } }) },
            { "Pulse 25%",  make ({ { 0, 1, 0, false }, { 0.25f, 0, 0, true }, { 1, 1, 0, true } }) },
            { "Exp Down",   make ({ { 0, 1, 0, false }, { 1, 0, 0.7f, false } }) },
            { "Stairs 4",   make ({ { 0, 0, 0, false }, { 0.25f, 0.333f, 0, true }, { 0.5f, 0.667f, 0, true },
                                    { 0.75f, 1, 0, true }, { 1, 0, 0, true } }) },
        } });
        f.push_back ({ "Rhythmic", {
            { "Straight 8ths", gates ("x.x.x.x.x.x.x.x.") },
            { "Offbeat",       gates ("..x...x...x...x.") },
            { "Tresillo",      gates ("x..x..x.x..x..x.") },
            { "Gallop",        gates ("x.xxx.xxx.xxx.xx") },
            { "Stutter",       gates ("xxxx..x.xxxx..x.") },
            { "Clave 3-2",     gates ("x..x..x...x.x...") },
            { "Sixteenths",    gates ("xxxxxxxxxxxxxxxx") },
            { "Decay Hits",    pumps (4, 1.0f, 0.0f, 0.0f, 0.0f) },
        } });
        // "Decay Hits": 4 hits falling from 1 to 0 over each quarter.
        {
            Curve& c = f.back().shapes.back().curve;
            c.points.clear();
            for (int i = 0; i < 4; ++i)
            {
                c.points.push_back ({ (float) i * 0.25f, 1.0f, 0.0f, i > 0 });
                c.points.push_back ({ (float) i * 0.25f + 0.2f, 0.0f, 0.6f, false });
            }
            c.points.push_back ({ 1.0f, 1.0f, 0.0f, true });
        }
        f.push_back ({ "Sidechain", {
            { "Pump Classic", pumps (1, 0.0f, 0.0f, 0.5f, 0.6f) },
            { "Pump Hard",    pumps (1, 0.0f, 0.1f, 0.5f, 0.3f) },
            { "Pump Soft",    pumps (1, 0.4f, 0.0f, 0.7f, 0.5f) },
            { "Pump Short",   pumps (1, 0.0f, 0.0f, 0.25f, 0.6f) },
            { "Pump Long",    pumps (1, 0.0f, 0.0f, 0.9f, 0.4f) },
            { "Pump 2x",      pumps (2, 0.0f, 0.0f, 0.6f, 0.6f) },
            { "Pump 4x",      pumps (4, 0.0f, 0.0f, 0.6f, 0.6f) },
            { "Offbeat Pump", make ({ { 0, 1, 0, false }, { 0.5f, 0, 0, true }, { 0.8f, 1, 0.6f, false }, { 1, 1, 0, false } }) },
        } });
        return f;
    }();
    return lib;
}

const Shape* find (const std::string& folder, const std::string& name)
{
    for (const auto& f : factory())
        if (f.name == folder)
            for (const auto& s : f.shapes)
                if (s.name == name) return &s;
    return nullptr;
}

Curve randomCurved (int gridX, uint32_t seed)
{
    gridX = std::clamp (gridX, 1, 64);
    Rng r (seed);
    std::vector<float> v ((size_t) gridX);
    for (auto& y : v) y = r.uni();
    Curve c;
    c.points.clear();
    const float w = 1.0f / (float) gridX;
    for (int i = 0; i < gridX; ++i)
    {
        const float a = v[(size_t) i], b = v[(size_t) ((i + 1) % gridX)];
        if (i == 0) c.points.push_back ({ 0.0f, a, 0.0f, false });
        // An S-shaped segment: ease out of a, ease into b.
        c.points.push_back ({ ((float) i + 0.5f) * w, 0.5f * (a + b), -0.5f, false });
        c.points.push_back ({ ((float) i + 1.0f) * w, b, 0.5f, false });
    }
    c.points.back().x = 1.0f;
    return c;
}

Curve randomFlat (int gridX, uint32_t seed)
{
    gridX = std::clamp (gridX, 1, 64);
    Rng r (seed);
    std::vector<float> v ((size_t) gridX);
    for (auto& y : v) y = r.uni();
    Curve c;
    c.points.clear();
    c.points.push_back ({ 0.0f, v[0], 0.0f, false });
    for (int i = 1; i < gridX; ++i) c.points.push_back ({ (float) i / (float) gridX, v[(size_t) i], 0.0f, true });
    c.points.push_back ({ 1.0f, v[0], 0.0f, true });
    return c;
}

Curve fromSamples (const std::vector<float>& ys, bool cyclic, float tol, int maxPoints)
{
    const int n = (int) ys.size();
    if (n < 2) return Curve::line (n ? ys[0] : 0.0f, n ? ys[0] : 0.0f);
    // Sample positions: i/n for cyclic data (x = 1 repeats the first), i/(n-1) otherwise.
    const int m = cyclic ? n + 1 : n;
    auto px = [&] (int i) { return cyclic ? (float) i / (float) n : (float) i / (float) (n - 1); };
    auto py = [&] (int i) { return clamp01 (ys[(size_t) (i % n)]); };

    // Ramer-Douglas-Peucker on the vertical error, iterative, with a growing
    // tolerance if the point budget is exceeded.
    std::vector<char> keep;
    for (float t = tol;; t *= 1.5f)
    {
        keep.assign ((size_t) m, 0);
        keep[0] = keep[(size_t) m - 1] = 1;
        std::vector<std::pair<int, int>> stack { { 0, m - 1 } };
        while (! stack.empty())
        {
            auto [a, b] = stack.back();
            stack.pop_back();
            float worst = 0.0f;
            int wi = -1;
            for (int i = a + 1; i < b; ++i)
            {
                const float u = (px (i) - px (a)) / (px (b) - px (a));
                const float e = std::abs (py (i) - (py (a) + (py (b) - py (a)) * u));
                if (e > worst) { worst = e; wi = i; }
            }
            if (wi >= 0 && worst > t) { keep[(size_t) wi] = 1; stack.push_back ({ a, wi }); stack.push_back ({ wi, b }); }
        }
        int count = 0;
        for (char k : keep) count += k;
        if (count <= maxPoints || t > 1.0f) break;
    }
    Curve c;
    c.points.clear();
    for (int i = 0; i < m; ++i)
        if (keep[(size_t) i]) c.points.push_back ({ px (i), py (i), 0.0f, false });
    return c;
}

Curve fromWavetableFrame (const std::vector<float>& frame, float tol, int maxPoints)
{
    float peak = 0.0f;
    for (float s : frame) peak = std::max (peak, std::abs (s));
    const float g = peak > 1.0e-6f ? 1.0f / peak : 1.0f;
    std::vector<float> y (frame.size());
    for (size_t i = 0; i < frame.size(); ++i) y[i] = 0.5f + 0.5f * frame[i] * g;
    return fromSamples (y, true, tol, maxPoints);
}

std::vector<float> toWavetableFrame (const Curve& c, int size)
{
    std::vector<float> out ((size_t) std::max (1, size));
    for (int i = 0; i < size; ++i) out[(size_t) i] = c.evaluate ((float) i / (float) size) * 2.0f - 1.0f;
    return out;
}
} // namespace LfoShapes
} // namespace tincture
