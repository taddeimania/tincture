// Filter.cpp — the FX-rack Filter module (global, stereo). Its own small set
// of types (not the voice filter code): Moog-style ZDF ladder low-passes
// with a saturating input (MG Low 6..24), TPT state-variable types, an acid
// ladder (saturated stages, high-passed feedback) and two combs.
//
// Slots: 0 TYPE, 1 CUTOFF, 2 RES, 3 DRIVE, 4 VAR (FAT / DAMP), 5 PAN (stereo
//        cutoff offset, +-1 oct), 6 KEY TRACK, 7 CLEAN DRIVE, 15 LEVEL.
#include "FxEngine.h"

#include <complex>

namespace tincture
{
namespace
{
using namespace fxdsp;
using namespace fxchoices;
constexpr FxType T = FxType::Filter;

struct Ladder
{
    float s[4] {};
    float hpz = 0.0f;               // acid: high-pass in the feedback path
    void clear() { s[0] = s[1] = s[2] = s[3] = hpz = 0.0f; }
};

class Filter final : public FxModule
{
public:
    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        for (auto& d : comb) d.init ((int) (sr / 15.0f) + 8);
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            lad[c].clear(); sv1[c].clear(); sv2[c].clear(); comb[c].clear(); damp[c] = OnePole();
        }
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const int type = fxInfo (T).params[0].index (b.v[0]);
        const bool key = b.v[6] >= 0.5f, clean = b.v[7] >= 0.5f;
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            float fc = fxPlain (T, 1, v[1]);
            if (key) fc *= std::exp2 ((float) (b.note - 60) * (1.0f / 12.0f));
            const float pan = fxPlain (T, 5, v[5]);
            const float res = v[2], drive = v[3], var = v[4];
            float gIn = 1.0f + drive * 15.0f, gOut = 1.0f;
            if (clean) { gIn *= 1.0f / 16.0f; gOut = 16.0f; }
            for (int c = 0; c < 2; ++c)
            {
                const float f = clampf (fc * std::exp2 (c == 0 ? -pan : pan), 10.0f, sr * 0.45f);
                runChannel (type, c, c == 0 ? L + off : R + off, m, f, res, gIn, gOut, var, drive);
            }
        }
    }

private:
    void runChannel (int type, int c, float* x, int m, float fc, float res, float gIn, float gOut, float var, float drive)
    {
        if (type <= FMgLow24 || type == FAcidLadder)
        {
            const float g = std::tan (kPi * fc / sr);
            const float G = g / (1.0f + g), inv1g = 1.0f / (1.0f + g);
            const float G2 = G * G, G3 = G2 * G, G4 = G2 * G2;
            Ladder& ld = lad[c];
            if (type == FAcidLadder)
            {
                const float k = res * 4.3f;
                const float hpA = onePoleCoef (100.0f, sr);
                for (int i = 0; i < m; ++i)
                {
                    const float S = (G3 * ld.s[0] + G2 * ld.s[1] + G * ld.s[2] + ld.s[3]) * inv1g;
                    const float y4e = (G4 * x[i] * gIn + S) / (1.0f + k * G4);
                    ld.hpz += hpA * (y4e - ld.hpz);
                    float u = fastTanh (x[i] * gIn - k * (y4e - ld.hpz));
                    for (int st = 0; st < 4; ++st)
                    {
                        const float vv = (u - ld.s[st]) * G;
                        const float y = vv + ld.s[st];
                        ld.s[st] = y + vv;
                        u = st < 3 ? fastTanh (y * 1.2f) * (1.0f / 1.2f) : y;
                    }
                    x[i] = u * gOut * (1.0f + var * k * 0.5f);
                }
                return;
            }
            const float k = res * 3.98f;
            const int tap = type;            // 0..3 -> pole 1..4
            const float fat = 1.0f + var * k * 0.5f;
            for (int i = 0; i < m; ++i)
            {
                const float S = (G3 * ld.s[0] + G2 * ld.s[1] + G * ld.s[2] + ld.s[3]) * inv1g;
                const float y4e = (G4 * x[i] * gIn + S) / (1.0f + k * G4);
                float u = fastTanh (x[i] * gIn - k * y4e);
                float y[4];
                for (int st = 0; st < 4; ++st)
                {
                    const float vv = (u - ld.s[st]) * G;
                    y[st] = vv + ld.s[st];
                    ld.s[st] = y[st] + vv;
                    u = y[st];
                }
                x[i] = y[tap] * gOut * fat;
            }
            for (float& s : ld.s) if (std::abs (s) < 1.0e-15f) s = 0.0f;
            return;
        }
        if (type == FCombPlus || type == FCombMinus)
        {
            const float d = clampf (sr / fc, 2.0f, (float) comb[c].capacity() - 4.0f);
            const float fb = (type == FCombPlus ? 1.0f : -1.0f) * res * 0.97f;
            const float a = 1.0f - var * 0.9f;               // DAMP: 1-pole in the loop
            DelayLine& dl = comb[c];
            OnePole& lp = damp[c];
            const bool sat = drive > 0.0f;
            for (int i = 0; i < m; ++i)
            {
                const float in = sat ? fastTanh (x[i] * gIn) : x[i] * gIn;
                const float y = in + fb * lp.lp (dl.hermite (d - 1.0f), a);
                dl.push (y + kDenorm);
                x[i] = y * (1.0f - std::abs (fb) * 0.5f) * gOut;
            }
            return;
        }
        // State-variable types.
        const float q = 0.5f + res * res * 24.0f;
        SvfCoefs co;
        co.set (fc, q, sr);
        const bool sat = drive > 0.0f || gOut != 1.0f;
        const bool two = type == FLow24 || type == FHigh24 || type == FBand24 || type == FNotch24;
        Svf& a = sv1[c];
        Svf& b2 = sv2[c];
        for (int i = 0; i < m; ++i)
        {
            const float in = sat ? fastTanh (x[i] * gIn) : x[i];
            float bp, hp, lp = a.tick (co, in, bp, hp), y = 0.0f;
            switch (type)
            {
                case FLow12: case FLow24:     y = lp; break;
                case FHigh12: case FHigh24:   y = hp; break;
                case FBand12: case FBand24:   y = bp * co.k; break;     // unity peak
                case FPeak12:                 y = lp - hp; break;
                default:                      y = in - co.k * bp; break; // notch
            }
            if (two)
            {
                float bp2, hp2, lp2 = b2.tick (co, y, bp2, hp2);
                switch (type)
                {
                    case FLow24:  y = lp2; break;
                    case FHigh24: y = hp2; break;
                    case FBand24: y = bp2 * co.k; break;
                    default:      y = y - co.k * bp2; break;
                }
            }
            x[i] = y * gOut;
        }
    }

    float sr = 48000.0f;
    SlotSmoother sm;
    Ladder lad[2];
    Svf sv1[2], sv2[2];
    DelayLine comb[2];
    OnePole damp[2];
};
} // namespace

std::unique_ptr<FxModule> makeFilter() { return std::make_unique<Filter>(); }

// Linear (small-signal) response for the display.
float filterMagnitudeDb (const float* v, float hz, double sampleRate)
{
    using C = std::complex<double>;
    const double sr = sampleRate;
    const int type = fxInfo (T).params[0].index (v[0]);
    const double fc = clampf (fxPlain (T, 1, v[1]), 10.0f, (float) sr * 0.45f);
    const double res = v[2];
    const C z = std::polar (1.0, 2.0 * 3.14159265358979323846 * hz / sr);
    const C zi = 1.0 / z;
    const double g = std::tan (3.14159265358979323846 * fc / sr);
    C h;
    if (type <= FMgLow24 || type == FAcidLadder)
    {
        const C h1 = g * (1.0 + zi) / ((1.0 + g) + (g - 1.0) * zi);       // TPT one-pole
        const double k = type == FAcidLadder ? res * 4.3 : res * 3.98;
        const int tap = type == FAcidLadder ? 3 : type;
        h = std::pow (h1, tap + 1) / (1.0 + k * std::pow (h1, 4));
        h *= 1.0 + v[4] * k * 0.5;
    }
    else if (type == FCombPlus || type == FCombMinus)
    {
        const double d = sr / fc;
        const double fb = (type == FCombPlus ? 1.0 : -1.0) * res * 0.97;
        h = (1.0 - std::abs (fb) * 0.5) / (1.0 - fb * std::pow (zi, d));
    }
    else
    {
        const double q = 0.5 + res * res * 24.0, k = 1.0 / q;
        // Bilinear SVF: s -> (1 - z^-1) / (g (1 + z^-1))
        const C s = (1.0 - zi) / (g * (1.0 + zi));
        const C den = s * s + k * s + 1.0;
        const C lp = 1.0 / den, bp = s / den, hp = s * s / den;
        switch (type)
        {
            case FLow12:  h = lp; break;
            case FLow24:  h = lp * lp; break;
            case FHigh12: h = hp; break;
            case FHigh24: h = hp * hp; break;
            case FBand12: h = bp * k; break;
            case FBand24: h = bp * bp * k * k; break;
            case FPeak12: h = lp - hp; break;
            case FNotch12: h = 1.0 - k * bp; break;
            default:      h = (1.0 - k * bp) * (1.0 - k * bp); break;
        }
    }
    return (float) (20.0 * std::log10 (std::max (std::abs (h), 1.0e-9)));
}
} // namespace tincture
