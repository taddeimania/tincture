// Wavetable.cpp — wavetable building (resampling, morphs, band-limited
// mip-maps via our own FFT), save/load, and the generated factory tables.
#include "Wavetable.h"

#include <cmath>
#include <functional>
#include <mutex>

namespace tincture
{
// ---------------------------------------------------------------------------
// FFT
// ---------------------------------------------------------------------------
void fft (float* re, float* im, int n, bool inverse)
{
    if (n < 2) return;
    // Bit reversal.
    for (int i = 1, j = 0; i < n; ++i)
    {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { std::swap (re[i], re[j]); std::swap (im[i], im[j]); }
    }
    // Twiddles for the largest stage, in double for accuracy.
    std::vector<double> cs ((size_t) n / 2), sn ((size_t) n / 2);
    const double sign = inverse ? 1.0 : -1.0;
    for (int k = 0; k < n / 2; ++k)
    {
        const double a = 2.0 * 3.14159265358979323846 * (double) k / (double) n;
        cs[(size_t) k] = std::cos (a);
        sn[(size_t) k] = sign * std::sin (a);
    }
    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len >> 1, step = n / len;
        for (int i = 0; i < n; i += len)
            for (int k = 0; k < half; ++k)
            {
                const float wr = (float) cs[(size_t) (k * step)], wi = (float) sn[(size_t) (k * step)];
                const int a = i + k, b = a + half;
                const float tr = re[b] * wr - im[b] * wi;
                const float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr; im[b] = im[a] - ti;
                re[a] += tr;        im[a] += ti;
            }
    }
    if (inverse)
    {
        const float s = 1.0f / (float) n;
        for (int i = 0; i < n; ++i) { re[i] *= s; im[i] *= s; }
    }
}

namespace
{
constexpr int N = Wavetable::kFrameSize;
constexpr double kPiD = 3.14159265358979323846;

std::vector<float> resampleFrame (const std::vector<float>& in)
{
    std::vector<float> out ((size_t) N, 0.0f);
    if (in.empty()) return out;
    if ((int) in.size() == N) return in;
    // Periodic linear resampling (imports of other cycle lengths).
    const double r = (double) in.size() / (double) N;
    for (int i = 0; i < N; ++i)
    {
        const double p = i * r;
        const size_t a = (size_t) p;
        const double f = p - (double) a;
        const float x0 = in[a % in.size()], x1 = in[(a + 1) % in.size()];
        out[(size_t) i] = (float) (x0 + (x1 - x0) * f);
    }
    return out;
}

double wrapPi (double x)
{
    while (x > kPiD) x -= 2.0 * kPiD;
    while (x < -kPiD) x += 2.0 * kPiD;
    return x;
}
} // namespace

// ---------------------------------------------------------------------------
// Wavetable
// ---------------------------------------------------------------------------
std::shared_ptr<Wavetable> Wavetable::create (std::string name, std::string category,
                                              std::vector<std::vector<float>> frames, Morph morph, bool normalise)
{
    auto t = std::make_shared<Wavetable>();
    t->name = std::move (name);
    t->category = std::move (category);
    if (frames.size() > (size_t) kMaxFrames) frames.resize ((size_t) kMaxFrames);
    for (auto& f : frames) t->sources.push_back (resampleFrame (f));
    if (t->sources.empty()) t->sources.push_back (std::vector<float> ((size_t) N, 0.0f));
    t->morphType = morph;
    t->normalised = normalise;
    t->build();
    return t;
}

bool Wavetable::isSourceFrame (int logical) const noexcept
{
    for (int s : sourceAt)
        if (s == logical) return true;
    return false;
}

void Wavetable::build()
{
    const int nsrc = (int) sources.size();
    const bool morphing = nsrc > 1 && morphType != Morph::None;
    logicalFrames = morphing ? kMaxFrames : nsrc;
    sourceAt.resize ((size_t) nsrc);
    for (int k = 0; k < nsrc; ++k)
        sourceAt[(size_t) k] = nsrc > 1 ? (int) std::lround ((double) k * (logicalFrames - 1) / (nsrc - 1)) : 0;

    // Spectra of the frames we store (DC and the Nyquist bin removed).
    std::vector<std::vector<float>> specRe, specIm;
    std::vector<float> re ((size_t) N), im ((size_t) N);

    auto pushSpectrum = [&] (const float* timeData)
    {
        std::copy (timeData, timeData + N, re.begin());
        std::fill (im.begin(), im.end(), 0.0f);
        fft (re.data(), im.data(), N, false);
        re[0] = im[0] = re[N / 2] = im[N / 2] = 0.0f;
        specRe.push_back (re);
        specIm.push_back (im);
    };

    const bool spectral = morphing && morphType != Morph::Crossfade;
    if (! spectral)
    {
        for (auto& s : sources) pushSpectrum (s.data());
    }
    else
    {
        // Polar spectra of the sources.
        const int K = N / 2;
        std::vector<std::vector<double>> mag ((size_t) nsrc, std::vector<double> ((size_t) K)),
                                         ph ((size_t) nsrc, std::vector<double> ((size_t) K));
        for (int s = 0; s < nsrc; ++s)
        {
            std::copy (sources[(size_t) s].begin(), sources[(size_t) s].end(), re.begin());
            std::fill (im.begin(), im.end(), 0.0f);
            fft (re.data(), im.data(), N, false);
            for (int k = 1; k < K; ++k)
            {
                mag[(size_t) s][(size_t) k] = std::hypot ((double) re[(size_t) k], (double) im[(size_t) k]);
                ph[(size_t) s][(size_t) k] = std::atan2 ((double) im[(size_t) k], (double) re[(size_t) k]);
            }
            if (morphType == Morph::SpectralZeroFund && mag[(size_t) s][1] > 1.0e-9)
            {
                // Time-shift so the fundamental's phase is zero.
                const double p1 = ph[(size_t) s][1];
                for (int k = 1; k < K; ++k) ph[(size_t) s][(size_t) k] = wrapPi (ph[(size_t) s][(size_t) k] - k * p1);
            }
            else if (morphType == Morph::SpectralZeroAll)
            {
                for (int k = 1; k < K; ++k) ph[(size_t) s][(size_t) k] = 0.0;
            }
        }
        double peakMag = 0.0;
        for (const auto& m : mag) for (double v : m) peakMag = std::max (peakMag, v);
        for (int j = 0; j < logicalFrames; ++j)
        {
            int s = 0;
            while (s < nsrc - 2 && j > sourceAt[(size_t) s + 1]) ++s;
            const int a = sourceAt[(size_t) s], b = sourceAt[(size_t) s + 1];
            const double t = b > a ? (double) (j - a) / (double) (b - a) : 0.0;
            std::fill (re.begin(), re.end(), 0.0f);
            std::fill (im.begin(), im.end(), 0.0f);
            for (int k = 1; k < K; ++k)
            {
                const double ma = mag[(size_t) s][(size_t) k], mb = mag[(size_t) s + 1][(size_t) k];
                const double m = ma + (mb - ma) * t;
                // A bin that is silent at one end has no meaningful phase there: keep the other's.
                double pa = ph[(size_t) s][(size_t) k], pb = ph[(size_t) s + 1][(size_t) k];
                if (ma < 1.0e-6 * peakMag) pa = pb;
                if (mb < 1.0e-6 * peakMag) pb = pa;
                const double p = pa + wrapPi (pb - pa) * t;
                re[(size_t) k] = (float) (m * std::cos (p));
                im[(size_t) k] = (float) (m * std::sin (p));
            }
            specRe.push_back (re);
            specIm.push_back (im);
        }
    }

    stored = (int) specRe.size();
    posScale = logicalFrames > 1 ? (float) (stored - 1) / (float) (logicalFrames - 1) : 0.0f;

    // Normalise: peak of the full-band frames over the whole table.
    float gain = 1.0f;
    if (normalised)
    {
        float peak = 0.0f;
        for (int f = 0; f < stored; ++f)
        {
            auto& sr = specRe[(size_t) f];
            auto& si = specIm[(size_t) f];
            for (int k = 0; k < N; ++k) { re[(size_t) k] = sr[(size_t) k]; im[(size_t) k] = si[(size_t) k]; }
            for (int k = 1; k < N / 2; ++k) { re[(size_t) (N - k)] = sr[(size_t) k]; im[(size_t) (N - k)] = -si[(size_t) k]; }
            fft (re.data(), im.data(), N, true);
            for (int i = 0; i < N; ++i) peak = std::max (peak, std::abs (re[(size_t) i]));
        }
        gain = peak > 1.0e-9f ? 1.0f / peak : 1.0f;
    }

    // Mip levels.
    size_t total = 0;
    for (int m = 0; m < kNumMips; ++m)
    {
        levelOffset[m] = total;
        total += (size_t) stored * (size_t) mipStride (m);
    }
    data.assign (total, 0.0f);
    for (int m = 0; m < kNumMips; ++m)
    {
        const int L = mipLength (m);
        const int H = std::min (mipHarmonics (m), L / 2 - 1);
        const float scale = gain * (float) L / (float) N;
        std::vector<float> lr ((size_t) L), li ((size_t) L);
        for (int f = 0; f < stored; ++f)
        {
            std::fill (lr.begin(), lr.end(), 0.0f);
            std::fill (li.begin(), li.end(), 0.0f);
            const auto& sr = specRe[(size_t) f];
            const auto& si = specIm[(size_t) f];
            for (int k = 1; k <= H; ++k)
            {
                lr[(size_t) k] = sr[(size_t) k] * scale;
                li[(size_t) k] = si[(size_t) k] * scale;
                lr[(size_t) (L - k)] = lr[(size_t) k];
                li[(size_t) (L - k)] = -li[(size_t) k];
            }
            fft (lr.data(), li.data(), L, true);
            float* dst = data.data() + levelOffset[m] + (size_t) f * (size_t) mipStride (m);
            for (int i = 0; i < L; ++i) dst[i] = lr[(size_t) i];
            for (int g = 0; g < kGuard; ++g) dst[L + g] = dst[g];
        }
    }
}

void Wavetable::renderFrame (int logical, float* out) const
{
    logical = std::clamp (logical, 0, logicalFrames - 1);
    const float pos = (float) logical * posScale;
    const int a = std::min ((int) pos, stored - 1);
    const int b = std::min (a + 1, stored - 1);
    const float t = pos - (float) a;
    const float* fa = mipFrame (0, a);
    const float* fb = mipFrame (0, b);
    for (int i = 0; i < N; ++i) out[i] = fa[i] + (fb[i] - fa[i]) * t;
}

void Wavetable::save (Node& n) const
{
    n.set ("name", name).set ("category", category)
     .set ("morph", (double) (int) morphType)
     .set ("normalise", normalised ? 1.0 : 0.0)
     .set ("frames", (double) sources.size());
    std::vector<float> all;
    all.reserve (sources.size() * (size_t) N);
    for (const auto& s : sources) all.insert (all.end(), s.begin(), s.end());
    n.set ("data", std::move (all));
}

std::shared_ptr<const Wavetable> Wavetable::load (const Node& n)
{
    std::vector<std::vector<float>> frames;
    if (const auto* d = n.floats ("data"))
    {
        const size_t count = std::min (d->size() / (size_t) N, (size_t) kMaxFrames);
        for (size_t f = 0; f < count; ++f)
            frames.emplace_back (d->begin() + (std::ptrdiff_t) (f * N), d->begin() + (std::ptrdiff_t) ((f + 1) * N));
    }
    const int morph = std::clamp (n.integer ("morph", 0), 0, (int) Morph::SpectralZeroAll);
    return create (n.str ("name", "Custom"), n.str ("category"), std::move (frames), (Morph) morph, n.flag ("normalise", true));
}

// ---------------------------------------------------------------------------
// Factory tables (all generated here; no external data)
// ---------------------------------------------------------------------------
namespace
{
using Frames = std::vector<std::vector<float>>;

template <typename F>
std::vector<float> shapeFrame (F&& f)
{
    std::vector<float> out ((size_t) N);
    for (int i = 0; i < N; ++i) out[(size_t) i] = (float) f ((double) i / N);
    return out;
}

// Additive frame: amp(h) and phase(h) for harmonics 1..maxH, sine phase reference.
template <typename A, typename P>
std::vector<float> harmonicFrame (int maxH, A&& amp, P&& phase)
{
    std::vector<float> re ((size_t) N, 0.0f), im ((size_t) N, 0.0f);
    maxH = std::min (maxH, N / 2 - 1);
    for (int h = 1; h <= maxH; ++h)
    {
        const double a = amp (h) * N * 0.5, p = phase (h) - kPiD * 0.5;
        re[(size_t) h] = (float) (a * std::cos (p));
        im[(size_t) h] = (float) (a * std::sin (p));
        re[(size_t) (N - h)] = re[(size_t) h];
        im[(size_t) (N - h)] = -im[(size_t) h];
    }
    fft (re.data(), im.data(), N, true);
    return re;
}
template <typename A>
std::vector<float> harmonicFrame (int maxH, A&& amp) { return harmonicFrame (maxH, amp, [] (int) { return 0.0; }); }

double sine (double t) { return std::sin (2.0 * kPiD * t); }
double tri (double t) { return t < 0.25 ? 4.0 * t : (t < 0.75 ? 2.0 - 4.0 * t : 4.0 * t - 4.0); }
double saw (double t) { return t < 0.5 ? 2.0 * t : 2.0 * t - 2.0; }    // rises through 0 at the cycle start
double square (double t) { return t < 0.5 ? 1.0 : -1.0; }
double pulse (double t, double w) { return t < w ? 1.0 : -1.0; }
double expSweep (double a, double b, double t) { return a * std::pow (b / a, t); }

template <typename F>
Frames sweep (int count, F&& makeFrame)
{
    Frames f;
    for (int k = 0; k < count; ++k) f.push_back (makeFrame (count > 1 ? (double) k / (count - 1) : 0.0));
    return f;
}

// Saw harmonic amplitudes (sign gives the shape of saw() above).
double sawAmp (int h) { return (h % 2 ? 2.0 : -2.0) / (kPiD * h); }

// Vowel formant envelope (Lorentzian peaks), amplitude at frequency f.
struct Formants { double f[3], bw[3], g[3]; };
double formantGain (const Formants& v, double f)
{
    double a = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        const double x = (f - v.f[i]) / (0.5 * v.bw[i]);
        a += v.g[i] / (1.0 + x * x);
    }
    return a + 0.002;
}
const Formants& vowel (int which, bool female)
{
    static const Formants male[5] {
        { { 730, 1090, 2440 }, { 90, 110, 170 }, { 1.0, 0.5, 0.25 } },     // A
        { { 530, 1840, 2480 }, { 70, 120, 170 }, { 1.0, 0.45, 0.3 } },     // E
        { { 270, 2290, 3010 }, { 60, 140, 200 }, { 1.0, 0.3, 0.25 } },     // I
        { { 570, 840, 2410 },  { 80, 100, 170 }, { 1.0, 0.6, 0.15 } },     // O
        { { 300, 870, 2240 },  { 60, 100, 160 }, { 1.0, 0.35, 0.1 } } };   // U
    static const Formants fem[5] {
        { { 850, 1220, 2810 }, { 120, 150, 250 }, { 1.0, 0.5, 0.25 } },
        { { 610, 2330, 2990 }, { 100, 170, 250 }, { 1.0, 0.45, 0.3 } },
        { { 310, 2790, 3310 }, { 80, 200, 280 },  { 1.0, 0.3, 0.25 } },
        { { 590, 920, 2710 },  { 110, 130, 240 }, { 1.0, 0.6, 0.15 } },
        { { 370, 950, 2670 },  { 90, 130, 230 },  { 1.0, 0.35, 0.1 } } };
    return female ? fem[which] : male[which];
}
// One cycle of a voiced vowel at fundamental f0: glottal-ish source tilt x formants.
std::vector<float> vowelFrame (int which, bool female, double f0, double tilt)
{
    const Formants& v = vowel (which, female);
    const int maxH = std::min (N / 2 - 1, (int) (9000.0 / f0));
    return harmonicFrame (maxH, [&] (int h) { return std::pow ((double) h, -tilt) * formantGain (v, h * f0); },
                          [&] (int h) { return -0.3 * h; });
}

struct Entry
{
    const char* category;
    const char* name;
    Wavetable::Morph morph;
    std::function<Frames()> make;
};

const std::vector<Entry>& entries()
{
    using M = Wavetable::Morph;
    static const std::vector<Entry> list {
        // ---- Analog ------------------------------------------------------
        { "Analog", "Basic Shapes", M::None, [] {
            return Frames { shapeFrame (sine), shapeFrame (tri), shapeFrame (saw), shapeFrame (square),
                            shapeFrame ([] (double t) { return pulse (t, 0.25); }), shapeFrame ([] (double t) { return pulse (t, 0.1); }) }; } },
        { "Analog", "Analog Saw", M::None, [] {
            // A saw through a one-pole low-pass whose cutoff sweeps up (with its phase lag).
            return sweep (32, [] (double x) {
                const double fc = expSweep (1.5, 600.0, x);
                return harmonicFrame (1023, [&] (int h) { return std::abs (sawAmp (h)) / std::sqrt (1.0 + (h / fc) * (h / fc)); },
                                      [&] (int h) { return (h % 2 ? 0.0 : kPiD) - std::atan (h / fc); }); }); } },
        { "Analog", "PWM Sweep", M::None, [] {
            return sweep (64, [] (double x) { const double w = 0.5 - 0.48 * x; return shapeFrame ([w] (double t) { return pulse (t, w); }); }); } },
        { "Analog", "Sync Sweep", M::None, [] {
            return sweep (64, [] (double x) {
                const double r = expSweep (1.0, 8.0, x);
                return shapeFrame ([r] (double t) { const double p = r * t; return 2.0 * (p - std::floor (p)) - 1.0; }); }); } },
        { "Analog", "Soft Square", M::None, [] {
            return sweep (32, [] (double x) {
                const double k = expSweep (0.5, 40.0, x);
                return shapeFrame ([k] (double t) { return std::tanh (k * sine (t)) / std::tanh (k); }); }); } },
        { "Analog", "Tri-Saw Skew", M::None, [] {
            return sweep (32, [] (double x) {
                const double pk = 0.5 + 0.495 * x;
                return shapeFrame ([pk] (double t) { return t < pk ? -1.0 + 2.0 * t / pk : 1.0 - 2.0 * (t - pk) / (1.0 - pk); }); }); } },

        // ---- Digital -----------------------------------------------------
        { "Digital", "FM 1-1", M::None, [] {
            return sweep (64, [] (double x) { const double I = 8.0 * x; return shapeFrame ([I] (double t) { return std::sin (2.0 * kPiD * t + I * sine (t)); }); }); } },
        { "Digital", "FM 2-1", M::None, [] {
            return sweep (64, [] (double x) { const double I = 6.0 * x; return shapeFrame ([I] (double t) { return std::sin (2.0 * kPiD * t + I * sine (2.0 * t)); }); }); } },
        { "Digital", "FM 1-3", M::None, [] {
            return sweep (64, [] (double x) { const double I = 5.0 * x; return shapeFrame ([I] (double t) { return std::sin (2.0 * kPiD * 3.0 * t + I * sine (t)); }); }); } },
        { "Digital", "Bitcrush", M::None, [] {
            return sweep (32, [] (double x) {
                const double levels = std::round (expSweep (64.0, 1.0, x));
                return shapeFrame ([levels] (double t) { return std::round (sine (t) * levels) / levels; }); }); } },
        { "Digital", "Wavefolder", M::None, [] {
            return sweep (64, [] (double x) { const double g = 1.0 + 7.0 * x; return shapeFrame ([g] (double t) { return std::sin (0.5 * kPiD * g * sine (t)); }); }); } },
        { "Digital", "Phase Distortion", M::None, [] {
            return sweep (32, [] (double x) {
                const double d = 0.5 - 0.49 * x;
                return shapeFrame ([d] (double t) { const double p = t < d ? 0.5 * t / d : 0.5 + 0.5 * (t - d) / (1.0 - d); return -std::cos (2.0 * kPiD * p); }); }); } },
        { "Digital", "Digital Harsh", M::None, [] {
            return sweep (32, [] (double x) { const double g = expSweep (1.0, 30.0, x); return shapeFrame ([g] (double t) { return std::clamp (g * sine (t), -1.0, 1.0); }); }); } },

        // ---- S2 Tables ---------------------------------------------------
        { "S2 Tables", "Default Shapes", M::Crossfade, [] {
            return Frames { shapeFrame (sine), shapeFrame (tri), shapeFrame (saw), shapeFrame (square) }; } },
        { "S2 Tables", "Harmonic Series", M::None, [] {
            Frames f;
            for (int k = 1; k <= 32; ++k) f.push_back (harmonicFrame (k, [] (int h) { return 1.0 / h; }));
            return f; } },
        { "S2 Tables", "Saw Stack", M::None, [] {
            return sweep (16, [] (double x) {
                return shapeFrame ([x] (double t) {
                    return saw (t) + x * (0.7 * saw (std::fmod (2.0 * t, 1.0)) + 0.5 * saw (std::fmod (3.0 * t, 1.0)) + 0.35 * saw (std::fmod (4.0 * t, 1.0))); }); }); } },
        { "S2 Tables", "Organ Drawbars", M::Crossfade, [] {
            // Harmonic footages 16' 8' 5 1/3' 4' 2 2/3' 2' 1 3/5' 1 1/3' 1' as harmonics of the 16' pipe.
            static const int harm[9] { 1, 2, 3, 4, 6, 8, 10, 12, 16 };
            static const int regs[8][9] { { 8, 8, 8, 0, 0, 0, 0, 0, 0 }, { 8, 8, 0, 0, 0, 0, 0, 0, 0 }, { 0, 8, 8, 8, 0, 0, 0, 0, 0 },
                                          { 8, 8, 8, 8, 8, 8, 8, 8, 8 }, { 8, 0, 0, 0, 0, 0, 0, 0, 8 }, { 6, 8, 8, 6, 0, 0, 0, 0, 0 },
                                          { 0, 0, 8, 8, 8, 0, 0, 0, 0 }, { 8, 3, 8, 0, 0, 0, 3, 0, 8 } };
            Frames f;
            for (auto& r : regs)
                f.push_back (harmonicFrame (16, [&] (int h) { double a = 0; for (int i = 0; i < 9; ++i) if (harm[i] == h) a += r[i] / 8.0; return a; }));
            return f; } },
        { "S2 Tables", "Resonant Saw", M::None, [] {
            return sweep (64, [] (double x) {
                const double r = expSweep (1.0, 24.0, x);
                return shapeFrame ([r] (double t) { return (1.0 - t) * std::sin (2.0 * kPiD * r * t); }); }); } },
        { "S2 Tables", "Shaper Sweep", M::None, [] {
            return sweep (32, [] (double x) {
                const double k = expSweep (1.0, 12.0, x);
                return shapeFrame ([k] (double t) { const double s = sine (t) + 0.3 * sine (2.0 * t); return std::tanh (k * s) / std::tanh (k); }); }); } },

        // ---- Spectral ----------------------------------------------------
        { "Spectral", "Additive Sweep", M::None, [] {
            return sweep (64, [] (double x) {
                const double c = expSweep (1.0, 256.0, x);
                return harmonicFrame (1023, [c] (int h) { const double r = h / c; return 1.0 / h / (1.0 + r * r * r * r); }); }); } },
        { "Spectral", "Odd Sweep", M::None, [] {
            return sweep (64, [] (double x) {
                const double c = expSweep (1.0, 256.0, x);
                return harmonicFrame (1023, [c] (int h) { if (h % 2 == 0) return 0.0; const double r = h / c; return 1.0 / h / (1.0 + r * r * r * r); }); }); } },
        { "Spectral", "Spectral Comb", M::None, [] {
            return sweep (64, [] (double x) {
                const double p = 2.0 + 14.0 * x;   // comb period in harmonics
                return harmonicFrame (512, [p] (int h) { return (0.55 + 0.45 * std::cos (2.0 * kPiD * h / p)) / h; }); }); } },
        { "Spectral", "Formant Peak", M::None, [] {
            return sweep (64, [] (double x) {
                const double c = std::log2 (expSweep (1.0, 128.0, x));
                return harmonicFrame (1023, [c] (int h) { const double d = std::log2 ((double) h) - c; return 0.05 / h + std::exp (-d * d * 8.0); }); }); } },
        { "Spectral", "Bell Spectrum", M::None, [] {
            static const int partials[7] { 1, 2, 3, 5, 7, 11, 13 };
            return sweep (16, [] (double x) {
                return harmonicFrame (64, [x] (int h) {
                    for (int i = 0; i < 7; ++i) if (partials[i] == h) return std::pow (0.8 - 0.5 * x, (double) i) * (i == 0 ? 1.0 : 0.8);
                    return 0.0; }, [] (int h) { return 0.7 * h; }); }); } },
        { "Spectral", "Spectral Noise", M::None, [] {
            // Correlated random spectra: frame k blends two seeded random sets.
            Rng rng (1234567u);
            std::vector<double> a (512), b (512), ph (512);
            for (int i = 0; i < 512; ++i) { a[(size_t) i] = rng.uni(); b[(size_t) i] = rng.uni(); ph[(size_t) i] = rng.uni() * 2.0 * kPiD; }
            return sweep (32, [&] (double x) {
                return harmonicFrame (511, [&] (int h) { return ((1.0 - x) * a[(size_t) h] + x * b[(size_t) h]) / std::sqrt ((double) h); },
                                      [&] (int h) { return ph[(size_t) h]; }); }); } },

        // ---- Vowel -------------------------------------------------------
        { "Vowel", "A-E-I-O-U", M::Spectral, [] {
            Frames f; for (int v = 0; v < 5; ++v) f.push_back (vowelFrame (v, false, 110.0, 1.0)); return f; } },
        { "Vowel", "Female Vowels", M::Crossfade, [] {
            Frames f; for (int v = 0; v < 5; ++v) f.push_back (vowelFrame (v, true, 220.0, 1.0)); return f; } },
        { "Vowel", "Bass Vowels", M::Crossfade, [] {
            Frames f; for (int v : { 3, 0, 1, 4 }) f.push_back (vowelFrame (v, false, 55.0, 1.2)); return f; } },
        { "Vowel", "Talk Box", M::Crossfade, [] {
            Frames f; for (int v : { 0, 2, 3, 1, 4 }) f.push_back (vowelFrame (v, false, 147.0, 0.6)); return f; } },
        { "Vowel", "Choir Ahh-Ooh", M::SpectralZeroFund, [] {
            return Frames { vowelFrame (0, true, 196.0, 1.4), vowelFrame (3, true, 196.0, 1.4), vowelFrame (4, true, 196.0, 1.4) }; } },
    };
    return list;
}

std::mutex& cacheLock() { static std::mutex m; return m; }
std::vector<std::shared_ptr<const Wavetable>>& cache()
{
    static std::vector<std::shared_ptr<const Wavetable>> c (entries().size());
    return c;
}
} // namespace

const std::vector<std::string>& FactoryTables::categories()
{
    static const std::vector<std::string> c { "Analog", "Digital", "S2 Tables", "Spectral", "Vowel" };
    return c;
}

std::vector<std::string> FactoryTables::names (const std::string& category)
{
    std::vector<std::string> out;
    for (const auto& e : entries())
        if (category == e.category) out.push_back (e.name);
    return out;
}

int FactoryTables::count() { return (int) entries().size(); }

std::shared_ptr<const Wavetable> FactoryTables::get (const std::string& category, const std::string& name)
{
    const auto& list = entries();
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (category != list[i].category || name != list[i].name) continue;
        std::lock_guard<std::mutex> lock (cacheLock());
        auto& slot = cache()[i];
        if (! slot)
        {
            auto t = Wavetable::create (list[i].name, list[i].category, list[i].make(), list[i].morph, true);
            t->factory = true;
            slot = std::move (t);
        }
        return slot;
    }
    return nullptr;
}

std::shared_ptr<const Wavetable> FactoryTables::defaultTable() { return get ("S2 Tables", "Default Shapes"); }
} // namespace tincture
