// WaveEdit.cpp — Wavetable Editor data operations: FFT, brush tools, frame
// buttons, PROCESS / MORPH / REMOVE / SORT / FFT menus. See WaveEdit.h.
#include "WaveEdit.h"

#include <algorithm>
#include <numeric>

namespace tincture::wtedit
{
namespace
{
constexpr int N = kFrameSize;
using cpx = std::complex<float>;

bool sameFrame (const Frame& a, const Frame& b) noexcept
{
    return std::memcmp (a.data(), b.data(), sizeof (float) * (size_t) N) == 0;
}

float wrapPhase (float p) noexcept
{
    p = std::fmod (p + kPi, kTwoPi);
    if (p < 0.0f) p += kTwoPi;
    return p - kPi;
}

Frame sawFrame()
{
    // Band-limited rising saw, peak ~1 (harmonics 1..1023, amp 2/(pi n)).
    Spectrum s;
    for (int n = 1; n < kNyquist; ++n)
    {
        s.mag[(size_t) n] = 2.0f / (kPi * (float) n);
        s.phase[(size_t) n] = kPi;     // -sum sin(nt)/n = rising ramp
    }
    Frame f;
    synthesise (s, f);
    return f;
}
} // namespace

// ---------------------------------------------------------------------------
// FrameSet / Selection
// ---------------------------------------------------------------------------
bool FrameSet::isKeyFrame (int i) const noexcept
{
    if (keyFrames.empty()) return true;
    return std::binary_search (keyFrames.begin(), keyFrames.end(), i);
}

FrameSet FrameSet::initSaw()
{
    FrameSet s;
    s.frames.push_back (sawFrame());
    return s;
}

void FrameSet::save (Node& n) const
{
    std::vector<float> all;
    all.reserve (frames.size() * (size_t) N);
    for (auto& f : frames) all.insert (all.end(), f.begin(), f.end());
    n.set ("name", name).set ("frames", std::move (all)).set ("interp", interpolate ? 1.0 : 0.0);
    n.set ("modified", modified ? 1.0 : 0.0);
    if (! keyFrames.empty())
    {
        std::vector<float> k;
        for (int i : keyFrames) k.push_back ((float) i);
        n.set ("keys", std::move (k));
    }
}

FrameSet FrameSet::load (const Node& n)
{
    FrameSet s;
    s.name = n.str ("name", s.name);
    s.interpolate = n.flag ("interp", true);
    s.modified = n.flag ("modified");
    if (auto* fl = n.floats ("frames"))
    {
        const size_t count = std::min<size_t> (fl->size() / (size_t) N, (size_t) kMaxFrames);
        s.frames.resize (count);
        for (size_t i = 0; i < count; ++i)
            std::copy (fl->begin() + (ptrdiff_t) (i * N), fl->begin() + (ptrdiff_t) ((i + 1) * N), s.frames[i].begin());
    }
    if (s.frames.empty()) s.frames.push_back (sawFrame());
    if (auto* k = n.floats ("keys"))
    {
        for (float v : *k)
            if ((int) v >= 0 && (int) v < s.size()) s.keyFrames.push_back ((int) v);
        std::sort (s.keyFrames.begin(), s.keyFrames.end());
        if (s.keyFrames.size() < 2) s.keyFrames.clear();
    }
    return s;
}

void Selection::extendTo (int i) noexcept
{
    first = std::min (current, i);
    last = std::max (current, i);
}

void Selection::clamp (int numFrames) noexcept
{
    const int hi = std::max (0, numFrames - 1);
    current = std::clamp (current, 0, hi);
    first = std::clamp (first, 0, hi);
    last = std::clamp (last, 0, hi);
    if (first > last) std::swap (first, last);
    if (current < first || current > last) first = last = current;
}

// ---------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------
Fft::Fft (int size) : n (size)
{
    // Per-stage twiddles stored contiguously: stage with half-length h uses
    // tw[h - 1 .. 2h - 2] = exp (-i*pi*k/h), k = 0..h-1.
    tw.resize ((size_t) std::max (1, n - 1));
    for (int h = 1; h < n; h <<= 1)
    {
        for (int k = 0; k < h; ++k)
        {
            const double a = -3.14159265358979323846 * k / h;
            tw[(size_t) (h - 1 + k)] = cpx ((float) std::cos (a), (float) std::sin (a));
        }
    }
    rev.resize ((size_t) n);
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        rev[(size_t) i] = r;
    }
}

void Fft::run (cpx* data, bool inv) const noexcept
{
    for (int i = 0; i < n; ++i)
        if (i < rev[(size_t) i]) std::swap (data[i], data[rev[(size_t) i]]);
    float* d = reinterpret_cast<float*> (data);       // interleaved re, im (std::complex layout guarantee)
    const float sign = inv ? -1.0f : 1.0f;
    // First stage (h = 1): twiddle 1.
    for (int i = 0; i < n; i += 2)
    {
        float* a = d + 2 * i;
        const float ar = a[0], ai = a[1], br = a[2], bi = a[3];
        a[0] = ar + br; a[1] = ai + bi; a[2] = ar - br; a[3] = ai - bi;
    }
    for (int h = 2; h < n; h <<= 1)
    {
        const float* w = reinterpret_cast<const float*> (tw.data() + (h - 1));
        for (int i = 0; i < n; i += 2 * h)
        {
            float* a = d + 2 * i;
            float* b = a + 2 * h;
            for (int k = 0; k < h; ++k)
            {
                const float wr = w[2 * k], wi = sign * w[2 * k + 1];
                const float br = b[2 * k], bi = b[2 * k + 1];
                const float vr = br * wr - bi * wi;
                const float vi = br * wi + bi * wr;
                const float ar = a[2 * k], ai = a[2 * k + 1];
                a[2 * k] = ar + vr; a[2 * k + 1] = ai + vi;
                b[2 * k] = ar - vr; b[2 * k + 1] = ai - vi;
            }
        }
    }
}

void Fft::forward (cpx* data) const noexcept { run (data, false); }

void Fft::inverse (cpx* data) const noexcept
{
    run (data, true);
    const float s = 1.0f / (float) n;
    for (int i = 0; i < n; ++i) data[i] *= s;
}

const Fft& frameFft()
{
    static const Fft f (N);
    return f;
}

void analyse (const Frame& f, Spectrum& s)
{
    std::array<cpx, N> buf;
    for (int i = 0; i < N; ++i) buf[(size_t) i] = cpx (f[(size_t) i], 0.0f);
    frameFft().forward (buf.data());
    const float inv = 1.0f / (float) N;
    s.mag[0] = buf[0].real() * inv;
    s.phase[0] = 0.0f;
    for (int k = 1; k < kNyquist; ++k)
    {
        const cpx c = buf[(size_t) k];
        const float m = 2.0f * std::abs (c) * inv;
        s.mag[(size_t) k] = m;
        // Empty bins read as phase 0 (sine phase), not atan2 (0, 0) + pi/2.
        s.phase[(size_t) k] = m < 1.0e-9f ? 0.0f : wrapPhase (std::atan2 (c.imag(), c.real()) + 0.5f * kPi);
    }
    s.mag[kNyquist] = buf[kNyquist].real() * inv;
    s.phase[kNyquist] = 0.0f;
}

void synthesise (const Spectrum& s, Frame& f)
{
    std::array<cpx, N> buf {};
    buf[0] = cpx (s.mag[0] * (float) N, 0.0f);
    for (int k = 1; k < kNyquist; ++k)
    {
        const float a = s.mag[(size_t) k] * (float) N * 0.5f;
        const float ph = s.phase[(size_t) k] - 0.5f * kPi;
        const cpx c (a * std::cos (ph), a * std::sin (ph));
        buf[(size_t) k] = c;
        buf[(size_t) (N - k)] = std::conj (c);
    }
    buf[kNyquist] = cpx (s.mag[kNyquist] * (float) N, 0.0f);
    frameFft().inverse (buf.data());
    for (int i = 0; i < N; ++i) f[(size_t) i] = buf[(size_t) i].real();
}

float harmonicMag (const Frame& f, int n)
{
    Spectrum s;
    analyse (f, s);
    return s.mag[(size_t) std::clamp (n, 0, kNyquist)];
}

float harmonicPhase (const Frame& f, int n)
{
    Spectrum s;
    analyse (f, s);
    return s.phase[(size_t) std::clamp (n, 0, kNyquist)];
}

bool setHarmonic (Frame& f, int n, float mag, float phase)
{
    if (n < 0 || n > kNyquist) return false;
    Spectrum s;
    analyse (f, s);
    s.mag[(size_t) n] = mag;
    s.phase[(size_t) n] = (n == 0 || n == kNyquist) ? 0.0f : wrapPhase (phase);
    const Frame old = f;
    synthesise (s, f);
    return ! sameFrame (old, f);
}

float magToBar (float mag, int bin, const FftDrawOptions& o) noexcept
{
    float m = std::abs (mag);
    if (o.on (FftDrawOptions::ScaleByBin)) m *= (float) std::max (1, bin);
    return clamp01 (std::sqrt (m));
}

float barToMag (float bar, int bin, const FftDrawOptions& o) noexcept
{
    float b = clamp01 (bar);
    if (o.on (FftDrawOptions::SnapQuarters)) b = std::round (b * 4.0f) * 0.25f;
    float m = b * b;
    if (o.on (FftDrawOptions::ScaleByBin)) m /= (float) std::max (1, bin);
    return m;
}

bool binDrawable (int bin, const FftDrawOptions& o) noexcept
{
    if (bin < 1 || bin >= kNyquist) return false;
    if (o.on (FftDrawOptions::EvenOnly) && (bin & 1)) return false;
    if (o.on (FftDrawOptions::OddOnly) && ! (bin & 1)) return false;
    return true;
}

bool drawMagnitude (Spectrum& s, int bin, float bar, const FftDrawOptions& o)
{
    if (! binDrawable (bin, o)) return false;
    const float m = barToMag (bar, bin, o);
    if (s.mag[(size_t) bin] == m) return false;
    s.mag[(size_t) bin] = m;
    return true;
}

bool drawPhase (Spectrum& s, int bin, float bar, const FftDrawOptions& o)
{
    if (! binDrawable (bin, o)) return false;
    float b = clamp01 (bar);
    if (o.on (FftDrawOptions::SnapQuarters)) b = std::round (b * 4.0f) * 0.25f;
    const float p = wrapPhase (barToPhase (b));
    if (s.phase[(size_t) bin] == p) return false;
    s.phase[(size_t) bin] = p;
    return true;
}

bool drawMagnitude (Frame& f, int bin, float bar, const FftDrawOptions& o)
{
    Spectrum s;
    analyse (f, s);
    if (! drawMagnitude (s, bin, bar, o)) return false;
    synthesise (s, f);
    return true;
}

bool drawPhase (Frame& f, int bin, float bar, const FftDrawOptions& o)
{
    Spectrum s;
    analyse (f, s);
    if (! drawPhase (s, bin, bar, o)) return false;
    synthesise (s, f);
    return true;
}

bool nudgePhase (Frame& f, int bin, float radians)
{
    if (bin < 1 || bin >= kNyquist || radians == 0.0f) return false;
    Spectrum s;
    analyse (f, s);
    s.phase[(size_t) bin] = wrapPhase (s.phase[(size_t) bin] + radians);
    synthesise (s, f);
    return true;
}

// ---------------------------------------------------------------------------
// Resampling
// ---------------------------------------------------------------------------
namespace
{
constexpr int kSincHalf = 16;          // zero crossings each side
constexpr int kSincOver = 512;         // table points per zero crossing

const std::vector<float>& sincTable()
{
    static const std::vector<float> t = []
    {
        std::vector<float> v ((size_t) (kSincHalf * kSincOver + 2));
        for (size_t i = 0; i < v.size(); ++i)
        {
            const double u = (double) i / kSincOver;
            if (u >= kSincHalf) { v[i] = 0.0f; continue; }
            const double pi = 3.14159265358979323846;
            const double s = u == 0.0 ? 1.0 : std::sin (pi * u) / (pi * u);
            const double w = 0.42 + 0.5 * std::cos (pi * u / kSincHalf) + 0.08 * std::cos (2.0 * pi * u / kSincHalf);   // Blackman
            v[i] = (float) (s * w);
        }
        return v;
    }();
    return t;
}
} // namespace

float sincRead (const float* x, int len, double pos, float cutoff, bool periodic) noexcept
{
    const auto& tab = sincTable();
    const float c = clampf (cutoff, 0.01f, 1.0f);
    const int hw = (int) std::ceil (kSincHalf / c);
    const double fl = std::floor (pos);
    const int ip = (int) fl;
    const float frac = (float) (pos - fl);
    float sum = 0.0f;
    for (int j = -hw + 1; j <= hw; ++j)
    {
        const float t = std::abs (((float) j - frac) * c) * (float) kSincOver;
        if (t >= (float) (kSincHalf * kSincOver)) continue;
        const int ti = (int) t;
        const float tf = t - (float) ti;
        const float k = tab[(size_t) ti] + (tab[(size_t) ti + 1] - tab[(size_t) ti]) * tf;
        int idx = ip + j;
        if (periodic)
        {
            idx %= len;
            if (idx < 0) idx += len;
        }
        else if (idx < 0 || idx >= len)
            continue;
        sum += x[idx] * k;
    }
    return sum * c;
}

void resampleCycle (const float* in, int len, Frame& out)
{
    if (len <= 0) { out.fill (0.0f); return; }
    if (len == N) { std::copy (in, in + N, out.begin()); return; }
    // Exact DFT of the cycle (O(len * harmonics)), then a 2048-point inverse.
    std::vector<cpx> twid ((size_t) len);
    for (int t = 0; t < len; ++t)
    {
        const double a = -2.0 * 3.14159265358979323846 * t / len;
        twid[(size_t) t] = cpx ((float) std::cos (a), (float) std::sin (a));
    }
    const int maxH = std::min ((len - 1) / 2 + ((len % 2) == 0 ? 1 : 0), kNyquist - 1);
    std::vector<cpx> buf ((size_t) N);
    const float scale = (float) N / (float) len;
    for (int k = 0; k <= maxH; ++k)
    {
        double re = 0.0, im = 0.0;
        int idx = 0;
        for (int t = 0; t < len; ++t)
        {
            const cpx w = twid[(size_t) idx];
            re += (double) in[t] * w.real();
            im += (double) in[t] * w.imag();
            idx += k;
            if (idx >= len) idx -= len;
        }
        cpx c ((float) re * scale, (float) im * scale);
        if (k == 0) { buf[0] = cpx (c.real(), 0.0f); continue; }
        if ((len % 2) == 0 && k == len / 2) c *= 0.5f;     // source Nyquist: split between +/- bins
        buf[(size_t) k] = c;
        buf[(size_t) (N - k)] = std::conj (c);
    }
    frameFft().inverse (buf.data());
    for (int i = 0; i < N; ++i) out[(size_t) i] = buf[(size_t) i].real();
}

void readCycle (const float* x, int xlen, double start, double len, Frame& out)
{
    const double step = len / (double) N;
    const float cutoff = (float) std::min (1.0, 1.0 / step) * (step > 1.0 ? 0.97f : 1.0f);
    for (int k = 0; k < N; ++k)
        out[(size_t) k] = sincRead (x, xlen, start + (double) k * step, cutoff, false);
}

// ---------------------------------------------------------------------------
// Brush tools
// ---------------------------------------------------------------------------
const char* brushName (Brush b) noexcept
{
    static const char* names[] {
        "Flat Line", "Sine", "Slope Up", "Slope Down", "Curve Up", "Curve Down", "Half Sine (Peak)",
        "Half Sine (Valley)", "Interpolate Linear", "Interpolate Curved", "Nudge", "Noise"
    };
    const int i = (int) b;
    return i >= 0 && i < (int) Brush::NumBrushes ? names[i] : "";
}

int gridCell (float x01, int gridX) noexcept
{
    const int cells = gridX > 0 ? gridX : N;
    return std::clamp ((int) (clamp01 (x01) * (float) cells), 0, cells - 1);
}

void cellRange (int cell, int gridX, int& start, int& end) noexcept
{
    const int cells = gridX > 0 ? std::min (gridX, N) : N;
    cell = std::clamp (cell, 0, cells - 1);
    start = (int) ((int64_t) cell * N / cells);
    end = (int) ((int64_t) (cell + 1) * N / cells);
}

float snapY (float y, int gridY) noexcept
{
    y = clampf (y, -1.0f, 1.0f);
    if (gridY <= 0) return y;
    const float step = 2.0f / (float) gridY;
    return clampf (std::round ((y + 1.0f) / step) * step - 1.0f, -1.0f, 1.0f);
}

namespace
{
void mirrorRange (Frame& f, int s, int e) noexcept
{
    for (int i = s; i < e; ++i) f[(size_t) (N - 1 - i)] = -f[(size_t) i];
}
} // namespace

bool applyBrush (Frame& f, Brush b, float x01, float y, int gridX, bool mirror, Rng& rng)
{
    int s, e;
    cellRange (gridCell (x01, gridX), gridX, s, e);
    const Frame old = f;
    const int len = e - s;
    const float a = clampf (y, -1.0f, 1.0f);
    const float before = f[(size_t) ((s - 1 + N) % N)], after = f[(size_t) (e % N)];
    for (int i = s; i < e; ++i)
    {
        const float t = len > 1 ? (float) (i - s) / (float) len : 0.0f;          // 0..1 within the cell
        float& v = f[(size_t) i];
        switch (b)
        {
            case Brush::FlatLine:          v = a; break;
            case Brush::Sine:              v = a * std::sin (kTwoPi * t); break;
            case Brush::SlopeUp:           v = a * (2.0f * t - 1.0f); break;
            case Brush::SlopeDown:         v = a * (1.0f - 2.0f * t); break;
            case Brush::CurveUp:           v = a * std::sin (0.5f * kPi * t); break;
            case Brush::CurveDown:         v = a * std::cos (0.5f * kPi * t); break;
            case Brush::HalfSinePeak:      v = a * std::sin (kPi * t); break;
            case Brush::HalfSineValley:    v = -a * std::sin (kPi * t); break;
            case Brush::InterpolateLinear:
            case Brush::InterpolateCurved:
            {
                float u = (float) (i - s + 1) / (float) (len + 1);
                if (b == Brush::InterpolateCurved) u = 0.5f - 0.5f * std::cos (kPi * u);
                v = lerp (before, after, u);
                break;
            }
            case Brush::Nudge:             v = clampf (v + y, -1.0f, 1.0f); break;
            case Brush::Noise:
                if (y > 0.0f) v = clampf (v + y * rng.bi(), -1.0f, 1.0f);
                break;
            case Brush::NumBrushes: break;
        }
    }
    if (b == Brush::Noise && y < 0.0f)
    {
        // Reduce noise: blend the cell toward a 5-tap smoothed copy of itself.
        const float amt = clamp01 (-y);
        for (int i = s; i < e; ++i)
        {
            float acc = 0.0f;
            for (int k = -2; k <= 2; ++k) acc += old[(size_t) ((i + k + N) % N)];
            f[(size_t) i] = lerp (old[(size_t) i], acc * 0.2f, amt);
        }
    }
    if (mirror) mirrorRange (f, s, e);
    return ! sameFrame (old, f);
}

bool drawLine (Frame& f, float x0, float y0, float x1, float y1, bool mirror)
{
    if (x1 < x0) { std::swap (x0, x1); std::swap (y0, y1); }
    const int i0 = std::clamp ((int) (clamp01 (x0) * N), 0, N - 1);
    const int i1 = std::clamp ((int) (clamp01 (x1) * N), 0, N - 1);
    const Frame old = f;
    for (int i = i0; i <= i1; ++i)
    {
        const float t = i1 > i0 ? (float) (i - i0) / (float) (i1 - i0) : 1.0f;
        f[(size_t) i] = clampf (lerp (y0, y1, t), -1.0f, 1.0f);
    }
    if (mirror) mirrorRange (f, i0, i1 + 1);
    return ! sameFrame (old, f);
}

bool shiftByGrid (Frame& f, int gridX, int dir)
{
    const int cells = gridX > 0 ? gridX : N;
    int sh = (N / cells) * (dir < 0 ? -1 : 1);
    sh = ((sh % N) + N) % N;
    if (sh == 0) return false;
    std::rotate (f.begin(), f.begin() + (N - sh), f.end());      // content moves right by sh
    return true;
}

bool nudgeVertical (Frame& f, float dy)
{
    if (dy == 0.0f) return false;
    for (auto& v : f) v = clampf (v + dy, -1.0f, 1.0f);
    return true;
}

// ---------------------------------------------------------------------------
// Frame buttons
// ---------------------------------------------------------------------------
bool insertFrame (FrameSet& s, Selection& sel)
{
    sel.clamp (s.size());
    if (s.size() >= kMaxFrames) return false;
    const Frame copy = s.frames[(size_t) sel.current];
    s.frames.insert (s.frames.begin() + sel.current + 1, copy);
    s.keyFrames.clear();
    s.modified = true;
    sel.select (sel.current + 1);
    return true;
}

bool appendFrame (FrameSet& s, Selection& sel)
{
    if (s.size() >= kMaxFrames) return false;
    const Frame copy = s.frames.back();
    s.frames.push_back (copy);
    s.keyFrames.clear();
    s.modified = true;
    sel.select (s.size() - 1);
    return true;
}

bool removeFrame (FrameSet& s, Selection& sel)
{
    sel.clamp (s.size());
    if (s.size() <= 1) return false;
    s.frames.erase (s.frames.begin() + sel.current);
    s.keyFrames.clear();
    s.modified = true;
    sel.select (std::min (sel.current, s.size() - 1));
    return true;
}

bool moveFrame (FrameSet& s, Selection& sel, int from, int insertBefore)
{
    if (from < 0 || from >= s.size()) return false;
    insertBefore = std::clamp (insertBefore, 0, s.size());
    if (insertBefore == from || insertBefore == from + 1) return false;
    const Frame f = s.frames[(size_t) from];
    s.frames.erase (s.frames.begin() + from);
    const int dest = insertBefore > from ? insertBefore - 1 : insertBefore;
    s.frames.insert (s.frames.begin() + dest, f);
    s.keyFrames.clear();
    s.modified = true;
    sel.select (dest);
    return true;
}

bool copyFrames (const FrameSet& s, const Selection& selIn, std::vector<Frame>& clipboard)
{
    Selection sel = selIn;
    sel.clamp (s.size());
    clipboard.assign (s.frames.begin() + sel.first, s.frames.begin() + sel.last + 1);
    return false;     // the table itself is unchanged
}

bool pasteFrames (FrameSet& s, Selection& sel, const std::vector<Frame>& clipboard)
{
    if (clipboard.empty()) return false;
    sel.clamp (s.size());
    const int start = sel.current;
    int written = 0;
    for (size_t i = 0; i < clipboard.size(); ++i)
    {
        const int idx = start + (int) i;
        if (idx >= kMaxFrames) break;
        if (idx < s.size()) s.frames[(size_t) idx] = clipboard[i];
        else s.frames.push_back (clipboard[i]);
        ++written;
    }
    s.keyFrames.clear();
    s.modified = true;
    sel.current = sel.first = start;
    sel.last = start + written - 1;
    return written > 0;
}

bool replaceFrame (FrameSet& s, int index, const Frame& f)
{
    if (index < 0 || index >= s.size()) return false;
    if (sameFrame (s.frames[(size_t) index], f)) return false;
    s.frames[(size_t) index] = f;
    s.modified = true;
    return true;
}

// ---------------------------------------------------------------------------
// Single-frame primitives
// ---------------------------------------------------------------------------
float peakOf (const Frame& f) noexcept
{
    float p = 0.0f;
    for (float v : f) p = std::max (p, std::abs (v));
    return p;
}

bool normalise (Frame& f, float peak)
{
    const float p = peakOf (f);
    if (p < 1.0e-9f) return false;
    const float g = peak / p;
    if (std::abs (g - 1.0f) < 1.0e-7f) return false;
    for (auto& v : f) v *= g;
    return true;
}

bool removeDc (Frame& f)
{
    double sum = 0.0;
    for (float v : f) sum += v;
    const float m = (float) (sum / N);
    if (std::abs (m) < 1.0e-9f) return false;
    for (auto& v : f) v -= m;
    return true;
}

bool invert (Frame& f)
{
    if (peakOf (f) == 0.0f) return false;
    for (auto& v : f) v = -v;
    return true;
}

bool reverse (Frame& f)
{
    // Periodic reverse about sample 0: out[i] = in[-i]. A sine becomes -sine.
    const Frame old = f;
    for (int i = 1; i < N; ++i) f[(size_t) i] = old[(size_t) (N - i)];
    return ! sameFrame (old, f);
}

bool shiftToZeroCrossing (Frame& f)
{
    // Rotate so the frame starts at the rising zero crossing nearest to sample 0.
    auto rising = [&f] (int i) { return f[(size_t) ((i - 1 + N) % N)] < 0.0f && f[(size_t) i] >= 0.0f; };
    for (int d = 0; d <= N / 2; ++d)
    {
        for (int i : { d, (N - d) % N })
        {
            if (! rising (i)) continue;
            if (i == 0) return false;
            std::rotate (f.begin(), f.begin() + i, f.end());
            return true;
        }
    }
    return false;
}

bool fadeEdges (Frame& f, int len)
{
    len = std::clamp (len, 1, N / 2);
    const Frame old = f;
    for (int i = 0; i < len; ++i)
    {
        const float g = 0.5f - 0.5f * std::cos (kPi * (float) i / (float) len);
        f[(size_t) i] *= g;
        f[(size_t) (N - 1 - i)] *= g;
    }
    return ! sameFrame (old, f);
}

bool crossfadeEdges (Frame& f, int len)
{
    // The last `len` samples fade into a reflection of the head, so the end
    // of the cycle arrives at f[0] and the wrap point becomes continuous.
    len = std::clamp (len, 1, N / 2);
    const Frame old = f;
    for (int j = 0; j < len; ++j)
    {
        const float t = (float) (j + 1) / (float) len;
        const float g = 0.5f - 0.5f * std::cos (kPi * t);
        const int i = N - len + j;
        f[(size_t) i] = lerp (old[(size_t) i], old[(size_t) (len - 1 - j)], g);
    }
    return ! sameFrame (old, f);
}

bool lowpassHarmonics (Frame& f, int keep)
{
    Spectrum s;
    analyse (f, s);
    s.mag[0] = 0.0f;
    for (int k = std::max (1, keep + 1); k <= kNyquist; ++k) s.mag[(size_t) k] = 0.0f;
    const Frame old = f;
    synthesise (s, f);
    return ! sameFrame (old, f);
}

bool sampleRedux (Frame& f, int hold)
{
    if (hold <= 1) return false;
    const Frame old = f;
    for (int i = 0; i < N; ++i) f[(size_t) i] = old[(size_t) (i - i % hold)];
    return ! sameFrame (old, f);
}

// ---------------------------------------------------------------------------
// MORPH
// ---------------------------------------------------------------------------
namespace
{
void zeroFundamentalPhase (Spectrum& s)
{
    if (s.mag[1] < 1.0e-9f) return;
    const float p1 = s.phase[1];
    for (int k = 1; k < kNyquist; ++k) s.phase[(size_t) k] = wrapPhase (s.phase[(size_t) k] - (float) k * p1);
}

void lerpSpectrum (const Spectrum& a, const Spectrum& b, float t, Spectrum& out)
{
    out.mag[0] = lerp (a.mag[0], b.mag[0], t);
    out.mag[kNyquist] = lerp (a.mag[kNyquist], b.mag[kNyquist], t);
    out.phase[0] = out.phase[kNyquist] = 0.0f;
    for (int k = 1; k < kNyquist; ++k)
    {
        const float ma = a.mag[(size_t) k], mb = b.mag[(size_t) k];
        out.mag[(size_t) k] = lerp (ma, mb, t);
        // A silent side has no meaningful phase: take the other one's.
        const float pa = ma > 1.0e-7f ? a.phase[(size_t) k] : b.phase[(size_t) k];
        const float pb = mb > 1.0e-7f ? b.phase[(size_t) k] : pa;
        out.phase[(size_t) k] = wrapPhase (pa + wrapPhase (pb - pa) * t);
    }
}
} // namespace

bool morph (FrameSet& s, MorphMode mode)
{
    std::vector<Frame> keys;
    if (s.morphed()) for (int i : s.keyFrames) keys.push_back (s.frames[(size_t) i]);
    else keys = s.frames;
    const int n = (int) keys.size();
    if (n < 2 || n >= kMaxFrames) return false;

    std::vector<int> pos ((size_t) n);
    for (int i = 0; i < n; ++i) pos[(size_t) i] = (int) std::lround ((double) i * (kMaxFrames - 1) / (n - 1));

    std::vector<Spectrum> sp;
    if (mode != MorphMode::Crossfade)
    {
        sp.resize ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            analyse (keys[(size_t) i], sp[(size_t) i]);
            if (mode == MorphMode::SpectralZeroFund) zeroFundamentalPhase (sp[(size_t) i]);
            if (mode == MorphMode::SpectralZeroAll)
                for (int k = 1; k < kNyquist; ++k) sp[(size_t) i].phase[(size_t) k] = 0.0f;
            if (mode != MorphMode::Spectral) synthesise (sp[(size_t) i], keys[(size_t) i]);   // sources are altered
        }
    }

    std::vector<Frame> out ((size_t) kMaxFrames);
    Spectrum tmp;
    for (int g = 0; g < n - 1; ++g)
    {
        const int a = pos[(size_t) g], b = pos[(size_t) g + 1];
        out[(size_t) a] = keys[(size_t) g];
        for (int k = a + 1; k < b; ++k)
        {
            const float t = (float) (k - a) / (float) (b - a);
            if (mode == MorphMode::Crossfade)
            {
                const Frame& fa = keys[(size_t) g];
                const Frame& fb = keys[(size_t) g + 1];
                Frame& o = out[(size_t) k];
                for (int i = 0; i < N; ++i) o[(size_t) i] = fa[(size_t) i] + (fb[(size_t) i] - fa[(size_t) i]) * t;
            }
            else
            {
                lerpSpectrum (sp[(size_t) g], sp[(size_t) g + 1], t, tmp);
                synthesise (tmp, out[(size_t) k]);
            }
        }
    }
    out[(size_t) pos.back()] = keys.back();
    s.frames = std::move (out);
    s.keyFrames = std::move (pos);
    s.modified = true;
    return true;
}

bool removeMorph (FrameSet& s)
{
    if (! s.morphed()) return false;
    std::vector<Frame> keys;
    for (int i : s.keyFrames) keys.push_back (s.frames[(size_t) i]);
    s.frames = std::move (keys);
    s.keyFrames.clear();
    s.modified = true;
    return true;
}

// ---------------------------------------------------------------------------
// Menu operations
// ---------------------------------------------------------------------------
namespace
{
using Ctx = EditContext;

template <typename Fn>
bool forFrames (Ctx& c, bool all, Fn&& fn)
{
    c.sel.clamp (c.set.size());
    const int a = all ? 0 : c.sel.first, b = all ? c.set.size() - 1 : c.sel.last;
    bool changed = false;
    for (int i = a; i <= b; ++i) changed |= fn (c.set.frames[(size_t) i]);
    if (changed) c.set.modified = true;
    return changed;
}

int cellLen (const Ctx& c) { return N / std::clamp (c.gridX, 1, N); }
int gridN (const Ctx& c) { return std::max (1, c.gridX); }

// Run a spectrum edit on the current frame. `limit`: scale down if the result
// clips (menu generators can build very loud spectra).
template <typename Fn>
bool withSpectrum (Ctx& c, bool limit, Fn&& fn)
{
    c.sel.clamp (c.set.size());
    Frame& f = c.set.frames[(size_t) c.sel.current];
    Spectrum s;
    analyse (f, s);
    if (! fn (s)) return false;
    const Frame old = f;
    synthesise (s, f);
    if (limit && peakOf (f) > 1.0f) normalise (f, 1.0f);
    if (sameFrame (old, f)) return false;
    c.set.modified = true;
    return true;
}

template <typename Fn>
bool allSpectra (Ctx& c, Fn&& fn)
{
    return forFrames (c, true, [&] (Frame& f)
    {
        Spectrum s;
        analyse (f, s);
        fn (s);
        const Frame old = f;
        synthesise (s, f);
        return ! sameFrame (old, f);
    });
}

const Frame& otherFrame (const Ctx& c, int i)
{
    const auto& o = c.otherOsc->frames;
    const int m = (int) o.size(), n = c.set.size();
    const int j = (m <= 1 || n <= 1) ? std::min (i, m - 1)
                                     : (int) std::lround ((double) i * (m - 1) / (n - 1));
    return o[(size_t) std::clamp (j, 0, m - 1)];
}

// ---- FFT menus ----
// Mags (phases = false) and phases (true) share the item list.
template <bool Phases> bool fftClearAll (Ctx& c)
{
    return withSpectrum (c, false, [] (Spectrum& s)
    {
        if (Phases) s.phase.fill (0.0f);
        else s.mag.fill (0.0f);
        return true;
    });
}
template <bool Phases> bool fftClearHF (Ctx& c)
{
    const int from = std::clamp (c.bin, 1, kNyquist);
    return withSpectrum (c, false, [from] (Spectrum& s)
    {
        for (int k = from; k <= kNyquist; ++k) (Phases ? s.phase : s.mag)[(size_t) k] = 0.0f;
        return true;
    });
}
template <bool Phases> bool fftClearLF (Ctx& c)
{
    const int to = std::clamp (c.bin, 1, kNyquist);
    return withSpectrum (c, false, [to] (Spectrum& s)
    {
        for (int k = 0; k < to; ++k) (Phases ? s.phase : s.mag)[(size_t) k] = 0.0f;
        return true;
    });
}
template <bool Phases> bool fftSaw (Ctx& c)
{
    return withSpectrum (c, true, [] (Spectrum& s)
    {
        if (Phases) { s.phase.fill (0.0f); return true; }
        s.mag.fill (0.0f);
        for (int k = 1; k < kNyquist; ++k) s.mag[(size_t) k] = 2.0f / (kPi * (float) k);
        return true;
    });
}
template <bool Phases, int Bins, bool Half> bool fftRandom (Ctx& c)
{
    Rng rng (c.seed);
    const bool ok = withSpectrum (c, true, [&rng] (Spectrum& s)
    {
        for (int k = 1; k <= Bins; ++k)
        {
            float u = rng.uni();
            if (Half) u = std::round (u * 2.0f) * 0.5f;          // 0, 0.5, 1
            if (Phases) s.phase[(size_t) k] = Half ? (u >= 0.75f ? kPi : 0.0f) : wrapPhase (u * kTwoPi);
            else s.mag[(size_t) k] = u * u;                       // bar u
        }
        return true;
    });
    c.seed = rng.next();
    return ok;
}
template <bool Phases> bool fftGaps (Ctx& c)
{
    Rng rng (c.seed);
    const bool ok = withSpectrum (c, true, [&rng] (Spectrum& s)
    {
        for (int k = 1; k <= kFftBins; ++k)
        {
            if (rng.uni() >= 0.15f) continue;
            const int run = 1 + (int) (rng.uni() * 8.0f);
            for (int j = k; j < std::min (k + run, kFftBins + 1); ++j) (Phases ? s.phase : s.mag)[(size_t) j] = 0.0f;
            k += run;
        }
        return true;
    });
    c.seed = rng.next();
    return ok;
}
template <bool Phases> bool fftProgFade (Ctx& c)
{
    return withSpectrum (c, false, [] (Spectrum& s)
    {
        for (int k = 1; k <= kNyquist; ++k)
        {
            const float g = k > kFftBins ? 0.0f : 1.0f - (float) (k - 1) / (float) kFftBins;
            (Phases ? s.phase : s.mag)[(size_t) k] *= g;
        }
        return true;
    });
}
template <bool Phases> bool fftOctUp (Ctx& c)
{
    return withSpectrum (c, true, [] (Spectrum& s)
    {
        auto& v = Phases ? s.phase : s.mag;
        auto src = v;
        for (int k = 1; k < kNyquist; ++k) v[(size_t) k] = 0.0f;
        for (int k = 1; 2 * k < kNyquist; ++k) v[(size_t) (2 * k)] = src[(size_t) k];
        return true;
    });
}
template <bool Phases> bool fftOctDown (Ctx& c)
{
    return withSpectrum (c, true, [] (Spectrum& s)
    {
        auto& v = Phases ? s.phase : s.mag;
        auto src = v;
        for (int k = 1; k < kNyquist; ++k) v[(size_t) k] = (2 * k < kNyquist) ? src[(size_t) (2 * k)] : 0.0f;
        return true;
    });
}
template <bool Phases> bool fftRepeat (Ctx& c)
{
    const int g = std::clamp (c.bin, 1, kNyquist - 1);
    return withSpectrum (c, true, [g] (Spectrum& s)
    {
        auto& v = Phases ? s.phase : s.mag;
        for (int k = g + 1; k < kNyquist; ++k) v[(size_t) k] = v[(size_t) (1 + (k - 1) % g)];
        return true;
    });
}
template <int Flag> bool fftToggle (Ctx& c)
{
    if (c.fftOptions == nullptr) return false;
    bool& f = c.fftOptions->flags[Flag];
    f = ! f;
    if (f && Flag == FftDrawOptions::EvenOnly) c.fftOptions->flags[FftDrawOptions::OddOnly] = false;
    if (f && Flag == FftDrawOptions::OddOnly) c.fftOptions->flags[FftDrawOptions::EvenOnly] = false;
    return false;     // drawing state only
}

#define FFT_MENU(P, TITLE)                                                                       \
    { WaveOp::Title, TITLE },                                                                    \
    { WaveOp::Separator },                                                                       \
    { WaveOp::Action, "Clear All", fftClearAll<P> },                                             \
    { WaveOp::Action, "Clear HF (Bin # to End)", fftClearHF<P> },                                \
    { WaveOp::Action, "Clear LF (Start to #)", fftClearLF<P> },                                  \
    { WaveOp::Separator },                                                                       \
    { WaveOp::Action, "Generate Saw", fftSaw<P> },                                               \
    { WaveOp::Action, "Randomize Low 16 Bins", fftRandom<P, 16, false> },                        \
    { WaveOp::Action, "Randomize Low 32 Bins", fftRandom<P, 32, false> },                        \
    { WaveOp::Action, "Randomize Low 64 Bins", fftRandom<P, 64, false> },                        \
    { WaveOp::Action, "Randomize Low 16 Bins (with Half)", fftRandom<P, 16, true> },             \
    { WaveOp::Action, "Randomize Low 32 Bins (with Half)", fftRandom<P, 32, true> },             \
    { WaveOp::Action, "Randomize Low 64 Bins (with Half)", fftRandom<P, 64, true> },             \
    { WaveOp::Action, "Randomize All", fftRandom<P, kFftBins, false> },                          \
    { WaveOp::Action, "Create Random Series Gaps", fftGaps<P> },                                 \
    { WaveOp::Action, "Progressive Fade", fftProgFade<P> },                                      \
    { WaveOp::Action, "Shift Octave Up", fftOctUp<P> },                                          \
    { WaveOp::Action, "Shift Octave Down", fftOctDown<P> },                                      \
    { WaveOp::Action, "Repeat Bin Group (Start to Cursor Pos)", fftRepeat<P> },                  \
    { WaveOp::Separator },                                                                       \
    { WaveOp::Toggle, "Draw Even Harmonics Only", fftToggle<FftDrawOptions::EvenOnly>, nullptr, FftDrawOptions::EvenOnly },  \
    { WaveOp::Toggle, "Draw Odd Harmonics Only", fftToggle<FftDrawOptions::OddOnly>, nullptr, FftDrawOptions::OddOnly },     \
    { WaveOp::Toggle, "Snap Vertical Draw to Quarters", fftToggle<FftDrawOptions::SnapQuarters>, nullptr, FftDrawOptions::SnapQuarters }, \
    { WaveOp::Toggle, "Scale Freq Values by Bin Index", fftToggle<FftDrawOptions::ScaleByBin>, nullptr, FftDrawOptions::ScaleByBin }

const WaveOp fftMagsMenu[] { FFT_MENU (false, "FFT Mags") };
const WaveOp fftPhasesMenu[] { FFT_MENU (true, "FFT Phases") };
#undef FFT_MENU

// ---- REMOVE... ----
bool eraseRange (Ctx& c, int a, int b)       // inclusive; never empties the table
{
    const int n = c.set.size();
    a = std::max (a, 0);
    b = std::min (b, n - 1);
    if (a > b || (b - a + 1) >= n) return false;
    c.set.frames.erase (c.set.frames.begin() + a, c.set.frames.begin() + b + 1);
    c.set.keyFrames.clear();
    c.set.modified = true;
    return true;
}

bool opInitAll (Ctx& c)
{
    const std::string name = c.set.name;
    c.set = FrameSet::initSaw();
    c.set.name = name;
    c.set.modified = true;
    c.sel.select (0);
    return true;
}
bool opRemoveMulti (Ctx& c)
{
    c.sel.clamp (c.set.size());
    if (! eraseRange (c, c.sel.first, c.sel.last)) return false;
    c.sel.select (std::min (c.sel.first, c.set.size() - 1));
    return true;
}
bool opRemoveToSelected (Ctx& c)
{
    c.sel.clamp (c.set.size());
    const int k = c.sel.first;
    if (! eraseRange (c, 0, k - 1)) return false;
    c.sel.current -= k; c.sel.first -= k; c.sel.last -= k;
    return true;
}
bool opRemoveFromSelected (Ctx& c)
{
    c.sel.clamp (c.set.size());
    return eraseRange (c, c.sel.last + 1, c.set.size() - 1);
}
bool opCrop (Ctx& c)
{
    c.sel.clamp (c.set.size());
    const int a = c.sel.first, b = c.sel.last;
    if (a == 0 && b == c.set.size() - 1) return false;
    std::vector<Frame> keep (c.set.frames.begin() + a, c.set.frames.begin() + b + 1);
    c.set.frames = std::move (keep);
    c.set.keyFrames.clear();
    c.set.modified = true;
    c.sel.current -= a; c.sel.first = 0; c.sel.last = b - a;
    return true;
}
template <int K> bool opReduce (Ctx& c)
{
    if (c.set.size() <= 1) return false;
    std::vector<Frame> keep;
    for (int i = 0; i < c.set.size(); i += K) keep.push_back (c.set.frames[(size_t) i]);
    if ((int) keep.size() == c.set.size()) return false;
    c.set.frames = std::move (keep);
    c.set.keyFrames.clear();
    c.set.modified = true;
    c.sel.select (std::min (c.sel.current / K, c.set.size() - 1));
    return true;
}

bool opInsert (Ctx& c) { return insertFrame (c.set, c.sel); }
bool opRemove (Ctx& c) { return removeFrame (c.set, c.sel); }

const WaveOp removeMenu[] {
    { WaveOp::Action, "Init All (Default)", opInitAll },
    { WaveOp::Action, "Insert (at current index)", opInsert },
    { WaveOp::Action, "Remove (current index)", opRemove },
    { WaveOp::Action, "Remove: multiselection", opRemoveMulti },
    { WaveOp::Action, "Remove: beginning->selected", opRemoveToSelected },
    { WaveOp::Action, "Remove: selected->end", opRemoveFromSelected },
    { WaveOp::Action, "Remove All (except Selected)", opCrop },
    { WaveOp::Action, "Every 2nd Frame", opReduce<2>, "Reduce to" },
    { WaveOp::Action, "Every 4th Frame", opReduce<4>, "Reduce to" },
    { WaveOp::Action, "Every 8th Frame", opReduce<8>, "Reduce to" },
    { WaveOp::Action, "Every 16th Frame", opReduce<16>, "Reduce to" },
};

// ---- SORT... ----
enum class SortKey { PeakBin, Centroid, PeakAmount, NumBins, HighestBin, Fundamental };

template <SortKey Key> bool opSort (Ctx& c)
{
    const int n = c.set.size();
    if (n < 2) return false;
    std::vector<float> key ((size_t) n);
    Spectrum s;
    for (int i = 0; i < n; ++i)
    {
        analyse (c.set.frames[(size_t) i], s);
        float peak = 0.0f, sum = 0.0f, wsum = 0.0f;
        int peakBin = 0, count = 0, highest = 0;
        for (int k = 1; k < kNyquist; ++k)
        {
            const float m = s.mag[(size_t) k];
            if (m > peak) { peak = m; peakBin = k; }
            sum += m;
            wsum += m * (float) k;
            if (m > 1.0e-4f) { ++count; highest = k; }
        }
        float v = 0.0f;
        switch (Key)
        {
            case SortKey::PeakBin:     v = (float) peakBin; break;
            case SortKey::Centroid:    v = sum > 0.0f ? wsum / sum : 0.0f; break;
            case SortKey::PeakAmount:  v = peak; break;
            case SortKey::NumBins:     v = (float) count; break;
            case SortKey::HighestBin:  v = (float) highest; break;
            case SortKey::Fundamental: v = s.mag[1]; break;
        }
        key[(size_t) i] = v;
    }
    std::vector<int> idx ((size_t) n);
    std::iota (idx.begin(), idx.end(), 0);
    std::stable_sort (idx.begin(), idx.end(), [&key] (int a, int b) { return key[(size_t) a] < key[(size_t) b]; });
    if (std::is_sorted (idx.begin(), idx.end())) return false;
    std::vector<Frame> out;
    out.reserve ((size_t) n);
    for (int i : idx) out.push_back (c.set.frames[(size_t) i]);
    c.set.frames = std::move (out);
    c.set.keyFrames.clear();
    c.set.modified = true;
    return true;
}

bool opShuffle (Ctx& c)
{
    const int n = c.set.size();
    if (n < 2) return false;
    Rng rng (c.seed);
    for (int i = n - 1; i > 0; --i) std::swap (c.set.frames[(size_t) i], c.set.frames[(size_t) (rng.next() % (uint32_t) (i + 1))]);
    c.seed = rng.next();
    c.set.keyFrames.clear();
    c.set.modified = true;
    return true;
}

bool opReverseOrder (Ctx& c)
{
    if (c.set.size() < 2) return false;
    std::reverse (c.set.frames.begin(), c.set.frames.end());
    c.set.keyFrames.clear();
    c.set.modified = true;
    c.sel.select (c.set.size() - 1 - c.sel.current);
    return true;
}

const WaveOp sortMenu[] {
    { WaveOp::Action, "Sort by spectrum (Peak Spect)", opSort<SortKey::PeakBin> },
    { WaveOp::Action, "Sort by spectrum (Average Spect)", opSort<SortKey::Centroid> },
    { WaveOp::Action, "Sort by spectrum (Peak Amount)", opSort<SortKey::PeakAmount> },
    { WaveOp::Action, "Sort by spectrum (Num w/Spect)", opSort<SortKey::NumBins> },
    { WaveOp::Action, "Sort by spectrum (Highest w/Spect)", opSort<SortKey::HighestBin> },
    { WaveOp::Action, "Sort by spectrum (Fundamental Amt.)", opSort<SortKey::Fundamental> },
    { WaveOp::Action, "Sort Randomize", opShuffle },
    { WaveOp::Action, "Reverse Entire Table Order", opReverseOrder },
};

// ---- PROCESS > SINGLE / ALL ----
template <bool All> bool pNormalize (Ctx& c) { return forFrames (c, All, [] (Frame& f) { return normalise (f); }); }
template <bool All> bool pDc (Ctx& c)        { return forFrames (c, All, [] (Frame& f) { return removeDc (f); }); }
template <bool All> bool pInvert (Ctx& c)    { return forFrames (c, All, [] (Frame& f) { return invert (f); }); }
template <bool All> bool pReverse (Ctx& c)   { return forFrames (c, All, [] (Frame& f) { return reverse (f); }); }
template <bool All> bool pZeroX (Ctx& c)     { return forFrames (c, All, [] (Frame& f) { return shiftToZeroCrossing (f); }); }
template <bool All> bool pFade (Ctx& c)      { const int l = cellLen (c); return forFrames (c, All, [l] (Frame& f) { return fadeEdges (f, l); }); }
template <bool All> bool pXFade (Ctx& c)     { const int l = cellLen (c); return forFrames (c, All, [l] (Frame& f) { return crossfadeEdges (f, l); }); }
template <bool All> bool pFilter (Ctx& c)    { const int k = gridN (c); return forFrames (c, All, [k] (Frame& f) { return lowpassHarmonics (f, k); }); }
template <bool All> bool pRedux (Ctx& c)     { const int h = gridN (c); return forFrames (c, All, [h] (Frame& f) { return sampleRedux (f, h); }); }

bool pSilence (Ctx& c)
{
    return forFrames (c, false, [] (Frame& f) { if (peakOf (f) == 0.0f) return false; f.fill (0.0f); return true; });
}

bool pSendToNoise (Ctx& c)
{
    if (c.noiseOut == nullptr) return false;
    c.sel.clamp (c.set.size());
    const Frame& f = c.set.frames[(size_t) c.sel.current];
    c.noiseOut->assign (f.begin(), f.end());
    return false;     // the table is unchanged; the caller loads noiseOut into the noise osc
}

bool pNormalizeSame (Ctx& c)
{
    float p = 0.0f;
    for (auto& f : c.set.frames) p = std::max (p, peakOf (f));
    if (p < 1.0e-9f || std::abs (p - 1.0f) < 1.0e-7f) return false;
    const float g = 1.0f / p;
    for (auto& f : c.set.frames) for (auto& v : f) v *= g;
    c.set.modified = true;
    return true;
}

bool pXFade16 (Ctx& c) { return forFrames (c, true, [] (Frame& f) { return crossfadeEdges (f, 16); }); }

bool pRemoveFund (Ctx& c)
{
    return allSpectra (c, [] (Spectrum& s) { s.mag[1] = 0.0f; });
}

bool pRemoveLowSpectra (Ctx& c)
{
    const int g = gridN (c);
    return allSpectra (c, [g] (Spectrum& s) { for (int k = 0; k <= std::min (g, kNyquist); ++k) s.mag[(size_t) k] = 0.0f; });
}

bool pRemoveLowPhases (Ctx& c)
{
    const int g = gridN (c);
    return allSpectra (c, [g] (Spectrum& s) { for (int k = 1; k <= std::min (g, kNyquist); ++k) s.phase[(size_t) k] = 0.0f; });
}

bool pResizeHalf (Ctx& c)
{
    // Each frame's two halves become frames of their own (pitch +1 octave per half-cycle).
    std::vector<Frame> out;
    for (auto& f : c.set.frames)
    {
        for (int h = 0; h < 2 && (int) out.size() < kMaxFrames; ++h)
        {
            Frame o;
            for (int k = 0; k < N; ++k) o[(size_t) k] = sincRead (f.data(), N, h * (N / 2) + k * 0.5, 1.0f, true);
            out.push_back (o);
        }
    }
    c.set.frames = std::move (out);
    c.set.keyFrames.clear();
    c.set.modified = true;
    c.sel.select (std::min (c.sel.current * 2, c.set.size() - 1));
    return true;
}

bool pResizeDouble (Ctx& c)
{
    // Pairs of frames become one cycle (pitch -1 octave): fixes octave-wrong imports.
    const int n = c.set.size();
    if (n < 2) return false;
    std::vector<Frame> out;
    std::vector<float> buf ((size_t) (2 * N));
    for (int i = 0; i < n; i += 2)
    {
        const Frame& a = c.set.frames[(size_t) i];
        const Frame& b = c.set.frames[(size_t) std::min (i + 1, n - 1)];
        std::copy (a.begin(), a.end(), buf.begin());
        std::copy (b.begin(), b.end(), buf.begin() + N);
        Frame o;
        for (int k = 0; k < N; ++k) o[(size_t) k] = sincRead (buf.data(), 2 * N, 2.0 * k, 0.5f, true);
        out.push_back (o);
    }
    c.set.frames = std::move (out);
    c.set.keyFrames.clear();
    c.set.modified = true;
    c.sel.select (std::min (c.sel.current / 2, c.set.size() - 1));
    return true;
}

bool pCreatePwm (Ctx& c)
{
    // 256 frames: the current frame minus a copy of itself shifted by a width
    // that sweeps from 50% down to ~0.2% (a saw gives a classic PWM square).
    c.sel.clamp (c.set.size());
    const Frame base = c.set.frames[(size_t) c.sel.current];
    if (peakOf (base) == 0.0f) return false;
    std::vector<Frame> out ((size_t) kMaxFrames);
    float peak = 0.0f;
    for (int k = 0; k < kMaxFrames; ++k)
    {
        const int d = std::max (4, (int) std::lround ((N / 2) * (1.0 - (double) k / kMaxFrames)));
        Frame& o = out[(size_t) k];
        for (int i = 0; i < N; ++i) o[(size_t) i] = 0.5f * (base[(size_t) i] - base[(size_t) ((i + d) % N)]);
        peak = std::max (peak, peakOf (o));
    }
    if (peak > 0.0f) for (auto& f : out) for (auto& v : f) v /= peak;
    c.set.frames = std::move (out);
    c.set.keyFrames.clear();
    c.set.modified = true;
    c.sel.select (0);
    return true;
}

bool pNudgeFundamental (Ctx& c)
{
    // Time-shift every frame so its fundamental sits at phase 0 (drawn at 50%
    // in the phase strip): all frames then line up.
    return allSpectra (c, [] (Spectrum& s) { zeroFundamentalPhase (s); });
}

template <bool Phases> bool pSetFromThis (Ctx& c)
{
    c.sel.clamp (c.set.size());
    Spectrum src;
    analyse (c.set.frames[(size_t) c.sel.current], src);
    return allSpectra (c, [&src] (Spectrum& s)
    {
        if (Phases) s.phase = src.phase;
        else s.mag = src.mag;
    });
}

template <int Mode> bool pFromOther (Ctx& c)       // 0 = set spectra, 1 = set phases, 2 = subtract spectra
{
    if (c.otherOsc == nullptr || c.otherOsc->frames.empty()) return false;
    bool changed = false;
    Spectrum s, o;
    for (int i = 0; i < c.set.size(); ++i)
    {
        Frame& f = c.set.frames[(size_t) i];
        analyse (f, s);
        analyse (otherFrame (c, i), o);
        for (int k = 0; k <= kNyquist; ++k)
        {
            if (Mode == 0) s.mag[(size_t) k] = o.mag[(size_t) k];
            else if (Mode == 1) s.phase[(size_t) k] = o.phase[(size_t) k];
            else if (k == 0 || k == kNyquist) s.mag[(size_t) k] -= o.mag[(size_t) k];
            else s.mag[(size_t) k] = std::max (0.0f, s.mag[(size_t) k] - o.mag[(size_t) k]);
        }
        const Frame old = f;
        synthesise (s, f);
        changed |= ! sameFrame (old, f);
    }
    if (changed) c.set.modified = true;
    return changed;
}

// Box blur over a window of `grid` bins (mags) or a circular mean (phases).
template <bool Phases> bool pBlurBins (Ctx& c)
{
    const int r = std::max (1, gridN (c) / 2);
    return allSpectra (c, [r] (Spectrum& s)
    {
        const Spectrum src = s;
        for (int k = 1; k < kNyquist; ++k)
        {
            const int a = std::max (1, k - r), b = std::min (kNyquist - 1, k + r);
            if (Phases)
            {
                float cs = 0.0f, sn = 0.0f;
                for (int j = a; j <= b; ++j) { cs += std::cos (src.phase[(size_t) j]); sn += std::sin (src.phase[(size_t) j]); }
                s.phase[(size_t) k] = std::atan2 (sn, cs);
            }
            else
            {
                float acc = 0.0f;
                for (int j = a; j <= b; ++j) acc += src.mag[(size_t) j];
                s.mag[(size_t) k] = acc / (float) (b - a + 1);
            }
        }
    });
}

template <bool Phases> bool pBlurFrames (Ctx& c)
{
    const int n = c.set.size();
    if (n < 2) return false;
    const int r = std::max (1, gridN (c) / 2);
    std::vector<Spectrum> sp ((size_t) n);
    for (int i = 0; i < n; ++i) analyse (c.set.frames[(size_t) i], sp[(size_t) i]);
    bool changed = false;
    Spectrum s;
    for (int i = 0; i < n; ++i)
    {
        s = sp[(size_t) i];
        const int a = std::max (0, i - r), b = std::min (n - 1, i + r);
        for (int k = 0; k <= kNyquist; ++k)
        {
            if (Phases)
            {
                if (k == 0 || k == kNyquist) continue;
                float cs = 0.0f, sn = 0.0f;
                for (int j = a; j <= b; ++j) { cs += std::cos (sp[(size_t) j].phase[(size_t) k]); sn += std::sin (sp[(size_t) j].phase[(size_t) k]); }
                s.phase[(size_t) k] = std::atan2 (sn, cs);
            }
            else
            {
                float acc = 0.0f;
                for (int j = a; j <= b; ++j) acc += sp[(size_t) j].mag[(size_t) k];
                s.mag[(size_t) k] = acc / (float) (b - a + 1);
            }
        }
        Frame& f = c.set.frames[(size_t) i];
        const Frame old = f;
        synthesise (s, f);
        changed |= ! sameFrame (old, f);
    }
    if (changed) c.set.modified = true;
    return changed;
}

const WaveOp processSingleMenu[] {
    { WaveOp::Action, "Normalize", pNormalize<false> },
    { WaveOp::Action, "Remove DC Offset", pDc<false> },
    { WaveOp::Action, "Flip Vertical (Polarity Invert)", pInvert<false> },
    { WaveOp::Action, "Flip Horizontal (Reverse)", pReverse<false> },
    { WaveOp::Action, "Shift Horizontal to Zero Crossing", pZeroX<false> },
    { WaveOp::Action, "Init (Silence)", pSilence },
    { WaveOp::Action, "Fade Edges (Grid Size)", pFade<false> },
    { WaveOp::Action, "X-Fade Edges (Grid Size)", pXFade<false> },
    { WaveOp::Action, "Filter (Grid Size)", pFilter<false> },
    { WaveOp::Action, "Sample Redux at Grid Size", pRedux<false> },
    { WaveOp::Action, "Send to Noise Oscillator", pSendToNoise },
};

const WaveOp processAllMenu[] {
    { WaveOp::Action, "Normalize Each (Gained Separately)", pNormalize<true> },
    { WaveOp::Action, "Normalize Same (Max From All Frames)", pNormalizeSame },
    { WaveOp::Action, "Remove DC Offset", pDc<true> },
    { WaveOp::Action, "Flip Vertical (Polarity Invert)", pInvert<true> },
    { WaveOp::Action, "Flip Horizontal (Reverse)", pReverse<true> },
    { WaveOp::Action, "Fade Edges (Grid Size)", pFade<true> },
    { WaveOp::Action, "X-Fade Edges (Grid Size)", pXFade<true> },
    { WaveOp::Action, "X-Fade Edges (16 Samples)", pXFade16 },
    { WaveOp::Action, "Filter (Grid Size)", pFilter<true> },
    { WaveOp::Action, "Remove Fundamental (HPF)", pRemoveFund },
    { WaveOp::Action, "Remove Low Spectra (Grid Size)", pRemoveLowSpectra },
    { WaveOp::Action, "Remove Low Phases (Grid Size)", pRemoveLowPhases },
    { WaveOp::Action, "Sample Redux at Grid Size", pRedux<true> },
    { WaveOp::Action, "Resize Tables to be Half (2x Total Number)", pResizeHalf },
    { WaveOp::Action, "Resize Tables to be Double (\xC2\xBD Total Number)", pResizeDouble },
    { WaveOp::Action, "Create PWM from This Table to All", pCreatePwm },
    { WaveOp::Action, "Nudge All Phases for Fundamental to 50%", pNudgeFundamental },
    { WaveOp::Action, "Set Spectra (This Frame to All)", pSetFromThis<false> },
    { WaveOp::Action, "Set Phases (This Frame to All)", pSetFromThis<true> },
    { WaveOp::Action, "Set Spectra from Other Osc", pFromOther<0> },
    { WaveOp::Action, "Set Phases from Other Osc", pFromOther<1> },
    { WaveOp::Action, "Subtract Spectra from Other Osc", pFromOther<2> },
    { WaveOp::Action, "Blur Spectra - Adjacent Bins (Grid Size)", pBlurBins<false> },
    { WaveOp::Action, "Blur Phases - Adjacent Bins (Grid Size)", pBlurBins<true> },
    { WaveOp::Action, "Blur Spectra - Adjacent Frames (Grid Size)", pBlurFrames<false> },
    { WaveOp::Action, "Blur Phases - Adjacent Frames (Grid Size)", pBlurFrames<true> },
    { WaveOp::Action, "Shift Horizontal to Zero-Crossing", pZeroX<true> },
};

// ---- MORPH ----
template <MorphMode M> bool mMorph (Ctx& c)
{
    if (! morph (c.set, M)) return false;
    c.sel.select (0);
    return true;
}

bool mRemove (Ctx& c)
{
    if (! removeMorph (c.set)) return false;
    c.sel.select (0);
    return true;
}

const WaveOp morphMenu[] {
    { WaveOp::Action, "Morph - Crossfade", mMorph<MorphMode::Crossfade> },
    { WaveOp::Action, "Morph - Spectral", mMorph<MorphMode::Spectral> },
    { WaveOp::Action, "Morph \xE2\x80\x93 Spectral (Zero Fund. Phase)", mMorph<MorphMode::SpectralZeroFund> },
    { WaveOp::Action, "Morph \xE2\x80\x93 Spectral (Zero All Phases)", mMorph<MorphMode::SpectralZeroAll> },
    { WaveOp::Action, "Remove Morph Tables", mRemove },
};

template <size_t K> WaveOpList listOf (const WaveOp (&a)[K], const char* title) { return { a, (int) K, title }; }
} // namespace

WaveOpList waveMenu (WaveMenu m) noexcept
{
    switch (m)
    {
        case WaveMenu::FftMags:       return listOf (fftMagsMenu, "FFT Mags");
        case WaveMenu::FftPhases:     return listOf (fftPhasesMenu, "FFT Phases");
        case WaveMenu::Remove:        return listOf (removeMenu, "REMOVE...");
        case WaveMenu::Sort:          return listOf (sortMenu, "SORT...");
        case WaveMenu::ProcessSingle: return listOf (processSingleMenu, "SINGLE");
        case WaveMenu::ProcessAll:    return listOf (processAllMenu, "ALL");
        case WaveMenu::Morph:         return listOf (morphMenu, "MORPH");
        case WaveMenu::NumMenus:      break;
    }
    return { nullptr, 0, "" };
}

std::string menuLabel (const WaveOp& op, int bin)
{
    std::string s = op.label ? op.label : "";
    if (auto p = s.find ('#'); p != std::string::npos) s.replace (p, 1, std::to_string (bin));
    return s;
}

bool isChecked (const WaveOp& op, const EditContext& ctx) noexcept
{
    return op.kind == WaveOp::Toggle && ctx.fftOptions != nullptr && op.toggleFlag >= 0
        && ctx.fftOptions->flags[op.toggleFlag];
}

bool runOp (WaveMenu m, const std::string& label, EditContext& ctx)
{
    const auto list = waveMenu (m);
    for (int i = 0; i < list.count; ++i)
    {
        const WaveOp& op = list.items[i];
        if (op.fn != nullptr && label == op.label) return op.fn (ctx);
    }
    return false;
}
} // namespace tincture::wtedit
