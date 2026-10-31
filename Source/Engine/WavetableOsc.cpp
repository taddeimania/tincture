// WavetableOsc.cpp — wavetable oscillator rendering.
//
// Per tick and per unison voice the work is done in passes over plain float
// buffers (n samples, or n*os when oversampling):
//   1. phase: a 32-bit accumulator per voice (exact, wraps for free) turned
//      into a 0..1 read phase; FM changes the increment or offsets the phase.
//   2. phase warps of both slots in slot order (Sync, Alt Warp modes, PD),
//      each a specialised loop chosen once per tick.
//   3. table read: linear interpolation in the mip level picked for this
//      voice's read rate, crossfading two frames for WT POS.
//   4. post warps (filter, distortion, AM, RM) through warp::processPost.
//   5. mix into L/R with gains ramped across the tick.
// Then the oversampled path decimates L/R once per oscillator voice.
#include "WavetableOsc.h"

#include <cmath>

namespace tincture
{
namespace
{
constexpr float kInv24 = 1.0f / 16777216.0f;
constexpr float kTwo32 = 4294967296.0f;

inline uint32_t toPhase (double cycles) noexcept { return (uint32_t) (int64_t) (cycles * 4294967296.0); }
// NaN-safe clamp (NaN -> hi): keeps garbage source audio from producing a bad table index.
inline float safeClamp (float x, float lo, float hi) noexcept { return std::max (lo, std::min (hi, x)); }

// Schlick's bias curve u -> u / (a (1 - u) + 1), a = 1/b - 2. b = 0.5 is the identity.
inline float biasParam (float b) noexcept { return 1.0f / b - 2.0f; }
inline float biasSlope (float a) noexcept { return std::max (a + 1.0f, 1.0f / (a + 1.0f)); }
// Bipolar amount -1..1 -> bias parameter: +1 = slow start (bends right), -1 = fast start.
inline float biasFromBipolar (float s) noexcept { return biasParam (0.5f - 0.45f * clampf (s, -1.0f, 1.0f)); }

bool modeNeedsOversampling (int m) noexcept
{
    using namespace warp;
    switch (kindOf (m))
    {
        case Kind::Sync: case Kind::Distortion: case Kind::Fm: case Kind::Pd: case Kind::Am: case Kind::Rm: return true;
        case Kind::Phase: return m == Flip || m == Quantize || (m >= Remap1 && m <= Remap4);
        default: return false;
    }
}

// ---- Unison helpers ---------------------------------------------------------
float detuneShape (int mode, float t, float rnd) noexcept
{
    const float a = std::abs (t), s = t < 0.0f ? -1.0f : 1.0f;
    switch (mode)
    {
        case 1:  return t * (0.35f + 0.65f * a);                      // Super: dense centre
        case 2:  return s * (fm::exp2 (4.0f * a) - 1.0f) * (1.0f / 15.0f);    // Exp
        case 3:  return s * (1.0f - (1.0f - a) * (1.0f - a));         // Inv: wide near the centre
        case 4:  return rnd;                                          // Random (per note)
        default: return t;                                            // Linear
    }
}

float stackOffset (int stack, int u, int count) noexcept
{
    switch (stack)
    {
        case 1: { static const float o[4] { 0, 12, 24, 36 }; return o[u % std::min (4, std::max (1, count))]; }
        case 2: { static const float o[7] { 0, 7, 12, 19, 24, 31, 36 }; return o[u % 7]; }
        case 3: case 4:
        {
            const bool centre = (count & 1) ? u == count / 2 : (u == count / 2 - 1 || u == count / 2);
            return centre && count > 1 ? (stack == 3 ? -12.0f : -24.0f) : 0.0f;
        }
        default: return 0.0f;
    }
}

// ---- Phase warps (in place on the 0..1 read phase) ------------------------
struct WarpCtx
{
    float* ph;
    float* gain;
    bool* gainUsed;
    int n;
};

inline void useGain (WarpCtx& c) noexcept
{
    if (! *c.gainUsed)
    {
        for (int i = 0; i < c.n; ++i) c.gain[i] = 1.0f;
        *c.gainUsed = true;
    }
}

void warpSync (WarpCtx& c, float ratio, float var) noexcept
{
    float* ph = c.ph;
    if (var > 0.001f)
    {
        // Soft sync: smooth window over the reset (both cycle edges).
        useGain (c);
        const float iw = 2.0f / var;
        for (int i = 0; i < c.n; ++i)
        {
            const float x = ph[i];
            const float d = std::min (std::min (x, 1.0f - x) * iw, 1.0f);
            c.gain[i] *= d * d * (3.0f - 2.0f * d);
            const float y = x * ratio;
            ph[i] = y - (float) (int) y;
        }
    }
    else
    {
        for (int i = 0; i < c.n; ++i)
        {
            const float y = ph[i] * ratio;
            ph[i] = y - (float) (int) y;
        }
    }
}

// Each half bent toward the centre (a > 0) or the edges (a < 0).
void warpBendHalves (WarpCtx& c, float a) noexcept
{
    float* ph = c.ph;
    for (int i = 0; i < c.n; ++i)
    {
        const float x = ph[i];
        const bool lo = x < 0.5f;
        const float u = lo ? 2.0f * x : 2.0f - 2.0f * x;
        const float y = 0.5f * u / (a * (1.0f - u) + 1.0f);
        ph[i] = lo ? y : 1.0f - y;
    }
}

void warpAsym (WarpCtx& c, float a) noexcept
{
    float* ph = c.ph;
    for (int i = 0; i < c.n; ++i) ph[i] = ph[i] / (a * (1.0f - ph[i]) + 1.0f);
}

void warpPwm (WarpCtx& c, float s) noexcept
{
    float* ph = c.ph;
    for (int i = 0; i < c.n; ++i) ph[i] = std::min (ph[i] * s, 1.0f);
}

void warpFlip (WarpCtx& c, float point) noexcept
{
    useGain (c);
    for (int i = 0; i < c.n; ++i) c.gain[i] *= c.ph[i] >= point ? -1.0f : 1.0f;
}

// Whole table forward in the first half, backward in the second, each half bent.
void warpMirror (WarpCtx& c, float a) noexcept
{
    float* ph = c.ph;
    for (int i = 0; i < c.n; ++i)
    {
        const float x = ph[i];
        const float u = x < 0.5f ? 2.0f * x : 2.0f - 2.0f * x;
        ph[i] = u / (a * (1.0f - u) + 1.0f);
    }
}

void warpRemap1 (WarpCtx& c, const BakedCurve& lut, float amt) noexcept
{
    float* ph = c.ph;
    for (int i = 0; i < c.n; ++i) ph[i] += (lut (ph[i]) - ph[i]) * amt;
}

// The graph applied to each of `segments` parts, every other one mirrored.
void warpRemapSegments (WarpCtx& c, const BakedCurve& lut, float amt, int segments) noexcept
{
    float* ph = c.ph;
    const float fs = (float) segments, inv = 1.0f / fs;
    for (int i = 0; i < c.n; ++i)
    {
        const float q = ph[i] * fs;
        const int k = std::min ((int) q, segments - 1);
        const float u = q - (float) k;
        if (k & 1)
        {
            const float v = 1.0f - u;
            ph[i] = ((float) k + 1.0f - (v + (lut (v) - v) * amt)) * inv;
        }
        else
        {
            ph[i] = ((float) k + u + (lut (u) - u) * amt) * inv;
        }
    }
}

void warpRemap3 (WarpCtx& c, float amt) noexcept
{
    // Sinusoidal ease: 0.5 - 0.5 cos (pi x) = sin^2 (pi x / 2).
    float* ph = c.ph;
    for (int i = 0; i < c.n; ++i)
    {
        const float s = fm::sin2pi (ph[i] * 0.25f);
        ph[i] += (s * s - ph[i]) * amt;
    }
}

void warpQuantize (WarpCtx& c, float steps) noexcept
{
    float* ph = c.ph;
    const float inv = 1.0f / steps;
    for (int i = 0; i < c.n; ++i) ph[i] = (float) (int) (ph[i] * steps) * inv;
}

void warpPd (WarpCtx& c, const float* src, float k) noexcept
{
    float* ph = c.ph;
    for (int i = 0; i < c.n; ++i) ph[i] = fm::wrap01 (ph[i] + k * safeClamp (src[i], -8.0f, 8.0f));
}

// ---- Table reads -------------------------------------------------------------
void readSingle (const float* f, float L, const float* ph, float* out, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float x = ph[i] * L;
        const int j = (int) x;
        const float fr = x - (float) j;
        const float a = f[j];
        out[i] = a + (f[j + 1] - a) * fr;
    }
}

void readPair (const float* fa, const float* fb, float L, float t, float dt, const float* ph, float* out, int n) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float x = ph[i] * L;
        const int j = (int) x;
        const float fr = x - (float) j;
        const float a0 = fa[j], b0 = fb[j];
        const float a = a0 + (fa[j + 1] - a0) * fr;
        const float b = b0 + (fb[j + 1] - b0) * fr;
        out[i] = a + (b - a) * (t + dt * (float) i);
    }
}

// PD (Self): the read phase is offset by the previous output sample.
float readSelf (const float* fa, const float* fb, float L, float t, float dt, const float* ph, float* out, int n,
                float k, float y) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float x = fm::wrap01 (ph[i] + k * y) * L;
        const int j = (int) x;
        const float fr = x - (float) j;
        const float a0 = fa[j], b0 = fb[j];
        const float a = a0 + (fa[j + 1] - a0) * fr;
        const float b = b0 + (fb[j + 1] - b0) * fr;
        y = a + (b - a) * (t + dt * (float) i);
        out[i] = y;
    }
    return y;
}

// ---- Half-band decimator design (Valenzuela & Constantinides polyphase IIR) --
double ipow (double x, int n) { double r = 1.0; while (n-- > 0) r *= x; return r; }

void designHalfband (double* coef, int count, double transition)
{
    const double pi = 3.14159265358979323846;
    double k = std::tan ((1.0 - transition * 2.0) * pi / 4.0);
    k *= k;
    const double kksqrt = std::pow (1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    const double e4 = e * e * e * e;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));
    const int order = count * 2 + 1;
    for (int index = 0; index < count; ++index)
    {
        const int c = index + 1;
        double num = 0.0, den = 0.0, term;
        int i = 0, j = 1;
        do { term = ipow (q, i * (i + 1)) * std::sin ((i * 2 + 1) * c * pi / order) * j; num += term; j = -j; ++i; }
        while (std::abs (term) > 1.0e-100 && i < 100);
        i = 1; j = -1;
        do { term = ipow (q, i * i) * std::cos (i * 2 * c * pi / order) * j; den += term; j = -j; ++i; }
        while (std::abs (term) > 1.0e-100 && i < 100);
        num *= std::pow (q, 0.25);
        den += 0.5;
        const double ww = num / den, wwsq = ww * ww;
        const double x = std::sqrt ((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        coef[index] = (1.0 - x) / (1.0 + x);
    }
}
} // namespace

// ---------------------------------------------------------------------------
// Decimator2x
// ---------------------------------------------------------------------------
const float* Decimator2x::coefficients() noexcept
{
    static const std::array<float, kCoefs> c = []
    {
        double d[kCoefs];
        designHalfband (d, kCoefs, 0.04);
        std::array<float, kCoefs> f {};
        for (int i = 0; i < kCoefs; ++i) f[(size_t) i] = (float) d[i];
        return f;
    }();
    return c.data();
}

void Decimator2x::process (const float* in, float* out, int n) noexcept
{
    const float* c = coefficients();
    float x0 = x[0], x1 = x[1], x2 = x[2], x3 = x[3], x4 = x[4], x5 = x[5], x6 = x[6], x7 = x[7];
    float y0 = y[0], y1 = y[1], y2 = y[2], y3 = y[3], y4 = y[4], y5 = y[5], y6 = y[6], y7 = y[7];
    for (int m = 0; m < n; ++m)
    {
        // Path 0 (even coefficients) takes the later sample, path 1 the earlier.
        float a = in[2 * m + 1], b = in[2 * m];
        float t;
        t = (a - y0) * c[0] + x0; x0 = a; y0 = t; a = t;
        t = (b - y1) * c[1] + x1; x1 = b; y1 = t; b = t;
        t = (a - y2) * c[2] + x2; x2 = a; y2 = t; a = t;
        t = (b - y3) * c[3] + x3; x3 = b; y3 = t; b = t;
        t = (a - y4) * c[4] + x4; x4 = a; y4 = t; a = t;
        t = (b - y5) * c[5] + x5; x5 = b; y5 = t; b = t;
        t = (a - y6) * c[6] + x6; x6 = a; y6 = t; a = t;
        t = (b - y7) * c[7] + x7; x7 = b; y7 = t; b = t;
        out[m] = 0.5f * (a + b);
    }
    auto snap = [] (float v) { return std::abs (v) < 1.0e-20f ? 0.0f : v; };
    x[0] = snap (x0); x[1] = snap (x1); x[2] = snap (x2); x[3] = snap (x3); x[4] = snap (x4); x[5] = snap (x5); x[6] = snap (x6); x[7] = snap (x7);
    y[0] = snap (y0); y[1] = snap (y1); y[2] = snap (y2); y[3] = snap (y3); y[4] = snap (y4); y[5] = snap (y5); y[6] = snap (y6); y[7] = snap (y7);
}

// ---------------------------------------------------------------------------
// WavetableOscData
// ---------------------------------------------------------------------------
void WavetableOscData::bakeRemaps()
{
    for (int s = 0; s < 2; ++s)
    {
        remapLut[s] = BakedCurve (remap[s]);
        float m = 0.0f;
        for (int i = 0; i < Curve::kLutSize; ++i)
            m = std::max (m, std::abs (remapLut[s].lut[(size_t) i + 1] - remapLut[s].lut[(size_t) i]) * (float) Curve::kLutSize);
        remapSlope[s] = std::max (1.0f, m);
    }
}

void WavetableOscData::save (Node& n) const
{
    if (table)
    {
        Node t ("table");
        if (table->factory) t.set ("factory", 1.0).set ("category", table->category).set ("name", table->name);
        else table->save (t);
        n.add (std::move (t));
    }
    for (int s = 0; s < 2; ++s)
    {
        Node c ("remap");
        c.set ("slot", (double) s);
        remap[s].save (c);
        n.add (std::move (c));
    }
}

void WavetableOscData::load (const Node& n)
{
    table.reset();
    if (const Node* t = n.child ("table"))
    {
        if (t->flag ("factory")) table = FactoryTables::get (t->str ("category"), t->str ("name"));
        else if (t->floats ("data") != nullptr) table = Wavetable::load (*t);
    }
    if (! table) table = FactoryTables::defaultTable();
    remap[0] = remap[1] = Curve();
    for (const Node* c : n.children ("remap"))
    {
        const int s = c->integer ("slot", 0);
        if (s >= 0 && s < 2) remap[s].load (*c);
    }
    bakeRemaps();
}

// ---------------------------------------------------------------------------
// WavetableOsc
// ---------------------------------------------------------------------------
void WavetableOsc::prepare (const Context& ctx)
{
    sampleRate = (float) ctx.sampleRate;
    (void) Decimator2x::coefficients();
    reset();
}

void WavetableOsc::reset() noexcept
{
    active = false;
    gainsValid = false;
    numUni = 1;
    for (int u = 0; u < kMaxUnison; ++u)
    {
        acc[u] = 0; fb[u] = 0.0f; gL[u] = gR[u] = 0.0f;
        post[0][u].reset(); post[1][u].reset();
    }
    lastSrc[0] = lastSrc[1] = 0.0f;
    for (auto& s : dec) for (auto& d : s) d.reset();
}

void WavetableOsc::noteOn (const NoteInfo& note, const OscTick& tick, const OscIds& ids, const WavetableOscData&, Shared& sh)
{
    const ParamView& p = *tick.p;
    const bool legatoOn = p.on (tincture::ids().global.legato);
    // Phase reset rules (spec 3.5). Default: a stolen voice in non-legato mode
    // and a legato re-pitch keep their phase. Phase Legato inverts this.
    bool keep = p.on (ids.phaseLegato) ? legatoOn : ((! legatoOn && note.stolen) || note.legato);
    keep = keep && active;

    stamp = ++sh.counter;
    sh.newest = stamp;
    rng = Rng ((uint32_t) note.unisonSeed * 2654435761u ^ ((uint32_t) note.voiceIndex * 40503u + 0x51ED27u));
    for (int u = 0; u < kMaxUnison; ++u) randDetune[u] = rng.bi();
    const int U = std::clamp (p.integer (ids.unison), 1, kMaxUnison);

    if (! keep)
    {
        const int mem = p.integer (ids.phaseMem);
        sh.lastMemMode = mem;   // only a mode change while notes sound captures phases
        const double start = (double) p (ids.phase) / 360.0;
        const float rnd = clamp01 (p (ids.rand));
        uint32_t fresh[kMaxUnison];
        for (int u = 0; u < kMaxUnison; ++u)
        {
            const double x = start + (double) (rnd * rng.uni());
            fresh[u] = toPhase (x - std::floor (x));
        }
        if (mem == 1 && sh.phaseValid)
            std::copy (sh.phase, sh.phase + kMaxUnison, acc);
        else if (mem == 2)
        {
            if (! sh.storedValid) { std::copy (fresh, fresh + kMaxUnison, sh.stored); sh.storedValid = true; }
            std::copy (sh.stored, sh.stored + kMaxUnison, acc);
        }
        else
            std::copy (fresh, fresh + kMaxUnison, acc);

        for (int u = 0; u < kMaxUnison; ++u) { fb[u] = 0.0f; post[0][u].reset(); post[1][u].reset(); }
        lastSrc[0] = lastSrc[1] = 0.0f;
        for (auto& s : dec) for (auto& d : s) d.reset();
        gainsValid = false;
    }
    else if (U > numUni)
    {
        for (int u = numUni; u < U; ++u) { gL[u] = gR[u] = 0.0f; acc[u] = rng.next(); }
    }
    numUni = U;
    active = true;
}

void WavetableOsc::render (OscTick& tick, const OscIds& ids, const WavetableOscData& data, Shared& sh)
{
    using namespace warp;
    const int n = std::clamp (tick.n, 0, kTick);
    const Wavetable* wt = data.table.get();
    if (wt == nullptr || n == 0 || tick.p == nullptr)
    {
        for (int i = 0; i < n; ++i) { tick.outL[i] = tick.outR[i] = 0.0f; if (tick.outMono) tick.outMono[i] = 0.0f; }
        tick.activeUnison = 0;
        return;
    }
    const ParamView& p = *tick.p;
    const float sr = tick.ctx != nullptr ? (float) tick.ctx->sampleRate : sampleRate;
    const int ctxOs = tick.ctx != nullptr ? tick.ctx->oversample : 1;

    // ---- Warp slots ------------------------------------------------------
    struct Slot { int mode; Kind kind; float amt, spread; const float* src; };
    Slot slot[2] {
        { p.integer (ids.warp1Mode), Kind::None, clamp01 (p (ids.warp1)), p (ids.uniWarp1), nullptr },
        { p.integer (ids.warp2Mode), Kind::None, clamp01 (p (ids.warp2)), p (ids.uniWarp2), nullptr } };
    bool wantOs = false;
    for (auto& s : slot)
    {
        s.kind = kindOf (s.mode);
        wantOs = wantOs || modeNeedsOversampling (s.mode);
    }
    const int os = (ctxOs > 1 && wantOs) ? (ctxOs >= 4 ? 4 : 2) : 1;
    if (os != lastOs)
    {
        for (auto& st : dec) for (auto& d : st) d.reset();
        lastOs = os;
    }
    const int N = n * os;
    const float srOs = sr * (float) os;

    // Source audio for FM / PD / AM / RM, upsampled (linear) when oversampling.
    for (int s = 0; s < 2; ++s)
    {
        const int si = sourceOf (slot[s].mode);
        const float* b = si >= 0 ? tick.src[si] : nullptr;
        if (b == nullptr || os == 1) { slot[s].src = b; continue; }
        float* up = sh.srcUp[s];
        float prev = lastSrc[s];
        const float inv = 1.0f / (float) os;
        for (int i = 0; i < n; ++i)
        {
            const float cur = b[i];
            for (int j = 0; j < os; ++j) up[i * os + j] = prev + (cur - prev) * (float) (j + 1) * inv;
            prev = cur;
        }
        lastSrc[s] = prev;
        slot[s].src = up;
    }

    // ---- Pitch -----------------------------------------------------------
    const bool track = p.on (ids.pitchTrack);
    const float base0 = track ? tick.pitchStart : tick.bendSemis;   // tracking off: MIDI 0 (+ bend)
    const float base1 = track ? tick.pitchEnd : tick.bendSemis;
    const int pitchMode = p.integer (ids.pitchMode);
    const int sem = p.integer (ids.sem);
    float offs = 12.0f * (float) p.integer (ids.oct) + 0.01f * p (ids.fine) + p (ids.coarse);
    float mult = 1.0f;
    if (pitchMode == 1) mult = sem >= 0 ? (float) (sem + 1) : 1.0f / (float) (1 - sem);   // Harmonics
    else if (pitchMode == 2) mult = std::max (0.0f, p (ids.ratio));                        // Ratio
    else offs += (float) sem;
    const float hz0 = noteToHz (base0 + offs) * mult, hz1 = noteToHz (base1 + offs) * mult;

    // ---- Unison layout ---------------------------------------------------
    const int U = std::clamp (p.integer (ids.unison), 1, kMaxUnison);
    if (U > numUni)
        for (int u = numUni; u < U; ++u) { gL[u] = gR[u] = 0.0f; acc[u] = rng.next(); }
    numUni = U;
    const float detune = clamp01 (p (ids.detune)) * p (ids.uniRange) * 0.5f;
    const float blend = clamp01 (p (ids.blend));
    const int uniMode = p.integer (ids.uniMode);
    const int stack = p.integer (ids.uniStack);
    const float width = clamp01 (p (ids.uniWidth));
    const float pan = p (ids.pan);
    const float level = std::max (0.0f, p (ids.level));
    const float wtpos0 = p.previous (ids.wtpos), wtpos1 = p (ids.wtpos);
    const float uniWt = p (ids.uniWtpos);
    const bool smooth = p.on (ids.wtSmooth);
    const float warpVar = clamp01 (p (ids.warpVar));
    const int fmType = p.integer (ids.fmType);
    const float centreIdx = 0.5f * (float) (U - 1);

    float uniGain[kMaxUnison];
    float sumSq = 0.0f;
    for (int u = 0; u < U; ++u)
    {
        float g = 1.0f;
        if (U > 2)
        {
            const bool centre = (U & 1) ? u == U / 2 : (u == U / 2 - 1 || u == U / 2);
            g = centre ? (blend <= 0.75f ? 1.0f : 1.0f - (blend - 0.75f) * 4.0f)
                       : std::min (1.0f, blend * (1.0f / 0.75f));
        }
        uniGain[u] = g;
        sumSq += g * g;
    }
    const float norm = sumSq > 1.0e-9f ? 1.0f / std::sqrt (sumSq) : 0.0f;

    const float maxCycles = std::max (0.5f, 1.0f - 20000.0f / srOs);
    const float invSr = 1.0f / srOs;
    const int stored = wt->storedFrames();

    bool useOddEven = false, useSelf = false;
    for (auto& s : slot) { useOddEven = useOddEven || s.mode == OddEven; useSelf = useSelf || s.mode == PdSelf; }

    float* accL = sh.accL;
    float* accR = sh.accR;
    for (int i = 0; i < N; ++i) accL[i] = accR[i] = 0.0f;

    for (int u = 0; u < U; ++u)
    {
        const float t = U > 1 ? (float) u / centreIdx - 1.0f : 0.0f;
        const float semis = detuneShape (uniMode, t, randDetune[u]) * detune + stackOffset (stack, u, U);
        const float ratio = fm::exp2 (semis * (1.0f / 12.0f));
        const float inc0 = std::min (hz0 * ratio * invSr, 0.49f);
        const float inc1 = std::min (hz1 * ratio * invSr, 0.49f);

        // Per-voice warp amounts and the read-rate speed-up they cause.
        float amt[2];
        float rateFactor = 1.0f;
        for (int s = 0; s < 2; ++s)
        {
            amt[s] = clamp01 (slot[s].amt + slot[s].spread * t * 0.5f);
            const float a = amt[s];
            float f = 1.0f;
            switch (slot[s].mode)
            {
                case Sync: f = fm::exp2 (4.0f * a); break;
                case BendPlus: case AsymPlus: f = biasSlope (biasFromBipolar (a)); break;
                case BendMinus: case AsymMinus: f = biasSlope (biasFromBipolar (-a)); break;
                case BendPlusMinus: case AsymPlusMinus: f = biasSlope (biasFromBipolar (2.0f * a - 1.0f)); break;
                case Pwm: f = 1.0f / (1.0f - 0.95f * a); break;
                case Mirror: f = 2.0f * biasSlope (biasFromBipolar (2.0f * a - 1.0f)); break;
                case Remap1: case Remap2: case Remap4: f = 1.0f + (data.remapSlope[s] - 1.0f) * a; break;
                case Remap3: f = 1.0f + (kPi * 0.5f - 1.0f) * a; break;
                default:
                    if (slot[s].kind == Kind::Fm || slot[s].kind == Kind::Pd) f = 1.0f + 2.0f * a;
                    break;
            }
            rateFactor *= std::min (f, slot[s].mode == Sync ? 16.0f : 8.0f);
        }
        const int mip = Wavetable::mipFor (std::max (inc0, inc1) * std::min (rateFactor, 64.0f), maxCycles);
        const float L = (float) Wavetable::mipLength (mip);

        // ---- 1. Phase --------------------------------------------------------
        float* ph = sh.ph;
        uint32_t a = acc[u];
        float fmK[2] { 0.0f, 0.0f };
        bool anyFm = false;
        for (int s = 0; s < 2; ++s)
            if (slot[s].kind == Kind::Fm && slot[s].src != nullptr)
            {
                // Phase: up to 1 cycle of phase deviation; Linear/Thru-Zero: +-4x the
                // frequency; Exp: +-2 octaves.
                fmK[s] = amt[s] * (fmType == 0 ? 1.0f : (fmType == 2 ? 2.0f : 4.0f));
                anyFm = anyFm || fmK[s] != 0.0f;
            }
        if (anyFm)
        {
            float* m = sh.fmMod;
            const float* s0 = slot[0].src;
            const float* s1 = slot[1].src;
            for (int i = 0; i < N; ++i)
            {
                float v = 0.0f;
                if (fmK[0] != 0.0f) v += fmK[0] * s0[i];
                if (fmK[1] != 0.0f) v += fmK[1] * s1[i];
                m[i] = safeClamp (v, -16.0f, 16.0f);
            }
        }
        if (! anyFm || fmType == 0)
        {
            // The exact state advances in integers; the read phases come from a
            // closed form (vectorisable) that restarts from it every tick.
            const uint32_t inc = toPhase (inc0);
            const int32_t dinc = (int32_t) (((int64_t) toPhase (inc1) - (int64_t) inc) / N);
            const float p0 = (float) (a >> 8) * kInv24;
            const float fi = (float) inc * (1.0f / kTwo32), fd = (float) dinc * (1.0f / kTwo32);
            for (int i = 0; i < N; ++i)
            {
                const float fl = (float) i;
                const float x = p0 + fl * fi + (0.5f * fl * (fl - 1.0f)) * fd;
                ph[i] = x - (float) (int) x;
            }
            a += (uint32_t) ((uint64_t) inc * (uint64_t) N + (uint64_t) ((int64_t) dinc * (int64_t) (N * (N - 1) / 2)));
            if (anyFm)
                for (int i = 0; i < N; ++i) ph[i] = fm::wrap01 (ph[i] + sh.fmMod[i]);
        }
        else
        {
            const float* m = sh.fmMod;
            const float dinc = (inc1 - inc0) / (float) N;
            float inc = inc0;
            for (int i = 0; i < N; ++i)
            {
                ph[i] = (float) (a >> 8) * kInv24;
                float d;
                if (fmType == 1) d = inc * (1.0f + m[i]);                       // Thru-Zero: may run backwards
                else if (fmType == 2) d = inc * fm::exp2 (m[i]);                // Exp
                else d = inc * std::max (0.0f, 1.0f + m[i]);                    // Linear: stops at 0 Hz
                d = clampf (d, -0.49f, 0.49f);
                a += (uint32_t) (int32_t) (d * kTwo32);
                inc += dinc;
            }
        }
        acc[u] = a;

        // ---- 2. Phase warps ------------------------------------------------
        bool gainUsed = false;
        WarpCtx wc { ph, sh.gain, &gainUsed, N };
        float oddGain = 1.0f, evenGain = 1.0f, selfK = 0.0f;
        for (int s = 0; s < 2; ++s)
        {
            const float w = amt[s];
            switch (slot[s].mode)
            {
                case Sync:          warpSync (wc, fm::exp2 (4.0f * w), warpVar); break;
                case BendPlus:      if (w > 0.0f) warpBendHalves (wc, biasFromBipolar (w)); break;
                case BendMinus:     if (w > 0.0f) warpBendHalves (wc, biasFromBipolar (-w)); break;
                case BendPlusMinus: if (w != 0.5f) warpBendHalves (wc, biasFromBipolar (2.0f * w - 1.0f)); break;
                case Pwm:           if (w > 0.0f) warpPwm (wc, 1.0f / (1.0f - 0.95f * w)); break;
                case AsymPlus:      if (w > 0.0f) warpAsym (wc, biasFromBipolar (w)); break;
                case AsymMinus:     if (w > 0.0f) warpAsym (wc, biasFromBipolar (-w)); break;
                case AsymPlusMinus: if (w != 0.5f) warpAsym (wc, biasFromBipolar (2.0f * w - 1.0f)); break;
                case Flip:          if (w > 0.0f) warpFlip (wc, 1.0f - w); break;
                case Mirror:        warpMirror (wc, biasFromBipolar (2.0f * w - 1.0f)); break;
                case Remap1:        if (w > 0.0f) warpRemap1 (wc, data.remapLut[s], w); break;
                case Remap2:        if (w > 0.0f) warpRemapSegments (wc, data.remapLut[s], w, 2); break;
                case Remap3:        if (w > 0.0f) warpRemap3 (wc, w); break;
                case Remap4:        if (w > 0.0f) warpRemapSegments (wc, data.remapLut[s], w, 4); break;
                case Quantize:      if (w > 0.0f) warpQuantize (wc, fm::exp2 (2.0f + 8.0f * (1.0f - w))); break;   // 1024 .. 4 steps
                case OddEven:       oddGain *= std::min (1.0f, 2.0f - 2.0f * w); evenGain *= std::min (1.0f, 2.0f * w); break;
                case PdSelf:        selfK += 0.5f * w; break;
                default:
                    if (slot[s].kind == Kind::Pd && slot[s].src != nullptr && w > 0.0f) warpPd (wc, slot[s].src, w);
                    break;
            }
        }

        // ---- 3. Table read ---------------------------------------------------
        const float pos0 = wt->storedPos (wtpos0 + uniWt * t * 0.5f, smooth);
        const float pos1 = wt->storedPos (wtpos1 + uniWt * t * 0.5f, smooth);
        const int fa = std::min ((int) pos1, stored - 1);
        const int fbI = std::min (fa + 1, stored - 1);
        const float t1 = fbI == fa ? 0.0f : pos1 - (float) fa;
        const float t0 = fbI == fa ? 0.0f : clamp01 (pos0 - (float) fa);
        const float* A = wt->mipFrame (mip, fa);
        const float* B = wt->mipFrame (mip, fbI);
        float* out = sh.out;
        const float dt = (t1 - t0) / (float) N;
        if (useSelf)
            fb[u] = readSelf (A, B, L, t0, dt, ph, out, N, selfK, fb[u]);
        else if (t0 == 0.0f && t1 == 0.0f)
            readSingle (A, L, ph, out, N);
        else
            readPair (A, B, L, t0, dt, ph, out, N);

        if (useOddEven && (oddGain != 1.0f || evenGain != 1.0f))
        {
            // Split into odd / even harmonics with a half-cycle-shifted read.
            float* ph2 = sh.ph2;
            float* o2 = sh.out2;
            for (int i = 0; i < N; ++i) { const float x = ph[i] + 0.5f; ph2[i] = x >= 1.0f ? x - 1.0f : x; }
            if (t0 == 0.0f && t1 == 0.0f) readSingle (A, L, ph2, o2, N);
            else readPair (A, B, L, t0, dt, ph2, o2, N);
            const float go = 0.5f * oddGain, ge = 0.5f * evenGain;
            for (int i = 0; i < N; ++i) out[i] = go * (out[i] - o2[i]) + ge * (out[i] + o2[i]);
        }
        if (gainUsed)
            for (int i = 0; i < N; ++i) out[i] *= sh.gain[i];

        // ---- 4. Post warps ----------------------------------------------------
        for (int s = 0; s < 2; ++s)
            if (isPost (slot[s].mode))
                processPost (slot[s].mode, amt[s], out, N, slot[s].src, post[s][u], srOs, hz1 * ratio);

        // ---- 5. Mix ----------------------------------------------------------
        float uPan = width * t * ((((int) std::abs ((float) u - centreIdx)) & 1) ? -1.0f : 1.0f);
        if (uniMode == 1) uPan = (uPan < 0.0f ? -1.0f : 1.0f) * std::sqrt (std::abs (uPan));
        // Equal-power pan as panGains() (centre = 1 each), without libm calls.
        const float pa = (clampf (pan + uPan, -1.0f, 1.0f) + 1.0f) * 0.125f;
        float tl = fm::sin2pi (pa + 0.25f) * 1.41421356f, tr = fm::sin2pi (pa) * 1.41421356f;
        const float g = level * uniGain[u] * norm;
        tl *= g; tr *= g;
        const float l0 = gainsValid ? gL[u] : tl, r0 = gainsValid ? gR[u] : tr;
        const float dl = (tl - l0) / (float) N, dr = (tr - r0) / (float) N;
        for (int i = 0; i < N; ++i)
        {
            const float fi = (float) (i + 1);
            accL[i] += out[i] * (l0 + dl * fi);
            accR[i] += out[i] * (r0 + dr * fi);
        }
        gL[u] = tl; gR[u] = tr;
    }
    gainsValid = true;

    // ---- Decimate --------------------------------------------------------
    if (os == 1)
    {
        std::copy (accL, accL + n, tick.outL);
        std::copy (accR, accR + n, tick.outR);
    }
    else if (os == 2)
    {
        dec[0][0].process (accL, tick.outL, n);
        dec[0][1].process (accR, tick.outR, n);
    }
    else
    {
        dec[1][0].process (accL, sh.mid[0], 2 * n);
        dec[1][1].process (accR, sh.mid[1], 2 * n);
        dec[0][0].process (sh.mid[0], tick.outL, n);
        dec[0][1].process (sh.mid[1], tick.outR, n);
    }
    if (tick.outMono != nullptr)
        for (int i = 0; i < n; ++i) tick.outMono[i] = 0.5f * (tick.outL[i] + tick.outR[i]);
    tick.activeUnison = U;

    // ---- Phase memory ----------------------------------------------------
    if (stamp == sh.newest)
    {
        std::copy (acc, acc + kMaxUnison, sh.phase);
        sh.phaseValid = true;
        const int mem = p.integer (ids.phaseMem);
        if (mem == 2 && sh.lastMemMode != 2)
        {
            std::copy (acc, acc + kMaxUnison, sh.stored);
            sh.storedValid = true;
        }
        sh.lastMemMode = mem;
    }
}
} // namespace tincture
