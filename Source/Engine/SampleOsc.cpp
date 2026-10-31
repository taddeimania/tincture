// SampleOsc.cpp — Multisample, Sample, Granular and Spectral oscillator
// modes: the patch data and its message-thread builders (edits, mip levels,
// slices, STFT analysis, wavetable import) and the per-voice renderer.
#include "SampleOsc.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <numeric>

namespace tincture
{
namespace
{
constexpr int kWinSize = 1024;
constexpr int kNumWin = 10;
constexpr double kHuge = 1.0e30;

// 4-point, 3rd-order Hermite interpolation. `d` must be readable at
// [i-1, i+2] for i = floor(pos) (the prepared data is zero padded).
inline float hermite (const float* d, double pos) noexcept
{
    // floor() without a libm call: positions are >= -kPad here.
    const int i = (int) (pos + 16.0) - 16;
    const float f = (float) (pos - (double) i);
    const float xm1 = d[i - 1], x0 = d[i], x1 = d[i + 1], x2 = d[i + 2];
    const float c = (x1 - xm1) * 0.5f;
    const float v = x0 - x1;
    const float w = c + v;
    const float a = w + v + (x2 - x0) * 0.5f;
    const float b = w + a;
    return ((a * f - b) * f + c) * f + x0;
}

// Wrap to [-pi, pi). Inputs here stay within a few hundred turns.
inline float wrapPi (float x) noexcept
{
    const float k = (x + kPi) * (1.0f / kTwoPi);
    int ik = (int) k;
    ik -= (k < (float) ik) ? 1 : 0;
    return x - kTwoPi * (float) ik;
}

// sin on [-pi, pi], ~4e-6 max error.
inline float fastSin (float x) noexcept
{
    if (x > kPi * 0.5f) x = kPi - x;
    else if (x < -kPi * 0.5f) x = -kPi - x;
    const float x2 = x * x;
    return x * (1.0f + x2 * (-1.0f / 6.0f + x2 * (1.0f / 120.0f + x2 * (-1.0f / 5040.0f + x2 * (1.0f / 362880.0f)))));
}
inline float fastCos (float x) noexcept
{
    float y = x + kPi * 0.5f;
    if (y > kPi) y -= kTwoPi;
    return fastSin (y);
}

inline int mipLevel (double absInc) noexcept
{
    if (absInc < 2.0) return 0;
    if (absInc < 4.0) return 1;
    if (absInc < 8.0) return 2;
    return 3;
}

// Window tables for the grain shapes (choices::winShapes order).
struct WindowTables
{
    float t[kNumWin][kWinSize + 1];
    WindowTables()
    {
        for (int s = 0; s < kNumWin; ++s)
            for (int i = 0; i <= kWinSize; ++i)
                t[s][i] = SampleOsc::windowShape (s, (float) i / (float) kWinSize);
    }
};
const WindowTables& windowTables()
{
    static const WindowTables w;
    return w;
}

inline float lookupWin (const float* tab, float x) noexcept
{
    x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
    const float f = x * (float) kWinSize;
    int i = (int) f;
    if (i >= kWinSize) i = kWinSize - 1;
    return tab[i] + (tab[i + 1] - tab[i]) * (f - (float) i);
}

// Grain kernel for the common case: no FM, no loop wrapping, and the read
// position stays inside the padded data for the whole run.
template <bool Stereo>
void grainKernel (const float* d0, const float* d1, double p, double inc, double lsc, float t, float invLen,
                  float peak, float slopeA, float slopeB, float winAmt, const float* winTab, int m, float* L, float* R) noexcept
{
    const float dry = 1.0f - winAmt;
    double q = p * lsc;
    const double dq = inc * lsc;
    for (int k = 0; k < m; ++k)
    {
        const float tt = t < peak ? t * slopeA : 0.5f + (t - peak) * slopeB;
        const float w = dry + winAmt * lookupWin (winTab, tt);
        const int i = (int) (q + 16.0) - 16;
        const float f = (float) (q - (double) i);
        {
            const float* d = d0 + i;
            const float c = (d[1] - d[-1]) * 0.5f, v = d[0] - d[1], ww = c + v, a = ww + v + (d[2] - d[0]) * 0.5f, b = ww + a;
            L[k] = (((a * f - b) * f + c) * f + d[0]) * w;
        }
        if constexpr (Stereo)
        {
            const float* d = d1 + i;
            const float c = (d[1] - d[-1]) * 0.5f, v = d[0] - d[1], ww = c + v, a = ww + v + (d[2] - d[0]) * 0.5f, b = ww + a;
            R[k] = (((a * f - b) * f + c) * f + d[0]) * w;
        }
        q += dq;
        t += invLen;
    }
}

// ---- Analysis helpers (message thread) -------------------------------------
std::vector<float> monoOf (const SampleData& s)
{
    std::vector<float> m (s.left);
    if (s.stereo())
        for (size_t i = 0; i < m.size() && i < s.right.size(); ++i)
            m[i] = 0.5f * (m[i] + s.right[i]);
    return m;
}

// YIN pitch estimate (period in samples, 0 if unpitched) of x[pos .. pos+W).
// Uses an FFT autocorrelation, so it's cheap enough to run many times.
struct PitchTracker
{
    static constexpr int W = 4096;
    SampleFft fft;
    std::vector<float> re, im, d;
    PitchTracker() { fft.init (2 * W); re.resize (2 * W); im.resize (2 * W); d.resize (W / 2 + 1); }

    float period (const float* x, int n, int pos, int minLag, int maxLag)
    {
        maxLag = std::min (maxLag, W / 2);
        if (minLag < 2) minLag = 2;
        if (maxLag <= minLag + 2) return 0.0f;
        for (int i = 0; i < 2 * W; ++i)
        {
            const int k = pos + i;
            re[(size_t) i] = (i < W && k >= 0 && k < n) ? x[k] : 0.0f;
            im[(size_t) i] = 0.0f;
        }
        // Energy prefix sums for the difference function.
        std::vector<double> e (W + 1, 0.0);
        for (int i = 0; i < W; ++i) e[(size_t) i + 1] = e[(size_t) i] + (double) re[(size_t) i] * re[(size_t) i];
        if (e[W] < 1.0e-8) return 0.0f;
        fft.forward (re.data(), im.data());
        for (int i = 0; i < 2 * W; ++i)
        {
            re[(size_t) i] = re[(size_t) i] * re[(size_t) i] + im[(size_t) i] * im[(size_t) i];
            im[(size_t) i] = 0.0f;
        }
        fft.inverse (re.data(), im.data());
        const double scale = 1.0 / (2.0 * W);
        // Difference function over the window: d(tau) = e[0..W-tau) + e[tau..W) - 2 r(tau).
        double run = 0.0;
        d[0] = 1.0f;
        float best = 0.0f;
        for (int tau = 1; tau <= maxLag; ++tau)
        {
            const double r = re[(size_t) tau] * scale;
            const double dt = (e[(size_t) (W - tau)] - e[0]) + (e[W] - e[(size_t) tau]) - 2.0 * r;
            run += dt;
            d[(size_t) tau] = run > 0.0 ? (float) (dt * tau / run) : 1.0f;
        }
        int tau = -1;
        for (int t = minLag; t < maxLag; ++t)
            if (d[(size_t) t] < 0.15f)
            {
                while (t + 1 < maxLag && d[(size_t) t + 1] < d[(size_t) t]) ++t;
                tau = t;
                break;
            }
        if (tau < 0)
        {
            // No dip under the threshold: take the global minimum if it is clear enough.
            float m = 1.0e9f;
            for (int t = minLag; t < maxLag; ++t)
                if (d[(size_t) t] < m) { m = d[(size_t) t]; tau = t; }
            if (m > 0.35f) return 0.0f;
        }
        best = (float) tau;
        if (tau > 1 && tau < maxLag - 1)
        {
            const float a = d[(size_t) tau - 1], b = d[(size_t) tau], c = d[(size_t) tau + 1];
            const float den = a - 2.0f * b + c;
            if (std::abs (den) > 1.0e-9f) best += 0.5f * (a - c) / den;
        }
        return best;
    }
};

// Cubic resample of x[start .. start+span) into `out` (len samples).
void resampleSpan (const std::vector<float>& x, double start, double span, float* out, int len)
{
    const int n = (int) x.size();
    auto at = [&] (int i) { return i >= 0 && i < n ? x[(size_t) i] : 0.0f; };
    for (int i = 0; i < len; ++i)
    {
        const double p = start + span * (double) i / (double) len;
        const double fl = std::floor (p);
        const int k = (int) fl;
        const float f = (float) (p - fl);
        const float xm1 = at (k - 1), x0 = at (k), x1 = at (k + 1), x2 = at (k + 2);
        const float c = (x1 - xm1) * 0.5f, v = x0 - x1, w = c + v, a = w + v + (x2 - x0) * 0.5f, b = w + a;
        out[i] = ((a * f - b) * f + c) * f + x0;
    }
}
} // namespace

// ============================================================================
// SampleFft
// ============================================================================
void SampleFft::init (int size)
{
    n = size;
    cosT.resize ((size_t) n / 2);
    sinT.resize ((size_t) n / 2);
    for (int k = 0; k < n / 2; ++k)
    {
        const double a = 2.0 * 3.14159265358979323846 * k / n;
        cosT[(size_t) k] = (float) std::cos (a);
        sinT[(size_t) k] = (float) std::sin (a);
    }
    bitrev.resize ((size_t) n);
    int bits = 0;
    while ((1 << bits) < n) ++bits;
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
        bitrev[(size_t) i] = r;
    }
}

void SampleFft::transform (float* re, float* im, float sign) const noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const int j = bitrev[(size_t) i];
        if (j > i) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
    }
    // First two stages without twiddles.
    for (int i = 0; i + 1 < n; i += 2)
    {
        const float ar = re[i], ai = im[i], br = re[i + 1], bi = im[i + 1];
        re[i] = ar + br; im[i] = ai + bi; re[i + 1] = ar - br; im[i + 1] = ai - bi;
    }
    for (int len = 4; len <= n; len <<= 1)
    {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len)
        {
            float* r0 = re + i; float* i0 = im + i;
            float* r1 = r0 + half; float* i1 = i0 + half;
            for (int k = 0; k < half; ++k)
            {
                const float wr = cosT[(size_t) (k * step)], wi = sign * sinT[(size_t) (k * step)];
                const float tr = r1[k] * wr - i1[k] * wi;
                const float ti = r1[k] * wi + i1[k] * wr;
                r1[k] = r0[k] - tr; i1[k] = i0[k] - ti;
                r0[k] += tr; i0[k] += ti;
            }
        }
    }
}

void SampleFft::forward (float* re, float* im) const noexcept { transform (re, im, -1.0f); }
void SampleFft::inverse (float* re, float* im) const noexcept { transform (re, im, 1.0f); }

// ============================================================================
// PreparedSample
// ============================================================================
std::shared_ptr<const PreparedSample> PreparedSample::build (const SampleData& s)
{
    auto p = std::make_shared<PreparedSample>();
    p->sampleRate = s.sampleRate > 0.0 ? s.sampleRate : 48000.0;
    p->frames = s.frames();
    p->channels = s.stereo() ? 2 : 1;
    p->rootNote = s.rootNote;
    p->loopStart = s.loopStart;
    p->loopEnd = s.loopEnd;
    p->slices = s.slices;

    // Half-band-ish decimation filter: windowed sinc, cutoff 0.225 of the
    // input rate (0.45 of the output Nyquist), Blackman window, 49 taps.
    constexpr int K = 24;
    float h[2 * K + 1];
    double sum = 0.0;
    for (int k = -K; k <= K; ++k)
    {
        const double fc = 0.225;
        const double x = 2.0 * fc * k;
        const double sinc = k == 0 ? 1.0 : std::sin (3.14159265358979323846 * x) / (3.14159265358979323846 * x);
        const double t = (double) (k + K) / (2.0 * K);
        const double win = 0.42 - 0.5 * std::cos (2.0 * 3.14159265358979323846 * t) + 0.08 * std::cos (4.0 * 3.14159265358979323846 * t);
        h[k + K] = (float) (2.0 * fc * sinc * win);
        sum += h[k + K];
    }
    for (auto& v : h) v = (float) (v / sum);

    for (int c = 0; c < p->channels; ++c)
    {
        const std::vector<float>& src = c == 0 ? s.left : s.right;
        auto& d0 = p->data[0][c];
        d0.assign ((size_t) (p->frames + 2 * kPad), 0.0f);
        std::copy (src.begin(), src.begin() + p->frames, d0.begin() + kPad);
        p->levelFrames[0] = p->frames;
        for (int L = 1; L < kLevels; ++L)
        {
            const auto& prev = p->data[L - 1][c];
            const int pf = p->levelFrames[L - 1];
            const int nf = (pf + 1) / 2;
            p->levelFrames[L] = nf;
            auto& d = p->data[L][c];
            d.assign ((size_t) (nf + 2 * kPad), 0.0f);
            for (int m = 0; m < nf; ++m)
            {
                float acc = 0.0f;
                for (int k = -K; k <= K; ++k)
                {
                    const int idx = 2 * m + k;
                    if (idx >= 0 && idx < pf) acc += h[k + K] * prev[(size_t) (idx + kPad)];
                }
                d[(size_t) (m + kPad)] = acc;
            }
        }
    }
    return p;
}

// ============================================================================
// SampleEnvSettings / MultiSample
// ============================================================================
void SampleEnvSettings::save (Node& n) const
{
    n.set ("delay", delay).set ("attack", attack).set ("hold", hold).set ("decay", decay)
     .set ("sustain", sustain).set ("release", release).set ("delaySync", delaySync ? 1.0 : 0.0);
}

void SampleEnvSettings::load (const Node& n)
{
    SampleEnvSettings d;
    delay = (float) n.num ("delay", d.delay);
    attack = (float) n.num ("attack", d.attack);
    hold = (float) n.num ("hold", d.hold);
    decay = (float) n.num ("decay", d.decay);
    sustain = (float) n.num ("sustain", d.sustain);
    release = (float) n.num ("release", d.release);
    delaySync = n.flag ("delaySync", d.delaySync);
}

int MultiSample::findZone (int key, int velocity) const noexcept
{
    key = std::clamp (key, 0, 127);
    velocity = std::clamp (velocity, 1, 127);
    int best = -1, bestDist = 1 << 30;
    for (int i = 0; i < (int) zones.size(); ++i)
    {
        const Zone& z = zones[(size_t) i];
        if (! z.sample) continue;
        if (key >= z.loKey && key <= z.hiKey && velocity >= z.loVel && velocity <= z.hiVel) return i;
        // Fallback: nearest zone (so notes outside the map still sound).
        const int kd = key < z.loKey ? z.loKey - key : (key > z.hiKey ? key - z.hiKey : 0);
        const int vd = velocity < z.loVel ? z.loVel - velocity : (velocity > z.hiVel ? velocity - z.hiVel : 0);
        const int dist = kd * 128 + vd;
        if (dist < bestDist) { bestDist = dist; best = i; }
    }
    return best;
}

MultiSample MultiSample::fromFiles (const std::string& name,
                                    const std::vector<std::pair<std::string, std::shared_ptr<const SampleData>>>& files)
{
    struct Item { std::shared_ptr<const SampleData> s; int root; float layer; };
    std::vector<Item> items;
    auto lower = [] (std::string s) { for (auto& c : s) c = (char) std::tolower ((unsigned char) c); return s; };
    for (const auto& [file, sample] : files)
    {
        if (! sample || sample->frames() == 0) continue;
        int root = rootNoteFromName (file);
        if (root < 0) root = sample->rootNote;
        // Velocity layer hint: "v3", "vel100", or a dynamic word as a token.
        std::string base = file;
        if (auto dot = base.find_last_of ('.'); dot != std::string::npos) base = base.substr (0, dot);
        float layer = -1.0f;
        std::vector<std::string> tokens;
        std::string cur;
        for (char c : base)
        {
            if (c == ' ' || c == '_' || c == '-' || c == '.') { if (! cur.empty()) tokens.push_back (lower (cur)); cur.clear(); }
            else cur += c;
        }
        if (! cur.empty()) tokens.push_back (lower (cur));
        static const char* dyn[] { "ppp", "pp", "p", "mp", "mf", "f", "ff", "fff" };
        for (const auto& t : tokens)
        {
            size_t k = 0;
            if (t.rfind ("vel", 0) == 0) k = 3;
            else if (t.size() > 1 && t[0] == 'v') k = 1;
            if (k > 0 && k < t.size() && std::all_of (t.begin() + (long) k, t.end(), [] (char c) { return std::isdigit ((unsigned char) c) != 0; }))
                layer = (float) std::atoi (t.c_str() + k);
            for (int d = 0; d < 8; ++d)
                if (t == dyn[d]) layer = 1000.0f + (float) d;      // dynamics sort after numbers; only compared within a root
        }
        if (layer < 0.0f)
        {
            float pk = 0.0f;
            for (float v : sample->left) pk = std::max (pk, std::abs (v));
            layer = 2000.0f + pk;
        }
        items.push_back ({ sample, root, layer });
    }
    MultiSample ms;
    ms.name = name;
    if (items.empty()) return ms;

    std::vector<int> roots;
    for (auto& it : items) roots.push_back (it.root);
    std::sort (roots.begin(), roots.end());
    roots.erase (std::unique (roots.begin(), roots.end()), roots.end());
    for (size_t r = 0; r < roots.size(); ++r)
    {
        const int lo = r == 0 ? 0 : (roots[r - 1] + roots[r]) / 2 + 1;
        const int hi = r + 1 == roots.size() ? 127 : (roots[r] + roots[r + 1]) / 2;
        std::vector<Item> layer;
        for (auto& it : items) if (it.root == roots[r]) layer.push_back (it);
        std::sort (layer.begin(), layer.end(), [] (const Item& a, const Item& b) { return a.layer < b.layer; });
        const int nl = (int) layer.size();
        for (int l = 0; l < nl; ++l)
        {
            Zone z;
            z.sample = layer[(size_t) l].s;
            z.root = roots[r];
            z.loKey = lo; z.hiKey = hi;
            z.loVel = 1 + (127 * l) / nl;
            z.hiVel = (127 * (l + 1)) / nl;
            if (l == nl - 1) z.hiVel = 127;
            z.loop = z.sample->loopStart >= 0 && z.sample->loopEnd > z.sample->loopStart;
            ms.zones.push_back (z);
        }
    }
    return ms;
}

void MultiSample::save (Node& n) const
{
    n.set ("name", name);
    for (const auto& z : zones)
    {
        Node& c = n.add ("zone");
        c.set ("loKey", (double) z.loKey).set ("hiKey", (double) z.hiKey).set ("loVel", (double) z.loVel)
         .set ("hiVel", (double) z.hiVel).set ("root", (double) z.root).set ("tune", z.tuneCents)
         .set ("gain", z.gainDb).set ("loop", z.loop ? 1.0 : 0.0).set ("hasEnv", z.hasEnv ? 1.0 : 0.0);
        if (z.hasEnv) z.env.save (c.add ("env"));
        if (z.sample) z.sample->save (c.add ("sample"));
    }
}

std::shared_ptr<const MultiSample> MultiSample::load (const Node& n)
{
    auto ms = std::make_shared<MultiSample>();
    ms->name = n.str ("name");
    for (const Node* c : n.children ("zone"))
    {
        Zone z;
        z.loKey = c->integer ("loKey", 0); z.hiKey = c->integer ("hiKey", 127);
        z.loVel = c->integer ("loVel", 1); z.hiVel = c->integer ("hiVel", 127);
        z.root = c->integer ("root", 60);
        z.tuneCents = (float) c->num ("tune", 0.0); z.gainDb = (float) c->num ("gain", 0.0);
        z.loop = c->flag ("loop"); z.hasEnv = c->flag ("hasEnv");
        if (const Node* e = c->child ("env")) z.env.load (*e);
        if (const Node* s = c->child ("sample")) z.sample = SampleData::load (*s);
        ms->zones.push_back (z);
    }
    return ms;
}

// ============================================================================
// SampleOscData
// ============================================================================
const std::vector<std::string>& SampleOscData::granularYTargets()
{
    static const std::vector<std::string> l { "None", "Level", "Warp", "Warp 2", "Density", "Grain Length", "Window Amt",
        "Window Skew", "Rand Offset", "Rand Dir", "Rand Pitch", "Rand Length", "Rand Pan", "Rand Gain", "Rand Window",
        "Rand Skew", "Rand Warp", "Rand Warp 2" };
    return l;
}

const std::vector<std::string>& SampleOscData::spectralYTargets()
{
    static const std::vector<std::string> l { "None", "Level", "Warp", "Warp 2", "Spec Flt Cutoff", "Spec Flt Wet/Dry", "Freq Lo", "Freq Hi" };
    return l;
}

const std::vector<std::string>& SampleOscData::fadeEdgeChoices()
{
    static const std::vector<std::string> l { "None", "1 ms", "2 ms", "4 ms", "8 ms", "16 ms", "32 ms", "64 ms", "128 ms" };
    return l;
}

const std::vector<std::string>& SampleOscData::wavetableImportChoices()
{
    static const std::vector<std::string> l { "Normal (dynamic pitch zero-snap)", "Normal (dynamic pitch follow)",
        "Constant framesize (pitch avg)", "Frequency estimation", "FFT 256", "FFT 512", "FFT 1024", "FFT 2048" };
    return l;
}

std::shared_ptr<const SampleData> SampleOscData::applyEdits (const SampleData& src, const std::vector<SampleEdit>& edits, int fadeEdges)
{
    auto s = std::make_shared<SampleData> (src);
    for (const auto& e : edits)
    {
        const int n = s->frames();
        if (n == 0) break;
        switch (e.kind)
        {
            case SampleEdit::Normalize:
            {
                float pk = 0.0f;
                for (float v : s->left) pk = std::max (pk, std::abs (v));
                for (float v : s->right) pk = std::max (pk, std::abs (v));
                if (pk > 1.0e-9f)
                {
                    const float g = 1.0f / pk;
                    for (auto& v : s->left) v *= g;
                    for (auto& v : s->right) v *= g;
                }
                break;
            }
            case SampleEdit::Reverse:
            {
                std::reverse (s->left.begin(), s->left.end());
                std::reverse (s->right.begin(), s->right.end());
                if (s->loopStart >= 0 && s->loopEnd > s->loopStart)
                {
                    const int a = n - s->loopEnd, b = n - s->loopStart;
                    s->loopStart = a; s->loopEnd = b;
                }
                for (auto& x : s->slices) x = n - x;
                std::sort (s->slices.begin(), s->slices.end());
                break;
            }
            case SampleEdit::Trim:
            {
                int a = (int) std::lround (std::clamp (e.a, 0.0f, 1.0f) * (float) n);
                int b = (int) std::lround (std::clamp (e.b, 0.0f, 1.0f) * (float) n);
                if (b < a) std::swap (a, b);
                if (b - a < 2) break;
                s->left = std::vector<float> (s->left.begin() + a, s->left.begin() + b);
                if (s->stereo()) s->right = std::vector<float> (s->right.begin() + a, s->right.begin() + b);
                auto shift = [&] (int& x) { if (x >= 0) x = std::clamp (x - a, 0, b - a); };
                shift (s->loopStart); shift (s->loopEnd);
                std::vector<int> sl;
                for (int x : s->slices) if (x >= a && x < b) sl.push_back (x - a);
                s->slices = sl;
                break;
            }
            default: break;
        }
    }
    static const int fadeMs[] { 0, 1, 2, 4, 8, 16, 32, 64, 128 };
    const int fi = std::clamp (fadeEdges, 0, 8);
    if (fadeMs[fi] > 0 && s->frames() > 4)
    {
        const int n = s->frames();
        const int len = std::min (n / 2, (int) std::lround (fadeMs[fi] * 0.001 * s->sampleRate));
        for (int i = 0; i < len; ++i)
        {
            const float g = (float) i / (float) len;
            s->left[(size_t) i] *= g; s->left[(size_t) (n - 1 - i)] *= g;
            if (s->stereo()) { s->right[(size_t) i] *= g; s->right[(size_t) (n - 1 - i)] *= g; }
        }
    }
    return s;
}

std::vector<int> SampleOscData::detectSlices (const SampleData& s, float threshold)
{
    std::vector<int> out { 0 };
    const int n = s.frames();
    if (n < 64) return out;
    const std::vector<float> m = monoOf (s);
    float peak = 0.0f;
    for (float v : m) peak = std::max (peak, std::abs (v));
    if (peak < 1.0e-6f) return out;

    // Peak envelope in hops of 64 frames, normalised to the sample's peak.
    constexpr int hop = 64;
    const int nh = (n + hop - 1) / hop;
    std::vector<float> env ((size_t) nh, 0.0f);
    for (int h = 0; h < nh; ++h)
    {
        float pk = 0.0f;
        for (int i = h * hop; i < std::min (n, (h + 1) * hop); ++i) pk = std::max (pk, std::abs (m[(size_t) i]));
        env[(size_t) h] = pk / peak;
    }
    const float thr = std::clamp (threshold, 0.001f, 1.0f);
    const int minGap = std::max (1, (int) (0.03 * s.sampleRate / hop));     // 30 ms between slices
    const int look = std::max (2, (int) (0.01 * s.sampleRate / hop));       // compare with the previous ~10 ms
    int last = -minGap;
    for (int h = 1; h < nh; ++h)
    {
        const float e = env[(size_t) h];
        if (e < thr) continue;
        float prev = 0.0f;
        for (int k = std::max (0, h - look); k < h; ++k) prev = std::max (prev, env[(size_t) k]);
        if (e < prev * 2.0f + 0.02f) continue;                              // needs a clear rise
        if (h - last < minGap) continue;
        // Precise onset: first frame in the previous/current hop over 30 % of this hop's peak.
        const float lvl = 0.3f * e * peak;
        int pos = h * hop;
        for (int i = std::max (0, (h - 1) * hop); i < std::min (n, (h + 1) * hop); ++i)
            if (std::abs (m[(size_t) i]) >= lvl) { pos = i; break; }
        pos = std::max (0, pos - 4);
        last = h;
        if (pos < (int) (0.01 * s.sampleRate)) continue;                    // merges with slice 1 at 0
        if (pos - out.back() < (int) (0.02 * s.sampleRate)) continue;
        out.push_back (pos);
    }
    return out;
}

std::shared_ptr<const SpectralData> SampleOscData::buildSpectral (const SampleData& s)
{
    constexpr int N = SpectralData::kFftSize, H = SpectralData::kHop, B = SpectralData::kBins, P = SpectralData::kPreFrames;
    auto sd = std::make_shared<SpectralData>();
    sd->sampleRate = s.sampleRate > 0.0 ? s.sampleRate : 48000.0;
    sd->sampleFrames = s.frames();
    const std::vector<float> m = monoOf (s);
    const int n = (int) m.size();
    sd->frames = n / H + 1 + P;
    sd->mag.assign ((size_t) sd->frames * B, 0.0f);
    sd->freq.assign ((size_t) sd->frames * B, 0.0f);
    sd->phase.assign ((size_t) sd->frames * B, 0.0f);
    sd->transient.assign ((size_t) sd->frames, 0);

    SampleFft fft;
    fft.init (N);
    std::vector<float> re (N), im (N), win (N), prevPhase (B, 0.0f), prevMag (B, 0.0f);
    for (int i = 0; i < N; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) N);
    std::vector<float> flux ((size_t) sd->frames, 0.0f);
    const float expect = kTwoPi * (float) H / (float) N;
    for (int f = 0; f < sd->frames; ++f)
    {
        const int start = (f - P) * H;
        for (int i = 0; i < N; ++i)
        {
            const int k = start + i;
            re[(size_t) i] = (k >= 0 && k < n ? m[(size_t) k] : 0.0f) * win[(size_t) i];
            im[(size_t) i] = 0.0f;
        }
        fft.forward (re.data(), im.data());
        float* mg = sd->mag.data() + (size_t) f * B;
        float* fr = sd->freq.data() + (size_t) f * B;
        float* ph = sd->phase.data() + (size_t) f * B;
        float fl = 0.0f;
        for (int k = 0; k < B; ++k)
        {
            const float a = std::sqrt (re[(size_t) k] * re[(size_t) k] + im[(size_t) k] * im[(size_t) k]);
            const float p = std::atan2 (im[(size_t) k], re[(size_t) k]);
            mg[k] = a;
            ph[k] = p;
            if (f == 0) fr[k] = (float) k;
            else
            {
                const float dev = wrapPi (p - prevPhase[(size_t) k] - expect * (float) k);
                fr[k] = (float) k + dev / expect;
            }
            // Spectral flux on log magnitudes (for transient flags).
            const float d = std::log (1.0f + a) - std::log (1.0f + prevMag[(size_t) k]);
            if (d > 0.0f) fl += d;
            prevPhase[(size_t) k] = p;
            prevMag[(size_t) k] = a;
        }
        flux[(size_t) f] = fl;
    }
    // Transient = flux peak well above its local median.
    for (int f = 1; f < sd->frames; ++f)
    {
        float avg = 0.0f; int cnt = 0;
        for (int k = std::max (0, f - 8); k < f; ++k) { avg += flux[(size_t) k]; ++cnt; }
        avg = cnt ? avg / (float) cnt : 0.0f;
        const bool peak = flux[(size_t) f] >= flux[(size_t) f - 1] && (f + 1 >= sd->frames || flux[(size_t) f] >= flux[(size_t) f + 1]);
        if (peak && flux[(size_t) f] > 2.0f * avg + 5.0f) sd->transient[(size_t) f] = 1;
    }
    return sd;
}

void SampleOscData::rebuild (bool withSpectral)
{
    processed.reset(); prepared.reset(); spectral.reset(); zones.reset();
    if (sample && sample->frames() > 0)
    {
        auto p = applyEdits (*sample, edits, fadeEdges);
        auto pm = std::const_pointer_cast<SampleData> (p);
        if (rootNote >= 0) pm->rootNote = rootNote;
        if (slicing == SliceAuto) pm->slices = detectSlices (*p, sliceThreshold);
        else if (slicing == SliceManual)
        {
            std::vector<int> sl { 0 };
            std::vector<float> fr (manualSlices);
            std::sort (fr.begin(), fr.end());
            for (float f : fr)
            {
                const int x = (int) std::lround (std::clamp (f, 0.0f, 1.0f) * (float) p->frames());
                if (x > sl.back() && x < p->frames()) sl.push_back (x);
            }
            pm->slices = sl;
        }
        else pm->slices.clear();
        processed = p;
        prepared = PreparedSample::build (*p);
        if (withSpectral || type == 4) spectral = buildSpectral (*p);
    }
    if (multi)
    {
        auto z = std::make_shared<std::vector<std::shared_ptr<const PreparedSample>>>();
        for (const auto& zone : multi->zones)
            z->push_back (zone.sample && zone.sample->frames() > 0 ? PreparedSample::build (*zone.sample) : nullptr);
        zones = z;
    }
    specLut = std::make_shared<const BakedCurve> (specFilter);
}

float SampleOscData::estimatePitch (const SampleData& s)
{
    const std::vector<float> m = monoOf (s);
    const int n = (int) m.size();
    if (n < 256) return 0.0f;
    PitchTracker pt;
    const double sr = s.sampleRate;
    const int minLag = std::max (2, (int) (sr / 4000.0)), maxLag = std::min (PitchTracker::W / 2 - 2, (int) (sr / 25.0));
    std::vector<float> periods;
    const int count = std::clamp (n / PitchTracker::W, 1, 24);
    for (int c = 0; c < count; ++c)
    {
        const int pos = (int) ((double) c * std::max (0, n - PitchTracker::W) / std::max (1, count - 1));
        const float p = pt.period (m.data(), n, count == 1 ? 0 : pos, minLag, maxLag);
        if (p > 0.0f) periods.push_back (p);
    }
    if (periods.empty()) return 0.0f;
    std::sort (periods.begin(), periods.end());
    return (float) (sr / periods[periods.size() / 2]);
}

std::vector<std::vector<float>> SampleOscData::toWavetable (const SampleData& s, int mode, int maxFrames)
{
    constexpr int W = kWtFrameSize;
    std::vector<std::vector<float>> out;
    const std::vector<float> m = monoOf (s);
    const int n = (int) m.size();
    if (n < 16) return out;
    maxFrames = std::clamp (maxFrames, 1, 256);
    const double sr = s.sampleRate;

    // Segments (start, length) to resample into frames.
    std::vector<std::pair<double, double>> segs;
    auto fixedChop = [&] (int size)
    {
        for (int p = 0; p + size / 2 < n || segs.empty(); p += size)
        {
            segs.push_back ({ (double) p, (double) size });
            if (p + size >= n) break;
        }
    };
    if (mode >= Fft256)
        fixedChop (256 << (mode - Fft256));
    else
    {
        const float f0 = estimatePitch (s);
        if (f0 <= 0.0f) fixedChop (W);
        else if (mode == ConstantFramesize || mode == FrequencyEstimation)
        {
            const double period = sr / f0;
            if (mode == ConstantFramesize)
                for (double p = 0.0; p + period <= n; p += period) segs.push_back ({ p, period });
            else
            {
                // One cycle at evenly spaced points through the file.
                const int cycles = std::max (1, (int) ((n - period) / period));
                const int count = std::min (cycles, maxFrames);
                for (int k = 0; k < count; ++k)
                    segs.push_back ({ std::floor ((double) k * (n - period) / std::max (1, count - 1 > 0 ? count - 1 : 1)), period });
            }
        }
        else
        {
            // Dynamic pitch: track the period through the file.
            PitchTracker pt;
            const int minLag = std::max (2, (int) (sr / 4000.0)), maxLag = std::min (PitchTracker::W / 2 - 2, (int) (sr / 25.0));
            const double defPeriod = sr / f0;
            double pos = 0.0, period = defPeriod;
            int nextEstimate = 0;
            // Start on a rising zero crossing for the zero-snap mode.
            auto risingZeroNear = [&] (double target, double radius) -> double
            {
                const int a = std::max (1, (int) (target - radius)), b = std::min (n - 1, (int) (target + radius));
                double best = -1.0, bestD = kHuge;
                for (int i = a; i <= b; ++i)
                    if (m[(size_t) i - 1] < 0.0f && m[(size_t) i] >= 0.0f)
                    {
                        const double frac = m[(size_t) i - 1] / (double) (m[(size_t) i - 1] - m[(size_t) i]);
                        const double z = i - 1 + frac;
                        if (std::abs (z - target) < bestD) { bestD = std::abs (z - target); best = z; }
                    }
                return best;
            };
            if (mode == NormalZeroSnap)
            {
                const double z = risingZeroNear (0.0, defPeriod);
                if (z >= 0.0) pos = z;
            }
            while (pos + period <= n && (int) segs.size() < 8192)
            {
                if ((int) pos >= nextEstimate)
                {
                    const float p = pt.period (m.data(), n, (int) pos, minLag, maxLag);
                    if (p > 0.0f && std::abs (std::log (p / defPeriod)) < 1.2) period = p;
                    nextEstimate = (int) pos + PitchTracker::W / 2;
                }
                double len = period;
                if (mode == NormalZeroSnap)
                {
                    const double z = risingZeroNear (pos + period, period * 0.25);
                    if (z > pos + 2.0) len = z - pos;
                }
                if (pos + len > n) break;
                segs.push_back ({ pos, len });
                pos += len;
            }
            if (segs.empty()) segs.push_back ({ 0.0, std::min ((double) n, defPeriod) });
        }
    }
    // Thin to maxFrames, evenly.
    std::vector<std::pair<double, double>> pick;
    if ((int) segs.size() <= maxFrames) pick = segs;
    else
        for (int k = 0; k < maxFrames; ++k)
            pick.push_back (segs[(size_t) ((double) k * (segs.size() - 1) / (maxFrames - 1 > 0 ? maxFrames - 1 : 1))]);

    float peak = 0.0f;
    for (const auto& [st, len] : pick)
    {
        std::vector<float> f ((size_t) W);
        resampleSpan (m, st, len, f.data(), W);
        // Remove DC so cycles join cleanly.
        double dc = 0.0;
        for (float v : f) dc += v;
        dc /= W;
        for (auto& v : f) { v -= (float) dc; peak = std::max (peak, std::abs (v)); }
        out.push_back (std::move (f));
    }
    if (peak > 1.0e-9f)
        for (auto& f : out) for (auto& v : f) v /= peak;
    return out;
}

void SampleOscData::save (Node& n) const
{
    n.set ("type", (double) type).set ("rootNote", (double) rootNote).set ("fadeEdges", (double) fadeEdges);
    if (sample) sample->save (n.add ("sample"));
    if (multi) multi->save (n.add ("multi"));
    for (const auto& e : edits)
        n.add ("edit").set ("kind", (double) e.kind).set ("a", e.a).set ("b", e.b);
    auto b = [] (bool v) { return v ? 1.0 : 0.0; };
    n.set ("showMarkerAnimation", b (showMarkerAnimation)).set ("zoomToStartEnd", b (zoomToStartEnd))
     .set ("showWaveform", b (showWaveform)).set ("snap", (double) snap).set ("lastPlayedZone", (double) lastPlayedZone)
     .set ("slicing", (double) slicing).set ("sliceThreshold", sliceThreshold)
     .set ("playSliceToEnd", b (playSliceToEnd)).set ("playSingleSlice", b (playSingleSlice))
     .set ("autoSyncToClip", b (autoSyncToClip)).set ("singleSlice", (double) singleSlice).set ("sliceRoot", (double) sliceRoot)
     .set ("manualLoop", b (manualLoop)).set ("relativeLoop", b (relativeLoop)).set ("linkLoopLength", b (linkLoopLength))
     .set ("exitLoopOnRelease", b (exitLoopOnRelease)).set ("loopGrains", b (loopGrains))
     .set ("scanRange", (double) scanRange).set ("scanReverse", b (scanReverse)).set ("scanKeyTrack", b (scanKeyTrack))
     .set ("scanTempoLock", b (scanTempoLock)).set ("lengthToBpm", b (lengthToBpm)).set ("lengthBeats", lengthBeats)
     .set ("densityMode", (double) densityMode).set ("densTriplet", b (densTriplet)).set ("densDotted", b (densDotted))
     .set ("jumpStart", b (jumpStart)).set ("maxGrains", (double) maxGrains).set ("lengthMode", (double) lengthMode)
     .set ("lenTriplet", b (lenTriplet)).set ("lenDotted", b (lenDotted)).set ("reverseGrains", b (reverseGrains))
     .set ("spawnPattern", (double) spawnPattern).set ("randWarp1", randWarp1).set ("randWarp2", randWarp2)
     .set ("showXY", b (showXY)).set ("yTargetGranular", (double) yTargetGranular).set ("yTargetSpectral", (double) yTargetSpectral)
     .set ("yValue", yValue).set ("phaseLock", b (phaseLock)).set ("transients", b (transients))
     .set ("specSmooth", b (specSmooth)).set ("specPostWarp", b (specPostWarp)).set ("specFilterTable", specFilterTable)
     .set ("envOverride", b (envOverride)).set ("velTrack", b (velTrack)).set ("velTrackAmount", velTrackAmount)
     .set ("phaseRand", phaseRand);
    if (! manualSlices.empty()) n.set ("manualSlices", manualSlices);
    if (! specFilterMask.empty()) n.set ("specFilterMask", specFilterMask);
    specFilter.save (n.add ("specFilter"));
    env.save (n.add ("env"));
}

void SampleOscData::load (const Node& n)
{
    const SampleOscData d;
    *this = d;
    type = std::clamp (n.integer ("type", d.type), 1, 4);
    rootNote = n.integer ("rootNote", d.rootNote);
    fadeEdges = std::clamp (n.integer ("fadeEdges", d.fadeEdges), 0, 8);
    if (const Node* c = n.child ("sample")) sample = SampleData::load (*c);
    if (const Node* c = n.child ("multi")) multi = MultiSample::load (*c);
    for (const Node* e : n.children ("edit"))
        edits.push_back ({ std::clamp (e->integer ("kind", 0), 0, 2), (float) e->num ("a", 0.0), (float) e->num ("b", 1.0) });
    showMarkerAnimation = n.flag ("showMarkerAnimation", d.showMarkerAnimation);
    zoomToStartEnd = n.flag ("zoomToStartEnd", d.zoomToStartEnd);
    showWaveform = n.flag ("showWaveform", d.showWaveform);
    snap = std::clamp (n.integer ("snap", d.snap), 0, 3);
    lastPlayedZone = n.integer ("lastPlayedZone", d.lastPlayedZone);
    slicing = std::clamp (n.integer ("slicing", d.slicing), 0, 2);
    sliceThreshold = (float) n.num ("sliceThreshold", d.sliceThreshold);
    playSliceToEnd = n.flag ("playSliceToEnd", d.playSliceToEnd);
    playSingleSlice = n.flag ("playSingleSlice", d.playSingleSlice);
    autoSyncToClip = n.flag ("autoSyncToClip", d.autoSyncToClip);
    singleSlice = std::max (0, n.integer ("singleSlice", d.singleSlice));
    sliceRoot = std::clamp (n.integer ("sliceRoot", d.sliceRoot), 0, 127);
    manualLoop = n.flag ("manualLoop", d.manualLoop);
    relativeLoop = n.flag ("relativeLoop", d.relativeLoop);
    linkLoopLength = n.flag ("linkLoopLength", d.linkLoopLength);
    exitLoopOnRelease = n.flag ("exitLoopOnRelease", d.exitLoopOnRelease);
    loopGrains = n.flag ("loopGrains", d.loopGrains);
    scanRange = std::clamp (n.integer ("scanRange", d.scanRange), 0, 2);
    scanReverse = n.flag ("scanReverse", d.scanReverse);
    scanKeyTrack = n.flag ("scanKeyTrack", d.scanKeyTrack);
    scanTempoLock = n.flag ("scanTempoLock", d.scanTempoLock);
    lengthToBpm = n.flag ("lengthToBpm", d.lengthToBpm);
    lengthBeats = (float) n.num ("lengthBeats", d.lengthBeats);
    densityMode = std::clamp (n.integer ("densityMode", d.densityMode), 0, 2);
    densTriplet = n.flag ("densTriplet", d.densTriplet);
    densDotted = n.flag ("densDotted", d.densDotted);
    jumpStart = n.flag ("jumpStart", d.jumpStart);
    maxGrains = std::clamp (n.integer ("maxGrains", d.maxGrains), 1, SampleOsc::kMaxGrains);
    lengthMode = std::clamp (n.integer ("lengthMode", d.lengthMode), 0, 2);
    lenTriplet = n.flag ("lenTriplet", d.lenTriplet);
    lenDotted = n.flag ("lenDotted", d.lenDotted);
    reverseGrains = n.flag ("reverseGrains", d.reverseGrains);
    spawnPattern = std::clamp (n.integer ("spawnPattern", d.spawnPattern), 0, 3);
    randWarp1 = (float) n.num ("randWarp1", d.randWarp1);
    randWarp2 = (float) n.num ("randWarp2", d.randWarp2);
    showXY = n.flag ("showXY", d.showXY);
    yTargetGranular = std::clamp (n.integer ("yTargetGranular", 0), 0, (int) granularYTargets().size() - 1);
    yTargetSpectral = std::clamp (n.integer ("yTargetSpectral", 0), 0, (int) spectralYTargets().size() - 1);
    yValue = (float) n.num ("yValue", d.yValue);
    phaseLock = n.flag ("phaseLock", d.phaseLock);
    transients = n.flag ("transients", d.transients);
    specSmooth = n.flag ("specSmooth", d.specSmooth);
    specPostWarp = n.flag ("specPostWarp", d.specPostWarp);
    specFilterTable = n.str ("specFilterTable", d.specFilterTable);
    envOverride = n.flag ("envOverride", d.envOverride);
    velTrack = n.flag ("velTrack", d.velTrack);
    velTrackAmount = (float) n.num ("velTrackAmount", d.velTrackAmount);
    phaseRand = (float) n.num ("phaseRand", d.phaseRand);
    if (auto* f = n.floats ("manualSlices")) manualSlices = *f;
    if (auto* f = n.floats ("specFilterMask")) specFilterMask = *f;
    if (const Node* c = n.child ("specFilter")) specFilter.load (*c);
    if (const Node* c = n.child ("env")) env.load (*c);
}

// ============================================================================
// SampleOsc
// ============================================================================
SampleOsc::Telemetry::Telemetry()
{
    for (auto& p : playhead) p.store (-1.0f, std::memory_order_relaxed);
    for (auto& g : grainPos) g.store (-1.0f, std::memory_order_relaxed);
}

float SampleOsc::windowShape (int shape, float t) noexcept
{
    t = clamp01 (t);
    const float u = 2.0f * t - 1.0f;            // -1..1
    switch (shape)
    {
        case 0: return 0.5f - 0.5f * std::cos (kTwoPi * t);                                  // Hann
        case 1: return 1.0f - u * u;                                                         // Welch
        case 2: { const float z = (t - 0.5f) / 0.15f; return std::exp (-0.5f * z * z); }    // Gaussian
        case 3: return 0.35875f - 0.48829f * std::cos (kTwoPi * t) + 0.14128f * std::cos (2.0f * kTwoPi * t)
                       - 0.01168f * std::cos (3.0f * kTwoPi * t);                            // Blackman-Harris
        case 4:                                                                              // Sinc (k = 3) with a Hann taper
        {
            const float x = kPi * 3.0f * u;
            const float s = std::abs (x) < 1.0e-5f ? 1.0f : std::sin (x) / x;
            return s * (0.5f + 0.5f * std::cos (kPi * u));
        }
        case 5:                                                                              // Tukey, alpha 0.5
        {
            const float a = 0.5f;
            if (t < a * 0.5f) return 0.5f - 0.5f * std::cos (kTwoPi * t / a);
            if (t > 1.0f - a * 0.5f) return 0.5f - 0.5f * std::cos (kTwoPi * (1.0f - t) / a);
            return 1.0f;
        }
        case 6: return 1.0f - std::abs (u);                                                  // Triangle
        case 7: return std::min (1.0f, std::min (t, 1.0f - t) * 4.0f);                       // Trapezoid (25 % ramps)
        case 8:                                                                              // ExpDec (1 % click-free attack)
        {
            const float e = (std::exp (-6.0f * t) - std::exp (-6.0f)) / (1.0f - std::exp (-6.0f));
            return t < 0.01f ? e * t * 100.0f : e;
        }
        case 9:                                                                              // Exp Dec Rev
        {
            const float r = 1.0f - t;
            const float e = (std::exp (-6.0f * r) - std::exp (-6.0f)) / (1.0f - std::exp (-6.0f));
            return r < 0.01f ? e * r * 100.0f : e;
        }
        default: return 1.0f;
    }
}

struct SampleOsc::TickParams
{
    const ParamView* P = nullptr;
    const OscTick* tick = nullptr;
    int n = kTick;
    int xyId = -1;
    float xyVal = 0.0f;
    float get (int id) const noexcept { return id == xyId ? xyVal : (*P) (id); }

    float semis0 = 0.0f, semis1 = 0.0f;     // pitch relative to the root, start / end of tick
    float pitchHz = 261.6f;
    float level = 0.75f, pan = 0.0f;
    int w1Mode = 0, w2Mode = 0;
    float w1 = 0.0f, w2 = 0.0f, uniW1 = 0.0f, uniW2 = 0.0f;
    bool w1Post = false, w2Post = false, fm = false;
    const float* w1Src = nullptr;
    const float* w2Src = nullptr;
    float sr = 48000.0f;
    double srcRatio = 1.0;
    double speed = 1.0;                     // signed, frames of the sample per output sample (before pitch for Sample)
    Region region;
    // Granular.
    double interval = 2400.0;
    int grainLen = 4800;
    float randWarp1 = 0.0f, randWarp2 = 0.0f;
    // Spectral.
    float cut = 0.0f, mix = 1.0f, lo = 20.0f, hi = 20000.0f;
};

void SampleOsc::prepare (const Context& c)
{
    ctx = c;
    reg = &params();
    winTables = &windowTables().t[0][0];
    constexpr int N = SpectralData::kFftSize, B = SpectralData::kBins;
    fft.init (N);
    specPhase.assign ((size_t) kMaxUnison * B, 0.0f);
    specLastFrame.assign ((size_t) kMaxUnison, -1);
    accL.assign (N, 0.0f); accR.assign (N, 0.0f);
    zr.assign (N, 0.0f); zi.assign (N, 0.0f);
    cLr.assign (B, 0.0f); cLi.assign (B, 0.0f); cRr.assign (B, 0.0f); cRi.assign (B, 0.0f);
    srcMag.assign (B, 0.0f); bestMag.assign (B, 0.0f); newPhase.assign (B, 0.0f);
    peaks.assign (B, 0);
    mask.assign (B, 1.0f); binX.assign (B, 0.0f);
    anaWin.assign (N, 0.0f); synWin.assign (N, 0.0f);
    hopL.assign (SpectralData::kHop, 0.0f); hopR.assign (SpectralData::kHop, 0.0f);
    for (int i = 0; i < N; ++i)
        anaWin[(size_t) i] = synWin[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) N);
    for (int j = 0; j < B; ++j)
    {
        const double f = std::max (1.0, j * c.sampleRate / N);
        binX[(size_t) j] = (float) (std::log (f / 20.0) / std::log (1000.0));
    }
    reset();
}

void SampleOsc::reset()
{
    active = false;
    isFinished = true;
    numGrains = 0;
    numStreams = 1;
    for (auto& s : streams) s = Stream();
    std::fill (accL.begin(), accL.end(), 0.0f);
    std::fill (accR.begin(), accR.end(), 0.0f);
    hopRead = SpectralData::kHop;
    specStarted = false;
    for (auto& k : maskKey) k = -1.0f;
    tele.numPlayheads.store (0, std::memory_order_relaxed);
    tele.numGrains.store (0, std::memory_order_relaxed);
}

void SampleOsc::computeUnison (const ParamView& P, const OscIds& ids, int n)
{
    const int mode = P.integer (ids.uniMode);
    const int stack = P.integer (ids.uniStack);
    const float det = clamp01 (P (ids.detune)) * std::max (0.0f, P (ids.uniRange)) * 0.5f;
    const float blend = clamp01 (P (ids.blend));
    const float width = clamp01 (P (ids.uniWidth));
    float sumSq = 0.0f;
    for (int u = 0; u < n; ++u)
    {
        Stream& s = streams[u];
        const float l = n == 1 ? 0.0f : -1.0f + 2.0f * (float) u / (float) (n - 1);
        const float a = std::abs (l), sg = l < 0.0f ? -1.0f : 1.0f;
        float x = l;
        switch (mode)
        {
            case 1: x = l * a; break;                                                // Super: dense near the centre
            case 2: x = sg * (std::exp2 (3.0f * a) - 1.0f) / 7.0f; break;            // Exp
            case 3: x = sg * std::sqrt (a); break;                                   // Inv
            case 4: x = s.rnd; break;                                                // Random (per note)
            default: break;
        }
        float st = 0.0f;
        const bool centre = (n % 2 == 1) ? u == n / 2 : (u == n / 2 || u == n / 2 - 1);
        switch (stack)
        {
            case 1: { const int k = std::min (n, 4); st = 12.0f * (float) (u % k); break; }
            case 2: { static const float iv[] { 0.0f, 7.0f, 12.0f, 19.0f, 24.0f, 31.0f, 36.0f }; st = iv[u % std::min (n, 7)]; break; }
            case 3: st = centre && n > 1 ? -12.0f : 0.0f; break;
            case 4: st = centre && n > 1 ? -24.0f : 0.0f; break;
            default: break;
        }
        s.detune = n == 1 ? 0.0f : x * det + st;
        s.pan = l * width;
        s.warpSpread = l;
        uniX[u] = l;
        float g = 1.0f;
        if (n > 2) g = centre ? std::min (1.0f, (1.0f - blend) * 4.0f) : std::min (1.0f, blend / 0.75f);
        s.gain = g;
        sumSq += g * g;
    }
    const float norm = sumSq > 1.0e-9f ? 1.0f / std::sqrt (sumSq) : 0.0f;
    for (int u = 0; u < n; ++u) streams[u].gain *= norm;
}

void SampleOsc::computeRegion (const ParamView& P, const OscIds& ids, const SampleOscData& data, const PreparedSample& ps, Region& r) const
{
    const double F = std::max (2, ps.frames);
    r.frames = F;
    const double st = clamp01 (P (ids.start)), en = clamp01 (P (ids.end));
    double S = 0.0, E = F;
    const bool sliced = sliceIndex >= 0 && ! ps.slices.empty();
    if (type == 1) { S = 0.0; E = F; }
    else if (sliced)
    {
        const int idx = std::min (sliceIndex, (int) ps.slices.size() - 1);
        const double a = ps.slices[(size_t) idx];
        const double b = (data.playSliceToEnd || idx + 1 >= (int) ps.slices.size()) ? F : (double) ps.slices[(size_t) idx + 1];
        S = a + st * (b - a); E = a + en * (b - a);
    }
    else { S = st * F; E = en * F; }
    if (E < S) std::swap (S, E);
    if (E - S < 2.0) { E = std::min (F, S + 2.0); S = std::max (0.0, E - 2.0); }
    r.start = S; r.end = E;

    int mode = 0;
    double ls = S, le = E;
    bool reversed = false;
    if (type == 1)
    {
        const bool zl = data.multi && zoneIndex >= 0 && zoneIndex < (int) data.multi->zones.size() && data.multi->zones[(size_t) zoneIndex].loop;
        if (zl && ps.loopStart >= 0 && ps.loopEnd > ps.loopStart) { mode = 1; ls = ps.loopStart; le = ps.loopEnd; }
    }
    else
    {
        mode = std::clamp (P.integer (ids.loopMode), 0, 4);
        if (data.manualLoop && type >= 3) mode = 0;
        double a = P (ids.loopStart), b = P (ids.loopEnd);
        if (data.linkLoopLength && P.base) b = a + (P.base[ids.loopEnd] - P.base[ids.loopStart]);
        if (data.relativeLoop || sliced) { ls = S + a * (E - S); le = S + b * (E - S); }
        else { ls = a * F; le = b * F; }
        if (le < ls) { std::swap (ls, le); reversed = true; }
        ls = std::clamp (ls, S, E); le = std::clamp (le, S, E);
        if (mode == 4) { ls = 0.5 * (S + E); le = E; reversed = false; }
    }
    if (le - ls < 2.0)
    {
        if (ls + 2.0 <= E) le = ls + 2.0;
        else { le = E; ls = std::max (S, le - 2.0); }
    }
    if (reversed) mode = mode == 1 ? 2 : (mode == 2 ? 1 : mode);
    r.mode = mode;
    r.looping = mode != 0;
    r.ls = ls; r.le = le;
    const double xf = clamp01 (P (ids.xfade)) * (le - ls);
    r.xlF = std::min (xf, ls);
    r.xlB = std::min (xf, F - le);
    if (r.xlF < 1.0) r.xlF = 0.0;
    if (r.xlB < 1.0) r.xlB = 0.0;
}

void SampleOsc::bounds (const Stream& s, const Region& r, double& lo, double& hi) noexcept
{
    if (! r.looping || s.exiting)
    {
        lo = s.pos >= r.start ? r.start : 0.0;
        hi = s.pos < r.end ? r.end : r.frames;
        return;
    }
    if (s.inLoop) { lo = r.ls; hi = r.le; return; }
    if (s.pos < r.ls) { lo = s.pos >= r.start ? r.start : 0.0; hi = r.ls; }
    else { lo = r.le; hi = s.pos < r.end ? r.end : r.frames; }
}

void SampleOsc::boundary (Stream& s, const Region& r, double lo, double hi) noexcept
{
    if (! r.looping || s.exiting) { s.done = true; return; }
    if (! s.inLoop)
    {
        const bool enter = (hi == r.ls && s.pos >= hi) || (lo == r.le && s.pos < lo);
        if (! enter) { s.done = true; return; }
        s.inLoop = true;
    }
    if (s.pos >= r.ls && s.pos < r.le) return;
    const double len = r.le - r.ls;
    if (r.mode == 3)
    {
        const double off = s.pos - r.ls;
        const double m = std::floor (off / len);
        const double u = off - m * len;
        const bool odd = (((long long) m) & 1) != 0;
        s.pos = odd ? r.le - u : r.ls + u;
        if (odd) s.dir = -s.dir;
    }
    else
    {
        if (r.mode == 2 && s.pos >= r.le && s.dir > 0)
        {
            s.pos = 2.0 * r.le - s.pos;
            s.dir = -1;
            if (s.pos >= r.le) s.pos = r.le - 1.0e-6;
        }
        if (s.pos < r.ls || s.pos >= r.le)
        {
            const double off = s.pos - r.ls;
            s.pos = r.ls + off - std::floor (off / len) * len;
        }
    }
    if (s.pos >= r.le) s.pos = r.le - 1.0e-6;
    if (s.pos < r.ls) s.pos = r.ls;
}

void SampleOsc::advancePlayhead (Stream& s, double delta, const Region& r) const noexcept
{
    if (s.done) return;
    if (r.looping && ! s.inLoop && ! s.exiting && s.pos >= r.ls && s.pos < r.le) s.inLoop = true;
    s.pos += delta * s.dir;
    for (int g = 0; g < 4 && ! s.done; ++g)
    {
        double lo, hi;
        bounds (s, r, lo, hi);
        if (s.pos >= hi || s.pos < lo) boundary (s, r, lo, hi);
        else break;
    }
}

void SampleOsc::applyPostWarps (float* buf, int n, int offset, warp::PostState& s1, warp::PostState& s2,
                                float a1, float a2, const TickParams& tp) noexcept
{
    if (tp.w1Post && a1 > 0.0f)
        warp::processPost (tp.w1Mode, a1, buf, n, tp.w1Src ? tp.w1Src + offset : nullptr, s1, tp.sr, tp.pitchHz);
    if (tp.w2Post && a2 > 0.0f)
        warp::processPost (tp.w2Mode, a2, buf, n, tp.w2Src ? tp.w2Src + offset : nullptr, s2, tp.sr, tp.pitchHz);
}

// ---------------------------------------------------------------------------
// Note on / off
// ---------------------------------------------------------------------------
void SampleOsc::noteOn (const NoteInfo& note, const OscTick& tick, const OscIds& ids, const SampleOscData& data)
{
    const bool wasActive = active && ! isFinished;
    noteInfo = note;
    if (note.legato && wasActive && type == data.type) return;       // keep playing, the pitch follows the tick

    type = data.type;
    rng = Rng ((uint32_t) note.unisonSeed * 2654435761u ^ (uint32_t) (note.voiceIndex + 1) * 0x85EBCA6Bu);
    active = true;
    isFinished = false;
    exitOnRelease = data.exitLoopOnRelease;
    numGrains = 0;
    spawnCountdown = 0.0;
    firstTick = true;
    specStarted = false;
    specTailHops = 0;
    hopRead = SpectralData::kHop;
    std::fill (accL.begin(), accL.end(), 0.0f);
    std::fill (accR.begin(), accR.end(), 0.0f);
    lvlPrev = -1.0f;
    velGain = 1.0f;
    zoneIndex = -1;
    sliceIndex = -1;
    envActive = false;

    const ParamView& P = *tick.p;
    if (type == 1)
    {
        if (data.velTrack) velGain = 1.0f - clamp01 (data.velTrackAmount) * (1.0f - clamp01 (note.velocity));
        if (data.multi && data.zones)
        {
            const int key = note.note - (int) std::lround (clampf (P (ids.timbre), -1.0f, 1.0f) * 24.0f);
            zoneIndex = data.multi->findZone (key, std::max (1, (int) std::lround (note.velocity * 127.0f)));
            if (zoneIndex >= 0 && zoneIndex < (int) data.zones->size() && (*data.zones)[(size_t) zoneIndex])
            {
                const auto& z = data.multi->zones[(size_t) zoneIndex];
                velGain *= dbToGain (z.gainDb);
                if (data.envOverride) { envActive = true; envSet = data.env; }
                else if (z.hasEnv) { envActive = true; envSet = z.env; }
            }
            else zoneIndex = -1;
        }
        tele.zone.store (zoneIndex, std::memory_order_relaxed);
        if (zoneIndex < 0) { active = false; isFinished = true; return; }
        envStage = 0; envTime = 0.0f; envLevel = 0.0f;
    }
    else if (data.prepared && data.slicing != SampleOscData::SlicingOff && ! (data.manualLoop && type >= 3)
             && ! data.prepared->slices.empty())
    {
        const int count = (int) data.prepared->slices.size();
        sliceIndex = data.playSingleSlice ? std::min (data.singleSlice, count - 1) : note.note - data.sliceRoot;
        if (sliceIndex < 0 || sliceIndex >= count) { sliceIndex = -1; active = false; isFinished = true; }
        tele.slice.store (sliceIndex, std::memory_order_relaxed);
        if (! active) return;
    }
    startStreams (tick, ids, data);
}

void SampleOsc::noteOff()
{
    if (exitOnRelease)
        for (auto& s : streams)
            if (s.inLoop) { s.exiting = true; s.dir = 1; }
    if (envActive && envStage < 5) { envStage = 5; envTime = 0.0f; envRelStart = envLevel; }
}

void SampleOsc::startStreams (const OscTick& tick, const OscIds& ids, const SampleOscData& data)
{
    const ParamView& P = *tick.p;
    const PreparedSample* ps = nullptr;
    if (type == 1) { if (data.zones && zoneIndex >= 0) ps = (*data.zones)[(size_t) zoneIndex].get(); }
    else ps = data.prepared.get();
    if (! ps || ps->frames < 2) { active = false; isFinished = true; return; }

    Region r;
    computeRegion (P, ids, data, *ps, r);
    numStreams = std::clamp (P.integer (ids.unison), 1, kMaxUnison);
    for (int u = 0; u < kMaxUnison; ++u)
    {
        Stream& s = streams[u];
        s = Stream();
        s.rnd = rng.bi();
    }
    computeUnison (P, ids, numStreams);

    double scan = P (ids.scan) * (data.scanReverse ? -1.0 : 1.0);
    const double span = r.end - r.start;
    const float uStart = type == 1 ? 0.0f : clamp01 (P (ids.uniStart));
    const float uSpan = type == 1 ? 0.0f : clamp01 (P (ids.uniSpan));
    const double phaseOff = type == 1 ? clamp01 (data.phaseRand) * rng.uni() * span : 0.0;
    for (int u = 0; u < numStreams; ++u)
    {
        Stream& s = streams[u];
        double off = phaseOff + (double) uStart * rng.uni() * span;
        if (numStreams > 1) off += (double) uSpan * span * (double) u / (double) numStreams;
        if (off >= span) off = std::fmod (off, span);
        s.pos = scan < 0.0 && type != 1 ? r.end - 1.0e-3 - off : r.start + off;
        s.pos = std::clamp (s.pos, 0.0, r.frames - 1.0e-3);
        s.dir = 1;
        s.gl = s.gr = -1.0f;                 // first tick: no ramp
        if (r.looping && s.pos >= r.ls && s.pos < r.le) s.inLoop = true;
    }
    for (int u = 0; u < kMaxUnison; ++u)
        specLastFrame[(size_t) u] = -1;
}

float SampleOsc::multiEnvelope (int n) noexcept
{
    if (! envActive) return 1.0f;
    float t = (float) n / (float) ctx.sampleRate;
    const float delay = envSet.delaySync ? envSet.delay * 60.0f / (float) std::max (1.0, ctx.bpm) : envSet.delay;
    for (int guard = 0; guard < 8 && t > 0.0f; ++guard)
    {
        auto seg = [&] (float len, int next) -> bool        // advances within a timed stage; true when it ends
        {
            const float rem = len - envTime;
            if (rem <= t) { t -= std::max (0.0f, rem); envTime = 0.0f; envStage = next; return true; }
            envTime += t; t = 0.0f; return false;
        };
        switch (envStage)
        {
            case 0: envLevel = 0.0f; seg (delay, 1); break;
            case 1: if (seg (envSet.attack, 2)) envLevel = 1.0f; else envLevel = envSet.attack > 0.0f ? envTime / envSet.attack : 1.0f; break;
            case 2: envLevel = 1.0f; seg (envSet.hold, 3); break;
            case 3:
                if (seg (envSet.decay, 4)) envLevel = envSet.sustain;
                else envLevel = 1.0f - (1.0f - envSet.sustain) * (envSet.decay > 0.0f ? envTime / envSet.decay : 1.0f);
                break;
            case 4: envLevel = envSet.sustain; t = 0.0f; break;
            case 5:
                if (seg (envSet.release, 6)) envLevel = 0.0f;
                else envLevel = envRelStart * (1.0f - (envSet.release > 0.0f ? envTime / envSet.release : 1.0f));
                break;
            default: envLevel = 0.0f; t = 0.0f; break;
        }
    }
    if (envStage == 6) isFinished = true;
    return envLevel;
}

// ---------------------------------------------------------------------------
// Render
// ---------------------------------------------------------------------------
void SampleOsc::render (OscTick& tick, const OscIds& ids, const SampleOscData& data)
{
    const int n = std::clamp (tick.n, 0, kTick);
    std::fill (tick.outL, tick.outL + n, 0.0f);
    std::fill (tick.outR, tick.outR + n, 0.0f);
    tick.activeUnison = 0;
    if (active && data.type != type)
    {
        // The mode changed under a sounding note: restart in the new mode.
        NoteInfo ni = noteInfo;
        ni.legato = false;
        noteOn (ni, tick, ids, data);
    }
    if (! active || isFinished)
    {
        if (tick.outMono) std::fill (tick.outMono, tick.outMono + n, 0.0f);
        tele.numPlayheads.store (0, std::memory_order_relaxed);
        tele.numGrains.store (0, std::memory_order_relaxed);
        return;
    }
    exitOnRelease = data.exitLoopOnRelease;

    const ParamView& P = *tick.p;
    TickParams tp;
    tp.P = &P;
    tp.tick = &tick;
    tp.n = n;
    tp.sr = (float) ctx.sampleRate;

    const PreparedSample* ps = nullptr;
    if (type == 1) { if (data.zones && zoneIndex >= 0 && zoneIndex < (int) data.zones->size()) ps = (*data.zones)[(size_t) zoneIndex].get(); }
    else ps = data.prepared.get();
    if (! ps || ps->frames < 2) { isFinished = true; return; }

    // X|Y target.
    if (data.showXY && (type == 3 || type == 4))
    {
        const int t = type == 3 ? data.yTargetGranular : data.yTargetSpectral;
        int id = -1;
        if (type == 3)
        {
            const int map[] { -1, ids.level, ids.warp1, ids.warp2, ids.density, ids.grainLen, ids.winAmt, ids.winSkew, ids.grOffset,
                              ids.grDir, ids.grPitch, ids.grLenRand, ids.grPanRand, ids.grGainRand, ids.winAmtRand, ids.winSkewRand };
            if (t > 0 && t < (int) (sizeof (map) / sizeof (map[0]))) id = map[t];
        }
        else
        {
            const int map[] { -1, ids.level, ids.warp1, ids.warp2, ids.specCut, ids.specMix, ids.specLo, ids.specHi };
            if (t > 0 && t < (int) (sizeof (map) / sizeof (map[0]))) id = map[t];
        }
        if (id >= 0 && reg) { tp.xyId = id; tp.xyVal = (*reg)[id].fromNorm (clamp01 (data.yValue)); }
    }
    tp.randWarp1 = data.randWarp1;
    tp.randWarp2 = data.randWarp2;
    if (data.showXY && type == 3 && data.yTargetGranular == 16) tp.randWarp1 = clamp01 (data.yValue);
    if (data.showXY && type == 3 && data.yTargetGranular == 17) tp.randWarp2 = clamp01 (data.yValue);

    // Pitch relative to the root.
    float b0 = tick.pitchStart, b1 = tick.pitchEnd;
    if (! P.on (ids.pitchTrack)) b0 = b1 = 60.0f + (P.on (ids.bendTrack) ? tick.bendSemis : 0.0f);
    const float off = (float) P.integer (ids.oct) * 12.0f + (float) P.integer (ids.sem) + P (ids.fine) * 0.01f + P (ids.coarse);
    float root = (float) ps->rootNote;
    if (type == 1 && data.multi && zoneIndex >= 0)
    {
        const auto& z = data.multi->zones[(size_t) zoneIndex];
        root = (float) z.root - z.tuneCents * 0.01f;
    }
    if (sliceIndex >= 0)
    {
        if (data.playSingleSlice) { tp.semis0 = b0 - (float) data.sliceRoot + off; tp.semis1 = b1 - (float) data.sliceRoot + off; }
        else { tp.semis0 = tp.semis1 = off + (b1 - (float) noteInfo.note); }    // slices play at their own rate (+ bend/glide)
    }
    else { tp.semis0 = b0 - root + off; tp.semis1 = b1 - root + off; }
    tp.pitchHz = noteToHz (b1 + off);
    tp.level = std::max (0.0f, tp.get (ids.level)) * velGain;
    tp.pan = tp.get (ids.pan);
    tp.srcRatio = ps->sampleRate / ctx.sampleRate;

    // Warps: POST warps run through Warp.cpp; FM becomes playback-rate modulation.
    tp.w1Mode = P.integer (ids.warp1Mode);
    tp.w2Mode = P.integer (ids.warp2Mode);
    tp.w1 = clamp01 (tp.get (ids.warp1));
    tp.w2 = clamp01 (tp.get (ids.warp2));
    tp.uniW1 = P (ids.uniWarp1);
    tp.uniW2 = P (ids.uniWarp2);
    auto srcFor = [&] (int mode) -> const float*
    {
        if (mode >= warp::AmFirst && mode < warp::AmFirst + 7) return tick.src[mode - warp::AmFirst];
        if (mode >= warp::RmFirst && mode < warp::RmFirst + 7) return tick.src[mode - warp::RmFirst];
        return nullptr;
    };
    tp.w1Post = tp.w1Mode != 0 && warp::isPost (tp.w1Mode);
    tp.w2Post = tp.w2Mode != 0 && warp::isPost (tp.w2Mode);
    tp.w1Src = srcFor (tp.w1Mode);
    tp.w2Src = srcFor (tp.w2Mode);
    tp.fm = false;
    for (int slot = 0; slot < 2; ++slot)
    {
        const int mode = slot == 0 ? tp.w1Mode : tp.w2Mode;
        const float amt = slot == 0 ? tp.w1 : tp.w2;
        if (mode == 0 || amt <= 0.0f || warp::kindOf (mode) != warp::Kind::Fm) continue;
        const int si = mode - warp::FmFirst;
        const float* src = si >= 0 && si < kNumSrcBuffers ? tick.src[si] : nullptr;
        if (! src) continue;
        if (! tp.fm) { std::fill (fmBuf, fmBuf + n, 1.0f); tp.fm = true; }
        const int fmType = P.integer (ids.fmType);
        const float k = amt * 4.0f;
        if (fmType == 2) for (int i = 0; i < n; ++i) fmBuf[i] *= std::exp2 (k * src[i]);
        else if (fmType == 1) for (int i = 0; i < n; ++i) fmBuf[i] *= 1.0f + k * src[i];
        else for (int i = 0; i < n; ++i) fmBuf[i] *= std::max (0.0f, 1.0f + k * src[i]);
    }

    // Region and scan speed.
    computeRegion (P, ids, data, *ps, tp.region);
    static const double rangeMul[] { 1.0, 2.0, 4.0 };
    double scan = P (ids.scan) * rangeMul[std::clamp (data.scanRange, 0, 2)] * (data.scanReverse ? -1.0 : 1.0);
    if (data.scanTempoLock) scan *= std::max (1.0, ctx.bpm) / 120.0;
    if (data.lengthToBpm)
    {
        const double secs = (tp.region.end - tp.region.start) / ps->sampleRate;
        const double target = std::max (0.01f, data.lengthBeats) * 60.0 / std::max (1.0, ctx.bpm);
        scan *= secs / target;
    }
    if (type == 1) scan = 1.0;
    if (type <= 2) tp.speed = scan;
    else
    {
        tp.speed = scan * tp.srcRatio;
        if (data.scanKeyTrack) tp.speed *= std::exp2 (tp.semis1 / 12.0);
    }

    // Unison layout follows the (modulatable) settings every tick.
    const int nu = std::clamp (P.integer (ids.unison), 1, kMaxUnison);
    if (nu > numStreams)
    {
        // Voices added mid-note start where voice 0 is.
        for (int u = numStreams; u < nu; ++u)
        {
            const float rnd = streams[u].rnd;
            streams[u] = streams[0];
            streams[u].rnd = rnd;
            streams[u].gl = streams[u].gr = 0.0f;
            for (auto& a : streams[u].post) for (auto& b : a) b.reset();
        }
    }
    numStreams = nu;
    computeUnison (P, ids, numStreams);

    switch (type)
    {
        case 1: case 2: renderSample (tick, ids, data, *ps, tp); break;
        case 3: renderGranular (tick, ids, data, *ps, tp); break;
        case 4: renderSpectral (tick, ids, data, tp); break;
        default: break;
    }

    if (tick.outMono)
        for (int i = 0; i < n; ++i) tick.outMono[i] = 0.5f * (tick.outL[i] + tick.outR[i]);

    // Telemetry.
    const float invF = 1.0f / (float) std::max (1, ps->frames);
    tele.numPlayheads.store (numStreams, std::memory_order_relaxed);
    for (int u = 0; u < numStreams; ++u)
        tele.playhead[u].store (streams[u].done ? -1.0f : (float) streams[u].pos * invF, std::memory_order_relaxed);
    tele.numGrains.store (numGrains, std::memory_order_relaxed);
    if (type == 3)
        for (int g = 0; g < std::min (numGrains, kMaxTelemetryGrains); ++g)
            tele.grainPos[g].store ((float) grains[g].pos * invF, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Sample / Multisample
// ---------------------------------------------------------------------------
int SampleOsc::boundedStream (Stream& s, const Region& r, const PreparedSample& ps, int level, double inc0, double incStep,
                              const float* fm, int n, float* L, float* R, bool stereo) noexcept
{
    const float* d0 = ps.channel (level, 0);
    const float* d1 = ps.channel (level, 1);
    const double lsc = 1.0 / (double) (1 << level);
    double lo, hi, fLo, fHi;
    const double len = r.le - r.ls;
    bool fwdWrap = false, backWrap = false;
    auto refresh = [&]
    {
        bounds (s, r, lo, hi);
        fLo = -kHuge; fHi = kHuge;
        fwdWrap = backWrap = false;
        if (s.inLoop && ! s.exiting && r.looping)
        {
            fwdWrap = r.mode == 1 || r.mode == 4 || (r.mode == 2 && s.dir < 0);
            backWrap = r.mode == 1 || r.mode == 2 || r.mode == 4;
            if (fwdWrap && r.xlF > 0.0) fHi = r.le - r.xlF;
            if (backWrap && r.xlB > 0.0) fLo = r.ls + r.xlB;
        }
    };
    if (r.looping && ! s.inLoop && ! s.exiting && s.pos >= r.ls && s.pos < r.le) s.inLoop = true;
    refresh();
    int i = 0;
    for (; i < n; ++i)
    {
        if (s.done) break;
        double step = (inc0 + incStep * (double) i) * (double) s.dir;
        if (fm) step *= (double) fm[i];
        const double q = s.pos * lsc;
        float a = hermite (d0, q);
        float b = stereo ? hermite (d1, q) : a;
        if (s.pos >= fHi || s.pos < fLo)
        {
            // Loop crossfade zone: blend with the audio the wrap jumps to.
            double partner = -1.0, t = 1.0;
            if (step >= 0.0 && fwdWrap && s.pos >= fHi) { t = (r.le - s.pos) / r.xlF; partner = s.pos - len; }
            else if (step < 0.0 && backWrap && s.pos < fLo) { t = (s.pos - r.ls) / r.xlB; partner = s.pos + len; }
            if (partner >= 0.0)
            {
                const float ang = (float) std::clamp (t, 0.0, 1.0) * (kPi * 0.5f);
                const float gm = fastSin (ang), gp = fastCos (ang);
                const double pq = partner * lsc;
                a = a * gm + hermite (d0, pq) * gp;
                b = stereo ? b * gm + hermite (d1, pq) * gp : a;
            }
        }
        L[i] = a;
        if (stereo) R[i] = b;
        s.pos += step;
        if (s.pos >= hi || s.pos < lo)
        {
            boundary (s, r, lo, hi);
            refresh();
        }
    }
    for (int k = i; k < n; ++k) { L[k] = 0.0f; if (stereo) R[k] = 0.0f; }
    return i;
}

void SampleOsc::renderSample (OscTick& tick, const OscIds&, const SampleOscData&, const PreparedSample& ps, TickParams& tp)
{
    const int n = tp.n;
    const bool stereo = ps.channels == 2;
    const Region& r = tp.region;
    float env0 = envLevel, env1 = 1.0f;
    if (envActive) env1 = multiEnvelope (n);
    else env0 = 1.0f;
    bool anyAlive = false;
    for (int u = 0; u < numStreams; ++u)
    {
        Stream& s = streams[u];
        if (s.done) continue;
        const double k = tp.speed * tp.srcRatio;
        const double inc0 = k * std::exp2 ((double) (tp.semis0 + s.detune) / 12.0);
        const double inc1 = k * std::exp2 ((double) (tp.semis1 + s.detune) / 12.0);
        const int level = mipLevel (std::max (std::abs (inc0), std::abs (inc1)) * (tp.fm ? 1.5 : 1.0));
        boundedStream (s, r, ps, level, inc0, (inc1 - inc0) / (double) n, tp.fm ? fmBuf : nullptr, n, tmpL, tmpR, stereo);
        if (tp.w1Post || tp.w2Post)
        {
            const float a1 = clamp01 (tp.w1 + tp.uniW1 * s.warpSpread), a2 = clamp01 (tp.w2 + tp.uniW2 * s.warpSpread);
            applyPostWarps (tmpL, n, 0, s.post[0][0], s.post[1][0], a1, a2, tp);
            if (stereo) applyPostWarps (tmpR, n, 0, s.post[0][1], s.post[1][1], a1, a2, tp);
        }
        float gl, gr;
        panGains (tp.pan + s.pan, gl, gr);
        gl *= s.gain * tp.level; gr *= s.gain * tp.level;
        if (s.gl < 0.0f) { s.gl = gl; s.gr = gr; }
        const float dl = (gl - s.gl) / (float) n, dr = (gr - s.gr) / (float) n;
        const float de = (env1 - env0) / (float) n;
        const float* srcR = stereo ? tmpR : tmpL;
        for (int i = 0; i < n; ++i)
        {
            const float e = env0 + de * (float) i;
            tick.outL[i] += tmpL[i] * (s.gl + dl * (float) i) * e;
            tick.outR[i] += srcR[i] * (s.gr + dr * (float) i) * e;
        }
        s.gl = gl; s.gr = gr;
        if (! s.done) anyAlive = true;
    }
    tick.activeUnison = numStreams;
    if (! anyAlive) isFinished = true;
}

// ---------------------------------------------------------------------------
// Granular
// ---------------------------------------------------------------------------
void SampleOsc::spawnGrains (int offset, int age0, const SampleOscData& data, const PreparedSample&, TickParams& tp)
{
    const Region& r = tp.region;
    const int cap = std::clamp (data.maxGrains, 1, kMaxGrains);
    const double F = r.frames;
    for (int u = 0; u < numStreams; ++u)
    {
        if (numGrains >= cap) return;                       // Max Grains: skip spawns until grains finish
        Stream& s = streams[u];
        if (s.done && ! data.manualLoop) continue;
        double delay = offset;
        switch (data.spawnPattern)
        {
            case SampleOscData::SpawnEven: delay += tp.interval * (double) u / (double) numStreams; break;
            case SampleOscData::SpawnExp: { const double f = (double) u / (double) numStreams; delay += tp.interval * f * f; break; }
            case SampleOscData::SpawnRandom: if (u > 0) delay += tp.interval * rng.uni(); break;
            default: break;
        }
        Grain& g = grains[numGrains];
        const double ph = s.pos + tp.speed * (double) offset * (double) s.dir;
        double pos = ph + (double) rng.bi() * (double) gOffset * (r.end - r.start) * 0.5;
        if (data.loopGrains)
        {
            const double len = r.le - r.ls;
            const double o = pos - r.ls;
            pos = r.ls + o - std::floor (o / len) * len;
        }
        else pos = std::clamp (pos, 0.0, F - 1.0);
        const bool rev = data.reverseGrains != (rng.uni() < gDir);
        const float semis = tp.semis1 + s.detune + rng.bi() * gPitch * 12.0f;
        g.inc = tp.srcRatio * std::exp2 ((double) semis / 12.0) * (rev ? -1.0 : 1.0);
        g.level = mipLevel (std::abs (g.inc) * (tp.fm ? 1.5 : 1.0));
        g.len = std::max (16, (int) ((double) tp.grainLen * (1.0 + (double) (rng.bi() * gLenRand) * 0.95)));
        g.invLen = 1.0f / (float) g.len;
        g.delay = (int) delay;
        g.age = std::min (age0, g.len - 1);
        g.pos = pos + g.inc * (double) g.age;
        g.stream = u;
        const float pan = clampf (tp.pan + s.pan + rng.bi() * gPanRand, -1.0f, 1.0f);
        const float gain = s.gain * (1.0f - rng.uni() * gGainRand);
        panGains (pan, g.gl, g.gr);
        g.gl *= gain; g.gr *= gain;
        g.winAmt = clamp01 (gWinAmt + rng.bi() * gWinAmtRand);
        const float skew = clampf (gWinSkew + rng.bi() * gWinSkewRand, -1.0f, 1.0f);
        g.peak = 0.5f + 0.45f * skew;
        g.slopeA = 0.5f / g.peak;
        g.slopeB = 0.5f / (1.0f - g.peak);
        g.warp1 = clamp01 (tp.w1 + tp.uniW1 * s.warpSpread + rng.bi() * tp.randWarp1);
        g.warp2 = clamp01 (tp.w2 + tp.uniW2 * s.warpSpread + rng.bi() * tp.randWarp2);
        g.dir = rev ? -1 : 1;
        for (auto& a : g.post) for (auto& b : a) b.reset();
        ++numGrains;
    }
}

void SampleOsc::renderGranular (OscTick& tick, const OscIds& ids, const SampleOscData& data, const PreparedSample& ps, TickParams& tp)
{
    const int n = tp.n;
    const Region& r = tp.region;
    const double sr = ctx.sampleRate;
    const double bpm = std::max (1.0, ctx.bpm);

    // Per-grain randomisation and window settings (X|Y may override one).
    gOffset = clamp01 (tp.get (ids.grOffset));
    gDir = clamp01 (tp.get (ids.grDir));
    gPitch = clamp01 (tp.get (ids.grPitch));
    gLenRand = clamp01 (tp.get (ids.grLenRand));
    gPanRand = clamp01 (tp.get (ids.grPanRand));
    gGainRand = clamp01 (tp.get (ids.grGainRand));
    gWinAmt = clamp01 (tp.get (ids.winAmt));
    gWinAmtRand = clamp01 (tp.get (ids.winAmtRand));
    gWinSkew = clampf (tp.get (ids.winSkew), -1.0f, 1.0f);
    gWinSkewRand = clamp01 (tp.get (ids.winSkewRand));
    const int shape = std::clamp (tp.P->integer (ids.winShape), 0, kNumWin - 1);
    const float* winTab = winTables + (size_t) shape * (kWinSize + 1);

    // Spawn interval and grain length.
    auto syncDiv = [&] (float norm, bool longerIsHigher, bool trip, bool dot)
    {
        const int idx = std::clamp ((int) std::lround ((longerIsHigher ? 1.0f - norm : norm) * 11.0f), 0, 11);
        double beats = syncRateBeats (idx);
        if (trip) beats *= 2.0 / 3.0;
        if (dot) beats *= 1.5;
        return beats * 60.0 / bpm * sr;                  // samples
    };
    const float densV = tp.get (ids.density), lenV = tp.get (ids.grainLen);
    const float densN = reg ? (*reg)[ids.density].toNorm (densV) : 0.5f;
    const float lenN = reg ? (*reg)[ids.grainLen].toNorm (lenV) : 0.5f;
    double lenS = std::max (0.001f, lenV) * sr;
    if (data.lengthMode == SampleOscData::LenSync) lenS = syncDiv (lenN, true, data.lenTriplet, data.lenDotted);
    double interval = sr / std::max (0.01f, densV);
    if (data.densityMode == SampleOscData::DensSync) interval = syncDiv (densN, false, data.densTriplet, data.densDotted);
    if (data.lengthMode == SampleOscData::LenPercent && data.densityMode != SampleOscData::DensGrains)
        lenS = std::max (0.0, (double) lenN * 2.0) * interval;
    if (data.densityMode == SampleOscData::DensGrains)
        interval = lenS / (double) std::clamp ((int) std::lround (densV), 1, 64);
    tp.grainLen = (int) std::clamp (lenS, 16.0, sr * 8.0);
    tp.interval = std::max (1.0, interval);

    // Manual loop mode: SCAN is the playhead position.
    if (data.manualLoop)
    {
        const float norm = reg ? (*reg)[ids.scan].toNorm ((*tp.P) (ids.scan)) : 0.5f;
        for (int u = 0; u < numStreams; ++u)
        {
            streams[u].pos = r.start + (double) norm * (r.end - r.start - 1.0);
            streams[u].done = false;
        }
    }

    // Spawn.
    if (firstTick)
    {
        firstTick = false;
        spawnCountdown = 0.0;
        if (data.jumpStart)
        {
            const int extra = std::min (kMaxGrains, (int) std::ceil ((double) tp.grainLen / tp.interval) - 1);
            for (int k = 1; k <= extra; ++k) spawnGrains (0, (int) (k * tp.interval), data, ps, tp);
        }
    }
    while (spawnCountdown < (double) n)
    {
        spawnGrains ((int) spawnCountdown, 0, data, ps, tp);
        spawnCountdown += tp.interval;
    }
    spawnCountdown -= (double) n;

    // Render the grains.
    const bool stereo = ps.channels == 2;
    const bool post = tp.w1Post || tp.w2Post;
    const bool loopG = data.loopGrains;
    const double ls = r.ls, le = r.le, llen = r.le - r.ls;
    for (int gi = 0; gi < numGrains;)
    {
        Grain& g = grains[gi];
        if (g.delay >= n) { g.delay -= n; ++gi; continue; }
        const int start = g.delay;
        g.delay = 0;
        const int m = std::min (n - start, g.len - g.age);
        const int lv = g.level;
        const double lsc = 1.0 / (double) (1 << lv);
        const double maxQ = (double) ps.levelFrames[lv] + 1.0;
        const float* d0 = ps.channel (lv, 0);
        const float* d1 = ps.channel (lv, 1);
        float t = (float) g.age * g.invLen;
        double p = g.pos;
        const float* fm = tp.fm ? fmBuf + start : nullptr;
        const double pEnd = p + g.inc * (double) m;
        const double safeHi = (double) ps.levelFrames[lv] * (double) (1 << lv) - 1.0;
        if (! fm && ! loopG && std::min (p, pEnd) >= 0.0 && std::max (p, pEnd) < safeHi)
        {
            if (stereo) grainKernel<true> (d0, d1, p, g.inc, lsc, t, g.invLen, g.peak, g.slopeA, g.slopeB, g.winAmt, winTab, m, tmpL, tmpR);
            else
            {
                grainKernel<false> (d0, d1, p, g.inc, lsc, t, g.invLen, g.peak, g.slopeA, g.slopeB, g.winAmt, winTab, m, tmpL, tmpR);
                std::copy (tmpL, tmpL + m, tmpR);
            }
            p = pEnd;
        }
        else for (int k = 0; k < m; ++k)
        {
            const float tt = t < g.peak ? t * g.slopeA : 0.5f + (t - g.peak) * g.slopeB;
            const float w = 1.0f - g.winAmt + g.winAmt * lookupWin (winTab, tt);
            double q = p * lsc;
            q = q < -2.0 ? -2.0 : (q > maxQ ? maxQ : q);
            float a = hermite (d0, q);
            float b = stereo ? hermite (d1, q) : a;
            double step = g.inc;
            if (fm) step *= (double) fm[k];
            if (loopG && (r.xlF > 0.0 || r.xlB > 0.0))
            {
                double partner = -1.0, x = 1.0;
                if (step >= 0.0 && r.xlF > 0.0 && p >= le - r.xlF) { x = (le - p) / r.xlF; partner = p - llen; }
                else if (step < 0.0 && r.xlB > 0.0 && p < ls + r.xlB) { x = (p - ls) / r.xlB; partner = p + llen; }
                if (partner >= 0.0)
                {
                    const float ang = (float) std::clamp (x, 0.0, 1.0) * (kPi * 0.5f);
                    const float gm = fastSin (ang), gp = fastCos (ang);
                    const double pq = partner * lsc;
                    a = a * gm + hermite (d0, pq) * gp;
                    b = stereo ? b * gm + hermite (d1, pq) * gp : a;
                }
            }
            tmpL[k] = a * w;
            tmpR[k] = b * w;
            p += step;
            if (loopG) { if (p >= le) p -= llen; else if (p < ls) p += llen; }
            t += g.invLen;
        }
        g.pos = p;
        g.age += m;
        if (post)
        {
            applyPostWarps (tmpL, m, start, g.post[0][0], g.post[1][0], g.warp1, g.warp2, tp);
            if (stereo) applyPostWarps (tmpR, m, start, g.post[0][1], g.post[1][1], g.warp1, g.warp2, tp);
        }
        float* oL = tick.outL + start;
        float* oR = tick.outR + start;
        for (int k = 0; k < m; ++k) { oL[k] += tmpL[k] * g.gl; oR[k] += tmpR[k] * g.gr; }
        if (g.age >= g.len || (! loopG && (p < -4.0 || p > r.frames + 4.0)))
            grains[gi] = grains[--numGrains];
        else ++gi;
    }

    // Move the playheads.
    bool anyHead = false;
    if (! data.manualLoop)
        for (int u = 0; u < numStreams; ++u)
        {
            advancePlayhead (streams[u], tp.speed * (double) n, r);
            if (! streams[u].done) anyHead = true;
        }
    else anyHead = true;

    // Output level ramp.
    if (lvlPrev < 0.0f) lvlPrev = tp.level;
    const float dl = (tp.level - lvlPrev) / (float) n;
    for (int i = 0; i < n; ++i) { const float g = lvlPrev + dl * (float) i; tick.outL[i] *= g; tick.outR[i] *= g; }
    lvlPrev = tp.level;
    tick.activeUnison = std::max (numGrains, 1);
    if (! anyHead && numGrains == 0) isFinished = true;
}

// ---------------------------------------------------------------------------
// Spectral
// ---------------------------------------------------------------------------
void SampleOsc::synthHop (const SampleOscData& data, const SpectralData& sd, TickParams& tp, bool prefill)
{
    constexpr int N = SpectralData::kFftSize, H = SpectralData::kHop, B = SpectralData::kBins, PF = SpectralData::kPreFrames;
    const float expect = kTwoPi * (float) H / (float) N;
    const float sr = tp.sr;

    // Band limits and the spectral filter mask (recomputed when they change).
    const float tableFlag = data.specFilterMask.empty() ? 0.0f : 1.0f;
    const float key[6] { tp.cut, tp.mix, tp.lo, tp.hi, data.specSmooth ? 1.0f : 0.0f, tableFlag };
    if (! std::equal (key, key + 6, maskKey))
    {
        std::copy (key, key + 6, maskKey);
        const BakedCurve* lut = data.specLut.get();
        const float lo = std::min (tp.lo, tp.hi), hi = std::max (tp.lo, tp.hi);
        const int nm = (int) data.specFilterMask.size();
        for (int j = 0; j < B; ++j)
        {
            const float f = std::max (1.0f, (float) j * sr / (float) N);
            float band;
            if (data.specSmooth)
            {
                const float a = f / hi, b = lo / f;
                const float a2 = a * a, b2 = b * b;
                band = 1.0f / std::sqrt ((1.0f + a2 * a2 * a2 * a2) * (1.0f + b2 * b2 * b2 * b2));
            }
            else band = (f >= lo && f <= hi) ? 1.0f : 0.0f;
            const float x = binX[(size_t) j] - tp.cut;
            float filt = 1.0f;
            if (nm > 1)
            {
                const float fx = clamp01 (x) * (float) (nm - 1);
                const int i0 = std::min ((int) fx, nm - 2);
                filt = lerp (data.specFilterMask[(size_t) i0], data.specFilterMask[(size_t) i0 + 1], fx - (float) i0);
            }
            else if (lut) filt = (*lut) (x) * 2.0f;
            mask[(size_t) j] = band * ((1.0f - tp.mix) + tp.mix * filt);
        }
    }

    std::fill (cLr.begin(), cLr.end(), 0.0f); std::fill (cLi.begin(), cLi.end(), 0.0f);
    std::fill (cRr.begin(), cRr.end(), 0.0f); std::fill (cRi.begin(), cRi.end(), 0.0f);
    bool any = false;
    const double binScale = sd.sampleRate / (double) sr;
    for (int u = 0; u < numStreams; ++u)
    {
        Stream& s = streams[u];
        if (s.done) continue;
        any = true;
        const double ff = std::clamp (s.pos / (double) H + PF, 0.0, (double) (sd.frames - 1));
        const int k0 = (int) ff;
        const int k1 = std::min (k0 + 1, sd.frames - 1);
        const float ft = (float) (ff - k0);
        const int kn = ft < 0.5f ? k0 : k1;
        const float* m0 = sd.magAt (k0);
        const float* m1 = sd.magAt (k1);
        const float* fr = sd.freqAt (kn);
        const float* ph0 = sd.phaseAt (kn);
        bool reset = specLastFrame[(size_t) u] < 0;
        if (data.transients && ! reset)
        {
            const int last = specLastFrame[(size_t) u];
            if (kn > last) { for (int f = last + 1; f <= kn; ++f) if (sd.transient[(size_t) f]) { reset = true; break; } }
        }
        specLastFrame[(size_t) u] = kn;

        // Pitch shift by moving each spectral peak's region (its whole main
        // lobe) to the peak's new frequency (Laroche-Dolson). The lobe shape
        // and so the level are kept; frequencies come from the phase advance.
        const float ratio = (float) (std::exp2 ((double) (tp.semis1 + s.detune) / 12.0) * binScale);
        float mx = 0.0f;
        for (int k = 0; k < B; ++k) { srcMag[(size_t) k] = m0[k] + (m1[k] - m0[k]) * ft; mx = std::max (mx, srcMag[(size_t) k]); }
        if (mx <= 1.0e-9f) { if (prefill) s.pos += H; else advancePlayhead (s, (double) H * tp.speed, tp.region); continue; }
        const float floorMag = mx * 1.0e-5f;
        int np = 0;
        for (int k = 1; k < B - 1; ++k)
        {
            const float v = srcMag[(size_t) k];
            // A real partial's frequency estimate sits near its own bin; window
            // sidelobes of a strong partial report that partial's frequency instead.
            if (v > floorMag && v > srcMag[(size_t) k - 1] && v >= srcMag[(size_t) k + 1] && std::abs (fr[k] - (float) k) < 1.0f)
                peaks[(size_t) np++] = k;
        }
        float* phase = specPhase.data() + (size_t) u * B;
        std::fill (bestMag.begin(), bestMag.end(), 0.0f);
        float gl, gr;
        panGains (tp.pan + s.pan, gl, gr);
        gl *= s.gain; gr *= s.gain;
        int a = 1;
        for (int pi = 0; pi < np; ++pi)
        {
            const int pk = peaks[(size_t) pi];
            // Region: from the previous boundary to the lowest bin before the next peak.
            int b = B - 2;
            if (pi + 1 < np)
            {
                const int nx = peaks[(size_t) pi + 1];
                b = pk;
                for (int k = pk + 1; k < nx; ++k) if (srcMag[(size_t) k] < srcMag[(size_t) b]) b = k;
                if (b == pk) b = (pk + nx) / 2;
            }
            // Move the lobe by the whole-bin shift that lands its centre nearest
            // the new frequency w; the phase advance then makes the pitch exact.
            const float w = fr[pk] * ratio;                         // the peak's new frequency in output bins
            const int shift = (int) std::floor (w - fr[pk] + 0.5f);
            const int q = pk + shift;
            const float peakPhase = (q >= 0 && q < B) ? wrapPi (phase[q] + w * expect) : 0.0f;
            for (int k = a; k <= b; ++k)
            {
                const int j = k + shift;
                if (j < 1 || j >= B - 1) continue;
                const float mg = srcMag[(size_t) k];
                float ph;
                if (reset) ph = ph0[k];
                else if (data.phaseLock || std::abs (k - pk) <= 2) ph = peakPhase + ph0[k] - ph0[pk];
                else ph = phase[j] + fr[k] * ratio * expect;        // Phase Lock off: bins off the main lobe run free
                if (mg > bestMag[(size_t) j]) { bestMag[(size_t) j] = mg; newPhase[(size_t) j] = ph; }
                const float am = mg * mask[(size_t) j];
                if (am == 0.0f) continue;
                const float wp = wrapPi (ph);
                const float c = fastCos (wp) * am, sn = fastSin (wp) * am;
                cLr[(size_t) j] += gl * c; cLi[(size_t) j] += gl * sn;
                cRr[(size_t) j] += gr * c; cRi[(size_t) j] += gr * sn;
            }
            a = b + 1;
        }
        for (int j = 1; j < B - 1; ++j)
            if (bestMag[(size_t) j] > 0.0f) phase[j] = wrapPi (newPhase[(size_t) j]);
        if (prefill) s.pos += H;
        else advancePlayhead (s, (double) H * tp.speed, tp.region);
    }

    // Pack L and R into one complex spectrum: z = L + iR, one inverse FFT.
    if (any)
    {
        zr[0] = zi[0] = zr[N / 2] = zi[N / 2] = 0.0f;
        for (int j = 1; j < N / 2; ++j)
        {
            const float lr = cLr[(size_t) j], li = cLi[(size_t) j], rr = cRr[(size_t) j], ri = cRi[(size_t) j];
            zr[(size_t) j] = lr - ri;      zi[(size_t) j] = li + rr;
            zr[(size_t) (N - j)] = lr + ri; zi[(size_t) (N - j)] = rr - li;
        }
        fft.inverse (zr.data(), zi.data());
        const float scale = 1.0f / ((float) N * 1.5f);      // Hann x Hann at 4x overlap sums to 1.5
        for (int i = 0; i < N; ++i)
        {
            const float w = synWin[(size_t) i] * scale;
            accL[(size_t) i] += zr[(size_t) i] * w;
            accR[(size_t) i] += zi[(size_t) i] * w;
        }
    }
    std::copy (accL.begin(), accL.begin() + H, hopL.begin());
    std::copy (accR.begin(), accR.begin() + H, hopR.begin());
    std::copy (accL.begin() + H, accL.end(), accL.begin());
    std::copy (accR.begin() + H, accR.end(), accR.begin());
    std::fill (accL.end() - H, accL.end(), 0.0f);
    std::fill (accR.end() - H, accR.end(), 0.0f);
    hopRead = 0;
    if (! any && ! prefill) ++specTailHops;
}

void SampleOsc::renderSpectral (OscTick& tick, const OscIds& ids, const SampleOscData& data, TickParams& tp)
{
    const SpectralData* sd = data.spectral.get();
    const int n = tp.n;
    if (! sd || sd->frames < 1) { isFinished = true; return; }
    constexpr int H = SpectralData::kHop;
    tp.cut = clamp01 (tp.get (ids.specCut));
    tp.mix = clamp01 (tp.get (ids.specMix));
    tp.lo = std::max (1.0f, tp.get (ids.specLo));
    tp.hi = std::max (1.0f, tp.get (ids.specHi));

    if (data.manualLoop)
    {
        const float norm = reg ? (*reg)[ids.scan].toNorm ((*tp.P) (ids.scan)) : 0.5f;
        for (int u = 0; u < numStreams; ++u)
        {
            streams[u].pos = tp.region.start + (double) norm * (tp.region.end - tp.region.start - 1.0);
            streams[u].done = false;
        }
        tp.speed = 0.0;
    }
    if (! specStarted)
    {
        // Pre-roll the overlap-add with the frames before the start point so
        // the first output samples are at full level.
        specStarted = true;
        for (int u = 0; u < numStreams; ++u) streams[u].pos -= (double) SpectralData::kPreFrames * H;
        for (int k = 0; k < SpectralData::kPreFrames; ++k) synthHop (data, *sd, tp, true);
        hopRead = H;
    }
    int i = 0;
    while (i < n)
    {
        if (hopRead >= H) synthHop (data, *sd, tp, false);
        const int c = std::min (n - i, H - hopRead);
        std::copy (hopL.begin() + hopRead, hopL.begin() + hopRead + c, tmpL + i);
        std::copy (hopR.begin() + hopRead, hopR.begin() + hopRead + c, tmpR + i);
        hopRead += c;
        i += c;
    }
    // POST warps run on the resynthesised output (one stream: streams[0]'s state).
    if (tp.w1Post || tp.w2Post)
    {
        applyPostWarps (tmpL, n, 0, streams[0].post[0][0], streams[0].post[1][0], tp.w1, tp.w2, tp);
        applyPostWarps (tmpR, n, 0, streams[0].post[0][1], streams[0].post[1][1], tp.w1, tp.w2, tp);
    }
    if (lvlPrev < 0.0f) lvlPrev = tp.level;
    const float dl = (tp.level - lvlPrev) / (float) n;
    for (int k = 0; k < n; ++k)
    {
        const float g = lvlPrev + dl * (float) k;
        tick.outL[k] = tmpL[k] * g;
        tick.outR[k] = tmpR[k] * g;
    }
    lvlPrev = tp.level;
    tick.activeUnison = numStreams;
    if (specTailHops > SpectralData::kFftSize / H) isFinished = true;
}
} // namespace tincture
