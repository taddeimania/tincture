// Warp.cpp — warp mode classification and the POST warps: the note-tracking
// LPF/HPF, the 14 distortion shapes, AM and RM. The phase warps live in
// WavetableOsc.cpp because they act on the oscillator's read phase.
#include "Warp.h"
#include "Wavetable.h"   // fm:: fast math

namespace tincture::warp
{
Kind kindOf (int m) noexcept
{
    if (m <= Off || m >= NumModes) return Kind::None;
    if (m == Sync) return Kind::Sync;
    if (m < Lpf) return Kind::Phase;
    if (m <= Hpf) return Kind::Filter;
    if (m <= DistLast) return Kind::Distortion;
    if (m < PdFirst) return Kind::Fm;
    if (m < AmFirst) return Kind::Pd;
    if (m < RmFirst) return Kind::Am;
    return Kind::Rm;
}

int sourceOf (int m) noexcept
{
    switch (kindOf (m))
    {
        case Kind::Fm: return m - FmFirst;
        case Kind::Pd: return m == PdSelf ? -1 : m - PdFirst;
        case Kind::Am: return m - AmFirst;
        case Kind::Rm: return m - RmFirst;
        default: return -1;
    }
}

bool isPost (int m) noexcept
{
    const Kind k = kindOf (m);
    return k == Kind::Filter || k == Kind::Distortion || k == Kind::Am || k == Kind::Rm;
}

namespace
{
// Per-call constants for the shapers (computed once per block, not per sample).
struct ShapeK
{
    float a = 0.0f;     // amount 0..1
    float d = 1.0f;     // drive
    float n = 1.0f;     // output normalisation
    float b = 0.0f;     // bias / threshold
    float c = 0.0f;     // spare
};

inline float sgn (float x) noexcept { return x < 0.0f ? -1.0f : 1.0f; }
inline float softClipCubic (float x) noexcept   // 0..1 slope, flat beyond +-1
{
    x = clampf (x, -1.0f, 1.0f);
    return 1.5f * x - 0.5f * x * x * x;
}
inline float linFold (float x) noexcept
{
    // Triangle fold: identity on [-1,1], folds back beyond, any range.
    float t = (x + 1.0f) * 0.25f;
    t -= (float) (int) t;
    t += t < 0.0f ? 1.0f : 0.0f;
    return 1.0f - 4.0f * std::abs (t - 0.5f);
}

ShapeK makeK (int mode, float amount) noexcept
{
    ShapeK k;
    k.a = clamp01 (amount);
    const float a = k.a;
    switch (mode)
    {
        case Tube:       k.d = 1.0f + 15.0f * a; k.b = 0.35f * a;
                         k.n = 1.0f / std::max (0.05f, fastTanh (k.d + k.b) - fastTanh (k.b)); break;
        case SoftClip:   k.d = 1.0f + 7.0f * a; break;
        case HardClip:   k.d = 1.0f + 15.0f * a; break;
        case Diode1:     k.d = 1.0f + 15.0f * a; k.n = (1.0f + k.d) / k.d; break;
        case Diode2:     k.d = 1.0f + 3.0f * a; break;
        case LinearFold: k.d = 1.0f + 7.0f * a; break;
        case SineFold:   k.d = 1.0f + 15.0f * a; break;
        case ZeroSquare: k.b = 0.6f * a; k.n = 1.0f / (1.0f - k.b); break;
        case Asym:       k.d = 1.0f + 11.0f * a; k.n = 1.0f / fastTanh (k.d); break;
        case Rectify:    break;
        case SineShaper: k.d = 0.25f * (1.0f + 3.0f * a); break;
        case StompBox:   k.d = 1.0f + 39.0f * a; k.n = 1.0f / std::max (0.05f, fastTanh (k.d + 0.2f) - fastTanh (0.2f)); break;
        case TapeSat:    k.d = 1.0f + 5.0f * a; k.n = std::sqrt (1.0f + k.d * k.d) / k.d; break;
        case SoftSat:    break;
        default: break;
    }
    return k;
}

template <int M>
inline float shapeT (float x, const ShapeK& k) noexcept
{
    if constexpr (M == Tube)        return (fastTanh (k.d * x + k.b) - fastTanh (k.b)) * k.n;
    else if constexpr (M == SoftClip)   return softClipCubic (k.d * x * (2.0f / 3.0f));
    else if constexpr (M == HardClip)   return clampf (k.d * x, -1.0f, 1.0f);
    else if constexpr (M == Diode1)
    {
        // Forward-biased side compresses hard, the reverse side gently.
        const float u = k.d * x;
        return x > 0.0f ? u / (1.0f + u) * k.n : u / (1.0f - 0.25f * u) * k.n * 0.5f;
    }
    else if constexpr (M == Diode2)     return fm::sin2pi (0.25f * clampf (k.d * x, -1.0f, 1.0f));
    else if constexpr (M == LinearFold) return linFold (k.d * x);
    else if constexpr (M == SineFold)   return fm::sin2pi (0.25f * k.d * x);
    else if constexpr (M == ZeroSquare)
    {
        const float m = std::min (std::abs (x), 1.0f);
        const float u = std::max (m - k.b, 0.0f) * k.n;
        return sgn (x) * (u + (u * u - u) * k.a);
    }
    else if constexpr (M == Asym)       return x > 0.0f ? fastTanh (k.d * x) * k.n : clampf (0.5f * k.d * x, -0.6f, 0.6f);
    else if constexpr (M == Rectify)    return x + (std::abs (x) - x) * k.a;
    else if constexpr (M == SineShaper) return x + (fm::sin2pi (k.d * x * 3.0f) - x) * k.a;
    else if constexpr (M == StompBox)
    {
        const float u = k.d * x + 0.2f;
        const float y = (u > 0.0f ? fastTanh (u) : fastTanh (0.6f * u) * 1.2f) - fastTanh (0.2f);
        return clampf (y * k.n, -1.2f, 1.2f);
    }
    else if constexpr (M == TapeSat)
    {
        const float u = k.d * x;
        return u / std::sqrt (1.0f + u * u) * k.n;
    }
    else if constexpr (M == SoftSat)    return x + (softClipCubic (x) - x) * k.a;
    else return x;
}

bool needsDcBlock (int mode) noexcept
{
    return mode == Tube || mode == Diode1 || mode == Asym || mode == Rectify || mode == StompBox;
}

template <int M>
void shapeBlock (float* buf, int n, const ShapeK& k, PostState& st, float dcCoeff) noexcept
{
    if (needsDcBlock (M))
    {
        float dc = st.dc;
        for (int i = 0; i < n; ++i)
        {
            const float y = shapeT<M> (buf[i], k);
            dc += (y - dc) * dcCoeff;
            buf[i] = y - dc;
        }
        st.dc = std::abs (dc) < 1.0e-20f ? 0.0f : dc;
    }
    else
    {
        for (int i = 0; i < n; ++i) buf[i] = shapeT<M> (buf[i], k);
    }
}

void svf (int mode, float cutoffHz, float* buf, int n, PostState& st, float sampleRate) noexcept
{
    const float fc = clampf (cutoffHz, 5.0f, 0.49f * sampleRate);
    const float g = std::tan (kPi * fc / sampleRate);
    const float kq = 1.41421356f;
    const float a1 = 1.0f / (1.0f + g * (g + kq)), a2 = g * a1, a3 = g * a2;
    float ic1 = st.z1, ic2 = st.z2;
    if (mode == Lpf)
    {
        for (int i = 0; i < n; ++i)
        {
            const float v3 = buf[i] - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            buf[i] = v2;
        }
    }
    else
    {
        for (int i = 0; i < n; ++i)
        {
            const float x = buf[i];
            const float v3 = x - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            buf[i] = x - kq * v1 - v2;
        }
    }
    st.z1 = std::abs (ic1) < 1.0e-20f ? 0.0f : ic1;
    st.z2 = std::abs (ic2) < 1.0e-20f ? 0.0f : ic2;
}
} // namespace

void processPost (int mode, float amount, float* buf, int n, const float* src,
                  PostState& st, float sampleRate, float pitchHz) noexcept
{
    amount = clamp01 (amount);
    switch (kindOf (mode))
    {
        case Kind::Filter:
        {
            const float hz = std::max (pitchHz, 1.0f);
            // LPF: 0 = open (9 octaves above the note), 100% = at the fundamental.
            // HPF: 0 = two octaves below the note (no effect), 100% = 7 octaves above.
            const float fc = mode == Lpf ? hz * std::exp2 (9.0f * (1.0f - amount))
                                         : hz * std::exp2 (9.0f * amount - 2.0f);
            if (mode == Lpf && fc >= 0.49f * sampleRate) { st.z1 = st.z2 = 0.0f; return; }   // fully open
            svf (mode, fc, buf, n, st, sampleRate);
            return;
        }
        case Kind::Distortion:
        {
            const ShapeK k = makeK (mode, amount);
            const float dcCoeff = clampf (kTwoPi * 6.0f / sampleRate, 0.0f, 0.1f);
            switch (mode)
            {
                case Tube:       shapeBlock<Tube> (buf, n, k, st, dcCoeff); break;
                case SoftClip:   shapeBlock<SoftClip> (buf, n, k, st, dcCoeff); break;
                case HardClip:   shapeBlock<HardClip> (buf, n, k, st, dcCoeff); break;
                case Diode1:     shapeBlock<Diode1> (buf, n, k, st, dcCoeff); break;
                case Diode2:     shapeBlock<Diode2> (buf, n, k, st, dcCoeff); break;
                case LinearFold: shapeBlock<LinearFold> (buf, n, k, st, dcCoeff); break;
                case SineFold:   shapeBlock<SineFold> (buf, n, k, st, dcCoeff); break;
                case ZeroSquare: shapeBlock<ZeroSquare> (buf, n, k, st, dcCoeff); break;
                case Asym:       shapeBlock<Asym> (buf, n, k, st, dcCoeff); break;
                case Rectify:    shapeBlock<Rectify> (buf, n, k, st, dcCoeff); break;
                case SineShaper: shapeBlock<SineShaper> (buf, n, k, st, dcCoeff); break;
                case StompBox:   shapeBlock<StompBox> (buf, n, k, st, dcCoeff); break;
                case TapeSat:    shapeBlock<TapeSat> (buf, n, k, st, dcCoeff); break;
                case SoftSat:    shapeBlock<SoftSat> (buf, n, k, st, dcCoeff); break;
                default: break;
            }
            return;
        }
        case Kind::Am:
        {
            if (src == nullptr) return;
            // Unipolar modulator: 100% = classic AM (gain 0..1 following the source).
            const float a = 0.5f * amount;
            for (int i = 0; i < n; ++i) buf[i] *= 1.0f - a + a * src[i];
            return;
        }
        case Kind::Rm:
        {
            if (src == nullptr) return;
            for (int i = 0; i < n; ++i) buf[i] += (buf[i] * src[i] - buf[i]) * amount;
            return;
        }
        default: return;
    }
}

float shape (int mode, float amount, float x) noexcept
{
    const ShapeK k = makeK (mode, amount);
    switch (mode)
    {
        case Tube:       return shapeT<Tube> (x, k);
        case SoftClip:   return shapeT<SoftClip> (x, k);
        case HardClip:   return shapeT<HardClip> (x, k);
        case Diode1:     return shapeT<Diode1> (x, k);
        case Diode2:     return shapeT<Diode2> (x, k);
        case LinearFold: return shapeT<LinearFold> (x, k);
        case SineFold:   return shapeT<SineFold> (x, k);
        case ZeroSquare: return shapeT<ZeroSquare> (x, k);
        case Asym:       return shapeT<Asym> (x, k);
        case Rectify:    return shapeT<Rectify> (x, k);
        case SineShaper: return shapeT<SineShaper> (x, k);
        case StompBox:   return shapeT<StompBox> (x, k);
        case TapeSat:    return shapeT<TapeSat> (x, k);
        case SoftSat:    return shapeT<SoftSat> (x, k);
        default:         return x;
    }
}
} // namespace tincture::warp
