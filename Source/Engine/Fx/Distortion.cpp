// Distortion.cpp — the Distortion FX module: 13 shaper modes run at 2x
// oversampling (half-band IIR), a sample-rate reducer (Downsample), the
// X-Shaper modes reading two user curves, a DC blocker, and an optional
// LP->BP->HP morphing state-variable filter before or after the shaper.
//
// Slots: 0 MODE, 1 FILTER (Off/Pre/Post), 2 TYPE (LP..BP..HP), 3 FREQ, 4 Q,
//        5 DRIVE, 6 KEY TRACK (filter), 15 LEVEL.
// DRIVE: pre-gain 0..+36 dB; Downsample: rate reduction 1..64x;
// X-Shaper modes: crossfade curve A (0%) -> B (100%), no pre-gain.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
using namespace fxchoices;
constexpr FxType T = FxType::Distortion;

struct DistExtra
{
    BakedCurve a, b;
};

inline float linFold (float x) noexcept
{
    // Triangle fold into -1..1.
    float t = (x + 1.0f) * 0.25f;
    t -= std::floor (t);
    return std::abs (t * 4.0f - 2.0f) - 1.0f;
}

template <int Mode>
inline float shape (float x, const DistExtra* e, float xfade) noexcept
{
    switch (Mode)
    {
        case Tube:       return fastTanh (x + 0.15f) - 0.14888f;                // tanh(0.15)
        case SoftClip:   { const float c = clampf (x, -1.0f, 1.0f); return 1.5f * c - 0.5f * c * c * c; }
        case HardClip:   return clampf (x, -1.0f, 1.0f);
        case Diode1:     return x >= 0.0f ? 1.0f - std::exp (-x) : -0.3f * (1.0f - std::exp (x * 3.0f));
        case Diode2:     { const float a = 1.0f - std::exp (-1.5f * std::abs (x)); return x >= 0.0f ? a : -a; }
        case LinFold:    return linFold (x);
        case SinFold:    return std::sin (x * (kPi * 0.5f));
        case ZeroSquare: return clampf (x * std::abs (x), -1.0f, 1.0f);
        case Asym:       return x > 0.0f ? std::min (x, 1.0f) : x / (1.0f - x);
        case Rectify:    return fastTanh (std::abs (x));
        case XShaper:
        {
            const float a = std::min (std::abs (x), 1.0f);
            const float y = e->a (a) + (e->b (a) - e->a (a)) * xfade;
            return x >= 0.0f ? y : -y;
        }
        case XShaperAsym:
        {
            const float u = clampf ((x + 1.0f) * 0.5f, 0.0f, 1.0f);
            return 2.0f * (e->a (u) + (e->b (u) - e->a (u)) * xfade) - 1.0f;
        }
        default:         return x;
    }
}

class Distortion final : public FxModule
{
public:
    void prepare (double s, int) override { sr = (float) s; sm.prepare (s); dcA = onePoleCoef (8.0f, sr); }
    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            up[c].clear(); dn[c].clear(); flt[c].clear(); dc[c] = OnePole(); held[c] = 0.0f;
        }
        holdPhase = 0.0f;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const auto& P = fxInfo (T).params;
        const int mode = P[0].index (b.v[0]);
        const int fpos = P[1].index (b.v[1]);
        const auto* e = static_cast<const DistExtra*> (b.extra);
        static const DistExtra fallback {};
        if (! e) e = &fallback;
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            float* l = L + off;
            float* r = R + off;
            float fhz = fxPlain (T, 3, v[3]);
            if (b.v[6] >= 0.5f) fhz *= std::exp2 ((float) (b.note - 60) * (1.0f / 12.0f));
            fc.set (fhz, fxPlain (T, 4, v[4]), sr);
            morph = v[2];
            if (fpos == 1) filter (l, r, m);
            const float drive = v[5];
            const float gain = dbToGain (drive * 36.0f);
            switch (mode)
            {
                case Tube:        run<Tube> (l, r, m, gain, e, drive); break;
                case SoftClip:    run<SoftClip> (l, r, m, gain, e, drive); break;
                case HardClip:    run<HardClip> (l, r, m, gain, e, drive); break;
                case Diode1:      run<Diode1> (l, r, m, gain, e, drive); break;
                case Diode2:      run<Diode2> (l, r, m, gain, e, drive); break;
                case LinFold:     run<LinFold> (l, r, m, gain, e, drive); break;
                case SinFold:     run<SinFold> (l, r, m, gain, e, drive); break;
                case ZeroSquare:  run<ZeroSquare> (l, r, m, gain, e, drive); break;
                case Asym:        run<Asym> (l, r, m, gain, e, drive); break;
                case Rectify:     run<Rectify> (l, r, m, gain, e, drive); break;
                case XShaper:     run<XShaper> (l, r, m, 1.0f, e, drive); break;
                case XShaperAsym: run<XShaperAsym> (l, r, m, 1.0f, e, drive); break;
                default:          downsample (l, r, m, drive); break;
            }
            if (fpos == 2) filter (l, r, m);
        }
    }

private:
    template <int Mode>
    void run (float* l, float* r, int m, float gain, const DistExtra* e, float xfade)
    {
        float* ch[2] { l, r };
        for (int c = 0; c < 2; ++c)
        {
            float* x = ch[c];
            Halfband& u = up[c];
            Halfband& d = dn[c];
            OnePole& z = dc[c];
            for (int i = 0; i < m; ++i)
            {
                float a, bb;
                u.up (x[i] * gain, a, bb);
                const float y = d.down (shape<Mode> (a, e, xfade), shape<Mode> (bb, e, xfade));
                x[i] = y - z.lp (y, dcA);
            }
        }
    }

    void downsample (float* l, float* r, int m, float drive)
    {
        const float step = 1.0f / (1.0f + drive * drive * 63.0f);
        for (int i = 0; i < m; ++i)
        {
            holdPhase += step;
            if (holdPhase >= 1.0f) { holdPhase -= 1.0f; held[0] = l[i]; held[1] = r[i]; }
            if (step >= 1.0f) { held[0] = l[i]; held[1] = r[i]; }
            l[i] = held[0];
            r[i] = held[1];
        }
    }

    void filter (float* l, float* r, int m)
    {
        // LP (0) -> BP (0.5) -> HP (1).
        const float t = morph * 2.0f;
        const float wl = t < 1.0f ? 1.0f - t : 0.0f, wb = t < 1.0f ? t : 2.0f - t, wh = t > 1.0f ? t - 1.0f : 0.0f;
        float* ch[2] { l, r };
        for (int c = 0; c < 2; ++c)
        {
            Svf& f = flt[c];
            float* x = ch[c];
            for (int i = 0; i < m; ++i)
            {
                float bp, hp;
                const float lp = f.tick (fc, x[i], bp, hp);
                x[i] = wl * lp + wb * bp + wh * hp;
            }
        }
    }

    float sr = 48000.0f, dcA = 0.001f, morph = 0.0f, holdPhase = 0.0f, held[2] {};
    SlotSmoother sm;
    SvfCoefs fc;
    Halfband up[2], dn[2];
    Svf flt[2];
    OnePole dc[2];
};

template <int Mode>
float shapeOne (float x, const DistExtra* e, float xf) { return shape<Mode> (x, e, xf); }
} // namespace

std::unique_ptr<FxModule> makeDistortion() { return std::make_unique<Distortion>(); }

std::shared_ptr<const void> bakeDistortion (const FxModuleSlot& s)
{
    auto e = std::make_shared<DistExtra>();
    e->a = BakedCurve (s.shapeA);
    e->b = BakedCurve (s.shapeB);
    return e;
}

float distortionShape (const float* v, const void* extra, const FxModuleSlot* slot, float x)
{
    const int mode = fxInfo (T).params[0].index (v[0]);
    const float drive = clamp01 (v[5]);
    DistExtra local;
    const DistExtra* e = static_cast<const DistExtra*> (extra);
    if (! e)
    {
        if (slot) { local.a = BakedCurve (slot->shapeA); local.b = BakedCurve (slot->shapeB); }
        e = &local;
    }
    const float g = dbToGain (drive * 36.0f);
    switch (mode)
    {
        case Tube:        return shapeOne<Tube> (x * g, e, drive);
        case SoftClip:    return shapeOne<SoftClip> (x * g, e, drive);
        case HardClip:    return shapeOne<HardClip> (x * g, e, drive);
        case Diode1:      return shapeOne<Diode1> (x * g, e, drive);
        case Diode2:      return shapeOne<Diode2> (x * g, e, drive);
        case LinFold:     return shapeOne<LinFold> (x * g, e, drive);
        case SinFold:     return shapeOne<SinFold> (x * g, e, drive);
        case ZeroSquare:  return shapeOne<ZeroSquare> (x * g, e, drive);
        case Asym:        return shapeOne<Asym> (x * g, e, drive);
        case Rectify:     return shapeOne<Rectify> (x * g, e, drive);
        case XShaper:     return shapeOne<XShaper> (x, e, drive);
        case XShaperAsym: return shapeOne<XShaperAsym> (x, e, drive);
        default:
        {
            // Downsample: show the quantised-in-time ramp as a staircase.
            const float steps = 2.0f + (1.0f - drive) * 62.0f;
            return std::round (x * steps) / steps;
        }
    }
}

float distortionFilterMagnitudeDb (const float* v, float hz, double sr)
{
    const int fpos = fxInfo (T).params[1].index (v[1]);
    if (fpos == 0) return 0.0f;
    const float f = fxPlain (T, 3, v[3]);
    const double k = 1.0 / std::max (fxPlain (T, 4, v[4]), 0.025f);
    const double g = std::tan (3.14159265358979323846 * std::min ((double) f, sr * 0.49) / sr);
    // Analog prototype evaluated at the warped frequency (matches the TPT SVF).
    const double wa = std::tan (3.14159265358979323846 * std::min ((double) hz, sr * 0.499) / sr) / g;
    const double t = clamp01 (v[2]) * 2.0;
    const double wl = t < 1 ? 1 - t : 0, wb = t < 1 ? t : 2 - t, wh = t > 1 ? t - 1 : 0;
    // H = (wl + wb s + wh s^2) / (s^2 + k s + 1), s = j wa
    const double nr = wl - wh * wa * wa, ni = wb * wa;
    const double dr = 1 - wa * wa, di = k * wa;
    const double m = (nr * nr + ni * ni) / std::max (dr * dr + di * di, 1.0e-30);
    return (float) (10.0 * std::log10 (std::max (m, 1.0e-24)));
}
} // namespace tincture
