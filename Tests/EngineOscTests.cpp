// EngineOscTests.cpp — tests and benchmarks for the oscillator core:
// Wavetable (FFT, mips, morphs, factory library, save/load), Warp,
// WavetableOsc, SubOsc and NoiseOsc. Returns non-zero on failure.
#include "Engine/NoiseOsc.h"
#include "Engine/SubOsc.h"
#include "Engine/Warp.h"
#include "Engine/Wavetable.h"
#include "Engine/WavetableOsc.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <set>
#include <vector>

using namespace tincture;

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECKV(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

constexpr double SR = 48000.0;
using Vec = std::vector<float>;

// ---------------------------------------------------------------------------
// Parameter harness
// ---------------------------------------------------------------------------
struct Harness
{
    ParamValues pv;
    std::vector<int16_t> slots;
    ParamView view;
    Context ctx;
    Harness()
    {
        pv.resetToDefaults();
        slots.assign (pv.v.size(), -1);
        view.base = pv.v.data();
        view.slot = slots.data();
        ctx.sampleRate = SR;
        ctx.invSampleRate = (float) (1.0 / SR);
    }
    void set (int id, float v) { pv[id] = v; }
};

const OscIds& A() { return tincture::ids().osc[0]; }

struct WtRig
{
    Harness h;
    WavetableOsc osc;
    std::unique_ptr<WavetableOsc::Shared> sh = std::make_unique<WavetableOsc::Shared>();
    WavetableOscData data;
    const Vec* src[kNumSrcBuffers] {};
    int seed = 1;
    WtRig()
    {
        data.table = FactoryTables::defaultTable();
        data.bakeRemaps();
        h.set (A().rand, 0.0f);
        h.set (A().phase, 0.0f);
        h.set (A().level, 1.0f);
        osc.prepare (h.ctx);
    }
    OscTick makeTick (float note, int n, float* L, float* R, int offset)
    {
        OscTick t;
        t.n = n; t.pitchStart = t.pitchEnd = note; t.p = &h.view; t.ctx = &h.ctx; t.outL = L; t.outR = R;
        for (int s = 0; s < kNumSrcBuffers; ++s) t.src[s] = src[s] ? src[s]->data() + offset : nullptr;
        return t;
    }
    void noteOn (float note, bool stolen = false)
    {
        NoteInfo ni; ni.note = (int) note; ni.unisonSeed = seed++; ni.stolen = stolen;
        float l[kTick], r[kTick];
        OscTick t = makeTick (note, kTick, l, r, 0);
        osc.noteOn (ni, t, A(), data, *sh);
    }
    // Renders `total` samples (note-on first if asked).
    void run (float note, int total, Vec& L, Vec& R, bool retrigger = true)
    {
        if (retrigger) noteOn (note);
        L.assign ((size_t) total, 0.0f); R.assign ((size_t) total, 0.0f);
        for (int off = 0; off < total; off += kTick)
        {
            const int n = std::min (kTick, total - off);
            OscTick t = makeTick (note, n, L.data() + off, R.data() + off, off);
            osc.render (t, A(), data, *sh);
        }
    }
    Vec mono (float note, int total)
    {
        Vec L, R;
        run (note, total, L, R);
        for (size_t i = 0; i < L.size(); ++i) L[i] = 0.5f * (L[i] + R[i]);
        return L;
    }
};

// ---------------------------------------------------------------------------
// Analysis helpers
// ---------------------------------------------------------------------------
// Frequency from rising zero crossings (linear interpolation), skipping `skip` samples.
double zeroCrossFreq (const Vec& x, int skip = 2000)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    for (size_t i = (size_t) skip + 1; i < x.size(); ++i)
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = (double) (i - 1) + (double) x[i - 1] / ((double) x[i - 1] - (double) x[i]);
            if (first < 0.0) first = t;
            last = t;
            ++count;
        }
    return count > 1 ? (count - 1) * SR / (last - first) : 0.0;
}
double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }

// Amplitude of a sinusoid at `hz` (correlation over whole samples; x should hold whole periods).
double amplitudeAt (const Vec& x, double hz, size_t start = 0)
{
    double re = 0.0, im = 0.0;
    const size_t n = x.size() - start;
    for (size_t i = 0; i < n; ++i)
    {
        const double a = 2.0 * 3.14159265358979323846 * hz * (double) i / SR;
        re += x[start + i] * std::cos (a);
        im += x[start + i] * std::sin (a);
    }
    return 2.0 * std::sqrt (re * re + im * im) / (double) n;
}

// Blackman-Harris windowed power spectrum (size power of two), in linear power per bin.
std::vector<double> powerSpectrum (const Vec& x, size_t start, int n)
{
    std::vector<float> re ((size_t) n), im ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const double t = 2.0 * 3.14159265358979323846 * i / (n - 1);
        const double w = 0.35875 - 0.48829 * std::cos (t) + 0.14128 * std::cos (2 * t) - 0.01168 * std::cos (3 * t);
        re[(size_t) i] = (float) (x[start + (size_t) i] * w);
    }
    fft (re.data(), im.data(), n, false);
    std::vector<double> p ((size_t) n / 2);
    for (int k = 0; k < n / 2; ++k) p[(size_t) k] = (double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k];
    return p;
}

// Welch-averaged spectrum.
std::vector<double> averagedSpectrum (const Vec& x, int n)
{
    std::vector<double> acc ((size_t) n / 2, 0.0);
    int count = 0;
    for (size_t s = 0; s + (size_t) n <= x.size(); s += (size_t) n / 2, ++count)
    {
        auto p = powerSpectrum (x, s, n);
        for (size_t k = 0; k < p.size(); ++k) acc[k] += p[k];
    }
    for (auto& v : acc) v /= std::max (1, count);
    return acc;
}

// Band power between two frequencies.
double bandPower (const std::vector<double>& p, int n, double lo, double hi)
{
    double s = 0.0;
    for (size_t k = 0; k < p.size(); ++k)
    {
        const double f = (double) k * SR / n;
        if (f >= lo && f < hi) s += p[k];
    }
    return s;
}

// Ratio (dB) of the strongest non-harmonic bin to the fundamental, and of
// total non-harmonic power to total harmonic power.
void aliasLevels (const Vec& x, double f0, double& worstDb, double& totalDb)
{
    const int n = 65536;
    auto p = powerSpectrum (x, x.size() - (size_t) n, n);
    const double binHz = SR / n;
    double harm = 0.0, non = 0.0, worst = 0.0, fund = 0.0;
    for (size_t k = 1; k < p.size(); ++k)
    {
        const double f = (double) k * binHz;
        const double h = f / f0;
        const double dist = std::abs (h - std::round (h)) * f0 / binHz;   // bins to the nearest harmonic
        if (std::round (h) >= 1.0 && dist <= 6.0) { harm += p[k]; if (std::round (h) == 1.0) fund = std::max (fund, p[k]); }
        else if (f > 5.0) { non += p[k]; worst = std::max (worst, p[k]); }
    }
    worstDb = 10.0 * std::log10 (std::max (worst, 1e-30) / std::max (fund, 1e-30));
    totalDb = 10.0 * std::log10 (std::max (non, 1e-30) / std::max (harm, 1e-30));
}

bool allFinite (const Vec& x, float bound, float& peak)
{
    peak = 0.0f;
    for (float v : x)
    {
        if (! std::isfinite (v)) return false;
        peak = std::max (peak, std::abs (v));
    }
    return peak <= bound;
}

Vec sineSignal (double hz, int total, double amp = 1.0)
{
    Vec v ((size_t) total);
    for (int i = 0; i < total; ++i) v[(size_t) i] = (float) (amp * std::sin (2.0 * 3.14159265358979323846 * hz * i / SR));
    return v;
}

double slopeDbPerOct (const std::vector<double>& p, int n, double f1, double f2)
{
    // Average power density in third-octave-ish bands at f1 and f2.
    const double d1 = bandPower (p, n, f1 / 1.12, f1 * 1.12) / (f1 * 0.226);
    const double d2 = bandPower (p, n, f2 / 1.12, f2 * 1.12) / (f2 * 0.226);
    return 10.0 * std::log10 (d2 / d1) / std::log2 (f2 / f1);
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------
void testFft()
{
    std::printf ("[fft]\n");
    const int n = 1024;
    Vec re ((size_t) n), im ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i) re[(size_t) i] = (float) std::cos (2.0 * 3.14159265358979323846 * 5 * i / n);
    Vec orig = re;
    fft (re.data(), im.data(), n, false);
    CHECK (std::abs (re[5] - n / 2.0f) < 1e-2f && std::abs (re[(size_t) n - 5] - n / 2.0f) < 1e-2f && std::abs (re[4]) < 1e-2f);
    fft (re.data(), im.data(), n, true);
    float err = 0.0f;
    for (int i = 0; i < n; ++i) err = std::max (err, std::abs (re[(size_t) i] - orig[(size_t) i]));
    CHECK (err < 1e-5f);
}

void testWavetableBuild()
{
    std::printf ("[wavetable build / mips / morph / factory]\n");
    // Mip band limit: a saw table's level m holds no energy above harmonic 1024>>m.
    auto saw = FactoryTables::get ("Analog", "Basic Shapes");
    CHECK (saw != nullptr && saw->numFrames() == 6 && saw->storedFrames() == 6);
    for (int m = 0; m < Wavetable::kNumMips; ++m)
    {
        const int L = Wavetable::mipLength (m);
        Vec re ((size_t) L), im ((size_t) L, 0.0f);
        const float* f = saw->mipFrame (m, 2);
        for (int i = 0; i < L; ++i) re[(size_t) i] = f[i];
        CHECK (f[L] == f[0] && f[L + 1] == f[1]);
        fft (re.data(), im.data(), L, false);
        double inBand = 0, outBand = 0;
        for (int k = 1; k < L / 2; ++k)
        {
            const double e = (double) re[(size_t) k] * re[(size_t) k] + (double) im[(size_t) k] * im[(size_t) k];
            (k <= Wavetable::mipHarmonics (m) ? inBand : outBand) += e;
        }
        CHECKV (outBand < inBand * 1e-10, "mip %d out-of-band %g", m, outBand / inBand);
    }
    // Default Shapes: 4 sources, crossfade morph -> 256 logical frames, 4 stored.
    auto def = FactoryTables::defaultTable();
    CHECK (def->numFrames() == 256 && def->storedFrames() == 4 && def->isSourceFrame (85) && ! def->isSourceFrame (84));
    CHECK (std::abs (def->storedPos (1.0f, true) - 3.0f) < 1e-5f);

    // Spectral morph between a sine and its octave: middle frame has both at half magnitude.
    std::vector<Vec> frames (2, Vec (2048));
    for (int i = 0; i < 2048; ++i)
    {
        frames[0][(size_t) i] = (float) std::sin (2.0 * 3.14159265358979323846 * i / 2048.0);
        frames[1][(size_t) i] = (float) std::sin (2.0 * 3.14159265358979323846 * 2.0 * i / 2048.0);
    }
    auto sm = Wavetable::create ("m", "", frames, Wavetable::Morph::Spectral, false);
    CHECK (sm->numFrames() == 256 && sm->storedFrames() == 256);
    {
        Vec f (2048);
        sm->renderFrame (128, f.data());
        double a1 = 0, a2 = 0;
        for (int i = 0; i < 2048; ++i)
        {
            a1 += f[(size_t) i] * std::sin (2.0 * 3.14159265358979323846 * i / 2048.0);
            a2 += f[(size_t) i] * std::sin (2.0 * 3.14159265358979323846 * 2.0 * i / 2048.0);
        }
        a1 /= 1024.0; a2 /= 1024.0;
        CHECKV (std::abs (a1 - 0.498) < 0.01 && std::abs (a2 - 0.502) < 0.01, "spectral mid %f %f", a1, a2);
    }
    // Non-2048 frames are resampled.
    auto small = Wavetable::create ("s", "", { Vec (600, 0.5f) }, Wavetable::Morph::None);
    CHECK (small->numFrames() == 1);

    // Factory library: ~30 tables, all build, unique names, finite, peak ~1.
    int count = 0;
    size_t bytes = 0;
    auto t0 = std::chrono::steady_clock::now();
    for (const auto& cat : FactoryTables::categories())
    {
        std::set<std::string> seen;
        for (const auto& name : FactoryTables::names (cat))
        {
            CHECK (seen.insert (name).second);
            auto t = FactoryTables::get (cat, name);
            CHECK (t != nullptr && t->factory);
            if (! t) continue;
            ++count;
            bytes += t->memoryBytes();
            float peak = 0.0f;
            bool finite = true;
            for (int f = 0; f < t->storedFrames(); ++f)
                for (int i = 0; i < 2048; ++i)
                {
                    const float v = t->mipFrame (0, f)[i];
                    finite = finite && std::isfinite (v);
                    peak = std::max (peak, std::abs (v));
                }
            CHECKV (finite && peak > 0.9f && peak < 1.01f, "%s/%s peak %f", cat.c_str(), name.c_str(), peak);
            CHECK (FactoryTables::get (cat, name) == t);   // cached
        }
    }
    const double ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - t0).count();
    CHECK (count == FactoryTables::count() && count >= 28);
    CHECK (FactoryTables::get ("Analog", "nope") == nullptr);
    std::printf ("  factory tables: %d, built in %.0f ms, %.1f MB total\n", count, ms, (double) bytes / 1048576.0);
}

void testSaveLoad()
{
    std::printf ("[save / load]\n");
    // Custom table with a morph: binary round trip reproduces the mips exactly.
    Rng rng (5u);
    std::vector<Vec> frames (3, Vec (2048));
    for (auto& f : frames) for (auto& v : f) v = rng.bi();
    auto t = Wavetable::create ("Mine", "User", frames, Wavetable::Morph::SpectralZeroFund);
    WavetableOscData d;
    d.table = t;
    d.remap[1].points = { { 0, 0, 0, false }, { 0.3f, 0.8f, 0.5f, false }, { 1, 1, 0, false } };
    d.bakeRemaps();
    Node n ("osc");
    d.save (n);
    Node back;
    auto bin = n.toBinary();
    CHECK (Node::fromBinary (bin.data(), bin.size(), back));
    WavetableOscData d2;
    d2.load (back);
    CHECK (d2.table && d2.table->name == "Mine" && d2.table->category == "User" && d2.table->numFrames() == 256);
    CHECK (d2.table->morph() == Wavetable::Morph::SpectralZeroFund && d2.table->numSourceFrames() == 3);
    bool same = d2.table && d2.table->memoryBytes() == t->memoryBytes();
    for (int m = 0; same && m < Wavetable::kNumMips; m += 3)
        for (int f = 0; same && f < t->storedFrames(); f += 17)
            same = std::equal (t->mipFrame (m, f), t->mipFrame (m, f) + Wavetable::mipStride (m), d2.table->mipFrame (m, f));
    CHECK (same);
    CHECK (d2.remap[1] == d.remap[1] && d2.remap[0] == Curve());
    CHECK (std::abs (d2.remapLut[1] (0.3f) - 0.8f) < 1e-3f && d2.remapSlope[1] > 1.0f);

    // Factory tables save a reference only.
    WavetableOscData f;
    f.table = FactoryTables::get ("Vowel", "Talk Box");
    Node fn ("osc");
    f.save (fn);
    CHECK (fn.child ("table") && fn.child ("table")->flag ("factory") && fn.child ("table")->floats ("data") == nullptr);
    WavetableOscData f2;
    f2.load (fn);
    CHECK (f2.table == f.table);
    // Missing table -> default.
    WavetableOscData e;
    e.load (Node ("osc"));
    CHECK (e.table == FactoryTables::defaultTable());

    // Noise data: factory reference and an embedded user sample.
    NoiseOscData nd;
    nd.sample = FactoryNoises::get ("Organics", "Rain");
    Node nn ("noise");
    nd.save (nn);
    NoiseOscData nd2;
    nd2.load (nn);
    CHECK (nd2.sample == nd.sample && nd.sample != nullptr);
    auto user = std::make_shared<SampleData>();
    user->name = "mine"; user->embedded = true; user->left = { 0.1f, 0.2f, 0.3f, 0.4f, 0.5f }; user->sampleRate = 44100.0;
    nd.sample = user;
    Node un ("noise");
    nd.save (un);
    auto ub = un.toBinary();
    Node uback;
    CHECK (Node::fromBinary (ub.data(), ub.size(), uback));
    nd2.load (uback);
    CHECK (nd2.sample && nd2.sample != nd.sample && nd2.sample->left == user->left && nd2.sample->sampleRate == 44100.0);
}

void testWtPitch()
{
    std::printf ("[wavetable osc pitch]\n");
    WtRig rig;
    const int total = (int) SR * 4;
    double f = zeroCrossFreq (rig.mono (69.0f, total));
    CHECKV (std::abs (cents (f, 440.0)) < 0.1, "A4 %f Hz", f);
    std::printf ("  note 69: %.5f Hz (%.4f cents)\n", f, cents (f, 440.0));

    rig.h.set (A().oct, 1); rig.h.set (A().sem, -5); rig.h.set (A().fine, 37.0f); rig.h.set (A().coarse, 0.25f);
    f = zeroCrossFreq (rig.mono (50.0f, total));
    const double want = noteToHz (50.0f + 12.0f - 5.0f + 0.37f + 0.25f);
    CHECKV (std::abs (cents (f, want)) < 0.1, "oct/sem/fine/crs %f vs %f", f, want);

    // Harmonics mode: SEM 2 -> 3rd harmonic.
    rig.h.set (A().oct, 0); rig.h.set (A().fine, 0.0f); rig.h.set (A().coarse, 0.0f);
    rig.h.set (A().pitchMode, 1); rig.h.set (A().sem, 2);
    f = zeroCrossFreq (rig.mono (45.0f, total));
    CHECKV (std::abs (cents (f, 3.0 * 110.0)) < 0.1, "harmonics %f", f);
    // Ratio mode.
    rig.h.set (A().pitchMode, 2); rig.h.set (A().ratio, 1.5f);
    f = zeroCrossFreq (rig.mono (57.0f, total));
    CHECKV (std::abs (cents (f, 330.0)) < 0.1, "ratio %f", f);
    rig.h.set (A().pitchMode, 0); rig.h.set (A().sem, 0);
    // Pitch tracking off: MIDI 0 whatever the note (an LFO-rate oscillator).
    rig.h.set (A().pitchTrack, 0.0f);
    rig.h.set (A().oct, 4);
    f = zeroCrossFreq (rig.mono (90.0f, total), 0);
    CHECKV (std::abs (cents (f, noteToHz (48.0f))) < 0.5, "track off %f", f);
    rig.h.set (A().pitchTrack, 1.0f);
    rig.h.set (A().oct, 0);
    // High note.
    f = zeroCrossFreq (rig.mono (117.0f, total));
    CHECKV (std::abs (cents (f, noteToHz (117.0f))) < 0.1, "note 117 %f", f);
}

void testWtAlias()
{
    std::printf ("[wavetable osc aliasing]\n");
    WtRig rig;
    rig.h.set (A().wtpos, 2.0f / 3.0f);   // Default Shapes frame 3 = saw
    for (float note : { 100.0f, 110.0f, 88.0f })
    {
        Vec x = rig.mono (note, 80000);
        double worst, total;
        aliasLevels (x, noteToHz (note), worst, total);
        std::printf ("  saw note %.0f: worst non-harmonic bin %.1f dB, total %.1f dB\n", note, worst, total);
        CHECKV (worst < -60.0 && total < -60.0, "note %f", note);
    }
    // Square (a hard case for linear interpolation) at note 100.
    rig.h.set (A().wtpos, 1.0f);
    {
        Vec x = rig.mono (100.0f, 80000);
        double worst, total;
        aliasLevels (x, noteToHz (100.0f), worst, total);
        std::printf ("  square note 100: worst %.1f dB, total %.1f dB\n", worst, total);
        CHECK (worst < -55.0);
    }
    // Pitch glide across a mip boundary stays clean (no clicks): max sample-to-sample jump bounded.
    rig.h.set (A().wtpos, 2.0f / 3.0f);
    Vec L, R;
    rig.noteOn (60.0f);
    float maxJump = 0.0f, prev = 0.0f;
    for (int k = 0; k < 3000; ++k)
    {
        float l[kTick], r[kTick];
        OscTick t = rig.makeTick (60.0f, kTick, l, r, 0);
        t.pitchStart = 60.0f + 0.01f * k; t.pitchEnd = 60.0f + 0.01f * (k + 1);
        rig.osc.render (t, A(), rig.data, *rig.sh);
        for (int i = 0; i < kTick; ++i) { if (k > 0) maxJump = std::max (maxJump, std::abs (l[i] - prev)); prev = l[i]; }
    }
    CHECK (maxJump < 2.5f);
}

void testWtPos()
{
    std::printf ("[wt pos interpolation]\n");
    WtRig rig;
    const float note = hzToNote (100.0f);          // 480-sample period
    auto spectrum = [&] (float pos, bool smooth, double& h1, double& h2, double& h3)
    {
        rig.h.set (A().wtpos, pos);
        rig.h.set (A().wtSmooth, smooth ? 1.0f : 0.0f);
        Vec x = rig.mono (note, 480 * 60);
        Vec tail (x.begin() + 480 * 20, x.end());
        h1 = amplitudeAt (tail, 100.0); h2 = amplitudeAt (tail, 200.0); h3 = amplitudeAt (tail, 300.0);
    };
    double s1, s2, s3, t1, t2, t3, m1, m2, m3;
    spectrum (0.0f, true, s1, s2, s3);                // sine
    spectrum (1.0f / 3.0f, true, t1, t2, t3);         // triangle
    spectrum (1.0f / 6.0f, true, m1, m2, m3);         // halfway
    CHECKV (s3 < s1 * 1e-3 && std::abs (t3 / t1 - 1.0 / 9.0) < 0.01, "sine h3 %g, tri h3/h1 %g", s3 / s1, t3 / t1);
    CHECKV (std::abs (m1 - 0.5 * (s1 + t1)) < 0.005 && std::abs (m3 - 0.5 * (s3 + t3)) < 0.002, "mid %f vs %f, %f vs %f", m1, 0.5 * (s1 + t1), m3, 0.5 * (s3 + t3));
    // Saw has even harmonics, square does not.
    double w1, w2, w3, q1, q2, q3;
    spectrum (2.0f / 3.0f, true, w1, w2, w3);
    spectrum (1.0f, true, q1, q2, q3);
    CHECKV (std::abs (w2 / w1 - 0.5) < 0.01 && q2 < q1 * 1e-3 && std::abs (q3 / q1 - 1.0 / 3.0) < 0.01, "saw %f square %f %f", w2 / w1, q2 / q1, q3 / q1);

    // Smooth off on a 6-frame table snaps to a frame; smooth on blends.
    rig.data.table = FactoryTables::get ("Analog", "Basic Shapes");
    double a1, a2, a3, b1, b2, b3;
    spectrum (0.08f, false, a1, a2, a3);   // 0.4 of the way to frame 2 -> frame 1 (sine)
    spectrum (0.08f, true, b1, b2, b3);
    CHECKV (a3 < a1 * 1e-3 && b3 > b1 * 0.02, "smooth off h3 %g, on %g", a3 / a1, b3 / b1);

    // Uni WT POS spread: voices read different frames (the sine gains harmonics).
    rig.data.table = FactoryTables::defaultTable();
    rig.h.set (A().unison, 3); rig.h.set (A().detune, 0.0f); rig.h.set (A().uniWtpos, 1.0f);
    rig.h.set (A().uniWidth, 0.0f);
    spectrum (0.0f, true, a1, a2, a3);
    CHECK (a3 > a1 * 0.01);
}

void testUnison()
{
    std::printf ("[unison]\n");
    WtRig rig;
    rig.h.set (A().rand, 1.0f);
    rig.h.set (A().unison, 7);
    rig.h.set (A().detune, 1.0f);
    rig.h.set (A().uniRange, 2.0f);   // outer voices at +-1 semitone
    const float note = 69.0f;
    Vec L, R;
    rig.run (note, 70000, L, R);
    Vec m (L.size());
    for (size_t i = 0; i < L.size(); ++i) m[i] = 0.5f * (L[i] + R[i]);
    const int n = 65536;
    auto p = powerSpectrum (m, m.size() - (size_t) n, n);
    auto peak = [&] (double hz) { return bandPower (p, n, hz * 0.995, hz * 1.005); };
    // 7 voices, Linear: offsets 0, +-1/3, +-2/3, +-1 semitone.
    const double c = peak (440.0), up = peak (440.0 * std::exp2 (1.0 / 12.0)), dn = peak (440.0 * std::exp2 (-1.0 / 12.0));
    const double third = peak (440.0 * std::exp2 (1.0 / 36.0)), gap = peak (440.0 * std::exp2 (0.5 / 12.0));
    CHECKV (up > c * 0.3 && dn > c * 0.3 && third > c * 0.3 && gap < c * 0.01, "unison peaks %g %g %g %g", up / c, dn / c, third / c, gap / c);

    // Width 0: mono. Width 1: decorrelated sides.
    auto corr = [] (const Vec& a, const Vec& b)
    {
        double ab = 0, aa = 0, bb = 0;
        for (size_t i = 0; i < a.size(); ++i) { ab += a[i] * b[i]; aa += a[i] * a[i]; bb += b[i] * b[i]; }
        return ab / std::sqrt (aa * bb + 1e-30);
    };
    const double c1 = corr (L, R);
    rig.h.set (A().uniWidth, 0.0f);
    rig.run (note, 20000, L, R);
    bool equal = true;
    for (size_t i = 0; i < L.size(); ++i) equal = equal && std::abs (L[i] - R[i]) < 1e-6f;
    CHECKV (equal && c1 < 0.8, "width corr %f", c1);

    // Level compensation: 1 vs 16 voices (random phases) within +-3 dB RMS.
    auto rms = [&] (int voices)
    {
        rig.h.set (A().unison, (float) voices);
        double s = 0;
        int cnt = 0;
        for (int k = 0; k < 8; ++k)
        {
            Vec l, r;
            rig.run (note, 24000, l, r);
            for (size_t i = 2000; i < l.size(); ++i) { s += l[i] * l[i] + r[i] * r[i]; ++cnt; }
        }
        return std::sqrt (s / cnt);
    };
    const double r1 = rms (1), r16 = rms (16);
    CHECKV (std::abs (20.0 * std::log10 (r16 / r1)) < 3.0, "level comp %f dB", 20.0 * std::log10 (r16 / r1));
    // Blend 0: only the centre voice -> a clean single pitch.
    rig.h.set (A().unison, 7); rig.h.set (A().blend, 0.0f);
    Vec x = rig.mono (note, 70000);
    p = powerSpectrum (x, x.size() - (size_t) n, n);
    CHECK (peak (440.0 * std::exp2 (1.0 / 12.0)) < peak (440.0) * 1e-5);
    // Stack 12: a voice an octave up appears.
    rig.h.set (A().blend, 0.75f); rig.h.set (A().detune, 0.0f); rig.h.set (A().uniStack, 1); rig.h.set (A().unison, 4);
    x = rig.mono (note, 70000);
    p = powerSpectrum (x, x.size() - (size_t) n, n);
    CHECK (peak (880.0) > peak (440.0) * 0.1 && peak (1760.0) > peak (440.0) * 0.1);
    // Every unison mode runs and stays bounded.
    rig.h.set (A().uniStack, 0); rig.h.set (A().detune, 1.0f); rig.h.set (A().unison, 16);
    for (int mode = 0; mode < 5; ++mode)
    {
        rig.h.set (A().uniMode, (float) mode);
        Vec l, r;
        rig.run (note, 8000, l, r);
        float pk;
        CHECK (allFinite (l, 5.7f, pk) && allFinite (r, 5.7f, pk));   // 16 x 1/4 x sqrt2 worst case
    }
}

void testPhase()
{
    std::printf ("[phase / memory]\n");
    WtRig rig;
    rig.h.set (A().phase, 90.0f);   // sine starts at its peak
    Vec x = rig.mono (60.0f, 64);
    CHECKV (std::abs (x[0] - 1.0f) < 0.01f, "phase 90 -> %f", x[0]);
    // Per Voice: identical starts with RAND 100%.
    rig.h.set (A().rand, 1.0f);
    rig.h.set (A().phaseMem, 2);
    Vec a = rig.mono (60.0f, 64), b = rig.mono (60.0f, 64);
    CHECK (a == b);
    // All Voices: RAND 100% gives different starts.
    rig.h.set (A().phaseMem, 0);
    a = rig.mono (60.0f, 64); b = rig.mono (60.0f, 64);
    CHECK (a != b);
    // Contiguous: a new voice continues from the newest voice's phase.
    rig.h.set (A().phaseMem, 1);
    rig.mono (60.0f, 1000);
    const float ph = rig.osc.currentPhase();
    WavetableOsc other;
    other.prepare (rig.h.ctx);
    NoteInfo ni;
    float l[kTick], r[kTick];
    OscTick t = rig.makeTick (60.0f, kTick, l, r, 0);
    other.noteOn (ni, t, A(), rig.data, *rig.sh);
    CHECK (std::abs (other.currentPhase() - ph) < 1e-6f);
    // A stolen voice in non-legato mode keeps its phase (Phase Legato off).
    rig.h.set (A().phaseMem, 0);
    rig.mono (60.0f, 500);
    const float before = rig.osc.currentPhase();
    rig.noteOn (64.0f, true);
    CHECK (rig.osc.currentPhase() == before);
    rig.h.set (A().phaseLegato, 1.0f);
    rig.noteOn (64.0f, true);
    CHECK (rig.osc.currentPhase() != before);
}

// Two source signals for FM/PD/AM/RM.
struct Sources
{
    Vec a, b, noise;
    Sources (int total)
    {
        a = sineSignal (330.0, total);
        b = sineSignal (1234.5, total, 0.8);
        noise.resize ((size_t) total);
        Rng rng (9u);
        for (auto& v : noise) v = rng.bi();
    }
    void attach (WtRig& rig)
    {
        rig.src[kSrcB] = &a; rig.src[kSrcC] = &b; rig.src[kSrcNoise] = &noise; rig.src[kSrcSub] = &a;
        rig.src[kSrcF1] = &b; rig.src[kSrcF2] = &a; rig.src[kSrcA] = &b;
    }
};

void testWarps()
{
    std::printf ("[warps: all modes, extremes]\n");
    CHECK (warp::kindOf (0) == warp::Kind::None && warp::kindOf (1) == warp::Kind::Sync && warp::kindOf (16) == warp::Kind::Phase);
    CHECK (warp::kindOf (17) == warp::Kind::Filter && warp::kindOf (32) == warp::Kind::Distortion && warp::kindOf (33) == warp::Kind::Fm);
    CHECK (warp::kindOf (47) == warp::Kind::Pd && warp::kindOf (48) == warp::Kind::Am && warp::kindOf (61) == warp::Kind::Rm);
    CHECK (warp::sourceOf (34) == kSrcB && warp::sourceOf (47) == -1 && warp::sourceOf (52) == kSrcSub && warp::sourceOf (61) == kSrcF2);
    CHECK (warp::isPost (17) && warp::isPost (50) && ! warp::isPost (40) && ! warp::isPost (1));
    // Shapes are bounded for inputs up to +-2.
    for (int m = warp::DistFirst; m <= warp::DistLast; ++m)
        for (float a : { 0.0f, 0.5f, 1.0f })
            for (float xin = -2.0f; xin <= 2.0f; xin += 0.01f)
            {
                const float y = warp::shape (m, a, xin);
                if (! std::isfinite (y) || std::abs (y) > 4.0f) { CHECKV (false, "shape %d a %f x %f -> %f", m, a, xin, y); break; }
            }

    const int total = 6000;
    Sources src (total + 64);
    int silentModes = 0;
    for (int os : { 1, 2, 4 })
    {
        WtRig rig;
        rig.h.ctx.oversample = os;
        src.attach (rig);
        rig.h.set (A().wtpos, 0.5f);
        rig.h.set (A().unison, 3);
        rig.h.set (A().uniWarp1, 1.0f);
        rig.h.set (A().uniWarp2, -1.0f);
        rig.h.set (A().warpVar, 0.5f);
        Vec ref = rig.mono (60.0f, total);
        for (int mode = 0; mode < warp::NumModes; ++mode)
        {
            for (int fmType = 0; fmType < (warp::kindOf (mode) == warp::Kind::Fm ? 4 : 1); ++fmType)
            {
                rig.h.set (A().fmType, (float) fmType);
                double diff = 0.0;
                for (float amt : { 0.0f, 0.5f, 1.0f })
                    for (float note : { 24.0f, 60.0f, 127.0f })
                        for (int slotIdx = 0; slotIdx < 2; ++slotIdx)
                        {
                            rig.h.set (A().warp1Mode, slotIdx == 0 ? (float) mode : (float) warp::Tube);
                            rig.h.set (A().warp2Mode, slotIdx == 1 ? (float) mode : 0.0f);
                            rig.h.set (A().warp1, slotIdx == 0 ? amt : 0.3f);
                            rig.h.set (A().warp2, amt);
                            Vec x = rig.mono (note, total);
                            float pk;
                            const bool ok = allFinite (x, 4.0f, pk);
                            CHECKV (ok, "mode %d fm %d amt %.1f note %.0f slot %d os %d peak %f", mode, fmType, amt, note, slotIdx, os, pk);
                            if (amt == 1.0f && note == 60.0f && slotIdx == 1)
                                for (size_t i = 0; i < x.size(); ++i) diff += std::abs (x[i] - ref[i]);
                        }
                if (mode != 0 && os == 1 && diff < 1.0) { ++silentModes; std::printf ("  mode %d (%s) had no effect\n", mode, choices::warpModes()[(size_t) mode].c_str()); }
            }
        }
    }
    CHECK (silentModes == 0);

    // Specific behaviours (single voice).
    WtRig rig;
    src.attach (rig);
    const float note = hzToNote (100.0f);
    auto harmonics = [&] (int mode, float amt, double& h1, double& h2, double& h5)
    {
        rig.h.set (A().warp1Mode, (float) mode);
        rig.h.set (A().warp1, amt);
        Vec x = rig.mono (note, 480 * 40);
        Vec tail (x.begin() + 480 * 10, x.end());
        h1 = amplitudeAt (tail, 100.0); h2 = amplitudeAt (tail, 200.0); h5 = amplitudeAt (tail, 500.0);
    };
    double a1, a2, a5, b1, b2, b5;
    rig.h.set (A().wtpos, 1.0f);   // square: odd harmonics only
    harmonics (warp::OddEven, 1.0f, a1, a2, a5);
    CHECKV (a1 < 1e-3 && a5 < 1e-3, "odd/even 100%% removes odd: %g %g", a1, a5);
    harmonics (warp::OddEven, 0.0f, a1, a2, a5);
    CHECK (a1 > 0.5 && a2 < 1e-3);
    rig.h.set (A().wtpos, 0.0f);   // sine
    harmonics (warp::Sync, 0.0f, a1, a2, a5);
    harmonics (warp::Sync, 0.6f, b1, b2, b5);
    CHECKV (b5 > a5 * 100.0, "sync adds harmonics %g -> %g", a5, b5);
    rig.h.set (A().wtpos, 2.0f / 3.0f);   // saw
    harmonics (warp::Lpf, 0.0f, a1, a2, a5);
    harmonics (warp::Lpf, 1.0f, b1, b2, b5);
    CHECKV (b5 < a5 * 0.1 && b1 > a1 * 0.5, "lpf %g %g", b5 / a5, b1 / a1);
    harmonics (warp::Hpf, 0.6f, b1, b2, b5);
    CHECK (b1 < a1 * 0.1);
    rig.h.set (A().wtpos, 0.0f);
    harmonics (warp::SineFold, 1.0f, b1, b2, b5);
    CHECK (b5 > 0.01);
    harmonics (warp::Flip, 0.5f, b1, b2, b5);
    CHECK (b2 > 0.1);   // half-cycle flip of a sine -> even harmonics
    harmonics (warp::Pwm, 0.5f, b1, b2, b5);
    CHECK (b2 > 0.05);
    harmonics (warp::Quantize, 1.0f, b1, b2, b5);   // 4 steps: harmonics 3, 5, 7, ...
    CHECK (b5 > 0.05);
}

void testFm()
{
    std::printf ("[fm from a source buffer]\n");
    const int total = 96000;
    Sources src (total + 64);
    WtRig rig;
    src.attach (rig);
    const int n = 65536;
    auto sideband = [&] (int mode, float amt, int fmType)
    {
        rig.h.set (A().warp1Mode, (float) mode);
        rig.h.set (A().warp1, amt);
        rig.h.set (A().fmType, (float) fmType);
        Vec x = rig.mono (57.0f, total);   // carrier 220 Hz, modulator (B) 330 Hz
        auto p = powerSpectrum (x, x.size() - (size_t) n, n);
        return 10.0 * std::log10 ((bandPower (p, n, 545.0, 555.0) + 1e-30) / (bandPower (p, n, 215.0, 225.0) + bandPower (p, n, 105.0, 115.0) + 1e-30));
    };
    const double off = sideband (0, 0.0f, 0);
    std::printf ("  550 Hz sideband: off %.1f dB", off);
    CHECK (off < -80.0);
    for (int type = 0; type < 4; ++type)
    {
        const double on = sideband (warp::FmFirst + 1, 0.3f, type);
        std::printf (", %s %.1f dB", choices::fmTypes()[(size_t) type].c_str(), on);
        CHECK (on > -30.0);
    }
    std::printf ("\n");
    const double pd = sideband (warp::PdFirst + 1, 0.3f, 0);
    CHECKV (pd > -30.0, "pd %f", pd);
    const double am = sideband (warp::AmFirst + 1, 1.0f, 0);
    CHECKV (am > -20.0, "am %f", am);
    const double rm = sideband (warp::RmFirst + 1, 1.0f, 0);
    CHECKV (rm > -10.0, "rm %f", rm);
    // FM with a null source (module off) does nothing.
    rig.src[kSrcB] = nullptr;
    CHECK (sideband (warp::FmFirst + 1, 1.0f, 0) < -80.0);
}

void testDecimator()
{
    std::printf ("[half-band decimator]\n");
    const float* c = Decimator2x::coefficients();
    for (int i = 0; i < Decimator2x::kCoefs; ++i) CHECK (c[i] > 0.0f && c[i] < 1.0f);
    auto gainAt = [] (double hzHigh)   // tone at the 2x rate (96 kHz) -> level after decimation
    {
        Decimator2x d;
        const int n = 20000;
        Vec in ((size_t) n * 2), out ((size_t) n);
        for (int i = 0; i < n * 2; ++i) in[(size_t) i] = (float) std::sin (2.0 * 3.14159265358979323846 * hzHigh * i / (2.0 * SR));
        for (int k = 0; k < n; k += kTick) d.process (in.data() + 2 * k, out.data() + k, kTick);
        double m = 0;
        for (int i = n / 2; i < n; ++i) m = std::max (m, (double) std::abs (out[(size_t) i]));
        return 20.0 * std::log10 (m + 1e-12);
    };
    const double pass1 = gainAt (1000.0), pass2 = gainAt (19000.0), stop1 = gainAt (28500.0), stop2 = gainAt (40000.0);
    std::printf ("  1k %.3f dB, 19k %.3f dB, 28.5k %.1f dB, 40k %.1f dB\n", pass1, pass2, stop1, stop2);
    CHECK (std::abs (pass1) < 0.05 && std::abs (pass2) < 0.3 && stop1 < -60.0 && stop2 < -60.0);
}

void testSub()
{
    std::printf ("[sub]\n");
    Harness h;
    const SubIds& ids = tincture::ids().sub;
    h.set (ids.level, 1.0f);
    SubOsc sub;
    sub.prepare (h.ctx);
    SubOsc::Shared sh;
    auto run = [&] (float note, int total, bool retrig = true)
    {
        Vec L ((size_t) total), R ((size_t) total);
        OscTick t; t.p = &h.view; t.ctx = &h.ctx;
        t.pitchStart = t.pitchEnd = note;
        NoteInfo ni;
        if (retrig) sub.noteOn (ni, t, ids, sh);
        for (int off = 0; off < total; off += kTick)
        {
            t.n = std::min (kTick, total - off); t.outL = L.data() + off; t.outR = R.data() + off;
            sub.render (t, ids, sh);
        }
        for (int i = 0; i < total; ++i) L[(size_t) i] = 0.5f * (L[(size_t) i] + R[(size_t) i]);
        return L;
    };
    double f = zeroCrossFreq (run (69.0f, (int) SR * 4));
    CHECKV (std::abs (cents (f, 440.0)) < 0.1, "sub A4 %f", f);
    h.set (ids.oct, -2); h.set (ids.coarse, 0.5f);
    f = zeroCrossFreq (run (69.0f, (int) SR * 4));
    CHECKV (std::abs (cents (f, noteToHz (69.0f - 24.0f + 0.5f))) < 0.1, "sub oct/crs %f", f);
    h.set (ids.oct, 0); h.set (ids.coarse, 0.0f);

    const float note = hzToNote (100.0f);
    struct Expect { int shape; double r2, r3; };
    const Expect ex[] { { SubOsc::Sine, 0.0, 0.0 }, { SubOsc::Triangle, 0.0, 1.0 / 9.0 }, { SubOsc::Saw, 0.5, 1.0 / 3.0 },
                        { SubOsc::Square, 0.0, 1.0 / 3.0 } };
    for (const auto& e : ex)
    {
        h.set (ids.shape, (float) e.shape);
        Vec x = run (note, 480 * 40);
        Vec tail (x.begin() + 4800, x.end());
        const double a1 = amplitudeAt (tail, 100.0), a2 = amplitudeAt (tail, 200.0), a3 = amplitudeAt (tail, 300.0);
        CHECKV (std::abs (a2 / a1 - e.r2) < 0.01 && std::abs (a3 / a1 - e.r3) < 0.01, "shape %d: %f %f", e.shape, a2 / a1, a3 / a1);
    }
    for (int s = 0; s < 6; ++s)
    {
        h.set (ids.shape, (float) s);
        for (float hiNote : { 84.0f, 100.0f })
        {
            Vec x = run (hiNote, 80000);
            double worst, total;
            aliasLevels (x, noteToHz (hiNote), worst, total);
            float pk;
            CHECK (allFinite (x, 1.6f, pk));
            std::printf ("  shape %-12s note %.0f: worst alias %.1f dB, total %.1f dB\n", choices::subShapes()[(size_t) s].c_str(), hiNote, worst, total);
            CHECK (worst < -60.0);
        }
    }
    // Phase and Contiguous.
    h.set (ids.shape, 0.0f);
    h.set (ids.phase, 90.0f);
    Vec x = run (60.0f, 64);
    CHECK (std::abs (x[0] - 1.0f) < 0.01f);
    h.set (ids.contiguous, 1.0f);
    run (60.0f, 777);
    const float ph = sub.currentPhase();
    SubOsc other;
    other.prepare (h.ctx);
    OscTick t; t.p = &h.view;
    other.noteOn (NoteInfo(), t, ids, sh);
    CHECK (std::abs (other.currentPhase() - ph) < 1e-6f);
}

void testNoise()
{
    std::printf ("[noise]\n");
    Harness h;
    const NoiseIds& ids = tincture::ids().noise;
    h.set (ids.level, 1.0f);
    NoiseOsc noise;
    noise.prepare (h.ctx);
    NoiseOscData data;
    auto run = [&] (float note, int total, Vec& L, Vec& R)
    {
        L.assign ((size_t) total, 0.0f); R.assign ((size_t) total, 0.0f);
        OscTick t; t.p = &h.view; t.ctx = &h.ctx; t.pitchStart = t.pitchEnd = note;
        NoteInfo ni;
        noise.noteOn (ni, t, ids, data);
        for (int off = 0; off < total; off += kTick)
        {
            t.n = std::min (kTick, total - off); t.outL = L.data() + off; t.outR = R.data() + off;
            noise.render (t, ids, data);
        }
    };
    Vec L, R;
    const int n = 8192;
    struct C { int colour; double slope; const char* name; };
    for (C c : { C { NoiseOsc::White, 0.0, "white" }, C { NoiseOsc::Pink, -3.0, "pink" }, C { NoiseOsc::Brown, -6.0, "brown" } })
    {
        h.set (ids.colour, (float) c.colour);
        run (60.0f, (int) SR * 10, L, R);
        auto p = averagedSpectrum (L, n);
        const double s1 = slopeDbPerOct (p, n, 200.0, 3200.0), s2 = slopeDbPerOct (p, n, 400.0, 12800.0);
        std::printf ("  %s: %.2f / %.2f dB/oct\n", c.name, s1, s2);
        CHECKV (std::abs (s1 - c.slope) < 0.6 && std::abs (s2 - c.slope) < 0.6, "%s slope", c.name);
        float pk;
        CHECK (allFinite (L, 2.5f, pk));
    }
    // Stereo: 0 = identical channels, 1 = uncorrelated.
    h.set (ids.colour, (float) NoiseOsc::White);
    h.set (ids.stereo, 0.0f);
    run (60.0f, 4800, L, R);
    CHECK (L == R);
    h.set (ids.stereo, 1.0f);
    run (60.0f, 48000, L, R);
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < L.size(); ++i) { ab += L[i] * R[i]; aa += L[i] * L[i]; bb += R[i] * R[i]; }
    CHECK (std::abs (ab / std::sqrt (aa * bb)) < 0.05);
    // FILTER tilt: negative darkens, positive thins.
    auto hfRatio = [&] (float tilt)
    {
        h.set (ids.tilt, tilt);
        run (60.0f, 96000, L, R);
        auto p = averagedSpectrum (L, n);
        return bandPower (p, n, 5000.0, 15000.0) / bandPower (p, n, 50.0, 500.0);
    };
    const double flat = hfRatio (0.0f), dark = hfRatio (-0.8f), thin = hfRatio (0.8f);
    CHECK (dark < flat * 0.05 && thin > flat * 20.0);
    h.set (ids.tilt, 0.0f);
    // Geiger: sparse clicks.
    h.set (ids.colour, (float) NoiseOsc::Geiger);
    run (60.0f, 48000, L, R);
    int quiet = 0;
    for (float v : L) quiet += std::abs (v) < 1e-3f;
    CHECK (quiet > 40000 && quiet < 48000);

    // Sample playback pitch: a 440 Hz sine recorded at 44.1 kHz, root A4.
    auto s = std::make_shared<SampleData>();
    s->sampleRate = 44100.0; s->rootNote = 69;
    for (int i = 0; i < 44100 * 3; ++i) s->left.push_back ((float) std::sin (2.0 * 3.14159265358979323846 * 440.0 * i / 44100.0));
    data.sample = s;
    h.set (ids.colour, 0.0f);
    h.set (ids.oneShot, 1.0f);
    run (60.0f, (int) SR * 2, L, R);
    double f = zeroCrossFreq (L);
    CHECKV (std::abs (cents (f, 440.0)) < 0.1, "sample 440: %f", f);
    h.set (ids.pitch, 0.5f + 12.0f / 96.0f);   // +12 st
    h.set (ids.fine, 10.0f);
    run (60.0f, (int) SR, L, R);
    f = zeroCrossFreq (L);
    CHECKV (std::abs (cents (f, 880.0 * std::exp2 (10.0 / 1200.0))) < 0.1, "sample +12st +10c: %f", f);
    h.set (ids.pitch, 0.5f); h.set (ids.fine, 0.0f);
    h.set (ids.pitchTrack, 1.0f);
    run (76.0f, (int) SR, L, R);
    f = zeroCrossFreq (L);
    CHECKV (std::abs (cents (f, 440.0 * std::exp2 (7.0 / 12.0))) < 0.1, "keytrack: %f", f);
    h.set (ids.pitchTrack, 0.0f);
    // One-shot ends (3 s file, 4 s render); loop keeps going.
    run (60.0f, (int) SR * 4, L, R);
    CHECK (std::abs (L[(size_t) SR * 3 + 2000]) == 0.0f && std::abs (L[(size_t) SR * 2]) > 0.0f);
    h.set (ids.oneShot, 0.0f);
    run (60.0f, (int) SR * 4, L, R);
    double tailE = 0;
    for (size_t i = (size_t) SR * 3 + 2000; i < L.size(); ++i) tailE += L[i] * L[i];
    CHECK (tailE > 1000.0);
    // START: half way into a ramp sample.
    auto ramp = std::make_shared<SampleData>();
    ramp->sampleRate = SR;
    for (int i = 0; i < 1000; ++i) ramp->left.push_back ((float) i / 1000.0f);
    data.sample = ramp;
    h.set (ids.start, 0.5f);
    h.set (ids.oneShot, 1.0f);
    run (60.0f, 64, L, R);
    CHECKV (std::abs (L[1] - 0.501f) < 0.003f, "start: %f", L[1]);
    h.set (ids.start, 0.0f);
    // Factory noises: all build, finite, the loops are seamless (no big jump at the wrap).
    for (const auto& cat : FactoryNoises::categories())
        for (const auto& name : FactoryNoises::names (cat))
        {
            auto fs = FactoryNoises::get (cat, name);
            CHECK (fs && fs->frames() > 1000 && FactoryNoises::isFactory (*fs));
            if (! fs) continue;
            float pk = 0;
            CHECK (allFinite (fs->left, 1.0f, pk) && pk > 0.5f);
            if (cat != "Attacks")
            {
                float maxStep = 0;
                for (int i = 1; i < fs->frames(); ++i) maxStep = std::max (maxStep, std::abs (fs->left[(size_t) i] - fs->left[(size_t) i - 1]));
                const float wrapStep = std::abs (fs->left.front() - fs->left.back());
                CHECKV (wrapStep <= maxStep * 1.5f + 1e-3f, "%s loop seam %f vs %f", name.c_str(), wrapStep, maxStep);
            }
        }
    // Sample playback of a factory noise with a moving START (scratch) stays finite.
    data.sample = FactoryNoises::get ("Organics", "Surf");
    h.set (ids.oneShot, 0.0f);
    {
        OscTick t; t.p = &h.view; t.ctx = &h.ctx;
        float l[kTick], r[kTick];
        noise.noteOn (NoteInfo(), t, ids, data);
        bool ok = true;
        for (int k = 0; k < 4000; ++k)
        {
            h.set (ids.start, 0.5f + 0.5f * std::sin (k * 0.01f));
            h.set (ids.pitch, (float) (k % 100) / 100.0f);
            t.n = kTick; t.outL = l; t.outR = r;
            noise.render (t, ids, data);
            for (int i = 0; i < kTick; ++i) ok = ok && std::isfinite (l[i]) && std::abs (l[i]) < 4.0f;
        }
        CHECK (ok);
    }
}

// ---------------------------------------------------------------------------
// Benchmarks
// ---------------------------------------------------------------------------
void benchmark()
{
    std::printf ("[benchmark]\n");
    Sources src (kTick * 4);
    struct Case { const char* name; int voices, unison, w1, w2, os; };
    const Case cases[] {
        { "1 voice x 16 unison, no warp", 1, 16, 0, 0, 1 },
        { "1 voice x 16 unison, Bend+ & Tube", 1, 16, warp::BendPlus, warp::Tube, 1 },
        { "1 voice x 16 unison, Sync & FM(B)", 1, 16, warp::Sync, warp::FmFirst + 1, 1 },
        { "1 voice x 16 unison, Bend+ & Tube, 2x OS", 1, 16, warp::BendPlus, warp::Tube, 2 },
        { "8 voices x 7 unison, no warp", 8, 7, 0, 0, 1 },
        { "8 voices x 7 unison, Mirror & LPF", 8, 7, warp::Mirror, warp::Lpf, 1 },
    };
    for (const auto& c : cases)
    {
        std::vector<std::unique_ptr<WtRig>> rigs;
        auto shared = std::make_unique<WavetableOsc::Shared>();
        for (int v = 0; v < c.voices; ++v)
        {
            auto r = std::make_unique<WtRig>();
            src.attach (*r);
            r->h.ctx.oversample = c.os;
            r->h.set (A().unison, (float) c.unison);
            r->h.set (A().rand, 1.0f);
            r->h.set (A().wtpos, 0.4f);
            r->h.set (A().warp1Mode, (float) c.w1); r->h.set (A().warp1, 0.5f);
            r->h.set (A().warp2Mode, (float) c.w2); r->h.set (A().warp2, 0.5f);
            r->noteOn (48.0f + 3.0f * v);
            rigs.push_back (std::move (r));
        }
        float l[kTick], rr[kTick];
        const int ticks = 20000;
        auto t0 = std::chrono::steady_clock::now();
        float sink = 0.0f;
        for (int k = 0; k < ticks; ++k)
            for (auto& r : rigs)
            {
                OscTick t = r->makeTick (48.0f + 0.0001f * k, kTick, l, rr, 0);
                r->osc.render (t, A(), r->data, *shared);
                sink += l[3];
            }
        const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        const double samples = (double) ticks * kTick;
        const double nsPerSample = sec * 1e9 / samples;
        const double cpu = nsPerSample * 48000.0 / 1e9 * 100.0;
        std::printf ("  %-42s %7.1f ns/sample (%.2f ns per unison-voice sample)  %.2f%% CPU @48k%s\n", c.name, nsPerSample,
                     nsPerSample / (c.voices * c.unison), cpu, sink == 12345.0f ? "!" : "");
    }
    // Sub and noise.
    {
        Harness h;
        h.set (tincture::ids().sub.shape, (float) SubOsc::Saw);
        SubOsc sub; sub.prepare (h.ctx); SubOsc::Shared sh;
        NoiseOsc noise; noise.prepare (h.ctx); NoiseOscData nd;
        h.set (tincture::ids().noise.colour, (float) NoiseOsc::Pink);
        float l[kTick], r[kTick];
        OscTick t; t.p = &h.view; t.ctx = &h.ctx; t.outL = l; t.outR = r;
        sub.noteOn (NoteInfo(), t, tincture::ids().sub, sh);
        auto t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 100000; ++k) sub.render (t, tincture::ids().sub, sh);
        double ns = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() * 1e9 / (100000.0 * kTick);
        std::printf ("  %-42s %7.2f ns/sample\n", "sub (saw)", ns);
        t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 100000; ++k) noise.render (t, tincture::ids().noise, nd);
        ns = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() * 1e9 / (100000.0 * kTick);
        std::printf ("  %-42s %7.2f ns/sample\n", "noise (pink, stereo)", ns);
        nd.sample = FactoryNoises::get ("Analog", "Tape Hiss");
        h.set (tincture::ids().noise.colour, 0.0f);
        h.set (tincture::ids().noise.pitch, 0.53f);
        noise.noteOn (NoteInfo(), t, tincture::ids().noise, nd);
        t0 = std::chrono::steady_clock::now();
        for (int k = 0; k < 100000; ++k) noise.render (t, tincture::ids().noise, nd);
        ns = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count() * 1e9 / (100000.0 * kTick);
        std::printf ("  %-42s %7.2f ns/sample\n", "noise (stereo sample, hermite)", ns);
    }
}
} // namespace

int main()
{
    ScopedNoDenormals noDenormals;
    (void) params();
    testFft();
    testWavetableBuild();
    testSaveLoad();
    testWtPitch();
    testWtAlias();
    testWtPos();
    testUnison();
    testPhase();
    testDecimator();
    testWarps();
    testFm();
    testSub();
    testNoise();
    benchmark();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
