// Convolve.cpp — the Convolve FX module: zero-latency stereo convolution.
//
// Partitioning (non-uniform, all on the audio thread, no extra latency):
//   taps [0, 64)        direct-form FIR
//   taps [64, 1024)     uniform partitions of 64 (FFT 128), 15 partitions
//   taps [1024, end)    uniform partitions of 1024 (FFT 2048); the sum over
//                       older partitions is spread across the 16 sub-blocks
//                       of each 1024 block to avoid CPU spikes.
// A stage with block B can serve taps from B on: its output for an input
// block is emitted during the next block. Stereo in and out run through one
// complex FFT (L in the real part, R in the imaginary part).
//
// The impulse is shaped on the message thread (bakeConvolve): resample to
// the engine rate and stretch (SIZE), TONE tilt, minimum phase (real
// cepstrum), ATTACK fade-in, DECAY truncation with a fade, DAMP (low-pass
// whose cutoff falls along the tail), energy normalisation. PRE-DLY and IR
// GAIN run on the audio thread.
//
// Slots: 0 SIZE (25..400%), 1 TONE, 2 MIN PHASE, 3 PRE-DLY, 4 BPM, 5 ATTACK,
//        6 DECAY, 7 DAMP, 8 IR GAIN, 15 LEVEL.
#include "FxEngine.h"

#include <mutex>

namespace tincture
{
namespace
{
using namespace fxdsp;
constexpr FxType T = FxType::Convolve;
constexpr int kHead = 64;
constexpr int kBlockB = 64, kPartsB = 15;           // taps 64 .. 1024
constexpr int kBlockC = 1024;                       // taps 1024 ..
constexpr float kMaxIrSeconds = 10.0f;
constexpr double kPiD = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// Radix-2 complex FFT on split real / imaginary arrays (unscaled).
// ---------------------------------------------------------------------------
struct Fft
{
    int n = 0;
    std::vector<int> rev;
    std::vector<float> cs, sn;      // cos / sin of 2 pi k / n, k < n/2

    void init (int size)
    {
        n = size;
        rev.assign ((size_t) n, 0);
        int bits = 0;
        while ((1 << bits) < n) ++bits;
        for (int i = 0; i < n; ++i)
        {
            int r = 0;
            for (int b = 0; b < bits; ++b) if (i & (1 << b)) r |= 1 << (bits - 1 - b);
            rev[(size_t) i] = r;
        }
        cs.resize ((size_t) std::max (1, n / 2));
        sn.resize ((size_t) std::max (1, n / 2));
        for (int k = 0; k < n / 2; ++k)
        {
            cs[(size_t) k] = (float) std::cos (2.0 * kPiD * k / n);
            sn[(size_t) k] = (float) std::sin (2.0 * kPiD * k / n);
        }
    }

    void run (float* re, float* im, bool inverse) const noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const int r = rev[(size_t) i];
            if (r > i) { std::swap (re[i], re[r]); std::swap (im[i], im[r]); }
        }
        const float sgn = inverse ? 1.0f : -1.0f;
        for (int len = 2; len <= n; len <<= 1)
        {
            const int half = len >> 1, step = n / len;
            for (int j = 0; j < half; ++j)
            {
                const float wr = cs[(size_t) (j * step)], wi = sgn * sn[(size_t) (j * step)];
                for (int i = j; i < n; i += len)
                {
                    const int b = i + half;
                    const float tr = re[b] * wr - im[b] * wi;
                    const float ti = re[b] * wi + im[b] * wr;
                    re[b] = re[i] - tr; im[b] = im[i] - ti;
                    re[i] += tr; im[i] += ti;
                }
            }
        }
    }
};

// Double-precision version for the (message-thread) IR processing.
void fftD (std::vector<double>& re, std::vector<double>& im, bool inverse)
{
    const int n = (int) re.size();
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap (re[(size_t) i], re[(size_t) j]); std::swap (im[(size_t) i], im[(size_t) j]); }
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const double ang = 2.0 * kPiD / len * (inverse ? 1.0 : -1.0);
        const double wr0 = std::cos (ang), wi0 = std::sin (ang);
        for (int i = 0; i < n; i += len)
        {
            double wr = 1.0, wi = 0.0;
            for (int j = 0; j < len / 2; ++j)
            {
                const size_t a = (size_t) (i + j), b = (size_t) (i + j + len / 2);
                const double tr = re[b] * wr - im[b] * wi, ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr; im[a] += ti;
                const double t = wr * wr0 - wi * wi0;
                wi = wr * wi0 + wi * wr0;
                wr = t;
            }
        }
    }
    if (inverse)
        for (int i = 0; i < n; ++i) { re[(size_t) i] /= n; im[(size_t) i] /= n; }
}

// ---------------------------------------------------------------------------
// Baked kernel.
// ---------------------------------------------------------------------------
struct ConvKernel
{
    int length = 0;
    bool stereo = false;
    std::vector<float> irL, irR;                    // shaped IR (display / tests)
    float head[2][kHead] {};
    std::vector<float> bRe[2], bIm[2];              // [ch][part * (B+1) + bin]
    int partsC = 0;
    std::vector<float> cRe[2], cIm[2];              // [ch][part * (C+1) + bin]
};

// Spectra of the partitions of `ir` starting at `first`, block B (FFT 2B).
void partitionSpectra (const std::vector<float>& ir, int first, int B, int parts, std::vector<float>& re, std::vector<float>& im)
{
    const int N = 2 * B, H = B + 1;
    Fft f;
    f.init (N);
    re.assign ((size_t) (parts * H), 0.0f);
    im.assign ((size_t) (parts * H), 0.0f);
    std::vector<float> wr ((size_t) N), wi ((size_t) N);
    for (int p = 0; p < parts; ++p)
    {
        std::fill (wr.begin(), wr.end(), 0.0f);
        std::fill (wi.begin(), wi.end(), 0.0f);
        for (int k = 0; k < B; ++k)
        {
            const int idx = first + p * B + k;
            if (idx < (int) ir.size()) wr[(size_t) k] = ir[(size_t) idx];
        }
        f.run (wr.data(), wi.data(), false);
        std::copy (wr.begin(), wr.begin() + H, re.begin() + p * H);
        std::copy (wi.begin(), wi.begin() + H, im.begin() + p * H);
    }
}

// Minimum-phase version of `x` (same magnitude), via the real cepstrum.
std::vector<float> minimumPhase (const std::vector<float>& x)
{
    const int len = (int) x.size();
    const int N = nextPow2 (std::max (len * 4, 64));
    std::vector<double> re ((size_t) N, 0.0), im ((size_t) N, 0.0);
    for (int i = 0; i < len; ++i) re[(size_t) i] = x[(size_t) i];
    fftD (re, im, false);
    for (int i = 0; i < N; ++i)
    {
        re[(size_t) i] = std::log (std::max (std::hypot (re[(size_t) i], im[(size_t) i]), 1.0e-9));
        im[(size_t) i] = 0.0;
    }
    fftD (re, im, true);                             // real cepstrum
    for (int i = 1; i < N / 2; ++i) { re[(size_t) i] *= 2.0; im[(size_t) i] = 0.0; }
    for (int i = N / 2 + 1; i < N; ++i) { re[(size_t) i] = 0.0; im[(size_t) i] = 0.0; }
    fftD (re, im, false);
    for (int i = 0; i < N; ++i)
    {
        const double m = std::exp (re[(size_t) i]);
        const double a = im[(size_t) i];
        re[(size_t) i] = m * std::cos (a);
        im[(size_t) i] = m * std::sin (a);
    }
    fftD (re, im, true);
    std::vector<float> y ((size_t) len);
    for (int i = 0; i < len; ++i) y[(size_t) i] = (float) re[(size_t) i];
    return y;
}

// ---------------------------------------------------------------------------
// Factory IRs (procedural, 48 kHz stereo).
// ---------------------------------------------------------------------------
constexpr double kIrRate = 48000.0;

std::shared_ptr<SampleData> makeIr (const char* name, double seconds)
{
    auto s = std::make_shared<SampleData>();
    s->name = name;
    s->category = "Factory";
    s->sampleRate = kIrRate;
    s->left.assign ((size_t) (seconds * kIrRate), 0.0f);
    s->right.assign (s->left.size(), 0.0f);
    return s;
}

// Diffuse decaying noise with early reflections; HF decays faster.
std::shared_ptr<SampleData> genRoom (const char* name, double rt, double erMs, double hfRt, float bright, uint32_t seed)
{
    auto s = makeIr (name, rt * 1.3 + 0.05);
    Rng rng (seed);
    const int n = s->frames();
    float lz[2] {}, hz[2] {};
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kIrRate;
        const float env = (float) std::exp (-6.9 * t / rt);
        const float build = (float) std::min (1.0, t / std::max (erMs * 0.001, 1.0e-3));
        // Low-pass coefficient falls with time (HF decays at hfRt).
        const float hfEnv = (float) std::exp (-6.9 * t / hfRt);
        const float a = 0.05f + 0.9f * hfEnv;
        for (int c = 0; c < 2; ++c)
        {
            const float w = rng.bi();
            lz[c] += a * (w - lz[c]);
            hz[c] = lz[c] + bright * (w - lz[c]) * hfEnv;
            (c == 0 ? s->left : s->right)[(size_t) i] = hz[c] * env * (0.3f + 0.7f * build);
        }
    }
    // Early reflections.
    for (int k = 0; k < 10; ++k)
    {
        const int pos = (int) ((0.002 + rng.uni() * erMs * 0.001) * kIrRate);
        if (pos >= n) continue;
        const float g = 0.9f * (1.0f - (float) k / 12.0f);
        s->left[(size_t) pos] += g * (rng.uni() < 0.5f ? -1.0f : 1.0f);
        s->right[(size_t) std::min (n - 1, pos + (int) (rng.uni() * 40))] += g * (rng.uni() < 0.5f ? -1.0f : 1.0f);
    }
    s->left[0] += 1.0f;
    s->right[0] += 1.0f;
    return s;
}

// Spring tank: a dispersive (all-pass chain) feedback loop -> chirpy drips.
std::shared_ptr<SampleData> genSpring (const char* name, double loopMs, int stages, float apG, double rt, uint32_t seed)
{
    auto s = makeIr (name, rt * 1.2);
    Rng rng (seed);
    const int n = s->frames();
    for (int c = 0; c < 2; ++c)
    {
        const int D = (int) (loopMs * (c == 0 ? 1.0 : 1.07) * 0.001 * kIrRate);
        std::vector<float> line ((size_t) D, 0.0f);
        std::vector<float> apx ((size_t) stages, 0.0f), apy ((size_t) stages, 0.0f);
        const float g = (float) std::pow (10.0, -3.0 * (D / kIrRate) / rt);
        int pos = 0;
        float lp = 0.0f;
        auto& out = c == 0 ? s->left : s->right;
        for (int i = 0; i < n; ++i)
        {
            float x = (i == 0 ? 1.0f : 0.0f) + 0.002f * rng.bi() * (float) std::exp (-6.9 * i / (kIrRate * rt));
            x += line[(size_t) pos] * g;
            for (int k = 0; k < stages; ++k)
            {
                const float y = apG * x + apx[(size_t) k] - apG * apy[(size_t) k];
                apx[(size_t) k] = x; apy[(size_t) k] = y;
                x = y;
            }
            lp += 0.35f * (x - lp);
            line[(size_t) pos] = lp;
            pos = (pos + 1) % D;
            out[(size_t) i] = lp;
        }
    }
    return s;
}

std::shared_ptr<SampleData> genReverse (uint32_t seed)
{
    auto s = makeIr ("Weird: Reverse", 1.3);
    Rng rng (seed);
    const int n = s->frames();
    for (int i = 0; i < n; ++i)
    {
        const double t = i / kIrRate;
        const float env = (float) (std::exp (6.9 * (t - 1.2) / 0.9) * std::min (1.0, (1.3 - t) / 0.01));
        s->left[(size_t) i] = rng.bi() * env;
        s->right[(size_t) i] = rng.bi() * env;
    }
    return s;
}

std::shared_ptr<SampleData> genMetal (uint32_t seed)
{
    auto s = makeIr ("Weird: Metal", 2.5);
    Rng rng (seed);
    const int n = s->frames();
    for (int k = 0; k < 28; ++k)
    {
        const double f = 180.0 * std::pow (30.0, rng.uni());
        const double dec = 0.3 + 1.6 * rng.uni();
        const double ph = rng.uni() * 6.283;
        const float amp = 0.3f / std::sqrt (1.0f + (float) k);
        for (int c = 0; c < 2; ++c)
        {
            auto& out = c == 0 ? s->left : s->right;
            const double fc = f * (c == 0 ? 1.0 : 1.0021);
            for (int i = 0; i < n; ++i)
            {
                const double t = i / kIrRate;
                out[(size_t) i] += amp * (float) (std::exp (-6.9 * t / dec) * std::sin (6.283185307 * fc * t + ph));
            }
        }
    }
    return s;
}

std::shared_ptr<SampleData> genGrains (uint32_t seed)
{
    auto s = makeIr ("Weird: Grains", 1.8);
    Rng rng (seed);
    const int n = s->frames();
    for (int g = 0; g < 160; ++g)
    {
        const double t0 = 1.7 * std::pow ((double) rng.uni(), 1.6);
        const int start = (int) (t0 * kIrRate), len = 48 + (int) (rng.uni() * 400);
        const float amp = (float) std::exp (-2.5 * t0) * (0.4f + 0.6f * rng.uni());
        auto& out = rng.uni() < 0.5f ? s->left : s->right;
        const double f = 300.0 + 4000.0 * rng.uni();
        for (int i = 0; i < len && start + i < n; ++i)
        {
            const float w = 0.5f - 0.5f * std::cos (6.2831853f * (float) i / (float) len);
            out[(size_t) (start + i)] += amp * w * (float) std::sin (6.283185307 * f * i / kIrRate);
        }
    }
    s->left[0] += 0.5f;
    s->right[0] += 0.5f;
    return s;
}

const std::vector<std::shared_ptr<const SampleData>>& factoryIrs()
{
    static const std::vector<std::shared_ptr<const SampleData>> irs = []
    {
        std::vector<std::shared_ptr<const SampleData>> v;
        v.push_back (genRoom ("Small Room", 0.45, 12.0, 0.25, 0.3f, 11));
        v.push_back (genRoom ("Medium Room", 0.9, 25.0, 0.5, 0.3f, 12));
        v.push_back (genRoom ("Large Hall", 2.8, 60.0, 1.4, 0.2f, 13));
        v.push_back (genRoom ("Plate", 1.8, 2.0, 1.5, 0.8f, 14));
        v.push_back (genRoom ("Bright Plate", 1.2, 1.0, 1.2, 1.0f, 15));
        v.push_back (genSpring ("Spring", 31.0, 24, 0.62f, 2.2, 16));
        v.push_back (genSpring ("Spring Twang", 23.0, 48, 0.7f, 2.8, 17));
        v.push_back (genReverse (18));
        v.push_back (genMetal (19));
        v.push_back (genGrains (20));
        return v;
    }();
    return irs;
}

// ---------------------------------------------------------------------------
// One uniformly partitioned stage (block B, FFT 2B), stereo packed.
// ---------------------------------------------------------------------------
struct Stage
{
    int B = 64, N = 128, H = 65, K = 1;             // K: FDL length (partitions)
    Fft fft;
    std::vector<float> in[2];                       // last 2B input samples
    std::vector<float> fdlRe[2], fdlIm[2];          // [slot * H + bin]
    std::vector<float> accRe[2], accIm[2];
    std::vector<float> out[2];
    std::vector<float> wr, wi;
    int fdlPos = 0, accDone = 1, fill = 0;
    int valid = 0;                                  // spectra stored since clear() (the FDL isn't zeroed)

    void prepare (int block, int parts)
    {
        B = block; N = 2 * B; H = B + 1; K = std::max (1, parts);
        fft.init (N);
        for (int c = 0; c < 2; ++c)
        {
            in[c].assign ((size_t) N, 0.0f);
            fdlRe[c].assign ((size_t) (K * H), 0.0f);
            fdlIm[c].assign ((size_t) (K * H), 0.0f);
            accRe[c].assign ((size_t) H, 0.0f);
            accIm[c].assign ((size_t) H, 0.0f);
            out[c].assign ((size_t) B, 0.0f);
        }
        wr.assign ((size_t) N, 0.0f);
        wi.assign ((size_t) N, 0.0f);
    }
    void clear()
    {
        for (int c = 0; c < 2; ++c)
        {
            std::fill (in[c].begin(), in[c].end(), 0.0f);
            std::fill (accRe[c].begin(), accRe[c].end(), 0.0f);
            std::fill (accIm[c].begin(), accIm[c].end(), 0.0f);
            std::fill (out[c].begin(), out[c].end(), 0.0f);
        }
        fdlPos = 0; accDone = 1; fill = 0; valid = 0;
    }

    // Accumulate up to `count` older partitions (j >= 1) for the next output.
    void accumulate (const float* const* hRe, const float* const* hIm, int parts, int count) noexcept
    {
        const int end = std::min (std::min (parts, valid + 1), accDone + count);
        for (int j = accDone; j < end; ++j)
        {
            const int slot = (fdlPos - (j - 1) + K) % K;
            for (int c = 0; c < 2; ++c)
            {
                const float* xr = fdlRe[c].data() + slot * H;
                const float* xi = fdlIm[c].data() + slot * H;
                const float* hr = hRe[c] + j * H;
                const float* hi = hIm[c] + j * H;
                float* ar = accRe[c].data();
                float* ai = accIm[c].data();
                for (int k = 0; k < H; ++k)
                {
                    ar[k] += xr[k] * hr[k] - xi[k] * hi[k];
                    ai[k] += xr[k] * hi[k] + xi[k] * hr[k];
                }
            }
        }
        accDone = std::max (accDone, end);
    }

    // The block is full: transform, convolve, produce the next output block.
    void block (const float* const* hRe, const float* const* hIm, int parts) noexcept
    {
        parts = std::min (parts, K);
        if (parts > 1) accumulate (hRe, hIm, parts, parts);
        // Packed FFT of the last 2B inputs.
        std::copy (in[0].begin(), in[0].end(), wr.begin());
        std::copy (in[1].begin(), in[1].end(), wi.begin());
        fft.run (wr.data(), wi.data(), false);
        fdlPos = (fdlPos + 1) % K;
        valid = std::min (valid + 1, K);
        float* lr = fdlRe[0].data() + fdlPos * H;
        float* li = fdlIm[0].data() + fdlPos * H;
        float* rr = fdlRe[1].data() + fdlPos * H;
        float* ri = fdlIm[1].data() + fdlPos * H;
        for (int k = 0; k < H; ++k)
        {
            const int m = (N - k) & (N - 1);
            // X_L = (Z[k] + conj Z[N-k]) / 2 ; X_R = (Z[k] - conj Z[N-k]) / 2i
            lr[k] = 0.5f * (wr[(size_t) k] + wr[(size_t) m]);
            li[k] = 0.5f * (wi[(size_t) k] - wi[(size_t) m]);
            rr[k] = 0.5f * (wi[(size_t) k] + wi[(size_t) m]);
            ri[k] = -0.5f * (wr[(size_t) k] - wr[(size_t) m]);
        }
        // Y = acc + X_new * H_0
        float yRe[2][1025], yIm[2][1025];
        for (int c = 0; c < 2; ++c)
        {
            const float* xr = fdlRe[c].data() + fdlPos * H;
            const float* xi = fdlIm[c].data() + fdlPos * H;
            const float* hr = hRe[c];
            const float* hi = hIm[c];
            for (int k = 0; k < H; ++k)
            {
                float ar = parts > 1 ? accRe[c][(size_t) k] : 0.0f, ai = parts > 1 ? accIm[c][(size_t) k] : 0.0f;
                if (parts > 0)
                {
                    ar += xr[k] * hr[k] - xi[k] * hi[k];
                    ai += xr[k] * hi[k] + xi[k] * hr[k];
                }
                yRe[c][k] = ar; yIm[c][k] = ai;
            }
            std::fill (accRe[c].begin(), accRe[c].end(), 0.0f);
            std::fill (accIm[c].begin(), accIm[c].end(), 0.0f);
        }
        accDone = 1;
        // Z = Y_L + i Y_R on the full circle, inverse FFT -> y_L + i y_R.
        for (int k = 0; k < H; ++k)
        {
            wr[(size_t) k] = yRe[0][k] - yIm[1][k];
            wi[(size_t) k] = yIm[0][k] + yRe[1][k];
            if (k > 0 && k < B)
            {
                wr[(size_t) (N - k)] = yRe[0][k] + yIm[1][k];
                wi[(size_t) (N - k)] = -yIm[0][k] + yRe[1][k];
            }
        }
        fft.run (wr.data(), wi.data(), true);
        const float sc = 1.0f / (float) N;
        for (int i = 0; i < B; ++i)
        {
            out[0][(size_t) i] = wr[(size_t) (B + i)] * sc;
            out[1][(size_t) i] = wi[(size_t) (B + i)] * sc;
        }
        for (int c = 0; c < 2; ++c) std::copy (in[c].begin() + B, in[c].end(), in[c].begin());
        fill = 0;
    }
};

class Convolve final : public FxModule
{
public:
    void reserve (int samples) override { cap = std::max (samples, kBlockC * 2); }
    int kernelCapacity() const noexcept override { return cap; }

    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        stB.prepare (kBlockB, kPartsB);
        stC.prepare (kBlockC, (cap - kBlockC + kBlockC - 1) / kBlockC);
        for (auto& p : pre) p.init ((int) (2.0f * sr) + 8);
    }
    void reset() override
    {
        stB.clear();
        stC.clear();
        for (auto& p : pre) p.clear();
        for (auto& h : hist) std::fill (std::begin (h), std::end (h), 0.0f);
        preCur = -1.0f;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const auto* k = static_cast<const ConvKernel*> (b.extra);
        if (! k || k->length == 0)
        {
            std::fill (L, L + n, 0.0f);
            std::fill (R, R + n, 0.0f);
            return;
        }
        const float* bRe[2] { k->bRe[0].data(), k->bRe[k->stereo ? 1 : 0].data() };
        const float* bIm[2] { k->bIm[0].data(), k->bIm[k->stereo ? 1 : 0].data() };
        const float* cRe[2] { k->cRe[0].data(), k->cRe[k->stereo ? 1 : 0].data() };
        const float* cIm[2] { k->cIm[0].data(), k->cIm[k->stereo ? 1 : 0].data() };
        const float* head[2] { k->head[0], k->head[k->stereo ? 1 : 0] };
        const int partsC = std::min (k->partsC, stC.K);
        const int accStep = std::max (1, (partsC + 14) / 15);

        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            const float gain = dbToGain (fxPlain (T, 8, v[8]));
            const float pd = clampf (fxTimeSeconds (T, 3, b.v, b.ctx->bpm) * sr, 0.0f, (float) pre[0].capacity() - 4.0f);
            if (preCur < 0.0f) preCur = pd;
            const float pdStep = (pd - preCur) / (float) m;
            float* l = L + off;
            float* r = R + off;
            for (int i = 0; i < m; ++i)
            {
                preCur += pdStep;
                pre[0].push (l[i]); pre[1].push (r[i]);
                if (preCur >= 0.5f) { l[i] = pre[0].linear (preCur); r[i] = pre[1].linear (preCur); }
            }
            // Sub-chunks aligned to the 64-sample stage boundary.
            int done = 0;
            while (done < m)
            {
                const int c = std::min (m - done, kBlockB - stB.fill);
                float* x[2] { l + done, r + done };
                for (int ch = 0; ch < 2; ++ch)
                {
                    float* hb = hist[ch];
                    std::copy (x[ch], x[ch] + c, hb + (kHead - 1));
                    const float* h = head[ch];
                    float* outB = stB.out[ch].data() + stB.fill;
                    float* outC = stC.out[ch].data() + stC.fill;
                    float* inB = stB.in[ch].data() + kBlockB + stB.fill;
                    float* inC = stC.in[ch].data() + kBlockC + stC.fill;
                    for (int i = 0; i < c; ++i)
                    {
                        inB[i] = x[ch][i];
                        inC[i] = x[ch][i];
                        float acc = 0.0f;
                        const float* hx = hb + (kHead - 1) + i;
                        for (int t = 0; t < kHead; ++t) acc += h[t] * hx[-t];
                        x[ch][i] = (acc + outB[i] + outC[i]) * gain;
                    }
                    std::copy (hb + c, hb + c + (kHead - 1), hb);
                }
                stB.fill += c;
                stC.fill += c;
                done += c;
                if (stB.fill == kBlockB)
                {
                    stB.block (bRe, bIm, kPartsB);
                    if (partsC > 1) stC.accumulate (cRe, cIm, partsC, accStep);
                }
                if (stC.fill == kBlockC)
                    stC.block (cRe, cIm, partsC);
            }
        }
    }

private:
    float sr = 48000.0f, preCur = -1.0f;
    int cap = kBlockC * 2;
    SlotSmoother sm;
    Stage stB, stC;
    DelayLine pre[2];
    float hist[2][kHead - 1 + kTick] {};
};

float q256 (float x) { return std::round (clamp01 (x) * 256.0f) / 256.0f; }
} // namespace

std::unique_ptr<FxModule> makeConvolve() { return std::make_unique<Convolve>(); }

const std::vector<std::string>& FxEngine::convolveIrNames()
{
    static const std::vector<std::string> names = []
    {
        std::vector<std::string> v;
        for (auto& s : factoryIrs()) v.push_back (s->name);
        return v;
    }();
    return names;
}

std::shared_ptr<const SampleData> FxEngine::convolveFactoryIr (int index)
{
    const auto& irs = factoryIrs();
    return irs[(size_t) std::clamp (index, 0, (int) irs.size() - 1)];
}

ConvolveKey convolveKey (const FxModuleSlot& s, const float* v)
{
    ConvolveKey k;
    k.irId = s.userIr ? (const void*) s.userIr.get() : nullptr;
    k.irIndex = s.userIr ? -1 : s.irIndex;
    const int slots[6] { 0, 1, 2, 5, 6, 7 };
    for (int i = 0; i < 6; ++i) k.k[i] = q256 (v[slots[i]]);
    return k;
}

std::shared_ptr<const void> bakeConvolve (const FxModuleSlot& slot, const ConvolveKey& key, double sr, int& lengthOut)
{
    auto k = std::make_shared<ConvKernel>();
    std::shared_ptr<const SampleData> src = slot.userIr ? slot.userIr : FxEngine::convolveFactoryIr (slot.irIndex);
    lengthOut = 0;
    if (! src || src->frames() == 0) return k;

    const float size = fxPlain (T, 0, key.k[0]);
    const float tone = fxPlain (T, 1, key.k[1]);
    const bool minPhase = key.k[2] >= 0.5f;
    const float attack = key.k[3], decay = key.k[4], damp = key.k[5];
    const double ratio = sr / src->sampleRate * (std::abs (size - 1.0f) < 1.0e-3f ? 1.0 : (double) size);
    const int maxLen = (int) (kMaxIrSeconds * sr);
    int len = std::min (maxLen, std::max (1, (int) std::ceil (src->frames() * ratio)));
    // DECAY truncates the tail.
    const int keep = std::max (1, (int) std::ceil (len * std::max (decay, 0.01f)));
    len = keep;
    k->stereo = src->stereo();
    const int chans = k->stereo ? 2 : 1;
    double energy = 0.0;
    for (int c = 0; c < chans; ++c)
    {
        const float* x = src->channel (c);
        const int sn = src->frames();
        std::vector<float> y ((size_t) len);
        // Resample / stretch (cubic).
        for (int i = 0; i < len; ++i)
        {
            if (ratio == 1.0) { y[(size_t) i] = i < sn ? x[i] : 0.0f; continue; }
            const double p = i / ratio;
            const int j = (int) p;
            const float f = (float) (p - j);
            auto at = [&] (int q) { return q >= 0 && q < sn ? x[q] : 0.0f; };
            const float xm1 = at (j - 1), x0 = at (j), x1 = at (j + 1), x2 = at (j + 2);
            const float c1 = 0.5f * (x1 - xm1), c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2, c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
            y[(size_t) i] = (((c3 * f + c2) * f + c1) * f + x0) * (float) std::min (1.0, ratio);
        }
        // TONE: +-12 dB tilt around 1 kHz.
        if (std::abs (tone) > 1.0e-3f)
        {
            BiquadCoefs lo, hi;
            lo.lowShelf (1000.0f, 0.5f, -12.0f * tone, (float) sr);
            hi.highShelf (1000.0f, 0.5f, 12.0f * tone, (float) sr);
            Biquad a, b2;
            for (auto& s : y) s = b2.tick (hi, a.tick (lo, s));
        }
        if (minPhase) y = minimumPhase (y);
        // DAMP: one-pole low-pass whose cutoff falls along the IR.
        if (damp > 1.0e-3f)
        {
            float z = 0.0f;
            for (int i = 0; i < len; ++i)
            {
                const float t = (float) i / (float) len;
                const float hz = 20000.0f * std::exp2 (-damp * 9.0f * t);
                z += onePoleCoef (hz, (float) sr) * (y[(size_t) i] - z);
                y[(size_t) i] = z;
            }
        }
        // ATTACK fade-in (up to half the IR), DECAY fade-out (last 25% of what's kept).
        const int att = (int) (attack * 0.5f * (float) len);
        for (int i = 0; i < att; ++i) { const float t = (float) i / (float) att; y[(size_t) i] *= t * t; }
        if (decay < 0.999f)
        {
            const int fl = std::max (1, len / 4);
            for (int i = 0; i < fl; ++i)
            {
                const float t = (float) i / (float) fl;
                y[(size_t) (len - fl + i)] *= 0.5f + 0.5f * std::cos (t * kPi);
            }
        }
        for (float s : y) energy += (double) s * s;
        (c == 0 ? k->irL : k->irR) = std::move (y);
    }
    // Normalise to unit energy (per channel average).
    const float g = energy > 0.0 ? (float) (1.0 / std::sqrt (energy / chans)) : 0.0f;
    for (float& s : k->irL) s *= g;
    for (float& s : k->irR) s *= g;

    k->length = len;
    k->partsC = len > kBlockC ? (len - kBlockC + kBlockC - 1) / kBlockC : 0;
    for (int c = 0; c < chans; ++c)
    {
        const auto& ir = c == 0 ? k->irL : k->irR;
        for (int t = 0; t < kHead; ++t) k->head[c][t] = t < len ? ir[(size_t) t] : 0.0f;
        partitionSpectra (ir, kHead, kBlockB, kPartsB, k->bRe[c], k->bIm[c]);
        partitionSpectra (ir, kBlockC, kBlockC, std::max (k->partsC, 1), k->cRe[c], k->cIm[c]);
    }
    lengthOut = len;
    return k;
}

// For tests: the shaped IR of a baked kernel.
const std::vector<float>* convolveKernelIr (const void* kernel, int channel)
{
    const auto* k = static_cast<const ConvKernel*> (kernel);
    if (! k) return nullptr;
    return channel == 1 && k->stereo ? &k->irR : &k->irL;
}
} // namespace tincture
