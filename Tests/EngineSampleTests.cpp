// EngineSampleTests.cpp — tests and benchmarks for the sample-based
// oscillator modes (SampleOsc: Multisample, Sample, Granular, Spectral).
// Build (see docs/dev/AGENTS.md) with -DTINCTURE_WARP_STUB while Warp.cpp
// isn't available; the stub below then stands in for the post warps.
#include "Engine/SampleOsc.h"
#include "Engine/ParamIds.h"

#include <chrono>
#include <ctime>
#include <cmath>
#include <cstdio>
#include <vector>

using namespace tincture;

#ifdef TINCTURE_WARP_STUB
// Minimal stand-ins for the three Warp.cpp functions SampleOsc calls.
namespace tincture::warp
{
Kind kindOf (int m) noexcept
{
    if (m <= Off) return Kind::None;
    if (m == Sync) return Kind::Sync;
    if (m < Lpf) return Kind::Phase;
    if (m <= Hpf) return Kind::Filter;
    if (m <= DistLast) return Kind::Distortion;
    if (m < PdFirst) return Kind::Fm;
    if (m < AmFirst) return Kind::Pd;
    if (m < RmFirst) return Kind::Am;
    return Kind::Rm;
}
bool isPost (int m) noexcept
{
    const Kind k = kindOf (m);
    return k == Kind::Filter || k == Kind::Distortion || k == Kind::Am || k == Kind::Rm;
}
void processPost (int mode, float amount, float* buf, int n, const float* src, PostState& st, float, float) noexcept
{
    const Kind k = kindOf (mode);
    for (int i = 0; i < n; ++i)
    {
        if (k == Kind::Distortion) buf[i] = std::tanh (buf[i] * (1.0f + 8.0f * amount));
        else if (k == Kind::Filter) { st.z1 += (buf[i] - st.z1) * (1.0f - 0.95f * amount); buf[i] = st.z1; }
        else if (src && (k == Kind::Am || k == Kind::Rm)) buf[i] *= 1.0f - amount + amount * src[i];
    }
}
} // namespace tincture::warp
#endif

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

constexpr double kSr = 48000.0;

// ---- Parameter rig -------------------------------------------------------
struct Rig
{
    ParamValues pv;
    std::vector<int16_t> slots;
    ParamView view;
    Context ctx;
    const OscIds& id;
    Rig() : id ((params(), ids().osc[0]))
    {
        pv.resetToDefaults();
        slots.assign ((size_t) params().size(), (int16_t) -1);
        view.base = pv.v.data();
        view.slot = slots.data();
        ctx.sampleRate = kSr;
        ctx.invSampleRate = (float) (1.0 / kSr);
        ctx.bpm = 120.0;
    }
    void set (int i, float v) { pv[i] = v; }
};

struct Out { std::vector<float> L, R; int maxUnison = 0; };

// Render `samples` of a note; optional note-off at `offAt`, and a per-tick hook.
template <typename Hook>
Out play (SampleOsc& osc, const SampleOscData& d, Rig& rig, int note, int samples, int offAt, Hook hook, float vel = 0.8f)
{
    Out o;
    o.L.resize ((size_t) samples); o.R.resize ((size_t) samples);
    OscTick t;
    t.p = &rig.view;
    t.ctx = &rig.ctx;
    t.pitchStart = t.pitchEnd = (float) note;
    float L[kTick], R[kTick], M[kTick];
    NoteInfo ni;
    ni.note = note; ni.velocity = vel; ni.unisonSeed = 1234;
    t.n = kTick; t.outL = L; t.outR = R; t.outMono = M;
    osc.noteOn (ni, t, rig.id, d);
    for (int pos = 0; pos < samples; pos += kTick)
    {
        t.n = std::min (kTick, samples - pos);
        if (offAt >= 0 && pos <= offAt && offAt < pos + kTick) osc.noteOff();
        hook (pos, t);
        osc.render (t, rig.id, d);
        o.maxUnison = std::max (o.maxUnison, t.activeUnison);
        for (int i = 0; i < t.n; ++i) { o.L[(size_t) (pos + i)] = L[i]; o.R[(size_t) (pos + i)] = R[i]; }
    }
    return o;
}
Out play (SampleOsc& osc, const SampleOscData& d, Rig& rig, int note, int samples, int offAt = -1)
{
    return play (osc, d, rig, note, samples, offAt, [] (int, OscTick&) {});
}

// ---- Signals ---------------------------------------------------------------
std::shared_ptr<SampleData> makeSample (int frames, double sr, int root, bool stereo = false)
{
    auto s = std::make_shared<SampleData>();
    s->name = "test";
    s->sampleRate = sr;
    s->rootNote = root;
    s->left.assign ((size_t) frames, 0.0f);
    if (stereo) s->right.assign ((size_t) frames, 0.0f);
    return s;
}
std::shared_ptr<SampleData> sine (double hz, double secs, double sr, int root, float amp = 0.5f, bool stereo = false)
{
    const int n = (int) (secs * sr);
    auto s = makeSample (n, sr, root, stereo);
    for (int i = 0; i < n; ++i)
    {
        s->left[(size_t) i] = amp * (float) std::sin (2.0 * 3.14159265358979323846 * hz * i / sr);
        if (stereo) s->right[(size_t) i] = s->left[(size_t) i];
    }
    return s;
}

double zcFreq (const std::vector<float>& x, int from, int to)
{
    double first = -1.0, last = -1.0;
    int count = 0;
    for (int i = std::max (1, from); i < to; ++i)
        if (x[(size_t) i - 1] < 0.0f && x[(size_t) i] >= 0.0f)
        {
            const double t = i - 1 + x[(size_t) i - 1] / (double) (x[(size_t) i - 1] - x[(size_t) i]);
            if (first < 0.0) first = t;
            last = t;
            ++count;
        }
    return count > 1 ? (count - 1) * kSr / (last - first) : 0.0;
}

double rms (const std::vector<float>& x, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += (double) x[(size_t) i] * x[(size_t) i];
    return std::sqrt (s / std::max (1, to - from));
}

// FFT peak frequency (Hann, parabolic interpolation) of x[from .. from+N).
double fftPeak (const std::vector<float>& x, int from, int N = 16384)
{
    SampleFft f;
    f.init (N);
    std::vector<float> re ((size_t) N), im ((size_t) N, 0.0f);
    for (int i = 0; i < N; ++i)
        re[(size_t) i] = x[(size_t) (from + i)] * (0.5f - 0.5f * (float) std::cos (2.0 * 3.14159265358979323846 * i / N));
    f.forward (re.data(), im.data());
    int best = 1; double bm = 0.0;
    std::vector<double> m ((size_t) N / 2);
    for (int k = 1; k < N / 2; ++k)
    {
        m[(size_t) k] = std::log (1e-12 + std::hypot (re[(size_t) k], im[(size_t) k]));
        if (m[(size_t) k] > bm || k == 1) { bm = m[(size_t) k]; best = k; }
    }
    double d = 0.0;
    if (best > 1 && best < N / 2 - 1)
    {
        const double a = m[(size_t) best - 1], b = m[(size_t) best], c = m[(size_t) best + 1];
        d = 0.5 * (a - c) / (a - 2 * b + c);
    }
    return (best + d) * kSr / N;
}

double cents (double f, double ref) { return 1200.0 * std::log2 (f / ref); }

bool allFinite (const Out& o)
{
    for (size_t i = 0; i < o.L.size(); ++i)
        if (! std::isfinite (o.L[i]) || ! std::isfinite (o.R[i]) || std::abs (o.L[i]) > 100.0f) return false;
    return true;
}

SampleOscData dataFor (int type, std::shared_ptr<SampleData> s)
{
    SampleOscData d;
    d.type = type;
    d.sample = s;
    d.rebuild();
    return d;
}

// ============================================================================
void testPitch()
{
    std::printf ("Sample pitch\n");
    Rig rig;
    rig.set (rig.id.level, 1.0f);
    SampleOsc osc;
    osc.prepare (rig.ctx);
    // 261.63 Hz sine rooted at C3 (60), 44.1 kHz source in a 48 kHz engine.
    auto d = dataFor (2, sine (noteToHz (60.0f), 2.0, 44100.0, 60));
    for (int note : { 60, 72, 67, 84 })
    {
        const Out o = play (osc, d, rig, note, 24000);
        const double f = zcFreq (o.L, 2000, 22000);
        const double c = cents (f, noteToHz ((float) note));
        CHECK (std::abs (c) < 0.5, "note %d: %.3f Hz, %.3f cents off", note, f, c);
        std::printf ("  note %d -> %.3f Hz (%.4f cents)\n", note, f, c);
    }
    // FIN / SEM / OCT apply.
    rig.set (rig.id.sem, 7.0f);
    rig.set (rig.id.fine, 50.0f);
    {
        const Out o = play (osc, d, rig, 60, 24000);
        const double c = cents (zcFreq (o.L, 2000, 22000), noteToHz (67.5f));
        CHECK (std::abs (c) < 0.5, "sem/fine: %.3f cents off", c);
    }
    rig.set (rig.id.sem, 0.0f);
    rig.set (rig.id.fine, 0.0f);

    // Mip levels: a 15 kHz sine played an octave up must not alias back in.
    auto hf = dataFor (2, sine (15000.0, 1.0, 48000.0, 60));
    const Out a0 = play (osc, hf, rig, 60, 9600);
    const Out a1 = play (osc, hf, rig, 72, 9600);
    const double r0 = rms (a0.L, 1000, 9000), r1 = rms (a1.L, 1000, 9000);
    const double rej = 20.0 * std::log10 (r1 / r0 + 1e-12);
    CHECK (rej < -40.0, "aliasing at +12 st only %.1f dB down", rej);
    std::printf ("  alias rejection, 15 kHz at +12 st: %.1f dB\n", rej);
    // A 3 kHz sine at +24 (level 2) keeps its level (passband).
    auto mf = dataFor (2, sine (2000.0, 1.0, 48000.0, 60));
    const Out b0 = play (osc, mf, rig, 60, 9600), b1 = play (osc, mf, rig, 84, 2400);
    const double pass = 20.0 * std::log10 (rms (b1.L, 200, 2000) / rms (b0.L, 1000, 9000));
    CHECK (std::abs (pass) < 0.5, "passband level at +24 st: %.2f dB", pass);
}

// Ramp sample: value = frame index / 4800, so the output shows the position.
void testLoops()
{
    std::printf ("Loop modes\n");
    Rig rig;
    rig.set (rig.id.level, 1.0f);
    rig.set (rig.id.loopStart, 0.25f);
    rig.set (rig.id.loopEnd, 0.5f);
    SampleOsc osc;
    osc.prepare (rig.ctx);
    constexpr int F = 4800;
    auto s = makeSample (F, kSr, 60);
    for (int i = 0; i < F; ++i) s->left[(size_t) i] = (float) i / F;
    auto d = dataFor (2, s);
    auto pos = [] (float v) { return (double) v * F; };

    // Reference model: positions for integer increment 1.
    auto expectAt = [&] (int mode, int i) -> double
    {
        const int ls = 1200, le = 2400, len = le - ls;
        if (mode == 0) return i < F ? i : -1;
        if (i < le) return i;
        const int k = i - le;
        switch (mode)
        {
            case 1: return ls + k % len;
            case 2: return le - 1 - (k % len);            // reverse: reflects at LE, wraps LS -> LE
            case 3: { const int c = k / len, u = k % len; return (c % 2 == 0) ? le - u : ls + u; }
            default: return -1;
        }
    };
    const char* names[] { "One-shot", "Fwd", "Rev", "Fwd/Rev" };
    for (int mode = 0; mode <= 3; ++mode)
    {
        rig.set (rig.id.loopMode, (float) mode);
        const Out o = play (osc, d, rig, 60, 9000);
        int bad = 0, firstBad = -1;
        for (int i = 0; i < 9000; ++i)
        {
            const double e = expectAt (mode, i);
            const double got = pos (o.L[(size_t) i]);
            const double err = e < 0 ? std::abs (o.L[(size_t) i]) * F : std::abs (got - e);
            // Reflections (Rev at LE, Fwd/Rev at both ends) land up to one frame off the integer grid.
            if (err > (mode >= 2 ? 1.01 : 0.01)) { ++bad; if (firstBad < 0) firstBad = i; }
        }
        CHECK (bad == 0, "%s: %d wrong positions (first at %d: got %.2f want %.2f)", names[mode], bad, firstBad,
               firstBad >= 0 ? pos (o.L[(size_t) firstBad]) : 0.0, firstBad >= 0 ? expectAt (mode, firstBad) : 0.0);
        std::printf ("  %-9s ok=%s\n", names[mode], bad == 0 ? "yes" : "NO");
    }
    // Tailed: loops [half, end].
    rig.set (rig.id.loopMode, 4.0f);
    {
        const Out o = play (osc, d, rig, 60, 12000);
        const double p = pos (o.L[(size_t) (F + 100)]);
        CHECK (std::abs (p - (2400 + 100)) < 0.01, "tailed: %.2f", p);
    }
    // LE before LS: a forward loop plays reversed.
    rig.set (rig.id.loopMode, 1.0f);
    rig.set (rig.id.loopStart, 0.5f);
    rig.set (rig.id.loopEnd, 0.25f);
    {
        const Out o = play (osc, d, rig, 60, 6000);
        const double p = pos (o.L[(size_t) (2400 + 10)]);
        CHECK (std::abs (p - (2400 - 1 - 10)) < 1.01, "LE<LS reversed: %.2f", p);
    }
    rig.set (rig.id.loopStart, 0.25f);
    rig.set (rig.id.loopEnd, 0.5f);
    // Exit Loop on Release: after note-off, playback leaves the loop and runs to the end.
    d.exitLoopOnRelease = true;
    {
        const Out o = play (osc, d, rig, 60, 9000, 3000);
        // Released in the tick at 2976: the loop was at 1200 + (2976-2400) = 1776, and playback
        // carries on forward past LE (2400) instead of wrapping.
        const double p = pos (o.L[(size_t) 4000]);
        CHECK (std::abs (p - (1776 + 1024)) < 0.01, "exit loop: pos %.1f after release", p);
        CHECK (o.L[(size_t) 8900] == 0.0f, "exit loop: plays to the end then stops");
        CHECK (osc.finished(), "exit loop: finished");
    }
    d.exitLoopOnRelease = false;
    // Relative Loop: the loop moves with the start marker.
    d.relativeLoop = true;
    rig.set (rig.id.start, 0.5f);
    {
        const Out o = play (osc, d, rig, 60, 6000);
        // Region 2400..4800, loop 3000..3600.
        const double p = pos (o.L[(size_t) 1300]);    // 2400+1300 = 3700 -> wraps to 3100
        CHECK (std::abs (p - 3100.0) < 0.01, "relative loop: %.2f", p);
    }
    d.relativeLoop = false;
    rig.set (rig.id.start, 0.0f);
    // Link Loop Length: modulating LS carries LE.
    d.linkLoopLength = true;
    {
        rig.pv[rig.id.loopStart] = 0.25f; rig.pv[rig.id.loopEnd] = 0.5f;
        std::vector<float> modded { 0.4f };                     // modulated LS
        rig.slots[(size_t) rig.id.loopStart] = 0;
        rig.view.modded = modded.data();
        rig.view.moddedPrev = modded.data();
        const Out o = play (osc, d, rig, 60, 6000);
        // Loop 1920..3120: at i = 3120 + 5 -> 1925.
        const double p = pos (o.L[(size_t) 3125]);
        CHECK (std::abs (p - 1925.0) < 0.01, "link loop length: %.2f", p);
        rig.slots[(size_t) rig.id.loopStart] = -1;
    }
    d.linkLoopLength = false;

    // SCAN: 50 % halves the rate (Sample mode resamples).
    rig.set (rig.id.loopMode, 0.0f);
    rig.set (rig.id.scan, 0.5f);
    {
        const Out o = play (osc, d, rig, 60, 2000);
        CHECK (std::abs (pos (o.L[1000]) - 500.0) < 0.01, "scan 50 %%: %.2f", pos (o.L[1000]));
    }
    rig.set (rig.id.scan, -1.0f);           // backwards from the end marker
    {
        const Out o = play (osc, d, rig, 60, 2000);
        CHECK (std::abs (pos (o.L[1000]) - (F - 1000.001)) < 0.05, "scan -100 %%: %.2f", pos (o.L[1000]));
    }
    rig.set (rig.id.scan, 1.0f);

    // Loop crossfade: a sine loop with a seam mid-cycle gets a smooth join.
    auto sn = dataFor (2, sine (220.0, 1.0, kSr, 60));
    rig.set (rig.id.loopMode, 1.0f);
    rig.set (rig.id.loopStart, 0.3f);
    rig.set (rig.id.loopEnd, 0.3123f);
    auto maxJump = [&] (const Out& o) { float m = 0.0f; for (int i = 16000; i < 40000; ++i) m = std::max (m, std::abs (o.L[(size_t) i] - o.L[(size_t) i - 1])); return m; };
    rig.set (rig.id.xfade, 0.0f);
    const float j0 = maxJump (play (osc, sn, rig, 60, 40000));
    rig.set (rig.id.xfade, 0.5f);
    const float j1 = maxJump (play (osc, sn, rig, 60, 40000));
    const float smooth = 0.5f * 2.0f * 3.14159f * 220.0f / 48000.0f;         // max slope of the sine
    CHECK (j0 > 3.0f * smooth, "seam expected without crossfade (%.4f)", j0);
    CHECK (j1 < 1.6f * smooth, "crossfade seam jump %.4f (sine slope %.4f)", j1, smooth);
    std::printf ("  crossfade: seam jump %.4f -> %.4f (sine slope %.4f)\n", j0, j1, smooth);

    // Unison SPAN offsets the start positions evenly.
    rig.set (rig.id.xfade, 0.0f);
    rig.set (rig.id.loopMode, 0.0f);
    rig.set (rig.id.unison, 4.0f);
    rig.set (rig.id.detune, 0.0f);
    rig.set (rig.id.uniSpan, 1.0f);
    {
        const Out o = play (osc, d, rig, 60, 64);
        const auto& t = osc.telemetry();
        bool okSpan = t.numPlayheads.load() == 4;
        for (int u = 0; u < 4; ++u)
            okSpan = okSpan && std::abs (t.playhead[u].load() - (0.25f * u + 64.0f / F)) < 1e-3f;
        CHECK (okSpan && o.maxUnison == 4, "unison span start offsets");
    }
    rig.set (rig.id.unison, 1.0f);
    rig.set (rig.id.uniSpan, 0.0f);
}

// Click track: decaying noise bursts at known positions.
void testSlices()
{
    std::printf ("Slicing\n");
    const int F = 48000 * 3;
    auto s = makeSample (F, kSr, 60);
    std::vector<int> at { 4800, 16000, 30000, 41000, 60000, 77777, 90000, 120000 };
    Rng r (7);
    for (size_t k = 0; k < at.size(); ++k)
    {
        const float amp = (k % 2 == 0) ? 0.9f : 0.3f;
        for (int i = 0; i < 960; ++i)
            s->left[(size_t) (at[k] + i)] += amp * r.bi() * std::exp (-(float) i / 150.0f);
    }
    auto sl = SampleOscData::detectSlices (*s, 0.1f);
    bool ok = sl.size() == at.size() + 1 && sl[0] == 0;
    double worst = 0.0;
    for (size_t k = 0; ok && k < at.size(); ++k) worst = std::max (worst, std::abs (sl[k + 1] - at[k]) * 1000.0 / kSr);
    CHECK (ok && worst < 2.0, "slices: %zu found (want %zu), worst error %.2f ms", sl.size(), at.size() + 1, worst);
    std::printf ("  threshold 0.10: %zu slices, worst onset error %.3f ms\n", sl.size(), worst);
    auto loud = SampleOscData::detectSlices (*s, 0.6f);
    CHECK (loud.size() == 5, "threshold 0.6 keeps the 4 loud clicks (+ slice 1), got %zu", loud.size());

    // Slice playback: slice root C3, note D3 plays slice 3 (starting at click 2).
    Rig rig;
    rig.set (rig.id.level, 1.0f);
    SampleOsc osc;
    osc.prepare (rig.ctx);
    SampleOscData d;
    d.type = 2; d.sample = s; d.slicing = SampleOscData::SliceAuto; d.sliceThreshold = 0.1f; d.sliceRoot = 60;
    d.rebuild();
    {
        const Out o = play (osc, d, rig, 62, 20000);
        const int start = d.prepared->slices[2];
        float err = 0.0f;
        for (int i = 0; i < 2000; ++i) err = std::max (err, std::abs (o.L[(size_t) i] - s->left[(size_t) (start + i)]));
        CHECK (err < 1e-4f, "slice 3 audio mismatch %.6f", err);
        // Stops at the next slice.
        const int len = d.prepared->slices[3] - start;
        CHECK (o.L[(size_t) len + 64] == 0.0f && osc.finished(), "slice stops at next marker");
        CHECK (osc.telemetry().slice.load() == 2, "telemetry slice");
    }
    {
        const Out o = play (osc, d, rig, 59, 1000);       // below slice 1: silent
        CHECK (rms (o.L, 0, 1000) == 0.0, "note below root is silent");
    }
    // Play Single Slice: every note plays slice 2; the rate follows the note (an octave up = half the length).
    d.playSingleSlice = true; d.singleSlice = 1;
    {
        const int len = d.prepared->slices[2] - d.prepared->slices[1];
        const Out o = play (osc, d, rig, 72, 20000);
        int last = 0;
        for (int i = 0; i < 20000; ++i) if (o.L[(size_t) i] != 0.0f) last = i;
        CHECK (std::abs (last - len / 2) <= 2 && osc.telemetry().slice.load() == 1, "single slice at +12: ends at %d (want %d)", last, len / 2);
    }
}

void testGranular()
{
    std::printf ("Granular\n");
    Rig rig;
    rig.set (rig.id.level, 1.0f);
    SampleOsc osc;
    osc.prepare (rig.ctx);
    // DC sample: each grain is its window shape.
    auto s = makeSample (48000 * 4, kSr, 60);
    std::fill (s->left.begin(), s->left.end(), 1.0f);
    auto d = dataFor (3, s);
    d.loopGrains = false;
    rig.set (rig.id.density, 100.0f);
    rig.set (rig.id.grainLen, 0.05f);
    rig.set (rig.id.scan, 0.5f);
    {
        double sumGrains = 0.0; int ticks = 0;
        const Out o = play (osc, d, rig, 60, 48000, -1, [&] (int pos, OscTick&) { if (pos > 9600) { sumGrains += osc.activeGrains(); ++ticks; } });
        // mid-sum of L + R with centre pan = 2 * mean overlap * mean(Hann)
        double mean = 0.0;
        for (int i = 9600; i < 48000; ++i) mean += o.L[(size_t) i];
        mean /= (48000 - 9600);
        const double avgGrains = sumGrains / ticks;
        CHECK (std::abs (avgGrains - 5.0) < 0.3, "avg concurrent grains %.3f (want 5)", avgGrains);
        CHECK (std::abs (mean - 2.5) < 0.1, "mean level %.3f (want 2.5)", mean);
        std::printf ("  100 grains/s x 50 ms: %.3f concurrent, mean %.3f\n", avgGrains, mean);
        CHECK (allFinite (o), "granular finite");
    }
    // Grain length: sparse grains, measure burst durations.
    rig.set (rig.id.density, 10.0f);
    rig.set (rig.id.grainLen, 0.03f);
    rig.set (rig.id.winAmt, 0.0f);       // rectangular
    {
        const Out o = play (osc, d, rig, 60, 48000);
        std::vector<int> lens;
        int run = 0;
        for (int i = 0; i < 48000; ++i)
        {
            if (o.L[(size_t) i] > 0.5f) ++run;
            else if (run) { lens.push_back (run); run = 0; }
        }
        bool ok = lens.size() >= 9;
        for (int l : lens) ok = ok && std::abs (l - 1440) <= 1;
        CHECK (ok, "grain lengths (n=%zu, first %d)", lens.size(), lens.empty() ? 0 : lens[0]);
        // Spawn interval: 10 Hz -> 4800 samples apart.
    }
    // Length randomisation: mean preserved, spread present.
    rig.set (rig.id.grLenRand, 0.8f);
    {
        const Out o = play (osc, d, rig, 60, 48000 * 4);
        std::vector<int> lens;
        int run = 0;
        for (int i = 0; i < 48000 * 4; ++i)
        {
            if (o.L[(size_t) i] > 0.5f) ++run;
            else if (run) { lens.push_back (run); run = 0; }
        }
        double m = 0.0, v = 0.0;
        for (int l : lens) m += l;
        m /= std::max<size_t> (1, lens.size());
        for (int l : lens) v += (l - m) * (l - m);
        const double sd = std::sqrt (v / std::max<size_t> (1, lens.size()));
        // Uniform +/-76 %: sd = 0.76 / sqrt(3) * 1440 = 632.
        CHECK (std::abs (m - 1440.0) < 150.0 && sd > 450.0 && sd < 800.0, "rand length mean %.0f sd %.0f", m, sd);
        std::printf ("  length rand 80 %%: mean %.0f sd %.0f samples (n=%zu)\n", m, sd, lens.size());
    }
    rig.set (rig.id.grLenRand, 0.0f);
    rig.set (rig.id.winAmt, 1.0f);
    // Max Grains cap.
    rig.set (rig.id.density, 400.0f);
    rig.set (rig.id.grainLen, 2.0f);
    d.maxGrains = 64;
    {
        int peak = 0;
        play (osc, d, rig, 60, 48000, -1, [&] (int, OscTick&) { peak = std::max (peak, osc.activeGrains()); });
        CHECK (peak == 64, "max grains cap: peak %d", peak);
    }
    // Grains mode: N overlapping grains.
    d.maxGrains = 128;
    d.densityMode = SampleOscData::DensGrains;
    rig.set (rig.id.density, 8.0f);
    rig.set (rig.id.grainLen, 0.1f);
    {
        double sum = 0.0; int ticks = 0;
        play (osc, d, rig, 60, 48000, -1, [&] (int pos, OscTick&) { if (pos > 9600) { sum += osc.activeGrains(); ++ticks; } });
        CHECK (std::abs (sum / ticks - 8.0) < 0.3, "grains mode: %.2f concurrent (want 8)", sum / ticks);
    }
    d.densityMode = SampleOscData::DensFree;
    // Pitch inside a grain follows the note (+12), measured on isolated rectangular grains.
    auto sn = dataFor (3, sine (noteToHz (60.0f), 4.0, kSr, 60));
    rig.set (rig.id.density, 4.0f);
    rig.set (rig.id.grainLen, 0.2f);
    rig.set (rig.id.winAmt, 0.0f);
    rig.set (rig.id.scan, 1.0f);
    {
        const Out o = play (osc, sn, rig, 72, 24000);
        const double c = cents (zcFreq (o.L, 200, 9400), noteToHz (72.0f));
        CHECK (std::abs (c) < 0.5, "granular pitch %.3f cents", c);
    }
    rig.set (rig.id.winAmt, 1.0f);
    // Jump start fills the cloud at once.
    sn.jumpStart = true;
    rig.set (rig.id.density, 100.0f);
    rig.set (rig.id.grainLen, 0.1f);
    {
        int first = 0;
        play (osc, sn, rig, 60, 64, -1, [&] (int pos, OscTick&) { if (pos == 32) first = osc.activeGrains(); });
        CHECK (first >= 9, "jump start: %d grains after one tick", first);
    }
    // One-shot: the playhead reaches the end and the voice finishes.
    sn.jumpStart = false;
    rig.set (rig.id.scan, 2.0f);
    {
        play (osc, sn, rig, 60, 48000 * 3);
        CHECK (osc.finished(), "granular one-shot finishes");
    }
    rig.set (rig.id.scan, 1.0f);
}

void testSpectral()
{
    std::printf ("Spectral\n");
    Rig rig;
    rig.set (rig.id.level, 1.0f);
    SampleOsc osc;
    osc.prepare (rig.ctx);
    auto src = sine (440.0, 3.0, kSr, 69, 0.5f);
    SampleOscData d = dataFor (4, src);
    const double inRms = 0.5 / std::sqrt (2.0);
    for (int pl = 0; pl < 2; ++pl)
    {
        d.phaseLock = pl == 1;
        for (int note : { 69, 81, 64, 57 })
        {
            const Out o = play (osc, d, rig, note, 48000);
            const double f = fftPeak (o.L, 8000);
            const double c = cents (f, noteToHz ((float) note));
            const double lv = 20.0 * std::log10 (rms (o.L, 4000, 40000) / inRms);
            CHECK (std::abs (c) < 3.0, "spectral note %d lock %d: %.2f Hz (%.2f cents)", note, pl, f, c);
            CHECK (std::abs (lv) < 1.0, "spectral note %d lock %d: level %.2f dB", note, pl, lv);
            std::printf ("  note %d lock %d: %.2f Hz (%+.2f cents), level %+.2f dB\n", note, pl, f, c, lv);
        }
    }
    // Sweep -12..+12 in semitone and quarter-tone steps: worst pitch / level error.
    {
        double worstC = 0.0, worstL = 0.0;
        for (int pl = 0; pl < 2; ++pl)
        {
            d.phaseLock = pl == 1;
            for (int q = -48; q <= 48; q += 2)
            {
                const float note = 69.0f + q * 0.25f;
                const Out o = play (osc, d, rig, 69, 24000, -1, [&] (int, OscTick& t) { t.pitchStart = t.pitchEnd = note; });
                worstC = std::max (worstC, std::abs (cents (fftPeak (o.L, 4000, 16384), noteToHz (note))));
                worstL = std::max (worstL, std::abs (20.0 * std::log10 (rms (o.L, 4000, 23000) / inRms)));
            }
        }
        CHECK (worstC < 3.0 && worstL < 1.5, "sweep: worst %.2f cents, %.2f dB", worstC, worstL);
        std::printf ("  sweep -12..+12 st (quarter tones, both lock modes): worst %.2f cents, %.2f dB\n", worstC, worstL);
    }
    d.phaseLock = false;
    // The first samples are already at level (pre-roll).
    {
        const Out o = play (osc, d, rig, 69, 4800);
        const double lv = 20.0 * std::log10 (rms (o.L, 0, 480) / inRms);
        CHECK (std::abs (lv) < 1.5, "onset level %.2f dB", lv);
    }
    // SCAN 25 %: time-stretched, same pitch.
    rig.set (rig.id.scan, 0.25f);
    {
        const Out o = play (osc, d, rig, 69, 48000);
        CHECK (std::abs (cents (fftPeak (o.L, 8000), 440.0)) < 3.0, "scan 25 %% keeps pitch");
    }
    rig.set (rig.id.scan, 1.0f);
    // Band limits: HI under the tone mutes it; LO under keeps it.
    rig.set (rig.id.specHi, 300.0f);
    {
        const Out o = play (osc, d, rig, 69, 24000);
        const double lv = 20.0 * std::log10 (rms (o.L, 4000, 20000) / inRms + 1e-12);
        CHECK (lv < -40.0, "HI 300 Hz mutes 440 Hz: %.1f dB", lv);
    }
    rig.set (rig.id.specHi, 20000.0f);
    // Spectral filter curve at 0 (gain 0) with MIX 100 % mutes; MIX 0 passes.
    d.specFilter = Curve::line (0.0f, 0.0f);
    d.rebuild();
    {
        const Out o = play (osc, d, rig, 69, 24000);
        CHECK (rms (o.L, 4000, 20000) < inRms * 0.01, "filter curve 0 mutes");
        rig.set (rig.id.specMix, 0.0f);
        const Out o2 = play (osc, d, rig, 69, 24000);
        CHECK (std::abs (20.0 * std::log10 (rms (o2.L, 4000, 20000) / inRms)) < 1.0, "MIX 0 bypasses the filter");
        rig.set (rig.id.specMix, 1.0f);
    }
    d.specFilter = Curve::line (0.5f, 0.5f);
    d.rebuild();
    // One-shot end -> finished after the tail.
    {
        play (osc, d, rig, 69, 48000 * 4);
        CHECK (osc.finished(), "spectral one-shot finishes");
    }
    // Transients option on a click track doesn't blow up and keeps clicks sharp-ish.
    auto clicks = makeSample (48000, kSr, 60);
    for (int k = 0; k < 4; ++k) clicks->left[(size_t) (6000 + k * 12000)] = 1.0f;
    SampleOscData dc = dataFor (4, clicks);
    dc.transients = true;
    {
        const Out o = play (osc, dc, rig, 60, 48000);
        CHECK (allFinite (o), "transients finite");
        int peakAt = 0; float pk = 0.0f;
        for (int i = 4000; i < 10000; ++i) if (std::abs (o.L[(size_t) i]) > pk) { pk = std::abs (o.L[(size_t) i]); peakAt = i; }
        CHECK (std::abs (peakAt - 6000) < 64, "click position kept (%d)", peakAt);
    }
}

void testMultisample()
{
    std::printf ("Multisample\n");
    std::vector<std::pair<std::string, std::shared_ptr<const SampleData>>> files;
    for (int oct = 2; oct <= 4; ++oct)
    {
        const int root = (oct + 2) * 12;
        files.push_back ({ "Kalimba C" + std::to_string (oct) + ".wav", sine (noteToHz ((float) root), 1.0, 48000.0, 60) });
    }
    files.push_back ({ "Kalimba C3 v2.wav", sine (noteToHz (60.0f), 1.0, 48000.0, 60, 0.9f) });
    files.push_back ({ "Kalimba C3 v1.wav", sine (noteToHz (60.0f), 1.0, 48000.0, 60, 0.2f) });
    auto ms = std::make_shared<MultiSample> (MultiSample::fromFiles ("Kalimba", files));
    CHECK (ms->zones.size() == 5, "zones %zu", ms->zones.size());
    // C2 zone covers up to (48+60)/2 = 54; C3 55..66; C4 67..127.
    const int z54 = ms->findZone (54, 100), z55 = ms->findZone (55, 100), z70 = ms->findZone (70, 100);
    CHECK (ms->zones[(size_t) z54].root == 48 && ms->zones[(size_t) z55].root == 60 && ms->zones[(size_t) z70].root == 72, "key ranges");
    // Velocity layers at C3: v1 below v2 (named layer), plain C3 (peak sort) after numbers.
    const int soft = ms->findZone (60, 10), hard = ms->findZone (60, 127);
    CHECK (soft != hard && ms->zones[(size_t) soft].sample->left[100] != ms->zones[(size_t) hard].sample->left[100], "velocity layers");

    Rig rig;
    rig.set (rig.id.level, 1.0f);
    SampleOsc osc;
    osc.prepare (rig.ctx);
    SampleOscData d;
    d.type = 1;
    d.multi = ms;
    d.rebuild();
    const Out o = play (osc, d, rig, 64, 24000);
    const double c = cents (zcFreq (o.L, 2000, 22000), noteToHz (64.0f));
    CHECK (std::abs (c) < 0.5, "multisample pitch %.3f cents", c);
    CHECK (osc.telemetry().zone.load() == ms->findZone (64, (int) std::lround (0.8f * 127)), "zone telemetry");
    // TIMBRE shifts the zone choice down while pitch stays.
    rig.set (rig.id.timbre, 0.5f);              // -12 st lookup
    play (osc, d, rig, 64, 64);
    CHECK (ms->zones[(size_t) osc.telemetry().zone.load()].root == 48, "timbre picks the lower zone");
    rig.set (rig.id.timbre, 0.0f);
    // Envelope override: attack 10 ms, release 10 ms.
    d.envOverride = true;
    d.env.attack = 0.01f; d.env.decay = 0.0f; d.env.sustain = 1.0f; d.env.release = 0.01f;
    {
        const Out e = play (osc, d, rig, 60, 9600, 4800);
        const double early = rms (e.L, 0, 96), mid = rms (e.L, 2000, 4000), after = rms (e.L, 5800, 9600);
        CHECK (early < mid * 0.2 && after == 0.0 && osc.finished(), "override env: %.3f %.3f %.3f", early, mid, after);
    }
    d.envOverride = false;
    // Vel track.
    d.velTrack = true;
    {
        const Out lo = play (osc, d, rig, 72, 4800, -1, [] (int, OscTick&) {}, 0.25f);
        const Out hi = play (osc, d, rig, 72, 4800, -1, [] (int, OscTick&) {}, 1.0f);
        const double ratio = rms (lo.L, 100, 4000) / rms (hi.L, 100, 4000);
        CHECK (std::abs (ratio - 0.25) < 0.01, "vel track ratio %.3f", ratio);
    }
}

void testWavetable()
{
    std::printf ("Switch to Wavetable\n");
    auto s = sine (220.0, 1.0, 48000.0, 60);
    const float f0 = SampleOscData::estimatePitch (*s);
    CHECK (std::abs (cents (f0, 220.0)) < 2.0, "pitch estimate %.2f", f0);
    for (int mode = 0; mode < 8; ++mode)
    {
        auto frames = SampleOscData::toWavetable (*s, mode);
        bool ok = ! frames.empty() && frames.size() <= 256;
        for (auto& f : frames) ok = ok && f.size() == (size_t) SampleOscData::kWtFrameSize;
        double fundamental = 0.0;
        if (ok && mode < 4)
        {
            // A single cycle: energy concentrated in harmonic 1.
            SampleFft fft;
            fft.init (2048);
            std::vector<float> re (frames[frames.size() / 2]), im (2048, 0.0f);
            fft.forward (re.data(), im.data());
            double e1 = std::hypot (re[1], im[1]), rest = 0.0;
            for (int k = 2; k < 1024; ++k) rest += std::hypot (re[(size_t) k], im[(size_t) k]);
            fundamental = e1 / (e1 + rest);
            ok = fundamental > 0.9;
        }
        CHECK (ok, "import %s: %zu frames, fundamental share %.3f", SampleOscData::wavetableImportChoices()[(size_t) mode].c_str(), frames.size(), fundamental);
        std::printf ("  %-34s %3zu frames%s\n", SampleOscData::wavetableImportChoices()[(size_t) mode].c_str(), frames.size(),
                     mode < 4 ? (fundamental > 0.9 ? "  (single cycles)" : "") : "");
    }
    // 48000 samples / 2048 = 23.4: the final part-frame under half a frame is dropped.
    CHECK (SampleOscData::toWavetable (*s, SampleOscData::Fft2048).size() == 23, "FFT 2048 frame count");
}

void testSaveLoad()
{
    std::printf ("Save / load\n");
    SampleOscData d;
    d.type = 3;
    auto s = sine (300.0, 0.1, 44100.0, 57, 0.4f, true);
    s->embedded = true;
    d.sample = s;
    d.rootNote = 62;
    d.edits = { { SampleEdit::Normalize, 0.0f, 1.0f }, { SampleEdit::Trim, 0.1f, 0.9f }, { SampleEdit::Reverse, 0.0f, 1.0f } };
    d.fadeEdges = 3;
    d.showMarkerAnimation = true; d.snap = 2; d.slicing = 2; d.manualSlices = { 0.2f, 0.5f };
    d.sliceThreshold = 0.4f; d.playSliceToEnd = true; d.playSingleSlice = true; d.singleSlice = 3; d.sliceRoot = 36;
    d.manualLoop = true; d.relativeLoop = true; d.linkLoopLength = true; d.exitLoopOnRelease = true; d.loopGrains = true;
    d.scanRange = 2; d.scanReverse = true; d.scanKeyTrack = true; d.scanTempoLock = true; d.lengthToBpm = true; d.lengthBeats = 8.0f;
    d.densityMode = 2; d.densTriplet = true; d.jumpStart = true; d.maxGrains = 32; d.lengthMode = 1; d.lenDotted = true;
    d.reverseGrains = true; d.spawnPattern = 3; d.randWarp1 = 0.3f; d.showXY = true; d.yTargetGranular = 5; d.yTargetSpectral = 6;
    d.yValue = 0.7f; d.phaseLock = true; d.transients = true; d.specSmooth = true; d.specPostWarp = true;
    d.specFilter = Curve::triangle(); d.specFilterTable = "Vowel/AEIOU";
    d.env.attack = 0.2f; d.env.release = 1.5f; d.env.delaySync = true; d.envOverride = true;
    d.velTrack = true; d.velTrackAmount = 0.6f; d.phaseRand = 0.25f;
    std::vector<std::pair<std::string, std::shared_ptr<const SampleData>>> files { { "x C3.wav", s } };
    d.multi = std::make_shared<MultiSample> (MultiSample::fromFiles ("Inst", files));

    Node n ("osc");
    d.save (n);
    const auto bin = n.toBinary();
    Node back;
    CHECK (Node::fromBinary (bin.data(), bin.size(), back), "binary round trip");
    SampleOscData e;
    e.load (back);
    bool same = e.type == d.type && e.rootNote == d.rootNote && e.edits == d.edits && e.fadeEdges == d.fadeEdges
        && e.showMarkerAnimation && e.snap == 2 && e.slicing == 2 && e.manualSlices == d.manualSlices
        && e.sliceThreshold == d.sliceThreshold && e.playSliceToEnd && e.playSingleSlice && e.singleSlice == 3 && e.sliceRoot == 36
        && e.manualLoop && e.relativeLoop && e.linkLoopLength && e.exitLoopOnRelease && e.loopGrains
        && e.scanRange == 2 && e.scanReverse && e.scanKeyTrack && e.scanTempoLock && e.lengthToBpm && e.lengthBeats == 8.0f
        && e.densityMode == 2 && e.densTriplet && e.jumpStart && e.maxGrains == 32 && e.lengthMode == 1 && e.lenDotted
        && e.reverseGrains && e.spawnPattern == 3 && e.randWarp1 == d.randWarp1 && e.showXY && e.yTargetGranular == 5
        && e.yTargetSpectral == 6 && e.yValue == d.yValue && e.phaseLock && e.transients && e.specSmooth && e.specPostWarp
        && e.specFilter == d.specFilter && e.specFilterTable == d.specFilterTable && e.env == d.env && e.envOverride
        && e.velTrack && e.velTrackAmount == d.velTrackAmount && e.phaseRand == d.phaseRand;
    CHECK (same, "fields round trip");
    CHECK (e.sample && e.sample->left == s->left && e.sample->right == s->right && e.sample->sampleRate == 44100.0
           && e.sample->rootNote == 57, "embedded sample round trip");
    CHECK (e.multi && e.multi->zones.size() == 1 && e.multi->zones[0].root == 60 && e.multi->zones[0].sample
           && e.multi->zones[0].sample->left == s->left, "multisample round trip");
    // Defaults on an empty node.
    SampleOscData f;
    f.load (Node ("osc"));
    CHECK (f.type == 2 && f.maxGrains == 128 && ! f.sample && f.specFilter == Curve::line (0.5f, 0.5f), "defaults from empty node");
    // Rebuild applies the edits: normalize -> trim 80 % -> reverse.
    e.rebuild (true);
    float pk = 0.0f;
    for (float v : e.processed->left) pk = std::max (pk, std::abs (v));
    CHECK (e.processed->frames() == (int) std::lround (0.8 * s->frames()) && std::abs (pk - 1.0f) < 1e-3f, "edits applied (%d frames, peak %.3f)", e.processed->frames(), pk);
    CHECK (e.spectral && e.prepared && e.zones && e.zones->size() == 1, "derived data built");
}

// Random parameter values in every mode: output must stay finite.
void testRobustness()
{
    std::printf ("Robustness (random params, all modes)\n");
    Rig rig;
    SampleOsc osc;
    osc.prepare (rig.ctx);
    auto s = sine (330.0, 1.0, 44100.0, 60, 0.8f, true);
    for (int i = 0; i < s->frames(); i += 997) s->left[(size_t) i] = 1.0f;
    SampleOscData base;
    base.sample = s;
    base.multi = std::make_shared<MultiSample> (MultiSample::fromFiles ("x", { { "a C3.wav", s } }));
    Rng r (99);
    std::vector<float> fm (kTick);
    bool finite = true;
    int runs = 0;
    for (int type = 1; type <= 4; ++type)
        for (int k = 0; k < 12; ++k)
        {
            SampleOscData d = base;
            d.type = type;
            d.slicing = k % 3;
            d.manualLoop = k % 4 == 1;
            d.loopGrains = k % 2 == 0;
            d.relativeLoop = k % 3 == 1;
            d.linkLoopLength = k % 5 == 2;
            d.exitLoopOnRelease = k % 2 == 1;
            d.phaseLock = k % 2 == 0;
            d.transients = k % 3 == 0;
            d.specSmooth = k % 2 == 1;
            d.densityMode = k % 3;
            d.lengthMode = (k / 3) % 3;
            d.spawnPattern = k % 4;
            d.jumpStart = k % 2 == 0;
            d.showXY = true; d.yTargetGranular = k; d.yTargetSpectral = k % 8; d.yValue = r.uni();
            d.scanRange = k % 3;
            d.rebuild();
            for (int p = 0; p < params().size(); ++p) rig.pv[p] = params()[p].def;
            const OscIds& id = rig.id;
            for (int pid : { id.start, id.end, id.loopStart, id.loopEnd, id.xfade, id.scan, id.timbre, id.density, id.grainLen,
                             id.grOffset, id.grDir, id.grPitch, id.grLenRand, id.grPanRand, id.grGainRand, id.winAmt, id.winAmtRand,
                             id.winSkew, id.winSkewRand, id.specCut, id.specMix, id.specLo, id.specHi, id.detune, id.blend, id.uniWidth,
                             id.uniRange, id.uniStart, id.uniSpan, id.warp1, id.warp2, id.uniWarp1, id.uniWarp2, id.fine, id.pan })
            {
                const auto& info = params()[pid];
                rig.pv[pid] = info.fromNorm (r.uni());
            }
            rig.pv[id.loopMode] = (float) (k % 5);
            rig.pv[id.unison] = (float) (1 + (k * 5) % 16);
            rig.pv[id.uniMode] = (float) (k % 5);
            rig.pv[id.uniStack] = (float) (k % 5);
            rig.pv[id.winShape] = (float) (k % 10);
            rig.pv[id.grainLen] = std::min (rig.pv[id.grainLen], 0.3f);
            rig.pv[id.warp1Mode] = (float) (k % 2 ? warp::Tube : warp::FmFirst + 1);
            rig.pv[id.warp2Mode] = (float) (k % 2 ? warp::AmFirst : warp::Lpf);
            rig.pv[id.fmType] = (float) (k % 4);
            const Out o = play (osc, d, rig, 40 + k * 4, 24000, 12000, [&] (int pos, OscTick& t)
            {
                for (int i = 0; i < kTick; ++i) fm[(size_t) i] = std::sin ((float) (pos + i) * 0.05f);
                t.src[kSrcB] = fm.data();
                t.src[kSrcA] = fm.data();
                t.pitchStart = t.pitchEnd;
                t.pitchEnd = (float) (40 + k * 4) + 3.0f * std::sin ((float) pos * 0.0003f);
            });
            if (! allFinite (o)) { finite = false; std::printf ("  non-finite: type %d case %d\n", type, k); }
            ++runs;
        }
    CHECK (finite, "all outputs finite");
    std::printf ("  %d random runs\n", runs);
}

// ---- Benchmarks ------------------------------------------------------------
double bench (int type, int voices, int unison, const SampleOscData& d, Rig& rig, double secs)
{
    std::vector<SampleOsc> osc ((size_t) voices);
    for (auto& o : osc) o.prepare (rig.ctx);
    rig.pv[rig.id.unison] = (float) unison;
    OscTick t;
    t.p = &rig.view; t.ctx = &rig.ctx;
    float L[kTick], R[kTick], M[kTick];
    t.outL = L; t.outR = R; t.outMono = M;
    // Best of 3 runs of `secs` each, in process CPU time (the machine is shared).
    const int ticks = (int) (secs * kSr / kTick);
    double sink = 0.0, best = 1.0e9;
    ScopedNoDenormals nd;
    for (int rep = 0; rep < 3; ++rep)
    {
        for (int v = 0; v < voices; ++v)
        {
            NoteInfo ni;
            ni.note = 48 + v * 3; ni.voiceIndex = v; ni.unisonSeed = v * 77 + rep;
            t.pitchStart = t.pitchEnd = (float) ni.note;
            osc[(size_t) v].noteOn (ni, t, rig.id, d);
        }
        const std::clock_t c0 = std::clock();
        for (int k = 0; k < ticks; ++k)
            for (int v = 0; v < voices; ++v)
            {
                t.n = kTick;
                t.pitchStart = t.pitchEnd = (float) (48 + v * 3);
                osc[(size_t) v].render (t, rig.id, d);
                sink += L[0] + R[kTick - 1];
            }
        best = std::min (best, (double) (std::clock() - c0) / CLOCKS_PER_SEC);
        for (auto& o : osc) if (o.finished()) std::printf ("  (bench: a voice finished early)\n");
    }
    if (! std::isfinite (sink)) std::printf ("  (non-finite sink)\n");
    (void) type;
    return 100.0 * best / secs;
}

void benchmarks()
{
    std::printf ("Benchmarks (CPU %% of one core @ 48 kHz)\n");
    Rig rig;
    rig.set (rig.id.level, 0.5f);
    auto s = sine (220.0, 4.0, 44100.0, 57, 0.5f, true);
    {
        SampleOscData d = dataFor (2, s);
        rig.set (rig.id.loopMode, 1.0f);
        rig.set (rig.id.loopStart, 0.2f); rig.set (rig.id.loopEnd, 0.8f); rig.set (rig.id.xfade, 0.2f);
        const double c = bench (2, 8, 4, d, rig, 3.0);
        std::printf ("  Sample, 8 voices x 4 unison, stereo, loop+xfade : %6.2f %%\n", c);
        CHECK (c < 10.0, "sample benchmark %.2f %%", c);
        rig.set (rig.id.warp1Mode, (float) warp::Tube); rig.set (rig.id.warp1, 0.5f);
        std::printf ("    (+ a POST warp via the test stub)             : %6.2f %%\n", bench (2, 8, 4, d, rig, 2.0));
        rig.set (rig.id.warp1Mode, 0.0f); rig.set (rig.id.warp1, 0.0f);
        rig.set (rig.id.loopMode, 0.0f); rig.set (rig.id.xfade, 0.0f);
    }
    {
        SampleOscData d = dataFor (3, s);
        rig.set (rig.id.density, 100.0f);
        rig.set (rig.id.grainLen, 0.1f);
        rig.set (rig.id.grPitch, 0.2f); rig.set (rig.id.grOffset, 0.2f); rig.set (rig.id.grPanRand, 0.5f);
        rig.set (rig.id.scan, 0.3f);
        const double c = bench (3, 8, 1, d, rig, 3.0);
        std::printf ("  Granular, 8 voices x 100 grains/s x 100 ms     : %6.2f %%\n", c);
        CHECK (c < 10.0, "granular benchmark %.2f %%", c);
        std::printf ("    (same, unison 4: 400 grains/s per voice)     : %6.2f %%\n", bench (3, 8, 4, d, rig, 2.0));
        rig.set (rig.id.scan, 1.0f);
    }
    {
        SampleOscData d = dataFor (4, s);
        const double c = bench (4, 8, 1, d, rig, 3.0);
        std::printf ("  Spectral, 8 voices                             : %6.2f %%\n", c);
        CHECK (c < 15.0, "spectral benchmark %.2f %%", c);
        d.phaseLock = true;
        std::printf ("    (phase lock on)                              : %6.2f %%\n", bench (4, 8, 1, d, rig, 2.0));
        d.phaseLock = false;
        std::printf ("    (unison 4)                                   : %6.2f %%\n", bench (4, 8, 4, d, rig, 2.0));
    }
    {
        // Analysis cost (message thread).
        auto big = sine (220.0, 10.0, 48000.0, 57);
        const auto t0 = std::chrono::steady_clock::now();
        auto sd = SampleOscData::buildSpectral (*big);
        const double el = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        std::printf ("  buildSpectral, 10 s mono: %.0f ms, %.1f MB\n", el * 1000.0,
                     (double) (sd->mag.size() * 3 * sizeof (float)) / 1.0e6);
    }
}
} // namespace

int main()
{
    params();
    testPitch();
    testLoops();
    testSlices();
    testGranular();
    testSpectral();
    testMultisample();
    testWavetable();
    testSaveLoad();
    testRobustness();
    benchmarks();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
