// FxEngine.cpp — the FX rack runner: graph building and lock-free swapping,
// per-module mix / level / bypass, splitter bands, telemetry; plus the
// shared DSP pieces declared in FxEngine.h (biquads, half-band design).
#include "FxEngine.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

namespace tincture
{
// ---------------------------------------------------------------------------
// fxdsp::BiquadCoefs (RBJ cookbook)
// ---------------------------------------------------------------------------
namespace fxdsp
{
namespace
{
void normalise (BiquadCoefs& c, double b0, double b1, double b2, double a0, double a1, double a2) noexcept
{
    const double ia = 1.0 / a0;
    c.b0 = (float) (b0 * ia); c.b1 = (float) (b1 * ia); c.b2 = (float) (b2 * ia);
    c.a1 = (float) (a1 * ia); c.a2 = (float) (a2 * ia);
}
struct Rbj { double w, cw, sw, alpha, A; };
Rbj rbj (float hz, float q, float db, float sr) noexcept
{
    Rbj r;
    r.w = 2.0 * 3.14159265358979323846 * clampf (hz, 1.0f, sr * 0.49f) / sr;
    r.cw = std::cos (r.w); r.sw = std::sin (r.w);
    r.alpha = r.sw / (2.0 * std::max (q, 0.025f));
    r.A = std::pow (10.0, db / 40.0);
    return r;
}
} // namespace

void BiquadCoefs::lowShelf (float hz, float q, float db, float sr) noexcept
{
    const Rbj r = rbj (hz, q, db, sr);
    const double A = r.A, s = 2.0 * std::sqrt (A) * r.alpha;
    normalise (*this, A * ((A + 1) - (A - 1) * r.cw + s), 2 * A * ((A - 1) - (A + 1) * r.cw), A * ((A + 1) - (A - 1) * r.cw - s),
               (A + 1) + (A - 1) * r.cw + s, -2 * ((A - 1) + (A + 1) * r.cw), (A + 1) + (A - 1) * r.cw - s);
}
void BiquadCoefs::highShelf (float hz, float q, float db, float sr) noexcept
{
    const Rbj r = rbj (hz, q, db, sr);
    const double A = r.A, s = 2.0 * std::sqrt (A) * r.alpha;
    normalise (*this, A * ((A + 1) + (A - 1) * r.cw + s), -2 * A * ((A - 1) + (A + 1) * r.cw), A * ((A + 1) + (A - 1) * r.cw - s),
               (A + 1) - (A - 1) * r.cw + s, 2 * ((A - 1) - (A + 1) * r.cw), (A + 1) - (A - 1) * r.cw - s);
}
void BiquadCoefs::peak (float hz, float q, float db, float sr) noexcept
{
    const Rbj r = rbj (hz, q, db, sr);
    normalise (*this, 1 + r.alpha * r.A, -2 * r.cw, 1 - r.alpha * r.A, 1 + r.alpha / r.A, -2 * r.cw, 1 - r.alpha / r.A);
}
void BiquadCoefs::lowPass (float hz, float q, float sr) noexcept
{
    const Rbj r = rbj (hz, q, 0.0f, sr);
    normalise (*this, (1 - r.cw) / 2, 1 - r.cw, (1 - r.cw) / 2, 1 + r.alpha, -2 * r.cw, 1 - r.alpha);
}
void BiquadCoefs::highPass (float hz, float q, float sr) noexcept
{
    const Rbj r = rbj (hz, q, 0.0f, sr);
    normalise (*this, (1 + r.cw) / 2, -(1 + r.cw), (1 + r.cw) / 2, 1 + r.alpha, -2 * r.cw, 1 - r.alpha);
}
float BiquadCoefs::magnitudeDb (float hz, float sr) const noexcept
{
    const double w = 2.0 * 3.14159265358979323846 * hz / sr;
    const double c1 = std::cos (w), s1 = std::sin (w), c2 = std::cos (2 * w), s2 = std::sin (2 * w);
    const double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2);
    const double dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    const double m = (nr * nr + ni * ni) / std::max (dr * dr + di * di, 1.0e-30);
    return (float) (10.0 * std::log10 (std::max (m, 1.0e-24)));
}

// Half-band all-pass coefficients (Laurent de Soras' polyphase IIR design).
const float* Halfband::coefs()
{
    static const std::array<float, kCoefs> c = []
    {
        std::array<float, kCoefs> out {};
        const double transition = 0.04;
        double k = std::tan ((1.0 - 2.0 * transition) * 3.14159265358979323846 / 4.0);
        k *= k;
        const double kksqrt = std::pow (1.0 - k * k, 0.25);
        const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
        const double e2 = e * e, e4 = e2 * e2;
        const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
        const int order = kCoefs * 2 + 1;
        for (int idx = 0; idx < kCoefs; ++idx)
        {
            const int c1 = idx + 1;
            double num = 0.0, den = 0.0;
            {
                int i = 0, j = 1;
                double term;
                do
                {
                    term = std::pow (q, (double) (i * (i + 1))) * std::sin ((i * 2 + 1) * c1 * 3.14159265358979323846 / order) * j;
                    num += term; j = -j; ++i;
                } while (std::abs (term) > 1.0e-100 && i < 100);
            }
            {
                int i = 1, j = -1;
                double term;
                do
                {
                    term = std::pow (q, (double) (i * i)) * std::cos (i * 2 * c1 * 3.14159265358979323846 / order) * j;
                    den += term; j = -j; ++i;
                } while (std::abs (term) > 1.0e-100 && i < 100);
            }
            num *= std::pow (q, 0.25);
            den += 0.5;
            const double ww = num / den, wwsq = ww * ww;
            const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
            out[(size_t) idx] = (float) ((1.0 - x) / (1.0 + x));
        }
        return out;
    }();
    return c.data();
}
} // namespace fxdsp

// ---------------------------------------------------------------------------
// Graph
// ---------------------------------------------------------------------------
struct FxEngine::Node
{
    FxType type = FxType::Utility;
    int unit = -1;
    bool bypass = false;
    bool hasMix = true, selfMix = false;
    std::shared_ptr<FxModule> proc;
    std::shared_ptr<const void> extra;
    ConvolveKey convKey;                     // Convolve: what the kernel was built from
    std::array<std::vector<Node>, 3> bands;
    std::array<bool, 3> bandBypass {};
    int numBands = 0;
};

struct FxEngine::Graph
{
    std::array<std::vector<Node>, kNumFxRacks> racks;
    std::shared_ptr<const FxLayout> layout;
};

namespace
{
std::unique_ptr<FxModule> create (FxType t)
{
    switch (t)
    {
        case FxType::Bode:        return makeBode();
        case FxType::Chorus:      return makeChorus();
        case FxType::Compressor:  return makeCompressor();
        case FxType::Convolve:    return makeConvolve();
        case FxType::Delay:       return makeDelay();
        case FxType::Distortion:  return makeDistortion();
        case FxType::Equalizer:   return makeEqualizer();
        case FxType::Filter:      return makeFilter();
        case FxType::Flanger:     return makeFlanger();
        case FxType::Hyper:       return makeHyper();
        case FxType::Phaser:      return makePhaser();
        case FxType::Reverb:      return makeReverb();
        case FxType::SplitterLH:
        case FxType::SplitterLMH:
        case FxType::SplitterMS:  return makeSplitter (t);
        case FxType::Utility:     return makeUtility();
    }
    return makeUtility();
}
} // namespace

bool ConvolveKey::operator== (const ConvolveKey& o) const noexcept
{
    if (irId != o.irId || irIndex != o.irIndex) return false;
    for (int i = 0; i < 6; ++i) if (k[i] != o.k[i]) return false;
    return true;
}

FxEngine::FxEngine()
{
    layout = std::make_shared<FxLayout>();
    for (auto& s : seenType) s.store (-1);
}

FxEngine::~FxEngine() = default;

void FxEngine::prepare (const Context& c, int mb)
{
    ctx = c;
    maxBlock = std::max (mb, 1);
    for (auto& d : dry) for (auto& ch : d) ch.assign ((size_t) maxBlock, 0.0f);
    publish (build (layout, true));
    // Audio is stopped: adopt it now.
    active = latest.load();
    inUse.store (active);
    collectGarbage();
}

void FxEngine::setLayout (std::shared_ptr<const FxLayout> l)
{
    if (! l) l = std::make_shared<FxLayout>();
    layout = std::move (l);
    publish (build (layout, false));
    collectGarbage();
}

FxEngine::Graph* FxEngine::build (const std::shared_ptr<const FxLayout>& lay, bool fresh)
{
    // Existing processors by unit (message thread owns `latest`).
    struct Old { FxType type; std::shared_ptr<FxModule> proc; std::shared_ptr<const void> extra; ConvolveKey key; };
    std::map<int, Old> old;
    if (Graph* g = latest.load(); g && ! fresh)
    {
        std::function<void (const std::vector<Node>&)> scan = [&] (const std::vector<Node>& l)
        {
            for (auto& n : l)
            {
                old[n.unit] = Old { n.type, n.proc, n.extra, n.convKey };
                for (auto& b : n.bands) scan (b);
            }
        };
        for (auto& r : g->racks) scan (r);
    }

    auto g = std::make_unique<Graph>();
    g->layout = lay;
    const double sr = ctx.sampleRate;
    std::function<Node (const FxModuleSlot&)> make = [&] (const FxModuleSlot& s) -> Node
    {
        Node n;
        n.type = s.type;
        n.unit = s.unit;
        n.bypass = s.bypass;
        const auto& info = fxInfo (s.type);
        n.hasMix = info.hasMix;
        auto it = old.find (s.unit);
        const bool reuse = it != old.end() && it->second.type == s.type;
        if (s.type == FxType::Distortion)
            n.extra = bakeDistortion (s);
        int kernelLen = 0;
        if (s.type == FxType::Convolve)
        {
            float v[kFxParams];
            const auto d = fxDefaults (s.type);
            const bool known = seenType[s.unit].load() == (int) FxType::Convolve;
            for (int k = 0; k < kFxParams; ++k) v[k] = known ? seen[s.unit][k].load() : d[(size_t) k];
            n.convKey = convolveKey (s, v);
            if (reuse && it->second.extra && it->second.key == n.convKey)
                n.extra = it->second.extra;
            else
                n.extra = bakeConvolve (s, n.convKey, sr, kernelLen);
        }
        // A reused Convolve must be able to hold the new kernel.
        const bool fits = ! reuse || s.type != FxType::Convolve || it->second.proc->kernelCapacity() >= kernelLen;
        if (reuse && fits)
            n.proc = it->second.proc;
        else
        {
            std::shared_ptr<FxModule> p = create (s.type);
            p->reserve (kernelLen);
            p->prepare (sr, maxBlock);
            p->reset();
            n.proc = std::move (p);
        }
        n.selfMix = n.proc->mixesItself();
        if (info.splitter)
        {
            n.numBands = info.numBands;
            for (int b = 0; b < info.numBands; ++b)
            {
                n.bandBypass[(size_t) b] = s.bandBypass[(size_t) b];
                for (auto& c : s.bands[(size_t) b])
                    if (! fxInfo (c.type).splitter && c.unit >= 0)
                        n.bands[(size_t) b].push_back (make (c));
            }
        }
        return n;
    };
    for (int r = 0; r < kNumFxRacks; ++r)
        for (auto& s : lay->racks[(size_t) r].modules)
            if (s.unit >= 0 && s.unit < kNumFxUnits)
                g->racks[(size_t) r].push_back (make (s));

    // Mark units' types (for Convolve key reading) — units that changed type
    // forget what the audio thread last saw.
    std::array<int, kNumFxUnits> types;
    types.fill (-1);
    lay->forEach ([&] (const FxModuleSlot& m, const FxPath&) { if (m.unit >= 0 && m.unit < kNumFxUnits) types[(size_t) m.unit] = (int) m.type; });
    for (int u = 0; u < kNumFxUnits; ++u)
        if (seenType[u].load() != types[(size_t) u]) seenType[u].store (-2 - types[(size_t) u]);   // pending: audio confirms

    Graph* raw = g.get();
    graphs.push_back (std::move (g));
    return raw;
}

void FxEngine::publish (Graph* g)
{
    latest.store (g);
}

void FxEngine::collectGarbage()
{
    Graph* keepA = latest.load();
    Graph* keepB = inUse.load();
    graphs.erase (std::remove_if (graphs.begin(), graphs.end(),
                                  [&] (const std::unique_ptr<Graph>& g) { return g.get() != keepA && g.get() != keepB; }),
                  graphs.end());
}

void FxEngine::idle()
{
    collectGarbage();
    Graph* g = latest.load();
    if (! g) return;
    // Convolve: rebuild kernels whose shaping knobs moved.
    bool changed = false;
    std::function<void (const std::vector<Node>&)> scan = [&] (const std::vector<Node>& l)
    {
        for (auto& n : l)
        {
            if (n.type == FxType::Convolve && seenType[n.unit].load() == (int) FxType::Convolve)
            {
                float v[kFxParams];
                for (int k = 0; k < kFxParams; ++k) v[k] = seen[n.unit][k].load();
                FxPath p;
                if (g->layout->findUnit (n.unit, p))
                    if (auto* s = g->layout->get (p))
                        if (! (convolveKey (*s, v) == n.convKey)) changed = true;
            }
            for (auto& b : n.bands) scan (b);
        }
    };
    for (auto& r : g->racks) scan (r);
    if (changed)
    {
        publish (build (layout, false));
        collectGarbage();
    }
}

void FxEngine::acquire() noexcept
{
    Graph* g;
    do
    {
        g = latest.load();
        inUse.store (g);
    } while (g != latest.load());
    active = g;
}

void FxEngine::beginBlock (const Context& c)
{
    ctx.bpm = c.bpm;
    ctx.ppq = c.ppq;
    ctx.playing = c.playing;
    acquire();
    // Latency of the MAIN rack (top-level modules; splitter bands: the max).
    int lat = 0;
    if (active)
        for (auto& n : active->racks[kFxMain])
        {
            if (n.bypass) continue;
            int l = n.proc->latency();
            for (int b = 0; b < n.numBands; ++b)
            {
                int bl = 0;
                for (auto& c2 : n.bands[(size_t) b]) if (! c2.bypass) bl += c2.proc->latency();
                l = std::max (l, bl);
            }
            lat += l;
        }
    mainLatency.store (lat, std::memory_order_relaxed);
}

namespace
{
void resetChain (const std::vector<FxEngine::Node>& l);
}

void FxEngine::reset()
{
    if (! active) return;
    for (auto& r : active->racks) resetChain (r);
}

namespace
{
void resetChain (const std::vector<FxEngine::Node>& l)
{
    for (auto& n : l)
    {
        n.proc->reset();
        n.proc->bypassGain = n.bypass ? 0.0f : 1.0f;
        for (auto& b : n.bands) resetChain (b);
    }
}
} // namespace

void FxEngine::process (int rack, float* L, float* R, int n, const ParamView& p)
{
    if (! active) acquire();
    if (! active || rack < 0 || rack >= kNumFxRacks) return;
    for (int off = 0; off < n; off += maxBlock)
    {
        const int m = std::min (maxBlock, n - off);
        runChain (active->racks[(size_t) rack], L + off, R + off, m, p, 0);
    }
    float pl = 0.0f, pr = 0.0f;
    for (int i = 0; i < n; ++i) { pl = std::max (pl, std::abs (L[i])); pr = std::max (pr, std::abs (R[i])); }
    if (pl > peaks[rack][0].load (std::memory_order_relaxed)) peaks[rack][0].store (pl, std::memory_order_relaxed);
    if (pr > peaks[rack][1].load (std::memory_order_relaxed)) peaks[rack][1].store (pr, std::memory_order_relaxed);
}

void FxEngine::runChain (const std::vector<Node>& chain, float* L, float* R, int n, const ParamView& p, int depth)
{
    const auto& id = ids();
    float* dL = dry[depth][0].data();
    float* dR = dry[depth][1].data();
    for (const Node& node : chain)
    {
        FxModule& m = *node.proc;
        const auto& u = id.fx[node.unit];
        float v[kFxParams];
        for (int k = 0; k < kFxParams; ++k)
        {
            v[k] = clamp01 (p (u.p[k]));
            seen[node.unit][k].store (v[k], std::memory_order_relaxed);
        }
        const int st = seenType[node.unit].load (std::memory_order_relaxed);
        if (st <= -2) seenType[node.unit].store (-2 - st, std::memory_order_relaxed);
        const float mix = clamp01 (p (u.mix));

        // Bypass crossfade, ~5 ms.
        const float target = node.bypass ? 0.0f : 1.0f;
        if (m.bypassGain <= 0.0f && target == 0.0f)
        {
            if (! m.cleared) { m.reset(); m.cleared = true; }
            continue;
        }
        m.cleared = false;
        const float bgStep = (float) n / (0.005f * (float) ctx.sampleRate);
        const float bg0 = m.bypassGain;
        const float bg1 = target > bg0 ? std::min (1.0f, bg0 + bgStep) : (target < bg0 ? std::max (0.0f, bg0 - bgStep) : bg0);
        m.bypassGain = bg1;

        const float level = levelGain (v[kFxLevelSlot]);
        const float mixUse = (node.hasMix && ! node.selfMix) ? mix : 1.0f;
        const float mix0 = m.prevMix < 0.0f ? mixUse : m.prevMix;
        const float lvl0 = m.prevLevel < 0.0f ? level : m.prevLevel;
        m.prevMix = mixUse;
        m.prevLevel = level;
        const bool fading = bg0 < 1.0f || bg1 < 1.0f;
        const bool needDry = fading || mix0 < 1.0f || mixUse < 1.0f;
        if (needDry)
        {
            std::copy (L, L + n, dL);
            std::copy (R, R + n, dR);
        }

        FxBlock b;
        b.v = v; b.mix = mix; b.ctx = &ctx; b.note = lastNote; b.noteCount = noteCount;
        b.extra = node.extra.get(); b.telemetry = &telemetry[node.unit];

        if (FxSplitter* sp = m.splitter())
        {
            sp->split (L, R, n, b);
            for (int bi = 0; bi < node.numBands; ++bi)
                if (! node.bandBypass[(size_t) bi] && depth == 0)
                    runChain (node.bands[(size_t) bi], sp->bandL (bi), sp->bandR (bi), n, p, 1);
            sp->join (L, R, n);
        }
        else
            m.process (L, R, n, b);

        // Mix, level and bypass fade, ramped across the block.
        const float inv = 1.0f / (float) n;
        if (! needDry)
        {
            if (lvl0 != 1.0f || level != 1.0f)
            {
                const float dl = (level - lvl0) * inv;
                for (int i = 0; i < n; ++i)
                {
                    const float g = lvl0 + dl * (float) (i + 1);
                    L[i] *= g; R[i] *= g;
                }
            }
        }
        else
        {
            const float dl = (level - lvl0) * inv, dm = (mixUse - mix0) * inv, db = (bg1 - bg0) * inv;
            for (int i = 0; i < n; ++i)
            {
                const float t = (float) (i + 1);
                const float g = lvl0 + dl * t, mx = mix0 + dm * t, bgv = bg0 + db * t;
                const float wl = (dL[i] + (L[i] - dL[i]) * mx) * g;
                const float wr = (dR[i] + (R[i] - dR[i]) * mx) * g;
                L[i] = dL[i] + (wl - dL[i]) * bgv;
                R[i] = dR[i] + (wr - dR[i]) * bgv;
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Telemetry and display helpers
// ---------------------------------------------------------------------------
bool FxEngine::gainReduction (int unit, float out[3]) const
{
    if (unit < 0 || unit >= kNumFxUnits || seenType[unit].load() != (int) FxType::Compressor) return false;
    for (int i = 0; i < 3; ++i) out[i] = telemetry[unit].gainReductionDb[i].load (std::memory_order_relaxed);
    return true;
}

float FxEngine::rackPeak (int rack, int ch)
{
    if (rack < 0 || rack >= kNumFxRacks || ch < 0 || ch > 1) return 0.0f;
    return peaks[rack][ch].exchange (0.0f);
}

float FxEngine::magnitudeDb (FxType t, const float* v, float hz, double sr)
{
    switch (t)
    {
        case FxType::Equalizer:  return equalizerMagnitudeDb (v, hz, sr);
        case FxType::Filter:     return filterMagnitudeDb (v, hz, sr);
        case FxType::Distortion: return distortionFilterMagnitudeDb (v, hz, sr);
        case FxType::Delay:      return delayFilterMagnitudeDb (v, hz, sr);
        default:                 return 0.0f;
    }
}

float FxEngine::distortionTransfer (const float* v, const FxModuleSlot* slot, float x)
{
    return distortionShape (v, nullptr, slot, x);
}
} // namespace tincture
