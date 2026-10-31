// Filters.cpp — FilterVoice: every filter type of choices::filterTypes(), its VAR
// behaviour, DRIVE (+ clean mode), PAN cutoff offset, MIX, type-switch declick,
// and the analytic small-signal response used to draw the filter curve.
//
// Layout:
//   1. helpers (prewarp, resonance maps, fast sine)
//   2. "plans": everything constant over a tick for each family (resonance,
//      VAR mapping, output mixes) — shared by process() and magnitudeDb()
//   3. FilterImpl: per-8-sample coefficients and the sample loops
//   4. FilterVoice entry points
//   5. magnitudeDb(): the same plans/coefficients evaluated on the unit circle
//
// All TPT structures are bilinear transforms of their analog prototypes with the
// cutoff prewarped, so the digital response at f equals the analog one at
// s = j tan(pi f / fs) / g — that is what magnitudeDb() evaluates.
#include "Filters.h"

#include <complex>

namespace tincture
{
namespace
{
using cpx = std::complex<double>;

constexpr int kSub = 8;                 // samples per coefficient update
constexpr float kFcMin = 4.0f;          // lowest cutoff after the pan offset
constexpr float kKMin = 0.04f;          // SVF damping (1/Q) at full resonance: Q = 25
constexpr float kLog2Range = 11.4252159f;   // log2 (22000 / 8)
constexpr double kPiD = 3.14159265358979323846;

// Type indices into choices::filterTypes().
enum : int
{
    tMgLow6 = 0, tMgLow24 = 3,
    tLow6 = 4, tLow12, tLow18, tLow24, tHigh6, tHigh12, tHigh18, tHigh24,
    tBand12, tBand24, tPeak12, tPeak24, tNotch12, tNotch24,
    tLH = 18, tNN = 29, tLBH12 = 30, tBPN24 = 37,
    tCmbL = 38, tPhsHL = 46,
    tLowEq6 = 47, tLowEq12, tBandEq12, tHighEq6, tHighEq12, tRing, tRing2, tSH, tSHm,
    tCombs, tAllpasses, tReverb, tFrench, tGerman, tAddBass, tFormant1, tFormant2, tFormant3,
    tBandreject, tDistComb1LP, tDistComb1BP, tDistComb2LP, tDistComb2BP, tScreamLP, tScreamBP,
    tWsp, tDj, tDiffusor, tMgLadder, tAcid, tEms, tMgDirty, tPzSvf, tComb2, tExpMM, tExpBPF
};
static_assert (tExpBPF == FilterVoice::kNumTypes - 1, "type list out of step with FilterTypes.cpp");

enum class Fam : uint8_t { Svf, Ladder, Diode, Flange, MultiDelay, DistComb, Scream, Ring, SampHold, Formant, Diffusor };

Fam familyOf (int t) noexcept
{
    if (t <= tMgLow24) return Fam::Ladder;
    if (t <= tBPN24) return Fam::Svf;
    if (t <= tPhsHL) return Fam::Flange;
    switch (t)
    {
        case tRing: case tRing2: return Fam::Ring;
        case tSH: case tSHm: return Fam::SampHold;
        case tCombs: case tAllpasses: case tReverb: case tComb2: return Fam::MultiDelay;
        case tGerman: case tMgLadder: case tMgDirty: case tExpBPF: return Fam::Ladder;
        case tFormant1: case tFormant2: case tFormant3: return Fam::Formant;
        case tDistComb1LP: case tDistComb1BP: case tDistComb2LP: case tDistComb2BP: return Fam::DistComb;
        case tScreamLP: case tScreamBP: return Fam::Scream;
        case tAcid: case tEms: return Fam::Diode;
        case tDiffusor: return Fam::Diffusor;
        default: return Fam::Svf;       // EQs, French, Add Bass, Bandreject, Wsp, DJ, PZ SVF, Exp MM
    }
}

// Drive is plain gain into the model's own input saturator for these; the
// other types get the generic soft clipper blended in by DRIVE.
bool modelDrive (int t) noexcept
{
    const Fam f = familyOf (t);
    return (f == Fam::Ladder && t != tGerman) || f == Fam::Diode || t == tFrench || t == tWsp || t == tAddBass;
}

bool smoothType (int t) noexcept { return t == tMgLadder || t == tAcid || t == tEms || t == tPzSvf; }
bool usesDelay (Fam f) noexcept { return f == Fam::Flange || f == Fam::MultiDelay || f == Fam::DistComb; }

// ---------------------------------------------------------------------------
// 1. Helpers
// ---------------------------------------------------------------------------
// tan() Pade [5/4]: error < 1e-6 up to x = 1, ~1% at x = 1.54 (0.49 fs). Every
// prewarp goes through this so process() and magnitudeDb() agree exactly.
inline float fastTan (float x) noexcept
{
    const float x2 = x * x;
    return x * (945.0f + x2 * (-105.0f + x2)) / (945.0f + x2 * (-420.0f + 15.0f * x2));
}

// 2^x, relative error ~2e-7 (degree-5 polynomial on the fraction).
inline float exp2Fast (float x) noexcept
{
    x = clampf (x, -126.0f, 126.0f);
    int i = (int) x;
    i -= x < (float) i ? 1 : 0;
    const float f = x - (float) i;
    float p = 1.8775767e-3f;
    p = p * f + 8.9893397e-3f;
    p = p * f + 5.5826318e-2f;
    p = p * f + 2.4015361e-1f;
    p = p * f + 6.9315308e-1f;
    p = p * f + 9.9999994e-1f;
    const int32_t b = (i + 127) << 23;
    float sc;
    std::memcpy (&sc, &b, 4);
    return p * sc;
}

inline float clampFc (float f, float fs) noexcept { return clampf (f, kFcMin, 0.49f * fs); }
inline float gOf (float f, float fs) noexcept { return fastTan (kPi * clampFc (f, fs) / fs); }
inline float onePoleG (float f, float fs) noexcept { const float g = gOf (f, fs); return g / (1.0f + g); }

// SVF damping k = 1/Q: `base` at RES 0 down to kKMin at RES 1, log-spaced.
inline float kRes (float base, float res) noexcept { return base * exp2Fast (res * std::log2 (kKMin / base)); }   // log2 of constants folds

// VAR 0..1 -> 8 Hz..22 kHz (log): FREQ, LP FRQ, HP FRQ, SCREAM, COMBFRQ.
inline float varHz (float v) noexcept { return 8.0f * exp2Fast (clamp01 (v) * kLog2Range); }

// sin (2 pi ph), ph in [0, 1); ~0.1% error.
inline float fastSin01 (float ph) noexcept
{
    const float x = 2.0f * ph - 1.0f;                       // sin (2 pi ph) = -sin (pi x)
    const float y = 4.0f * x * (1.0f - std::fabs (x));
    return -(0.225f * (y * std::fabs (y) - y) + y);
}

// Negative when |x| >= 65536, Inf or NaN (exponent field >= 143); OR these over a block.
inline int32_t badBits (float x) noexcept
{
    uint32_t b;
    std::memcpy (&b, &x, 4);
    return (int32_t) ~((b & 0x7f800000u) - (143u << 23));
}

// Linear-interpolated read `d` samples behind write index w (d >= 1).
inline float dlRead (const float* b, uint32_t mask, uint32_t w, float d) noexcept
{
    const uint32_t di = (uint32_t) d;
    const float fr = d - (float) di;
    const float a = b[(w - di) & mask], c = b[(w - di - 1u) & mask];
    return a + (c - a) * fr;
}

// ---------------------------------------------------------------------------
// 2. Plans
// ---------------------------------------------------------------------------
struct Mix3 { float l, b, h; };     // y = l*LP + b*(k*BP) + h*HP   (k*BP has unity peak)
constexpr Mix3 mL { 1, 0, 0 }, mB { 0, 1, 0 }, mH { 0, 0, 1 }, mN { 1, 0, 1 };
inline Mix3 mP (float g) noexcept { return { 1, g, 1 }; }          // bell: notch + g * band
inline Mix3 mlerp (Mix3 a, Mix3 b, float t) noexcept { return { lerp (a.l, b.l, t), lerp (a.b, b.b, t), lerp (a.h, b.h, t) }; }
inline Mix3 morph3 (Mix3 a, Mix3 b, Mix3 c, float m) noexcept
{
    m = 2.0f * clamp01 (m);
    return m < 1.0f ? mlerp (a, b, m) : mlerp (b, c, m - 1.0f);
}

inline float dbGain (float db) noexcept { return exp2Fast (db * 0.166096405f); }
inline float peakGain (float res) noexcept { return dbGain (3.0f + 15.0f * res); }
inline float peakK (float res) noexcept { return exp2Fast (-3.0f * res); }
inline float notchK (float res) noexcept { return 1.41421356f * exp2Fast (-4.0f * res); }
// DB +/-: VAR 0.5 = 0 dB, 1 = +24 dB, 0 = -inf (the "high-pass" end for Low EQ).
inline float eqGain (float v) noexcept { v = clamp01 (v); return v >= 0.5f ? dbGain ((v - 0.5f) * 48.0f) : 4.0f * v * v; }

constexpr float kButter2 = 1.41421356f;     // 2-pole Butterworth k
constexpr float kButter4a = 1.84775907f;    // 4-pole Butterworth as two SVFs
constexpr float kButter4b = 0.76536686f;

// --- SVF chain: [one-pole] -> SVF -> [SVF] ---------------------------------
struct SvfPlan
{
    bool op = false;            // a one-pole stage first
    int ns = 1;                 // SVF stages 0..2
    int nlm = 0;                // 0 linear, 1 saturating band state (FAT), 2 also tanh input
    float fat = 0.0f;
    float opL = 1.0f, opH = 0.0f;
    Mix3 mix[2] { mL, mL };
    float k[2] { kButter2, kButter2 };
    float mul[2] { 1.0f, 1.0f };    // stage frequency = cutoff * mul ...
    float absHz[2] { 0.0f, 0.0f };  // ... or this (times the pan factor) when > 0
    bool dj = false;
    float res = 0.0f;
};

inline void setStage (SvfPlan& p, int j, Mix3 m, float k) noexcept { p.mix[j] = m; p.k[j] = k; }

void dualStage (SvfPlan& p, int j, char c, float res) noexcept
{
    switch (c)
    {
        case 'L': setStage (p, j, mL, kRes (kButter2, res)); break;
        case 'H': setStage (p, j, mH, kRes (kButter2, res)); break;
        case 'B': setStage (p, j, mB, kRes (kButter2, res)); break;
        case 'P': setStage (p, j, mP (peakGain (res)), peakK (res)); break;
        default:  setStage (p, j, mN, notchK (res)); break;
    }
}

void planSvf (int t, float res, float var, SvfPlan& p) noexcept
{
    p = SvfPlan();
    p.res = res;
    const float k12 = kRes (kButter2, res);
    auto k24a = [res] { return kButter4a * exp2Fast (-res); };
    auto k24b = [res] { return kRes (kButter4b, res); };
    if (t >= tLow6 && t <= tNotch24 && var > 0.0f) { p.nlm = 1; p.fat = var; }    // FAT

    switch (t)
    {
        case tLow6:     p.op = true; p.ns = 0; break;
        case tLow12:    setStage (p, 0, mL, k12); break;
        case tLow18:    p.op = true; setStage (p, 0, mL, kRes (1.0f, res)); break;
        case tLow24:    p.ns = 2; setStage (p, 0, mL, k24a ()); setStage (p, 1, mL, k24b ()); break;
        case tHigh6:    p.op = true; p.ns = 0; p.opL = 0; p.opH = 1; break;
        case tHigh12:   setStage (p, 0, mH, k12); break;
        case tHigh18:   p.op = true; p.opL = 0; p.opH = 1; setStage (p, 0, mH, kRes (1.0f, res)); break;
        case tHigh24:   p.ns = 2; setStage (p, 0, mH, k24a ()); setStage (p, 1, mH, k24b ()); break;
        case tBand12:   setStage (p, 0, mB, k12); break;
        case tBand24:   p.ns = 2; setStage (p, 0, mB, k12); setStage (p, 1, mB, k12); break;
        case tPeak12:   setStage (p, 0, mP (peakGain (res)), peakK (res)); break;
        case tPeak24:
        {
            const float g = std::sqrt (peakGain (res));
            p.ns = 2; setStage (p, 0, mP (g), peakK (res)); setStage (p, 1, mP (g), peakK (res));
            break;
        }
        case tNotch12:  setStage (p, 0, mN, notchK (res)); break;
        case tNotch24:  p.ns = 2; setStage (p, 0, mN, notchK (res)); setStage (p, 1, mN, notchK (res)); break;

        case tLowEq6:   p.op = true; p.ns = 0; p.opL = eqGain (var); p.opH = 1; break;
        case tLowEq12:  setStage (p, 0, { eqGain (var), 1, 1 }, kButter2 * exp2Fast (-1.5f * res)); break;
        case tBandEq12: setStage (p, 0, { 1, eqGain (var), 1 }, kButter2 * peakK (res)); break;
        case tHighEq6:  p.op = true; p.ns = 0; p.opL = 1; p.opH = eqGain (var); break;
        case tHighEq12: setStage (p, 0, { 1, 1, eqGain (var) }, kButter2 * exp2Fast (-1.5f * res)); break;

        case tFrench:   // two saturating LPs; BOEUF raises the 2nd stage and gives it RES
            p.ns = 2; p.nlm = 2; p.fat = 1.0f;
            setStage (p, 0, mL, k12);
            setStage (p, 1, mL, kRes (kButter2, res * var));
            p.mul[1] = std::exp2 (var * 1.5f);
            break;
        case tAddBass:  // boosted LP plus THRU x allpass (= phase-rotated dry)
            p.nlm = 2; p.fat = 0.5f;
            setStage (p, 0, { 1.0f + var, -var, var }, k12);
            break;
        case tBandreject:   // two wide notches spread +-WIDTH around the cutoff
        {
            const float w = std::exp2 (var * 1.5f), k = 2.0f * std::exp2 (-2.0f * res);
            p.ns = 2; setStage (p, 0, mN, k); setStage (p, 1, mN, k);
            p.mul[0] = 1.0f / w; p.mul[1] = w;
            break;
        }
        case tWsp:      p.nlm = 2; p.fat = 1.0f; setStage (p, 0, morph3 (mL, mN, mH, var), k12); break;
        case tDj:       p.dj = true; break;
        case tPzSvf:    p.nlm = 1; p.fat = 0.3f; setStage (p, 0, mL, k12); break;
        case tExpMM:    p.nlm = 1; p.fat = 0.2f; setStage (p, 0, morph3 (mL, mN, mH, var), k12); break;
        default:
            if (t >= tLH && t <= tNN)       // dual: 2nd stage at FREQ
            {
                static const char* const codes[] { "LH", "LB", "LP", "LN", "HB", "HP", "HN", "BP", "BN", "PP", "PN", "NN" };
                const char* c = codes[t - tLH];
                p.ns = 2; p.nlm = 1; p.fat = 0.15f;      // two resonances can coincide: tame them at high level
                dualStage (p, 0, c[0], res);
                dualStage (p, 1, c[1], res);
                p.absHz[1] = varHz (var);
            }
            else if (t >= tLBH12 && t <= tBPN24)    // morphing
            {
                const bool x24 = ((t - tLBH12) & 1) != 0;
                const float pg = x24 ? std::sqrt (peakGain (res)) : peakGain (res);
                Mix3 a = mL, b = mB, c = mH;
                switch ((t - tLBH12) / 2)
                {
                    case 0: break;
                    case 1: b = mP (pg); break;
                    case 2: b = mN; break;
                    default: a = mB; b = mP (pg); c = mN; break;
                }
                const Mix3 m = morph3 (a, b, c, var);
                p.nlm = 1; p.fat = 0.15f;
                if (x24) { p.ns = 2; setStage (p, 0, m, k24a ()); setStage (p, 1, m, k24b ()); }
                else setStage (p, 0, m, k12);
            }
            break;
    }
}

// DJ Mixer: left half of the cutoff travel sweeps a low-pass down, right half a
// high-pass up, flat in the middle (the mix fades to all-pass x around 50%).
void djStage (float fc, float res, float& f, float& k, Mix3& m) noexcept
{
    const float p = clamp01 (std::log2 (std::max (fc, 8.0f) * 0.125f) / kLog2Range);
    k = kRes (kButter2, res * 0.8f);
    if (p < 0.5f)
    {
        f = 20.0f * std::exp2 (p * 20.0f);
        const float b = clamp01 ((p - 0.45f) * 20.0f);
        m = { 1.0f, b, b };
    }
    else
    {
        f = 10.0f * std::exp2 ((p - 0.5f) * 22.0f);
        const float b = clamp01 ((0.55f - p) * 20.0f);
        m = { b, b, 1.0f };
    }
}

struct StageRes { float g, k; Mix3 m; };
inline StageRes resolveStage (const SvfPlan& p, int j, float fc, float pm, float fs) noexcept
{
    if (p.dj)
    {
        float f, k; Mix3 m;
        djStage (fc, p.res, f, k, m);
        return { gOf (f, fs), k, m };
    }
    const float f = p.absHz[j] > 0.0f ? p.absHz[j] * pm : fc * p.mul[j];
    return { gOf (f, fs), p.k[j], p.mix[j] };
}

// Simper/Zavalishin TPT SVF coefficients with the output mix folded in:
// y = m0*x + m1*v1 + m2*v2 (v1 = band, v2 = low).
struct SvfSt { float a1, a2, a3, m0, m1, m2; };
inline SvfSt svfMake (float g, float k, Mix3 m) noexcept
{
    SvfSt s;
    s.a1 = 1.0f / (1.0f + g * (g + k));
    s.a2 = g * s.a1;
    s.a3 = g * s.a2;
    s.m0 = m.h;
    s.m1 = (m.b - m.h) * k;
    s.m2 = m.l - m.h;
    return s;
}

struct SvfCo { float G = 0.0f, o0 = 1.0f, o1 = 0.0f; SvfSt s[2] {}; };
inline void svfCoefs (const SvfPlan& p, float fc, float pm, float fs, SvfCo& c) noexcept
{
    if (p.op)
    {
        c.G = onePoleG (fc, fs);
        c.o0 = p.opH;
        c.o1 = p.opL - p.opH;
    }
    for (int j = 0; j < p.ns; ++j)
    {
        const StageRes r = resolveStage (p, j, fc, pm, fs);
        c.s[j] = svfMake (r.g, r.k, r.m);
    }
}

template <bool FAT>
inline float svfStage (const SvfSt& s, float* z, float x, float fat) noexcept
{
    const float v3 = x - z[1];
    const float v1 = s.a1 * z[0] + s.a2 * v3;
    const float v2 = z[1] + s.a2 * z[0] + s.a3 * v3;
    float ic1 = 2.0f * v1 - z[0];
    if constexpr (FAT) ic1 += fat * (fastTanh (ic1) - ic1);   // saturate the resonance (band) state
    z[0] = ic1;
    z[1] = 2.0f * v2 - z[1];
    return s.m0 * x + s.m1 * v1 + s.m2 * v2;
}

template <bool OP, int NS, int NLM>
inline float svfTick (const SvfCo& c, float* z, float x, float fat) noexcept
{
    if constexpr (NLM == 2) x = fastTanh (x);
    if constexpr (OP)
    {
        const float v = (x - z[4]) * c.G, lp = v + z[4];
        z[4] = lp + v;
        x = c.o0 * x + c.o1 * lp;
    }
    for (int j = 0; j < NS; ++j)
        x = svfStage<(NLM >= 1)> (c.s[j], z + 2 * j, x, fat);
    return x;
}

// --- Transistor ladder (MG types, German, Exp BPF) ------------------------
// Four TPT one-poles, global feedback k solved without delay (linear), the
// tanh input stage applied to the solved input. Self-oscillates at the cutoff
// (each stage gives -45 deg there) once k > 4.
struct LadderPlan
{
    int mode = 0;               // 0: tanh input (+FAT in feedback), 1: tanh in feedback only (German), 2: every stage saturates (Dirty)
    float k = 0.0f, fat = 0.0f, comp = 1.0f, bias = 0.0f, tb = 0.0f;
    float c[5] { 0, 0, 0, 0, 1 };   // output = sum c[i] * y[i], y[0] = ladder input
};

void planLadder (int t, float res, float var, LadderPlan& p) noexcept
{
    p = LadderPlan();
    switch (t)
    {
        case tGerman:   p.mode = 1; p.k = 4.3f * res; break;
        case tMgLadder: p.k = 4.4f * res; break;
        case tMgDirty:  p.mode = 2; p.k = 4.6f * res; p.fat = 0.3f; p.bias = 0.5f * var; p.tb = fastTanh (p.bias); break;
        case tExpBPF:   p.k = 3.6f * res; p.c[2] = 4.0f; p.c[3] = -8.0f; p.c[4] = 4.0f; break;    // 4 H^2 (1-H)^2
        default:        p.k = 4.6f * res; p.fat = var; p.c[4] = 0.0f; p.c[t - tMgLow6 + 1] = 1.0f; break;
    }
    if (t != tExpBPF) p.comp = 1.0f + 0.5f * p.k;       // partial passband-loss compensation
}

struct LadCo { float G, G2, G3, G4, omG, d, q[4]; };
inline LadCo ladCo (float fc, float k, float fs) noexcept
{
    LadCo c;
    c.G = onePoleG (fc, fs);
    c.G2 = c.G * c.G; c.G3 = c.G2 * c.G; c.G4 = c.G2 * c.G2;
    c.omG = 1.0f - c.G;
    c.d = 1.0f / (1.0f + k * c.G4);
    const float kd = -k * c.d * c.omG;      // ul = d x + sum q_i s_i
    c.q[0] = kd * c.G3; c.q[1] = kd * c.G2; c.q[2] = kd * c.G; c.q[3] = kd;
    return c;
}

template <int MODE, bool FAT>
inline float ladTick (const LadCo& c, const LadderPlan& p, float* s, float x) noexcept
{
    // Stage outputs are y_i = G^i u + B_i with B_i from the states alone, so
    // only one multiply-add per stage sits on the u -> state critical path.
    const float B1 = c.omG * s[0];
    const float B2 = c.omG * (c.G * s[0] + s[1]);
    const float B3 = c.omG * (c.G2 * s[0] + c.G * s[1] + s[2]);
    const float S = c.omG * (c.G3 * s[0] + c.G2 * s[1] + c.G * s[2] + s[3]);   // y4 with zero input
    const float ul = c.d * x + (c.q[0] * s[0] + c.q[1] * s[1]) + (c.q[2] * s[2] + c.q[3] * s[3]);
    float u;
    if constexpr (MODE == 1)
        u = x - p.k * fastTanh (c.G4 * ul + S);
    else if constexpr (FAT || MODE == 2)
    {
        float fb = p.k * (c.G4 * ul + S);
        fb += p.fat * (fastTanh (fb) - fb);     // FAT: saturate the feedback
        u = fastTanh (x - fb);
    }
    else
        u = fastTanh (ul);                      // = tanh (x - k * y4) solved without delay
    float y1, y2, y3, y4;
    if constexpr (MODE == 2)        // Dirty: every stage input saturates (sequential)
    {
        float v = (u - s[0]) * c.G;
        y1 = v + s[0];
        v = (fastTanh (y1 + p.bias) - p.tb - s[1]) * c.G; y2 = v + s[1];
        v = (fastTanh (y2 + p.bias) - p.tb - s[2]) * c.G; y3 = v + s[2];
        v = (fastTanh (y3 + p.bias) - p.tb - s[3]) * c.G; y4 = v + s[3];
    }
    else
    {
        y1 = c.G * u + B1;
        y2 = c.G2 * u + B2;
        y3 = c.G3 * u + B3;
        y4 = c.G4 * u + S;
    }
    s[0] = 2.0f * y1 - s[0];
    s[1] = 2.0f * y2 - s[1];
    s[2] = 2.0f * y3 - s[2];
    s[3] = 2.0f * y4 - s[3];
    return p.comp * (p.c[0] * u + p.c[1] * y1 + p.c[2] * y2 + p.c[3] * y3 + p.c[4] * y4);
}

// Fast path for the plain tanh-input ladder (MG Low with FAT 0, MG Ladder,
// Exp BPF). With y_i = G^i u + B_i the state update is s' = r u + P s, P lower
// triangular Toeplitz (diag 1-2G, then 2(1-G)G^j). The next sample's input
// solve ul = d x + q.s' is expanded so that its state part (q P).s uses the
// old state and runs while the tanh is evaluated: the per-sample critical path
// is one multiply-add and the tanh.
struct LadFast { float d, alpha, q[4], p[4], r[4], qp[4], comp; };
inline LadFast ladFast (float fc, const LadderPlan& lp, float fs) noexcept
{
    LadFast c;
    const float G = onePoleG (fc, fs), omG = 1.0f - G;
    const float Gp[5] { 1.0f, G, G * G, G * G * G, G * G * G * G };
    c.d = 1.0f / (1.0f + lp.k * Gp[4]);
    for (int j = 0; j < 4; ++j)
    {
        c.q[j] = -lp.k * c.d * omG * Gp[3 - j];
        c.r[j] = 2.0f * Gp[j + 1];
        c.p[j] = j == 0 ? 1.0f - 2.0f * G : 2.0f * omG * Gp[j];
    }
    c.alpha = 0.0f;
    for (int j = 0; j < 4; ++j)
    {
        c.alpha += c.q[j] * c.r[j];
        float acc = 0.0f;
        for (int i = j; i < 4; ++i) acc += c.q[i] * c.p[i - j];
        c.qp[j] = acc;
    }
    c.comp = lp.comp;
    return c;
}

// --- Diode ladders (Acid, EMS) ---------------------------------------------
// Four coupled capacitors: dy/dt = wc (A y + e1 u), A tridiagonal (diag -2, off
// +1; last diagonal -1 for Acid, -2 for EMS). TPT: (I - gA) y = s + g e1 u,
// solved per sample with precomputed Thomas factors; feedback u = x - k y4 is
// solved without delay, then tanh'd. The cutoff is scaled so the -180 deg
// point (= self-oscillation pitch) lands on CUTOFF.
struct DiodeCal { double w180 = 1, kcrit = 1, h0 = 1; };

cpx diodeH4 (cpx S, double dLast) noexcept      // y4 / u for the analog prototype at s = S
{
    const cpx b[4] { S + 2.0, S + 2.0, S + 2.0, S - dLast };
    cpx cp = -1.0 / b[0], dp = 1.0 / b[0];
    for (int i = 1; i < 4; ++i)
    {
        const cpx den = b[i] + cp;
        cp = -1.0 / den;
        dp = dp / den;
    }
    return dp;
}

DiodeCal calibrate (double dLast) noexcept
{
    DiodeCal c;
    c.h0 = diodeH4 (cpx (0.0, 0.0), dLast).real();
    double lo = 0.0, hi = 0.0, prev = 1.0e-3;
    for (double w = 1.0e-3; w < 1.0e3; w *= 1.05)       // first crossing of the negative real axis
    {
        const cpx h = diodeH4 (cpx (0.0, w), dLast);
        if (h.real() < 0.0 && h.imag() >= 0.0) { lo = prev; hi = w; break; }
        prev = w;
    }
    for (int i = 0; i < 80; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        const cpx h = diodeH4 (cpx (0.0, mid), dLast);
        (h.imag() < 0.0 ? lo : hi) = mid;
    }
    c.w180 = 0.5 * (lo + hi);
    c.kcrit = 1.0 / std::abs (diodeH4 (cpx (0.0, c.w180), dLast));
    return c;
}

const DiodeCal& diodeCal (int t) noexcept
{
    static const DiodeCal acid = calibrate (-1.0), ems = calibrate (-2.0);
    return t == tAcid ? acid : ems;
}

struct DiodePlan { float dLast, k, comp, w180; };
void planDiode (int t, float res, DiodePlan& p) noexcept
{
    const DiodeCal& c = diodeCal (t);
    p.dLast = t == tAcid ? -1.0f : -2.0f;
    p.k = (float) c.kcrit * (t == tAcid ? 1.06f : 1.15f) * res;
    p.comp = (1.0f + 0.5f * p.k * (float) c.h0) / (float) c.h0;
    p.w180 = (float) c.w180;
}

struct DioCo { float g, inv[4], cp[4], w[4], d, M[4][4]; };     // M = (I - gA)^-1
inline void dioSolve (const DioCo& c, const float* r, float* y) noexcept
{
    const float d0 = r[0] * c.inv[0];
    const float d1 = (r[1] + c.g * d0) * c.inv[1];
    const float d2 = (r[2] + c.g * d1) * c.inv[2];
    const float d3 = (r[3] + c.g * d2) * c.inv[3];
    y[3] = d3;
    y[2] = d2 - c.cp[2] * y[3];
    y[1] = d1 - c.cp[1] * y[2];
    y[0] = d0 - c.cp[0] * y[1];
}
inline DioCo dioCo (float fc, float fs, const DiodePlan& p) noexcept
{
    DioCo c;
    const float g = gOf (fc, fs) / p.w180;
    c.g = g;
    const float b[4] { 1.0f + 2.0f * g, 1.0f + 2.0f * g, 1.0f + 2.0f * g, 1.0f - g * p.dLast };
    c.inv[0] = 1.0f / b[0];
    c.cp[0] = -g * c.inv[0];
    for (int i = 1; i < 4; ++i)
    {
        c.inv[i] = 1.0f / (b[i] + g * c.cp[i - 1]);
        c.cp[i] = -g * c.inv[i];
    }
    const float e[4] { g, 0.0f, 0.0f, 0.0f };
    dioSolve (c, e, c.w);
    c.d = 1.0f / (1.0f + p.k * c.w[3]);
    for (int j = 0; j < 4; ++j)
    {
        float col[4] {}, y[4];
        col[j] = 1.0f;
        dioSolve (c, col, y);
        for (int i = 0; i < 4; ++i) c.M[i][j] = y[i];
    }
    return c;
}

// --- Flanges: comb / flanger / phaser with a filter in the feedback loop ---
struct FlangePlan { int kind, fmode; float fb, Glp, Ghp, wid, lpHz, hpHz; };
void planFlange (int t, float res, float var, float fs, FlangePlan& p) noexcept
{
    const int i = t - tCmbL;
    p.kind = i % 3;             // 0 comb, 1 flanger, 2 phaser
    p.fmode = i / 3;            // 0 LP, 1 HP, 2 HP+LP band
    p.fb = p.kind == 0 ? 0.985f * res : (p.kind == 1 ? -0.985f * res : 0.9f * res);
    p.lpHz = varHz (var);
    p.hpHz = varHz (var);
    p.Glp = onePoleG (p.lpHz, fs);
    p.Ghp = onePoleG (p.hpHz, fs);
    p.wid = std::exp2 (0.25f + 3.75f * var);
}

inline float fbFilt (int fmode, float x, float* z, float Glp, float Ghp) noexcept
{
    if (fmode != 0) { const float v = (x - z[0]) * Ghp, lp = v + z[0]; z[0] = lp + v; x -= lp; }
    if (fmode != 1) { const float v = (x - z[1]) * Glp, lp = v + z[1]; z[1] = lp + v; x = lp; }
    return x;
}

// --- Combs / Allpasses / Reverb / Comb 2 ------------------------------------
struct MdPlan { int kind, nReg; float fb, ap, Gd, wet; float ratio[6]; };
void planMd (int t, float res, float var, float fs, MdPlan& p) noexcept
{
    static const float combR[4] { 1.0f, 0.8706f, 0.7529f, 0.6667f };
    p.Gd = onePoleG (22000.0f * std::exp2 (-10.0f * var), fs);     // DAMP
    p.ap = 0.6f;
    p.wet = 1.0f;
    switch (t)
    {
        case tCombs:
            p.kind = 0; p.nReg = 4; p.fb = 0.97f * res;
            for (int i = 0; i < 4; ++i) p.ratio[i] = combR[i];
            break;
        case tAllpasses:
        {
            static const float r[4] { 1.0f, 0.6823f, 0.4471f, 0.2941f };
            p.kind = 1; p.nReg = 4; p.fb = 0.0f; p.ap = 0.3f + 0.6f * res;
            for (int i = 0; i < 4; ++i) p.ratio[i] = r[i];
            break;
        }
        case tReverb:
            p.kind = 2; p.nReg = 8; p.fb = 0.6f + 0.38f * res;
            for (int i = 0; i < 4; ++i) p.ratio[i] = combR[i];
            p.ratio[4] = 0.25f; p.ratio[5] = 0.0833f;
            break;
        default:    // Comb 2: two combs, FRQ2 = 2nd comb +-2 octaves; extreme feedback
            p.kind = 3; p.nReg = 2;
            p.fb = (1.0f - std::exp2 (-10.0f * res)) * 0.9995f;
            p.ratio[0] = 1.0f;
            p.ratio[1] = std::exp2 (-(clamp01 (var) - 0.5f) * 4.0f);
            p.wet = std::pow (1.0f - p.fb, 0.15f);   // keeps Comb 2 near the other combs' level at high feedback
            p.Gd = 1.0f;
            break;
    }
}

// --- Dist.Comb and Scream ------------------------------------------------------
struct DcPlan { bool bp; float sfb, k, combHz; };
void planDc (int t, float res, float var, DcPlan& p) noexcept
{
    p.bp = t == tDistComb1BP || t == tDistComb2BP;
    const bool neg = t == tDistComb2LP || t == tDistComb2BP;
    p.sfb = (neg ? -0.97f : 0.97f) * res;
    p.k = 1.0f;
    p.combHz = varHz (var);
}

struct ScPlan { bool bp; float k, amt, Ghp, hpHz; };
void planScream (int t, float res, float var, float drive, float fs, ScPlan& p) noexcept
{
    p.bp = t == tScreamBP;
    p.k = kRes (kButter2, res);
    p.amt = 1.6f * clamp01 ((drive - 0.5f) * 2.0f);      // audible above DRIVE 50%
    p.hpHz = varHz (var);
    p.Ghp = onePoleG (p.hpHz, fs);
}

// --- Formants ------------------------------------------------------------------
struct Vowel { float f[3], db[3]; };
// Formant-I tenor, -II alto, -III soprano; vowels A E I O U (classic formant tables).
const Vowel kVowels[3][5] {
    { { { 650, 1080, 2650 }, { 0, -6, -7 } }, { { 400, 1700, 2600 }, { 0, -14, -12 } }, { { 290, 1870, 2800 }, { 0, -15, -18 } },
      { { 400, 800, 2600 }, { 0, -10, -12 } }, { { 350, 600, 2700 }, { 0, -20, -17 } } },
    { { { 800, 1150, 2800 }, { 0, -4, -20 } }, { { 400, 1600, 2700 }, { 0, -24, -30 } }, { { 350, 1700, 2700 }, { 0, -20, -30 } },
      { { 450, 800, 2830 }, { 0, -9, -16 } }, { { 325, 700, 2530 }, { 0, -12, -30 } } },
    { { { 800, 1150, 2900 }, { 0, -6, -32 } }, { { 350, 2000, 2800 }, { 0, -20, -15 } }, { { 270, 2140, 2950 }, { 0, -12, -26 } },
      { { 450, 800, 2830 }, { 0, -11, -22 } }, { { 325, 700, 2700 }, { 0, -16, -35 } } }
};
constexpr float kFormantBw[3] { 80.0f, 90.0f, 120.0f };

struct FormantGains { float a[3][5][3]; };
const FormantGains& formantGains() noexcept
{
    static const FormantGains g = [] {
        FormantGains r {};
        for (int s = 0; s < 3; ++s)
            for (int v = 0; v < 5; ++v)
                for (int j = 0; j < 3; ++j)
                    r.a[s][v][j] = dbToGain (kVowels[s][v].db[j]);
        return r;
    }();
    return g;
}

struct FmPlan { int set; float shift, kscale; };
void planFormant (int t, float res, float var, FmPlan& p) noexcept
{
    p.set = t - tFormant1;
    p.shift = std::exp2 ((clamp01 (var) - 0.5f) * 2.0f);    // FORMNT: +-1 octave
    p.kscale = std::exp2 (0.5f - 1.5f * res);
    (void) formantGains();
}

// The three band-pass sections for a cutoff: CUTOFF 8 Hz..22 kHz walks A-E-I-O-U.
inline void formantBands (const FmPlan& p, float fc, float fs, SvfSt* s) noexcept
{
    const float pos = clamp01 (std::log2 (std::max (fc, 8.0f) * 0.125f) / kLog2Range) * 4.0f;
    const int i = std::min ((int) pos, 3);
    const float fr = pos - (float) i;
    const FormantGains& G = formantGains();
    for (int j = 0; j < 3; ++j)
    {
        const float f = lerp (kVowels[p.set][i].f[j], kVowels[p.set][i + 1].f[j], fr) * p.shift;
        const float a = lerp (G.a[p.set][i][j], G.a[p.set][i + 1][j], fr) * 1.5f;
        const float k = clampf (kFormantBw[j] / f * p.kscale, 0.02f, 2.0f);
        s[j] = svfMake (gOf (f, fs), k, { 0.0f, a, 0.0f });
    }
}

// --- Ring mod, sample & hold, diffusor ---------------------------------------
inline float ringSpread (float var) noexcept { return std::exp2 (clamp01 (var) * 1.5f); }
inline int diffStages (float var) noexcept { return 1 + (int) std::lround (clamp01 (var) * 31.0f); }
inline float diffK (float res) noexcept { return 2.0f * std::exp2 (-3.0f * res); }
} // namespace

// ---------------------------------------------------------------------------
// 3. Sample loops
// ---------------------------------------------------------------------------
struct FilterImpl
{
    struct Tk
    {
        int type = 0, n = 0;
        float lc0[2] {}, lc1[2] {};     // log2 cutoff per channel at the chunk start / end (pan included)
        float pm[2] { 1.0f, 1.0f };     // pan frequency factor per channel
        float res = 0, var = 0, drive = 0, fs = 48000.0f;
        float smoothA = 1.0f;           // SMOOTH: per-sub-block slew coefficient (1 = none)
        float pain = 0.0f;              // MG Dirty cutoff jitter
        bool same = false;              // L and R cutoffs identical (pan 0): share coefficients
        int nch = 2;                    // 1: mono, left channel only
        float* x[2] {};
    };

    static void cutoffs (FilterVoice& v, const Tk& t, int i0, int m, float* fc) noexcept
    {
        const float fr = ((float) i0 + 0.5f * (float) m) / (float) t.n;
        const int nc = (t.same || t.nch == 1) ? 1 : 2;
        for (int c = 0; c < nc; ++c)
        {
            float lc = t.lc0[c] + (t.lc1[c] - t.lc0[c]) * fr;
            if (t.smoothA < 1.0f)
            {
                v.smoothLc[c] += (lc - v.smoothLc[c]) * t.smoothA;
                lc = v.smoothLc[c];
            }
            if (t.pain > 0.0f) lc += t.pain * 0.06f * v.rng.bi();
            fc[c] = clampFc (exp2Fast (lc), t.fs);
        }
        if (t.same) fc[1] = fc[0];
    }

    // ---- SVF chain ----
    template <bool OP, int NS, int NLM>
    static void svfLoop (FilterVoice& v, const Tk& t, const SvfPlan& p) noexcept
    {
        float zl[5], zr[5];
        std::copy_n (v.st[0], 5, zl);
        std::copy_n (v.st[1], 5, zr);
        float* L = t.x[0];
        float* R = t.x[1];
        const float fat = p.fat;
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            SvfCo cl, cr;
            svfCoefs (p, fc[0], t.pm[0], t.fs, cl);
            if (t.same || t.nch == 1) cr = cl; else svfCoefs (p, fc[1], t.pm[1], t.fs, cr);
            for (int i = i0; i < i0 + m; ++i)
            {
                L[i] = svfTick<OP, NS, NLM> (cl, zl, L[i], fat);
                if (t.nch == 2) R[i] = svfTick<OP, NS, NLM> (cr, zr, R[i], fat);
            }
        }
        std::copy_n (zl, 5, v.st[0]);
        std::copy_n (zr, 5, v.st[1]);
    }

    template <bool OP, int NS>
    static void svfNl (FilterVoice& v, const Tk& t, const SvfPlan& p) noexcept
    {
        if (p.nlm == 0) svfLoop<OP, NS, 0> (v, t, p);
        else if (p.nlm == 1) svfLoop<OP, NS, 1> (v, t, p);
        else svfLoop<OP, NS, 2> (v, t, p);
    }

    static void runSvf (FilterVoice& v, const Tk& t) noexcept
    {
        SvfPlan p;
        planSvf (t.type, t.res, t.var, p);
        if (p.op && p.ns == 0) svfNl<true, 0> (v, t, p);
        else if (p.op) svfNl<true, 1> (v, t, p);
        else if (p.ns == 1) svfNl<false, 1> (v, t, p);
        else svfNl<false, 2> (v, t, p);
    }

    // ---- Ladder ----
    template <int MODE, bool FAT>
    static void ladLoop (FilterVoice& v, const Tk& t, const LadderPlan& p) noexcept
    {
        float sl[4], sr[4];
        std::copy_n (v.st[0], 4, sl);
        std::copy_n (v.st[1], 4, sr);
        float* L = t.x[0];
        float* R = t.x[1];
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            const LadCo cl = ladCo (fc[0], p.k, t.fs), cr = t.same ? cl : ladCo (fc[1], p.k, t.fs);
            for (int i = i0; i < i0 + m; ++i)
            {
                L[i] = ladTick<MODE, FAT> (cl, p, sl, L[i]);
                if (t.nch == 2) R[i] = ladTick<MODE, FAT> (cr, p, sr, R[i]);
            }
        }
        std::copy_n (sl, 4, v.st[0]);
        std::copy_n (sr, 4, v.st[1]);
    }

    // TAP 1..4: output = stage TAP (low-pass 6..24 dB); 0: the plan's tap mix.
    // FAT: the feedback k*y4 (= x - ul exactly) is saturated before the input tanh.
    template <int TAP, bool FAT, int NCH>
    static void ladFastLoop (FilterVoice& v, const Tk& t, const LadderPlan& p) noexcept
    {
        float s[2][4];
        std::copy_n (v.st[0], 4, s[0]);
        std::copy_n (v.st[1], 4, s[1]);
        float* X[2] { t.x[0], t.x[1] };
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0), end = i0 + m;
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            LadFast co[2];
            co[0] = ladFast (fc[0], p, t.fs);
            co[1] = t.same ? co[0] : ladFast (fc[1], p, t.fs);
            float ul[2];
            for (int c = 0; c < NCH; ++c)
                ul[c] = co[c].d * X[c][i0] + (co[c].q[0] * s[c][0] + co[c].q[1] * s[c][1]) + (co[c].q[2] * s[c][2] + co[c].q[3] * s[c][3]);
            for (int i = i0; i < end; ++i)
                for (int c = 0; c < NCH; ++c)
                {
                    const LadFast& k = co[c];
                    float* sc = s[c];
                    float u;
                    if constexpr (FAT)
                    {
                        const float x = X[c][i], fb = x - ul[c];
                        u = fastTanh (x - (fb + p.fat * (fastTanh (fb) - fb)));
                    }
                    else
                        u = fastTanh (ul[c]);
                    const float n0 = k.r[0] * u + k.p[0] * sc[0];
                    const float n1 = k.r[1] * u + (k.p[1] * sc[0] + k.p[0] * sc[1]);
                    const float n2 = k.r[2] * u + (k.p[2] * sc[0] + k.p[1] * sc[1] + k.p[0] * sc[2]);
                    const float n3 = k.r[3] * u + ((k.p[3] * sc[0] + k.p[2] * sc[1]) + (k.p[1] * sc[2] + k.p[0] * sc[3]));
                    float out;
                    if constexpr (TAP == 1) out = n0 + sc[0];
                    else if constexpr (TAP == 2) out = n1 + sc[1];
                    else if constexpr (TAP == 3) out = n2 + sc[2];
                    else if constexpr (TAP == 4) out = n3 + sc[3];
                    else out = 2.0f * p.c[0] * u + p.c[1] * (n0 + sc[0]) + p.c[2] * (n1 + sc[1]) + p.c[3] * (n2 + sc[2]) + p.c[4] * (n3 + sc[3]);
                    if (i + 1 < end)
                        ul[c] = k.d * X[c][i + 1] + k.alpha * u + ((k.qp[0] * sc[0] + k.qp[1] * sc[1]) + (k.qp[2] * sc[2] + k.qp[3] * sc[3]));
                    sc[0] = n0; sc[1] = n1; sc[2] = n2; sc[3] = n3;
                    X[c][i] = 0.5f * k.comp * out;      // y_i = (s_i' + s_i) / 2
                }
        }
        std::copy_n (s[0], 4, v.st[0]);
        std::copy_n (s[1], 4, v.st[1]);
    }

    template <bool FAT>
    static void fastTaps (FilterVoice& v, const Tk& t, const LadderPlan& p) noexcept
    {
        const bool single = p.c[0] == 0.0f && (p.c[1] + p.c[2] + p.c[3] + p.c[4]) == 1.0f;
        if (t.nch == 1) fastTapsN<FAT, 1> (v, t, p, single);
        else fastTapsN<FAT, 2> (v, t, p, single);
    }

    template <bool FAT, int NCH>
    static void fastTapsN (FilterVoice& v, const Tk& t, const LadderPlan& p, bool single) noexcept
    {
        if (! single) ladFastLoop<0, FAT, NCH> (v, t, p);
        else if (p.c[1] == 1.0f) ladFastLoop<1, FAT, NCH> (v, t, p);
        else if (p.c[2] == 1.0f) ladFastLoop<2, FAT, NCH> (v, t, p);
        else if (p.c[3] == 1.0f) ladFastLoop<3, FAT, NCH> (v, t, p);
        else ladFastLoop<4, FAT, NCH> (v, t, p);
    }

    static void runLadder (FilterVoice& v, const Tk& t) noexcept
    {
        LadderPlan p;
        planLadder (t.type, t.res, t.var, p);
        if (p.mode == 0)
        {
            if (p.fat > 0.0f) fastTaps<true> (v, t, p);
            else fastTaps<false> (v, t, p);
        }
        else if (p.mode == 1) ladLoop<1, false> (v, t, p);
        else ladLoop<2, true> (v, t, p);
    }

    // ---- Diode ladder ----
    static void runDiode (FilterVoice& v, const Tk& t) noexcept
    {
        DiodePlan p;
        planDiode (t.type, t.res, p);
        float s[2][4];
        std::copy_n (v.st[0], 4, s[0]);
        std::copy_n (v.st[1], 4, s[1]);
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            DioCo co[2];
            co[0] = dioCo (fc[0], t.fs, p);
            co[1] = t.same ? co[0] : dioCo (fc[1], t.fs, p);
            for (int i = i0; i < i0 + m; ++i)
                for (int c = 0; c < t.nch; ++c)
                {
                    const DioCo& k = co[c];
                    float y[4];
                    for (int j = 0; j < 4; ++j)
                        y[j] = (k.M[j][0] * s[c][0] + k.M[j][1] * s[c][1]) + (k.M[j][2] * s[c][2] + k.M[j][3] * s[c][3]);
                    const float u = fastTanh ((t.x[c][i] - p.k * y[3]) * k.d);
                    for (int j = 0; j < 4; ++j)
                    {
                        y[j] += u * k.w[j];
                        s[c][j] = 2.0f * y[j] - s[c][j];
                    }
                    t.x[c][i] = p.comp * y[3];
                }
        }
        std::copy_n (s[0], 4, v.st[0]);
        std::copy_n (s[1], 4, v.st[1]);
    }

    // ---- Flanges ----
    static void runFlange (FilterVoice& v, const Tk& t) noexcept
    {
        FlangePlan p;
        planFlange (t.type, t.res, t.var, t.fs, p);
        const uint32_t mask = (uint32_t) v.dlSize - 1u;
        const float maxD = (float) v.dlSize - 2.0f;
        float z[2][8];
        for (int c = 0; c < 2; ++c) std::copy_n (v.st[c], 8, z[c]);
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            for (int c = 0; c < 2; ++c)
            {
                float* b = v.dl[c].data();
                float* x = t.x[c];
                float* zc = z[c];
                float Glp = p.Glp, Ghp = p.Ghp;
                float lpHz = p.lpHz, hpHz = p.hpHz;
                if (p.fmode == 2) { hpHz = fc[c] / p.wid; lpHz = fc[c] * p.wid; Ghp = onePoleG (hpHz, t.fs); Glp = onePoleG (lpHz, t.fs); }
                // The filter in the feedback loop delays the signal: shorten the
                // line by its phase delay at the comb frequency so the comb stays in tune.
                // (Band-limited loops are left alone: their low- and high-pass
                // phase shifts cancel around the cutoff.)
                float lagSamples = 0.0f;
                if (p.kind < 2 && p.fmode == 0)
                {
                    const float w = std::tan (kPi * std::min (fc[c], 0.49f * t.fs) / t.fs);
                    float phase = 0.0f;   // radians, negative = lag
                    // Only the low-pass lag: with a high-pass in the loop the ringing
                    // harmonics sit above its cutoff, where it adds almost no phase.
                    if (p.fmode != 1) phase -= std::atan (w / std::tan (kPi * std::min (lpHz, 0.49f * t.fs) / t.fs));
                    lagSamples = -phase * t.fs / (kTwoPi * fc[c]);
                }
                const float T = clampf ((p.kind == 1 ? 0.5f : 1.0f) * t.fs / fc[c] - lagSamples, 1.0f, maxD);
                if (p.kind < 2)
                {
                    for (int i = i0; i < i0 + m; ++i)
                    {
                        const uint32_t w = v.dlPos + (uint32_t) i;
                        const float wv = fbFilt (p.fmode, dlRead (b, mask, w, T), zc, Glp, Ghp);
                        b[w & mask] = x[i] + p.fb * fastTanh (wv);
                        x[i] = wv;
                    }
                }
                else
                {
                    const float Ga = onePoleG (fc[c], t.fs);
                    for (int i = i0; i < i0 + m; ++i)
                    {
                        float a = x[i] + p.fb * fastTanh (fbFilt (p.fmode, zc[6], zc, Glp, Ghp));
                        for (int j = 0; j < 4; ++j)
                        {
                            const float vv = (a - zc[2 + j]) * Ga, lp = vv + zc[2 + j];
                            zc[2 + j] = lp + vv;
                            a = 2.0f * lp - a;
                        }
                        zc[6] = a;
                        x[i] = a;
                    }
                }
            }
        }
        for (int c = 0; c < 2; ++c) std::copy_n (z[c], 8, v.st[c]);
    }

    // ---- Combs / Allpasses / Reverb / Comb 2 ----
    static void runMd (FilterVoice& v, const Tk& t) noexcept
    {
        MdPlan p;
        planMd (t.type, t.res, t.var, t.fs, p);
        const uint32_t reg = (uint32_t) v.dlSize / (uint32_t) p.nReg, mask = reg - 1u;
        const float maxD = (float) reg - 2.0f;
        const int nd = p.kind == 2 ? 6 : (p.kind == 3 ? 2 : 4);
        float z[2][8];
        for (int c = 0; c < 2; ++c) std::copy_n (v.st[c], 8, z[c]);
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            for (int c = 0; c < 2; ++c)
            {
                float* b = v.dl[c].data();
                float* x = t.x[c];
                float* zc = z[c];
                const float base = t.fs / fc[c];
                float T[6];
                for (int r = 0; r < nd; ++r) T[r] = clampf (base * p.ratio[r], 1.0f, maxD);
                for (int i = i0; i < i0 + m; ++i)
                {
                    const uint32_t w = v.dlPos + (uint32_t) i;
                    const float in = x[i];
                    float y = 0.0f;
                    if (p.kind == 1)        // Allpasses: 4 damped Schroeder all-passes in series
                    {
                        y = in;
                        for (int r = 0; r < 4; ++r)
                        {
                            float* br = b + r * reg;
                            const float D = dlRead (br, mask, w, T[r]);
                            const float u = y + p.ap * D;
                            const float vv = (u - zc[r]) * p.Gd, lp = vv + zc[r];
                            zc[r] = lp + vv;
                            br[w & mask] = lp;
                            y = D - p.ap * u;
                        }
                    }
                    else if (p.kind == 3)   // Comb 2
                    {
                        for (int r = 0; r < 2; ++r)
                        {
                            float* br = b + r * reg;
                            const float wv = dlRead (br, mask, w, T[r]);
                            br[w & mask] = in + p.fb * fastTanh (wv);
                            y += wv;
                        }
                        y *= 0.5f * p.wet;
                    }
                    else                    // Combs / Reverb: 4 damped feedback combs in parallel
                    {
                        for (int r = 0; r < 4; ++r)
                        {
                            float* br = b + r * reg;
                            const float wv = dlRead (br, mask, w, T[r]);
                            const float vv = (wv - zc[r]) * p.Gd, lp = vv + zc[r];
                            zc[r] = lp + vv;
                            br[w & mask] = in + p.fb * lp;
                            y += lp;
                        }
                        y *= 0.25f;
                        if (p.kind == 2)    // Reverb: then two plain all-passes
                            for (int r = 4; r < 6; ++r)
                            {
                                float* br = b + r * reg;
                                const float D = dlRead (br, mask, w, T[r]);
                                const float u = y + p.ap * D;
                                br[w & mask] = u;
                                y = D - p.ap * u;
                            }
                    }
                    x[i] = y;
                }
            }
        }
        for (int c = 0; c < 2; ++c) std::copy_n (z[c], 8, v.st[c]);
    }

    // ---- Dist.Comb: LP/BP SVF with a saturated comb in its feedback ----
    static void runDistComb (FilterVoice& v, const Tk& t) noexcept
    {
        DcPlan p;
        planDc (t.type, t.res, t.var, p);
        const uint32_t mask = (uint32_t) v.dlSize - 1u;
        const float T = clampf (t.fs / p.combHz, 1.0f, (float) v.dlSize - 2.0f);
        float z[2][2];
        for (int c = 0; c < 2; ++c) std::copy_n (v.st[c], 2, z[c]);
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            for (int c = 0; c < 2; ++c)
            {
                const SvfSt s = svfMake (gOf (fc[c], t.fs), p.k, p.bp ? mB : mL);
                float* b = v.dl[c].data();
                float* x = t.x[c];
                for (int i = i0; i < i0 + m; ++i)
                {
                    const uint32_t w = v.dlPos + (uint32_t) i;
                    const float u = x[i] + p.sfb * fastTanh (dlRead (b, mask, w, T));
                    const float y = svfStage<true> (s, z[c], u, 0.5f);
                    b[w & mask] = y;
                    x[i] = y;
                }
            }
        }
        for (int c = 0; c < 2; ++c) std::copy_n (z[c], 2, v.st[c]);
    }

    // ---- Scream: SVF with a high-passed, saturated positive feedback ----
    static void runScream (FilterVoice& v, const Tk& t) noexcept
    {
        ScPlan p;
        planScream (t.type, t.res, t.var, t.drive, t.fs, p);
        float z[2][4];
        for (int c = 0; c < t.nch; ++c) std::copy_n (v.st[c], 4, z[c]);
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            for (int c = 0; c < t.nch; ++c)
            {
                const SvfSt s = svfMake (gOf (fc[c], t.fs), p.k, p.bp ? mB : mL);
                float* x = t.x[c];
                float* zc = z[c];
                for (int i = i0; i < i0 + m; ++i)
                {
                    const float vv = (zc[3] - zc[2]) * p.Ghp, lp = vv + zc[2];
                    zc[2] = lp + vv;
                    const float u = x[i] + p.amt * fastTanh (zc[3] - lp);
                    const float y = svfStage<true> (s, zc, u, 0.5f);
                    zc[3] = y;
                    x[i] = y;
                }
            }
        }
        for (int c = 0; c < t.nch; ++c) std::copy_n (z[c], 4, v.st[c]);
    }

    // ---- Ring mod ----
    static void runRing (FilterVoice& v, const Tk& t) noexcept
    {
        const bool two = t.type == tRing2;
        const float sp = ringSpread (t.var);
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            for (int c = 0; c < t.nch; ++c)
            {
                float* x = t.x[c];
                float& p1 = v.st[c][0];
                float& p2 = v.st[c][1];
                if (! two)
                {
                    const float inc = fc[c] / t.fs;
                    for (int i = i0; i < i0 + m; ++i)
                    {
                        p1 += inc; p1 -= (float) (int) p1;
                        x[i] *= fastSin01 (p1);
                    }
                }
                else
                {
                    const float i1 = std::min (fc[c] / sp / t.fs, 0.99f), i2 = std::min (fc[c] * sp / t.fs, 0.99f);
                    for (int i = i0; i < i0 + m; ++i)
                    {
                        p1 += i1; p1 -= (float) (int) p1;
                        p2 += i2; p2 -= (float) (int) p2;
                        x[i] *= 0.5f * (fastSin01 (p1) + fastSin01 (p2));
                    }
                }
            }
        }
    }

    // ---- Sample & hold ----
    static void runSampHold (FilterVoice& v, const Tk& t) noexcept
    {
        const bool diff = t.type == tSHm;
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            for (int c = 0; c < t.nch; ++c)
            {
                float* x = t.x[c];
                float ph = v.st[c][0], hold = v.st[c][1];
                const float inc = fc[c] / t.fs;
                for (int i = i0; i < i0 + m; ++i)
                {
                    ph += inc;
                    const bool tick = ph >= 1.0f;
                    ph -= tick ? 1.0f : 0.0f;
                    hold = tick ? x[i] : hold;
                    x[i] = diff ? hold - x[i] : hold;
                }
                v.st[c][0] = ph;
                v.st[c][1] = hold;
            }
        }
    }

    // ---- Formants: three parallel band-passes ----
    static void runFormant (FilterVoice& v, const Tk& t) noexcept
    {
        FmPlan p;
        planFormant (t.type, t.res, t.var, p);
        float z[2][6];
        for (int c = 0; c < t.nch; ++c) std::copy_n (v.st[c], 6, z[c]);
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            for (int c = 0; c < t.nch; ++c)
            {
                SvfSt s[3];
                formantBands (p, fc[c], t.fs, s);
                float* x = t.x[c];
                float* zc = z[c];
                for (int i = i0; i < i0 + m; ++i)
                {
                    const float in = x[i];
                    x[i] = svfStage<false> (s[0], zc, in, 0.0f) + svfStage<false> (s[1], zc + 2, in, 0.0f)
                         + svfStage<false> (s[2], zc + 4, in, 0.0f);
                }
            }
        }
        for (int c = 0; c < t.nch; ++c) std::copy_n (z[c], 6, v.st[c]);
    }

    // ---- Diffusor: STAGES 2nd-order all-passes in series ----
    static void runDiffusor (FilterVoice& v, const Tk& t) noexcept
    {
        const int ns = diffStages (t.var);
        const float k = diffK (t.res);
        for (int i0 = 0; i0 < t.n; i0 += kSub)
        {
            const int m = std::min (kSub, t.n - i0);
            float fc[2];
            cutoffs (v, t, i0, m, fc);
            const SvfSt sl = svfMake (gOf (fc[0], t.fs), k, { 1.0f, -1.0f, 1.0f });
            const SvfSt sr = t.same ? sl : svfMake (gOf (fc[1], t.fs), k, { 1.0f, -1.0f, 1.0f });
            float* zl = v.st[0];
            float* zr = v.st[1];
            for (int i = i0; i < i0 + m; ++i)
            {
                if (t.nch == 1)
                {
                    float a = t.x[0][i];
                    for (int j = 0; j < ns; ++j) a = svfStage<false> (sl, zl + 2 * j, a, 0.0f);
                    t.x[0][i] = a;
                    continue;
                }
                float a = t.x[0][i], b = t.x[1][i];    // both channels per stage: two independent chains
                for (int j = 0; j < ns; ++j)
                {
                    a = svfStage<false> (sl, zl + 2 * j, a, 0.0f);
                    b = svfStage<false> (sr, zr + 2 * j, b, 0.0f);
                }
                t.x[0][i] = a;
                t.x[1][i] = b;
            }
        }
    }

    // ---- One chunk (<= kTick samples) ----
    // Mono: only the left channel is filtered, and R's state is left "stale"
    // (it logically equals L's). This happens for processMono(), and for
    // process() when pan is 0, L and R inputs are bitwise equal and R is stale
    // (then R's output is a copy of L's). Before a genuine stereo run, L's state
    // is copied into R. Delay types always run both channels (mono: on a scratch
    // copy of L) so their two delay lines stay in step.
    static void chunk (FilterVoice& v, const FilterTick& ft, int off, int n, float fr0, float fr1, bool mono) noexcept
    {
        const int type = std::clamp (ft.type, 0, FilterVoice::kNumTypes - 1);
        const bool delayFam = usesDelay (familyOf (type));
        if (mono && delayFam) std::copy_n (ft.L + off, n, v.scratchR);
        const bool autoMono = ! mono && ! delayFam && v.rStale && ft.pan == 0.0f
                              && std::memcmp (ft.L + off, ft.R + off, sizeof (float) * (size_t) n) == 0;
        float* X[2] { ft.L + off, mono ? (delayFam ? v.scratchR : nullptr) : ft.R + off };
        const int nch = ((mono || autoMono) && ! delayFam) ? 1 : 2;
        if (nch == 2 && v.rStale)
        {
            std::copy_n (v.st[0], FilterVoice::kStateSize, v.st[1]);
            v.smoothLc[1] = v.smoothLc[0];
            v.lastWet[1] = v.lastWet[0];
            v.declick[1] = v.declick[0];
        }
        v.rStale = nch == 1;
        float dry[2][kTick];
        for (int c = 0; c < nch; ++c) std::copy_n (X[c], n, dry[c]);

        bool switched = false;
        if (type != v.curType)
        {
            switched = v.curType >= 0;
            v.clearState();
            v.curType = type;
        }
        const Fam fam = familyOf (type);

        Tk t;
        t.type = type;
        t.n = n;
        t.fs = (float) v.fs;
        t.res = clamp01 (ft.res);
        t.var = clamp01 (ft.var);
        t.drive = clamp01 (ft.drive);
        t.x[0] = X[0];
        t.x[1] = X[1];
        t.nch = nch;
        const float lA = v.log2Hz (ft.cutoffHzPrev), lB = v.log2Hz (ft.cutoffHz);
        const float l0 = lerp (lA, lB, fr0), l1 = lerp (lA, lB, fr1);
        const float pan = clampf (ft.pan, -1.0f, 1.0f);
        t.lc0[0] = l0 - pan; t.lc1[0] = l1 - pan;       // pan < 0: left cutoff up, right down
        t.lc0[1] = l0 + pan; t.lc1[1] = l1 + pan;
        if (pan != 0.0f)
        {
            t.pm[0] = exp2Fast (-pan);
            t.pm[1] = exp2Fast (pan);
        }
        if (smoothType (type))
        {
            const float tau = t.var * t.var * 0.25f;    // SMOOTH: up to 250 ms
            t.smoothA = tau < 1.0e-4f ? 1.0f : 1.0f - std::exp (-(float) kSub / (tau * t.fs));
            if (! v.smoothInit)
            {
                v.smoothLc[0] = t.lc0[0];
                v.smoothLc[1] = t.lc0[1];
                v.smoothInit = true;
            }
        }
        if (type == tMgDirty) t.pain = t.var;
        t.same = pan == 0.0f;       // (PAIN jitter is then shared too: mono in, mono out)

        // Input stage. Model types: DRIVE is gain into the model's own saturation.
        // Others: a soft clipper blended in by DRIVE. Clean mode wraps the model
        // (filter kernel) in -24 dB / +24 dB.
        const float cs = ft.cleanDrive ? 1.0f / 16.0f : 1.0f;
        const float gD = t.drive > 0.0f ? exp2Fast (t.drive * 3.98631371f) : 1.0f;    // 0..+24 dB
        const bool md = modelDrive (type);
        const float g = (md ? gD : 1.0f) * cs;
        for (int c = 0; c < nch; ++c)
        {
            float* x = X[c];
            if (! md && t.drive > 0.0f)
                for (int i = 0; i < n; ++i) x[i] += t.drive * (fastTanh (x[i] * gD) - x[i]);
            if (g != 1.0f)
                for (int i = 0; i < n; ++i) x[i] *= g;
        }

        switch (fam)
        {
            case Fam::Svf:        runSvf (v, t); break;
            case Fam::Ladder:     runLadder (v, t); break;
            case Fam::Diode:      runDiode (v, t); break;
            case Fam::Flange:     runFlange (v, t); break;
            case Fam::MultiDelay: runMd (v, t); break;
            case Fam::DistComb:   runDistComb (v, t); break;
            case Fam::Scream:     runScream (v, t); break;
            case Fam::Ring:       runRing (v, t); break;
            case Fam::SampHold:   runSampHold (v, t); break;
            case Fam::Formant:    runFormant (v, t); break;
            case Fam::Diffusor:   runDiffusor (v, t); break;
        }
        if (usesDelay (fam))
        {
            v.dlPos += (uint32_t) n;
            v.dlDirty = true;
        }

        // Clean-mode makeup, then a safety net: a blown-up state is cleared.
        int32_t bad = 0;
        for (int c = 0; c < nch; ++c)
        {
            float* x = X[c];
            if (cs != 1.0f)
                for (int i = 0; i < n; ++i) x[i] *= 16.0f;
            for (int i = 0; i < n; ++i) bad |= badBits (x[i]);
        }
        if (bad < 0)
        {
            v.clearState();
            for (int c = 0; c < nch; ++c) std::fill_n (X[c], n, 0.0f);
        }

        // Type switch: start the new type where the old one left off, decaying the offset.
        for (int c = 0; c < nch; ++c)
        {
            float* x = X[c];
            if (switched) v.declick[c] = v.lastWet[c] - x[0];
            float d = v.declick[c];
            if (d != 0.0f)
            {
                for (int i = 0; i < n; ++i) { x[i] += d; d *= v.declickCoef; }
                v.declick[c] = std::fabs (d) < 1.0e-6f ? 0.0f : d;
            }
            v.lastWet[c] = x[n - 1];
        }

        // Dry/wet, ramped from the previous tick's MIX.
        const float mix = clamp01 (ft.mix);
        const float m0 = v.mixPrev < 0.0f ? mix : v.mixPrev;
        if (mix < 1.0f || m0 < 1.0f)
        {
            const float dm = (mix - m0) / (float) n;
            for (int c = 0; c < nch; ++c)
                for (int i = 0; i < n; ++i)
                {
                    const float mm = m0 + dm * (float) (i + 1);
                    X[c][i] = dry[c][i] + mm * (X[c][i] - dry[c][i]);
                }
        }
        v.mixPrev = mix;

        // Snap decayed state to zero (feedback loops that would go denormal).
        if (autoMono) std::copy_n (X[0], n, X[1]);

        const int used = fam == Fam::Diffusor ? FilterVoice::kStateSize : 8;
        for (int c = 0; c < nch; ++c)
            for (int i = 0; i < used; ++i) v.st[c][i] = std::fabs (v.st[c][i]) < 1.0e-15f ? 0.0f : v.st[c][i];
    }
};

// ---------------------------------------------------------------------------
// 4. FilterVoice
// ---------------------------------------------------------------------------
void FilterVoice::prepare (const Context& ctx)
{
    fs = ctx.sampleRate > 0.0 ? ctx.sampleRate : 48000.0;
    invFs = (float) (1.0 / fs);
    const int need = (int) std::ceil (fs / 8.0) + 8;      // 8 Hz comb at this rate
    dlSize = 1;
    while (dlSize < need) dlSize <<= 1;
    for (auto& d : dl) d.assign ((size_t) dlSize, 0.0f);
    dlDirty = false;
    declickCoef = (float) std::exp (-1.0 / (0.004 * fs));    // 4 ms
    (void) diodeCal (tAcid);                                  // build the static tables off the audio thread
    (void) formantGains();
    reset();
}

void FilterVoice::clearState()
{
    for (auto& c : st) std::fill (std::begin (c), std::end (c), 0.0f);
    smoothInit = false;
    if (dlDirty)
    {
        for (auto& d : dl) std::fill (d.begin(), d.end(), 0.0f);
        dlDirty = false;
    }
}

void FilterVoice::reset()
{
    clearState();
    curType = -1;
    rStale = true;          // both states are zero: R equals L
    for (int c = 0; c < 2; ++c) { lastWet[c] = 0.0f; declick[c] = 0.0f; }
    mixPrev = -1.0f;
}

void FilterVoice::process (FilterTick& t)
{
    const int n = t.n;
    if (n <= 0 || t.L == nullptr || t.R == nullptr || dlSize == 0) return;
    for (int off = 0; off < n; off += kTick)
    {
        const int m = std::min (kTick, n - off);
        processChunk (t, off, m, (float) off / (float) n, (float) (off + m) / (float) n, false);
    }
}

void FilterVoice::processMono (FilterTick& t)
{
    const int n = t.n;
    if (n <= 0 || t.L == nullptr || dlSize == 0) return;
    for (int off = 0; off < n; off += kTick)
    {
        const int m = std::min (kTick, n - off);
        processChunk (t, off, m, (float) off / (float) n, (float) (off + m) / (float) n, true);
    }
}

void FilterVoice::processChunk (FilterTick& t, int offset, int n, float frac0, float frac1, bool mono)
{
    FilterImpl::chunk (*this, t, offset, n, frac0, frac1, mono);
}

bool FilterVoice::hasVar (int type)
{
    const char* l = choices::filterVarLabel (type);
    return l != nullptr && l[0] != 0;
}

// ---------------------------------------------------------------------------
// 5. Analytic response
// ---------------------------------------------------------------------------
namespace
{
cpx svfResp (const SvfPlan& p, float fc, float fs, cpx W) noexcept
{
    cpx H = 1.0;
    if (p.op)
    {
        const cpx lp = 1.0 / (1.0 + W / (double) gOf (fc, fs));
        H *= (double) p.opL * lp + (double) p.opH * (1.0 - lp);
    }
    for (int j = 0; j < p.ns; ++j)
    {
        const StageRes r = resolveStage (p, j, fc, 1.0f, fs);
        const cpx s = W / (double) r.g;
        const double k = r.k;
        H *= ((double) r.m.l + (double) r.m.b * k * s + (double) r.m.h * s * s) / (s * s + k * s + 1.0);
    }
    return H;
}

cpx svfStageResp (float g, float k, Mix3 m, cpx W) noexcept
{
    const cpx s = W / (double) g;
    return ((double) m.l + (double) m.b * k * s + (double) m.h * s * s) / (s * s + (double) k * s + 1.0);
}

cpx onePoleLp (float f, float fs, cpx W) noexcept { return 1.0 / (1.0 + W / (double) gOf (f, fs)); }
} // namespace

float FilterVoice::magnitudeDb (int type, float cutoffHz, float res, float drive, float var, float freqHz, double sampleRate)
{
    type = std::clamp (type, 0, kNumTypes - 1);
    const float fs = (float) sampleRate;
    res = clamp01 (res); var = clamp01 (var); drive = clamp01 (drive);
    const float fc = clampFc (cutoffHz, fs);
    const double f = std::clamp ((double) freqHz, 0.0, 0.4999 * sampleRate);
    const double w = 2.0 * kPiD * f / sampleRate;
    const cpx W (0.0, std::tan (0.5 * w));
    const cpx z1 = std::polar (1.0, -w);
    auto delay = [&] (double T) { return std::polar (1.0, -w * T); };

    const float gD = dbToGain (24.0f * drive);
    double gain = modelDrive (type) ? gD : 1.0 + drive * (gD - 1.0f);
    cpx H = 1.0;

    switch (familyOf (type))
    {
        case Fam::Svf:
        {
            SvfPlan p;
            planSvf (type, res, var, p);
            H = svfResp (p, fc, fs, W);
            break;
        }
        case Fam::Ladder:
        {
            LadderPlan p;
            planLadder (type, res, var, p);
            const cpx h1 = 1.0 / (1.0 + W / (double) gOf (fc, fs));
            const cpx h4 = h1 * h1 * h1 * h1;
            cpx sum = p.c[0], hp = 1.0;
            for (int j = 1; j <= 4; ++j) { hp *= h1; sum += (double) p.c[j] * hp; }
            H = (double) p.comp * sum / (1.0 + (double) p.k * h4);
            if (p.mode == 2) gain *= std::pow (1.0 - (double) p.tb * p.tb, 3.0);
            break;
        }
        case Fam::Diode:
        {
            DiodePlan p;
            planDiode (type, res, p);
            const cpx h4 = diodeH4 (W / (double) (gOf (fc, fs) / p.w180), p.dLast);
            H = (double) p.comp * h4 / (1.0 + (double) p.k * h4);
            break;
        }
        case Fam::Flange:
        {
            FlangePlan p;
            planFlange (type, res, var, fs, p);
            cpx F = 1.0;
            if (p.fmode == 0) F = onePoleLp (p.lpHz, fs, W);
            else if (p.fmode == 1) F = 1.0 - onePoleLp (p.hpHz, fs, W);
            else F = (1.0 - onePoleLp (fc / p.wid, fs, W)) * onePoleLp (fc * p.wid, fs, W);
            if (p.kind < 2)
            {
                const int size = [&] { int s = 1; while (s < (int) std::ceil (sampleRate / 8.0) + 8) s <<= 1; return s; }();
                const double T = clampf ((p.kind == 1 ? 0.5f : 1.0f) * fs / fc, 1.0f, (float) size - 2.0f);
                const cpx e = delay (T);
                H = F * e / (1.0 - (double) p.fb * F * e);
            }
            else
            {
                const cpx s = W / (double) gOf (fc, fs);
                const cpx ap = (1.0 - s) / (1.0 + s);
                const cpx a4 = ap * ap * ap * ap;
                H = a4 / (1.0 - (double) p.fb * F * z1 * a4);
            }
            break;
        }
        case Fam::MultiDelay:
        {
            MdPlan p;
            planMd (type, res, var, fs, p);
            int size = 1;
            while (size < (int) std::ceil (sampleRate / 8.0) + 8) size <<= 1;
            const float maxD = (float) (size / p.nReg) - 2.0f;
            const float base = fs / fc;
            const cpx D = onePoleLp (22000.0f * std::exp2 (-10.0f * var), fs, W);
            auto T = [&] (int r) { return (double) clampf (base * p.ratio[r], 1.0f, maxD); };
            if (p.kind == 1)
            {
                for (int r = 0; r < 4; ++r)
                {
                    const cpx e = D * delay (T (r));
                    H *= (e - (double) p.ap) / (1.0 - (double) p.ap * e);
                }
            }
            else if (p.kind == 3)
            {
                H = 0.0;
                for (int r = 0; r < 2; ++r) { const cpx e = delay (T (r)); H += e / (1.0 - (double) p.fb * e); }
                H *= 0.5 * p.wet;
            }
            else
            {
                H = 0.0;
                for (int r = 0; r < 4; ++r) { const cpx e = delay (T (r)); H += D * e / (1.0 - (double) p.fb * D * e); }
                H *= 0.25;
                if (p.kind == 2)
                    for (int r = 4; r < 6; ++r) { const cpx e = delay (T (r)); H *= (e - (double) p.ap) / (1.0 - (double) p.ap * e); }
            }
            break;
        }
        case Fam::DistComb:
        {
            DcPlan p;
            planDc (type, res, var, p);
            int size = 1;
            while (size < (int) std::ceil (sampleRate / 8.0) + 8) size <<= 1;
            const double T = clampf (fs / p.combHz, 1.0f, (float) size - 2.0f);
            const cpx hs = svfStageResp (gOf (fc, fs), p.k, p.bp ? mB : mL, W);
            H = hs / (1.0 - (double) p.sfb * hs * delay (T));
            break;
        }
        case Fam::Scream:
        {
            ScPlan p;
            planScream (type, res, var, drive, fs, p);
            const cpx hs = svfStageResp (gOf (fc, fs), p.k, p.bp ? mB : mL, W);
            const cpx hp = 1.0 - onePoleLp (p.hpHz, fs, W);
            H = hs / (1.0 - std::min (0.9, (double) p.amt) * hp * z1 * hs);
            break;
        }
        case Fam::Ring:
            H = 0.5;        // each sideband carries half the input amplitude
            break;
        case Fam::SampHold:
        {
            const double x = kPiD * f / (double) fc;     // zero-order hold at the S&H rate
            const cpx zoh = x < 1.0e-9 ? cpx (1.0) : std::polar (std::sin (x) / x, -x);
            H = type == tSHm ? zoh - 1.0 : zoh;
            break;
        }
        case Fam::Formant:
        {
            FmPlan p;
            planFormant (type, res, var, p);
            SvfSt s[3];
            formantBands (p, fc, fs, s);
            H = 0.0;
            for (int j = 0; j < 3; ++j)
            {
                // Recover g, k and the band gain from the stage (a2 = g a1, a1 = 1 / (1 + g (g + k))).
                const double g = (double) s[j].a2 / (double) s[j].a1;
                const double k = (1.0 / (double) s[j].a1 - 1.0) / g - g;
                const cpx S = W / g;
                H += (double) s[j].m1 * S / (S * S + k * S + 1.0);
            }
            break;
        }
        case Fam::Diffusor:
        {
            const cpx a = svfStageResp (gOf (fc, fs), diffK (res), { 1.0f, -1.0f, 1.0f }, W);
            H = std::pow (a, diffStages (var));
            break;
        }
    }
    const double mag = std::abs (H) * gain;
    return (float) (20.0 * std::log10 (std::max (mag, 1.0e-6)));
}
} // namespace tincture
