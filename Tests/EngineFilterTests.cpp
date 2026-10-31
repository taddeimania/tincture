// EngineFilterTests.cpp — tests and micro-benchmark for FilterVoice (Filters.h):
// stability of all 83 types at max RES/DRIVE, extreme cutoffs and 44.1/96/192 kHz;
// cutoff accuracy and slopes measured with stepped sine sweeps; ladder
// self-oscillation pitch; magnitudeDb() against the measured response; type
// switching click safety; pan, clean drive and mix; ns/sample per category.
//   EngineFilterTests            (returns non-zero on failure)
#include "Engine/Filters.h"
#include "Engine/ParamIds.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

using namespace tincture;

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond, ...) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s  ", __FILE__, __LINE__, #cond); std::printf (__VA_ARGS__); std::printf ("\n"); } } while (0)

constexpr double kPiD = 3.14159265358979323846;

// NaN/Inf check that survives -ffast-math (finiteBits may be folded to true).
bool finiteBits (float x) { uint32_t b; std::memcpy (&b, &x, 4); return (b & 0x7f800000u) != 0x7f800000u; }

const std::string& typeName (int t) { return choices::filterTypes()[(size_t) t]; }

struct Set
{
    int type = 5;
    float fc = 1000.0f, res = 0.0f, drive = 0.0f, var = 0.0f, pan = 0.0f, mix = 1.0f;
    bool clean = false;
};

// Runs in/out (same signal both channels unless inR given) in kTick ticks at a fixed cutoff.
void run (FilterVoice& f, const Set& s, const Context& ctx, const float* inL, const float* inR,
          float* outL, float* outR, int n)
{
    FilterTick t;
    t.type = s.type; t.cutoffHz = t.cutoffHzPrev = s.fc;
    t.res = s.res; t.drive = s.drive; t.var = s.var; t.pan = s.pan; t.mix = s.mix; t.cleanDrive = s.clean;
    t.ctx = &ctx;
    for (int i = 0; i < n; i += kTick)
    {
        const int m = std::min (kTick, n - i);
        std::copy_n (inL + i, m, outL + i);
        std::copy_n ((inR ? inR : inL) + i, m, outR + i);
        t.n = m; t.L = outL + i; t.R = outR + i;
        f.process (t);
    }
}

Context makeCtx (double fs)
{
    Context c;
    c.sampleRate = fs;
    c.invSampleRate = (float) (1.0 / fs);
    return c;
}

// Steady-state sine gain (linear) of the left (ch 0) or right (ch 1) channel.
double sineGain (const Set& s, double fs, double freq, float amp = 0.1f, int ch = 0, double settleSec = 0.25)
{
    const Context ctx = makeCtx (fs);
    FilterVoice f;
    f.prepare (ctx);
    const int settle = (int) (settleSec * fs);
    const int cycles = std::max (20, (int) std::ceil (0.1 * freq));
    const int win = (int) std::lround (cycles * fs / freq);
    const int n = settle + win;
    std::vector<float> in ((size_t) n), L ((size_t) n), R ((size_t) n);
    for (int i = 0; i < n; ++i) in[(size_t) i] = amp * (float) std::sin (2.0 * kPiD * freq * i / fs);
    run (f, s, ctx, in.data(), nullptr, L.data(), R.data(), n);
    const std::vector<float>& y = ch == 0 ? L : R;
    double sc = 0, cc = 0;
    for (int i = settle; i < n; ++i)
    {
        const double ph = 2.0 * kPiD * freq * i / fs;
        sc += y[(size_t) i] * std::sin (ph);
        cc += y[(size_t) i] * std::cos (ph);
    }
    return 2.0 * std::sqrt (sc * sc + cc * cc) / win / amp;
}
double sineDb (const Set& s, double fs, double f, float amp = 0.1f, int ch = 0) { return 20.0 * std::log10 (std::max (sineGain (s, fs, f, amp, ch), 1e-9)); }

// Frequency where the response crosses `db` (falling for LP, rising for HP), bisection in log f.
double findCrossing (const Set& s, double fs, double lo, double hi, double db, bool falling)
{
    for (int i = 0; i < 22; ++i)
    {
        const double mid = std::sqrt (lo * hi);
        const bool above = sineDb (s, fs, mid) > db;
        if (above == falling) lo = mid; else hi = mid;
    }
    return std::sqrt (lo * hi);
}

double findPeak (const Set& s, double fs, double lo, double hi)
{
    double a = std::log (lo), b = std::log (hi);
    const double r = 0.6180339887;
    for (int i = 0; i < 30; ++i)
    {
        const double c = b - r * (b - a), d = a + r * (b - a);
        if (sineDb (s, fs, std::exp (c)) > sineDb (s, fs, std::exp (d))) b = d; else a = c;
    }
    return std::exp (0.5 * (a + b));
}

// ---------------------------------------------------------------------------
void testLabels()
{
    std::printf ("labels\n");
    CHECK (choices::filterTypes().size() == (size_t) FilterVoice::kNumTypes, "%d", (int) choices::filterTypes().size());
    CHECK (std::string (choices::filterVarLabel (0)) == "FAT", "-");
    CHECK (std::string (choices::filterVarLabel (18)) == "FREQ", "-");
    CHECK (std::string (choices::filterVarLabel (30)) == "MORPH", "-");
    CHECK (std::string (choices::filterVarLabel (44)) == "HL WID", "-");
    CHECK (std::string (choices::filterVarLabel (41)) == "HP FRQ", "-");
    CHECK (std::string (choices::filterVarLabel (59)) == "BOEUF", "-");
    CHECK (std::string (choices::filterVarLabel (81)) == "MIX", "-");
    CHECK (! FilterVoice::hasVar (52) && FilterVoice::hasVar (53) && ! FilterVoice::hasVar (82), "-");
    int unused = 0;
    for (int t = 0; t < FilterVoice::kNumTypes; ++t) unused += FilterVoice::hasVar (t) ? 0 : 1;
    std::printf ("  %d types without VAR\n", unused);
    CHECK (unused == 6, "%d", unused);      // Ring Mod, SampHold, SampHold-, German LP, DJ Mixer, Exp BPF
}

// Every type, max RES and DRIVE, extreme cutoffs, three rates, pan, clean.
void testStability()
{
    std::printf ("stability: 83 types x {44.1,96,192} kHz x cutoff {8,300,5k,22k} x VAR {0,.5,1}, RES=DRIVE=1\n");
    const double rates[] { 44100.0, 96000.0, 192000.0 };
    const float cuts[] { 8.0f, 300.0f, 5000.0f, 22000.0f }, vars[] { 0.0f, 0.5f, 1.0f };
    float worst = 0.0f;
    int worstType = -1;
    for (int t = 0; t < FilterVoice::kNumTypes; ++t)
    {
        float typeMax = 0.0f;
        bool finite = true;
        for (double fs : rates)
        {
            const Context ctx = makeCtx (fs);
            FilterVoice f;
            f.prepare (ctx);
            const int n = (int) (0.08 * fs);
            std::vector<float> in ((size_t) n), L ((size_t) n), R ((size_t) n);
            Rng rng (1234u + (uint32_t) t);
            int combo = 0;
            for (float fc : cuts)
                for (float var : vars)
                {
                    for (int i = 0; i < n; ++i) in[(size_t) i] = i == 0 ? 1.0f : (i < n / 2 ? rng.bi() : 0.0f);
                    Set s; s.type = t; s.fc = fc; s.res = 1.0f; s.drive = 1.0f; s.var = var;
                    s.pan = (combo & 1) ? 1.0f : -1.0f; s.clean = (combo % 3) == 2;
                    ++combo;
                    f.reset();
                    run (f, s, ctx, in.data(), nullptr, L.data(), R.data(), n);
                    for (int i = 0; i < n; ++i)
                    {
                        const float a = std::max (std::fabs (L[(size_t) i]), std::fabs (R[(size_t) i]));
                        if (! finiteBits (L[(size_t) i]) || ! finiteBits (R[(size_t) i])) finite = false;
                        typeMax = std::max (typeMax, a);
                    }
                }
            // A fast full-range cutoff sweep with random RES, not reset between ticks.
            FilterTick tk;
            tk.type = t; tk.res = 1.0f; tk.drive = 1.0f; tk.var = 0.7f; tk.ctx = &ctx;
            float lc = 3.0f, dir = 1.0f;
            for (int i = 0; i < n; i += kTick)
            {
                for (int j = 0; j < kTick && i + j < n; ++j) { L[(size_t) (i + j)] = R[(size_t) (i + j)] = rng.bi(); }
                tk.cutoffHzPrev = std::exp2 (lc);
                lc += dir * 0.4f;
                if (lc > 14.4f || lc < 3.0f) dir = -dir;
                tk.cutoffHz = std::exp2 (lc);
                tk.res = rng.uni();
                tk.n = std::min (kTick, n - i); tk.L = L.data() + i; tk.R = R.data() + i;
                f.process (tk);
                for (int j = 0; j < tk.n; ++j)
                {
                    if (! finiteBits (tk.L[j]) || ! finiteBits (tk.R[j])) finite = false;
                    typeMax = std::max (typeMax, std::max (std::fabs (tk.L[j]), std::fabs (tk.R[j])));
                }
            }
        }
        CHECK (finite, "%s produced NaN/Inf", typeName (t).c_str());
        CHECK (typeMax < 64.0f, "%s max |y| = %.1f", typeName (t).c_str(), typeMax);
        if (typeMax > worst) { worst = typeMax; worstType = t; }
    }
    std::printf ("  loudest type: %s, peak |y| = %.2f (input noise +-1, drive +24 dB)\n", typeName (worstType).c_str(), worst);
}

void testCutoffAccuracy()
{
    std::printf ("cutoff accuracy (-3 dB point, res 0, stepped sines)\n");
    struct C { int type; bool lp; };
    const C cases[] { { 4, true }, { 5, true }, { 6, true }, { 7, true }, { 8, false }, { 9, false }, { 10, false }, { 11, false } };
    for (double fs : { 48000.0, 96000.0 })
        for (float fc : { 100.0f, 1000.0f, 5000.0f, 12000.0f })
        {
            std::printf ("  fs %.0f fc %6.0f:", fs, fc);
            for (const C& c : cases)
            {
                Set s; s.type = c.type; s.fc = fc;
                const double f3 = findCrossing (s, fs, fc / 3.0, std::min (fc * 3.0, 0.48 * fs), -3.0103, c.lp);
                const double err = f3 / fc - 1.0;
                std::printf (" %s %+.1f%%", typeName (c.type).c_str(), 100.0 * err);
                CHECK (std::fabs (err) < 0.05, "%s fc %.0f fs %.0f: -3 dB at %.1f", typeName (c.type).c_str(), fc, fs, f3);
            }
            for (int t : { 12, 13 })
            {
                Set s; s.type = t; s.fc = fc; s.res = 0.5f;
                const double pk = findPeak (s, fs, fc / 2.0, std::min (fc * 2.0, 0.48 * fs));
                const double g = sineDb (s, fs, fc);
                std::printf (" %s peak %+.1f%% (%.2f dB)", typeName (t).c_str(), 100.0 * (pk / fc - 1.0), g);
                CHECK (std::fabs (pk / fc - 1.0) < 0.05 && std::fabs (g) < 0.5, "%s fc %.0f: peak %.1f Hz %.2f dB", typeName (t).c_str(), fc, pk, g);
            }
            std::printf ("\n");
        }
}

void testSlopes()
{
    std::printf ("slopes (dB/oct between 4 fc and 8 fc; HP between fc/8 and fc/4)\n");
    struct C { int type; bool lp; double expect; };
    const C cases[] { { 3, true, 24 }, { 7, true, 24 }, { 11, false, 24 }, { 1, true, 12 }, { 5, true, 12 }, { 2, true, 18 }, { 6, true, 18 }, { 0, true, 6 } };
    for (const C& c : cases)
    {
        Set s; s.type = c.type; s.fc = c.lp ? 400.0f : 4000.0f;
        const double a = c.lp ? sineDb (s, 48000.0, 4 * s.fc, 0.05f) : sineDb (s, 48000.0, s.fc / 8, 0.05f);
        const double b = c.lp ? sineDb (s, 48000.0, 8 * s.fc, 0.05f) : sineDb (s, 48000.0, s.fc / 4, 0.05f);
        const double slope = c.lp ? a - b : b - a;
        std::printf ("  %-10s %.1f dB/oct\n", typeName (c.type).c_str(), slope);
        CHECK (std::fabs (slope - c.expect) < 2.0, "%s slope %.1f", typeName (c.type).c_str(), slope);
    }
}

// Ladder self-oscillation: impulse then silence, pitch from zero crossings.
void testSelfOsc()
{
    std::printf ("self-oscillation at RES 1 (cutoff = note frequency, cents error)\n");
    for (double fs : { 48000.0, 96000.0 })
        for (int t : { 0, 1, 2, 3, 75, 60, 77, 76 })
        {
            std::printf ("  fs %.0f %-11s", fs, typeName (t).c_str());
            for (int note : { 45, 57, 69, 81, 93 })
            {
                const Context ctx = makeCtx (fs);
                FilterVoice f;
                f.prepare (ctx);
                const int n = (int) (1.5 * fs);
                std::vector<float> in ((size_t) n, 0.0f), L ((size_t) n), R ((size_t) n);
                in[0] = 0.1f;
                Set s; s.type = t; s.fc = noteToHz ((float) note); s.res = 1.0f;
                run (f, s, ctx, in.data(), nullptr, L.data(), R.data(), n);
                double first = -1, last = -1, peak = 0;
                int count = 0;
                for (int i = n / 2; i < n; ++i)
                {
                    peak = std::max (peak, (double) std::fabs (L[(size_t) i]));
                    if (L[(size_t) (i - 1)] < 0.0f && L[(size_t) i] >= 0.0f)
                    {
                        const double tz = (double) (i - 1) + (double) L[(size_t) (i - 1)] / (double) (L[(size_t) (i - 1)] - L[(size_t) i]);
                        if (first < 0) first = tz;
                        last = tz;
                        ++count;
                    }
                }
                const double hz = count > 1 ? (count - 1) * fs / (last - first) : 0.0;
                const double cents = hz > 0 ? 1200.0 * std::log2 (hz / s.fc) : 9999.0;
                std::printf (" %d:%+.1fc", note, cents);
                const bool required = t <= 3;      // MG Low types: must oscillate at pitch
                if (required)
                {
                    CHECK (peak > 0.05, "%s note %d not oscillating (peak %.3f)", typeName (t).c_str(), note, peak);
                    CHECK (std::fabs (cents) < 5.0, "%s note %d: %.1f Hz vs %.1f (%.1f cents)", typeName (t).c_str(), note, hz, s.fc, cents);
                    CHECK (peak < 4.0, "%s self-osc peak %.2f", typeName (t).c_str(), peak);
                }
            }
            std::printf ("\n");
        }
}

// magnitudeDb() against measured steady-state sine gain for the linear (small-signal) types.
void testMagnitude()
{
    std::printf ("magnitudeDb vs measured (fc 1 kHz, res .3, var .4, 48 kHz, |H| > -20 dB, tol 1 dB)\n");
    const double fs = 48000.0;
    const double freqs[] { 125, 250, 500, 707, 1000, 1414, 2000, 2828, 4000, 8000 };
    double worst = 0.0;
    int worstType = -1, tested = 0;
    for (int t = 0; t < FilterVoice::kNumTypes; ++t)
    {
        if (t == 52 || t == 53 || t == 54 || t == 55) continue;    // ring mod / S&H: time-variant
        Set s; s.type = t; s.fc = 1000.0f; s.res = 0.3f; s.var = t == 78 ? 0.0f : 0.4f;
        double typeWorst = 0.0;
        for (double f : freqs)
        {
            const double a = FilterVoice::magnitudeDb (t, s.fc, s.res, s.drive, s.var, (float) f, fs);
            if (a < -20.0) continue;
            const double m = sineDb (s, fs, f, 0.01f);
            typeWorst = std::max (typeWorst, std::fabs (a - m));
            CHECK (std::fabs (a - m) < 1.0, "%s at %.0f Hz: analytic %.2f dB measured %.2f dB", typeName (t).c_str(), f, a, m);
        }
        ++tested;
        if (typeWorst > worst) { worst = typeWorst; worstType = t; }
    }
    std::printf ("  %d types, worst deviation %.2f dB (%s)\n", tested, worst, typeName (worstType).c_str());
}

// Switch type every 50 ms through all types while a sine plays; the step at
// each switch must not exceed the signal's own sample-to-sample steps much.
void testSwitching()
{
    std::printf ("type switching click safety\n");
    const double fs = 48000.0;
    const Context ctx = makeCtx (fs);
    FilterVoice f;
    f.prepare (ctx);
    const int seg = 2400;
    std::vector<int> order;
    for (int t = 0; t < FilterVoice::kNumTypes; ++t) order.push_back (t);
    Rng rng (99u);
    for (int i = (int) order.size() - 1; i > 0; --i) std::swap (order[(size_t) i], order[(size_t) (rng.next() % (uint32_t) (i + 1))]);
    order.push_back (order[0]);
    const int n = seg * (int) order.size();
    std::vector<float> L ((size_t) n), R ((size_t) n);
    FilterTick tk;
    tk.cutoffHz = tk.cutoffHzPrev = 2000.0f; tk.res = 0.5f; tk.var = 0.5f; tk.drive = 0.2f; tk.ctx = &ctx;
    for (int i = 0; i < n; i += kTick)
    {
        for (int j = 0; j < kTick; ++j) L[(size_t) (i + j)] = R[(size_t) (i + j)] = 0.5f * (float) std::sin (2.0 * kPiD * 220.0 * (i + j) / fs);
        tk.type = order[(size_t) (i / seg)];
        tk.n = kTick; tk.L = L.data() + i; tk.R = R.data() + i;
        f.process (tk);
    }
    double worstRatio = 0.0;
    int worstAt = 0;
    for (size_t k = 1; k < order.size(); ++k)
    {
        const int s0 = (int) k * seg;
        double before = 0, after = 0, at = 0;
        for (int i = s0 - 1200; i < s0; ++i) before = std::max (before, (double) std::fabs (L[(size_t) i] - L[(size_t) (i - 1)]));
        for (int i = s0 + 1200; i < s0 + seg; ++i) after = std::max (after, (double) std::fabs (L[(size_t) i] - L[(size_t) (i - 1)]));
        for (int i = s0; i < s0 + 4; ++i) at = std::max (at, (double) std::fabs (L[(size_t) i] - L[(size_t) (i - 1)]));
        const double bound = 1.5 * std::max (before, after) + 0.02;
        const double ratio = at / bound;
        if (ratio > worstRatio) { worstRatio = ratio; worstAt = (int) k; }
        CHECK (at <= bound, "%s -> %s: step %.3f (signal steps %.3f / %.3f)", typeName (order[k - 1]).c_str(), typeName (order[k]).c_str(), at, before, after);
    }
    std::printf ("  %d switches, worst step / bound = %.2f (%s -> %s)\n", (int) order.size() - 1, worstRatio,
                 typeName (order[(size_t) worstAt - 1]).c_str(), typeName (order[(size_t) worstAt]).c_str());
}

void testPanCleanMix()
{
    std::printf ("pan / clean drive / mix\n");
    {
        Set s; s.type = 5; s.fc = 1000.0f; s.pan = -1.0f;      // pan left: L cutoff 2 kHz, R 500 Hz
        const double l = sineDb (s, 48000.0, 2000.0, 0.1f, 0), r = sineDb (s, 48000.0, 500.0, 0.1f, 1);
        std::printf ("  pan -1: L at 2 kHz %.2f dB, R at 500 Hz %.2f dB\n", l, r);
        CHECK (std::fabs (l + 3.01) < 0.3 && std::fabs (r + 3.01) < 0.3, "%.2f %.2f", l, r);
    }
    {
        // Harmonic residue of a driven MG Low 24 with and without clean mode.
        auto thd = [] (bool clean) {
            const double fs = 48000.0, f0 = 187.5;
            const Context ctx = makeCtx (fs);
            FilterVoice f;
            f.prepare (ctx);
            const int n = 48000;
            std::vector<float> in ((size_t) n), L ((size_t) n), R ((size_t) n);
            for (int i = 0; i < n; ++i) in[(size_t) i] = 0.5f * (float) std::sin (2.0 * kPiD * f0 * i / fs);
            Set s; s.type = 3; s.fc = 15000.0f; s.drive = 0.8f; s.clean = clean;
            run (f, s, ctx, in.data(), nullptr, L.data(), R.data(), n);
            double sc = 0, cc = 0, tot = 0;
            for (int i = n / 2; i < n; ++i)
            {
                const double ph = 2.0 * kPiD * f0 * i / fs;
                sc += L[(size_t) i] * std::sin (ph); cc += L[(size_t) i] * std::cos (ph);
            }
            const int w = n / 2;
            const double a = 2.0 * sc / w, b = 2.0 * cc / w;
            double res = 0;
            for (int i = n / 2; i < n; ++i)
            {
                const double ph = 2.0 * kPiD * f0 * i / fs;
                const double e = L[(size_t) i] - a * std::sin (ph) - b * std::cos (ph);
                res += e * e; tot += (double) L[(size_t) i] * L[(size_t) i];
            }
            return std::sqrt (res / tot);
        };
        const double dirty = thd (false), clean = thd (true);
        std::printf ("  MG Low 24 drive 80%%: THD %.1f%% normal, %.2f%% clean\n", 100 * dirty, 100 * clean);
        CHECK (clean < dirty * 0.1, "clean %.4f dirty %.4f", clean, dirty);
    }
    {
        const Context ctx = makeCtx (48000.0);
        FilterVoice f;
        f.prepare (ctx);
        std::vector<float> in (512), L (512), R (512);
        Rng rng (5u);
        for (auto& x : in) x = rng.bi();
        Set s; s.type = 3; s.fc = 200.0f; s.mix = 0.0f;
        run (f, s, ctx, in.data(), nullptr, L.data(), R.data(), 512);
        float err = 0;
        for (int i = 0; i < 512; ++i) err = std::max (err, std::fabs (L[(size_t) i] - in[(size_t) i]));
        CHECK (err < 1e-6f, "mix 0 differs from dry by %g", err);
    }
}

// processMono() against process() on L == R, pan 0: for every type, runs of mono
// and stereo ticks in random order (mono->stereo->mono...), parameters moving
// every tick, a type switch half way. Output must match within 1e-6.
void testMono()
{
    std::printf ("processMono == process (L == R, pan 0), mono/stereo tick runs\n");
    double worst = 0.0, worstWide = 0.0;    // before / after the reference had a genuinely stereo run
    int worstType = 0, transitions = 0;
    for (double fs : { 48000.0, 192000.0 })
        for (int t = 0; t < FilterVoice::kNumTypes; ++t)
        {
            const Context ctx = makeCtx (fs);
            FilterVoice a, b;
            a.prepare (ctx);
            b.prepare (ctx);
            Rng rng (77u + (uint32_t) t);
            const int ticks = 600;
            float aL[kTick], aR[kTick], bL[kTick], bR[kTick];
            bool mono = true, wide = false;     // wide: a genuinely stereo run (R input differs, pan != 0)
            int runLeft = 0;
            double typeWorst = 0.0, typeWide = 0.0;
            bool rDiverged = false;     // the reference's R carries a genuinely stereo history
            for (int k = 0; k < ticks; ++k)
            {
                if (runLeft-- <= 0)
                {
                    mono = ! mono;
                    wide = ! mono && (rng.next() & 1u) != 0;
                    runLeft = 1 + (int) (rng.next() % 40u);
                    ++transitions;
                }
                FilterTick tk;
                tk.type = k < ticks / 2 ? t : (t + 37) % FilterVoice::kNumTypes;
                tk.cutoffHzPrev = 30.0f * std::exp2 ((float) ((k * 7) % 90) * 0.1f);
                tk.cutoffHz = 30.0f * std::exp2 ((float) (((k + 1) * 7) % 90) * 0.1f);
                tk.res = rng.uni(); tk.var = rng.uni(); tk.drive = (k / 100) % 2 ? rng.uni() : 0.0f;
                tk.mix = (k / 150) % 2 ? rng.uni() : 1.0f;
                tk.cleanDrive = (k / 50) % 3 == 2;
                tk.ctx = &ctx; tk.n = kTick;
                for (int i = 0; i < kTick; ++i) aL[i] = aR[i] = bL[i] = bR[i] = (i == 0 && k == 0) ? 1.0f : 0.7f * rng.bi();
                if (wide)
                {
                    tk.pan = 0.3f;
                    for (int i = 0; i < kTick; ++i) aR[i] = bR[i] = 0.7f * rng.bi();
                }
                if (wide) rDiverged = true;
                FilterTick ta = tk; ta.L = aL; ta.R = aR;
                a.process (ta);
                FilterTick tb = tk; tb.L = bL; tb.R = mono ? nullptr : bR;
                if (mono) b.processMono (tb); else b.process (tb);
                for (int i = 0; i < kTick; ++i)
                {
                    const double tol = std::max (1.0, (double) std::fabs (aL[i]));
                    double e = std::fabs ((double) aL[i] - bL[i]) / tol;     // relative above |y| = 1, else absolute
                    if (! mono && ! rDiverged) e = std::max (e, 1e3 * std::fabs ((double) aR[i] - bR[i]) / tol);
                    (rDiverged ? typeWide : typeWorst) = std::max (rDiverged ? typeWide : typeWorst, e);
                }
            }
            // Mono input throughout: bit-identical. After a genuinely stereo run the
            // reference computes L in the 2-channel loop and the mono path in the
            // 1-channel loop; -ffast-math may associate their sums differently.
            CHECK (typeWorst <= 1e-6, "%s at %.0f Hz: mono differs by %g", typeName (t).c_str(), fs, typeWorst);
            CHECK (typeWide <= 1e-5, "%s at %.0f Hz: mono after stereo differs by %g", typeName (t).c_str(), fs, typeWide);
            if (typeWorst > worst) { worst = typeWorst; worstType = t; }
            worstWide = std::max (worstWide, typeWide);
        }
    std::printf ("  83 types x 2 rates, %d mono/stereo transitions: worst difference %g (%s); after genuine stereo runs %g\n",
                 transitions, worst, typeName (worstType).c_str(), worstWide);
}

// ns per sample per stereo filter, per type, cutoff ramping every tick.
// Best of `reps` runs: the machine is shared, the minimum is the stable figure.
double benchOnce (int t, double fs, double seconds, float pan, float drive, float var);
bool benchMono = false;
double benchType (int t, double fs, double seconds = 0.25, int reps = 7, float pan = 0.2f, float drive = 0.3f, float var = 0.5f)
{
    double best = 1e30;
    for (int r = 0; r < reps; ++r) best = std::min (best, benchOnce (t, fs, seconds, pan, drive, var));
    return best;
}
double benchOnce (int t, double fs, double seconds, float pan, float drive, float var)
{
    const Context ctx = makeCtx (fs);
    FilterVoice f;
    f.prepare (ctx);
    const int n = (int) (fs * seconds);
    std::vector<float> noise ((size_t) n);
    Rng rng (7u);
    for (auto& x : noise) x = 0.5f * rng.bi();
    std::vector<float> L ((size_t) kTick), R ((size_t) kTick);
    FilterTick tk;
    tk.type = t; tk.res = 0.6f; tk.var = var; tk.drive = drive; tk.pan = pan; tk.ctx = &ctx; tk.n = kTick;
    tk.L = L.data(); tk.R = R.data();
    float lc = 8.0f, dir = 0.02f;
    double sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i + kTick <= n; i += kTick)
    {
        std::copy_n (noise.data() + i, kTick, L.data());
        for (int j = 0; j < kTick; ++j) R[(size_t) j] = -noise[(size_t) (i + j)];   // genuinely stereo: no automatic mono path
        tk.cutoffHzPrev = std::exp2 (lc);
        lc += dir;
        if (lc > 13.5f || lc < 7.0f) dir = -dir;
        tk.cutoffHz = std::exp2 (lc);
        if (benchMono) f.processMono (tk); else f.process (tk);
        sink += L[0] + R[kTick - 1];
    }
    const auto t1 = std::chrono::steady_clock::now();
    if (sink == 12345.678) std::printf (" ");
    return std::chrono::duration<double, std::nano> (t1 - t0).count() / n;
}

void benchmark()
{
    std::printf ("benchmark: ns/sample per stereo filter at 48 kHz (cutoff ramping, res .6, drive .3)\n");
    const auto& cats = choices::filterCategories();
    std::printf ("  %-8s %8s %8s  %s\n", "category", "mean", "max", "slowest");
    for (const auto& c : cats)
    {
        double sum = 0, mx = 0;
        int slow = c.first;
        for (int t = c.first; t < c.first + c.count; ++t)
        {
            const double ns = benchType (t, 48000.0);
            sum += ns;
            if (ns > mx) { mx = ns; slow = t; }
        }
        std::printf ("  %-8s %8.1f %8.1f  %s\n", c.name, sum / c.count, mx, typeName (slow).c_str());
    }
    std::printf ("  selected types:");
    for (int t : { 1, 3, 5, 7, 31, 38, 58, 62, 74, 76, 78 }) std::printf (" %s %.1f |", typeName (t).c_str(), benchType (t, 48000.0));
    std::printf ("\n");
    std::printf ("  mono fast path (pan 0, drive 0; FAT/VAR as noted), ns/sample: stereo process (L != R) -> processMono\n");
    struct M { int type; float var; };
    for (M mm : { M { 1, 0.0f }, M { 3, 0.0f }, M { 1, 0.5f }, M { 3, 0.5f }, M { 4, 0.0f }, M { 5, 0.0f }, M { 7, 0.0f }, M { 9, 0.0f },
                  M { 12, 0.0f }, M { 5, 0.5f }, M { 19, 0.4f }, M { 31, 0.5f }, M { 75, 0.0f }, M { 76, 0.0f } })
    {
        benchMono = false;
        const double st = benchType (mm.type, 48000.0, 0.25, 9, 0.0f, 0.0f, mm.var);
        benchMono = true;
        const double mo = benchType (mm.type, 48000.0, 0.25, 9, 0.0f, 0.0f, mm.var);
        benchMono = false;
        std::printf ("    %-11s var %.1f: %5.1f -> %5.1f  (x%.2f)\n", typeName (mm.type).c_str(), mm.var, st, mo, st / mo);
    }
    const char* label[3] { "FAT 0, pan 0, drive 0 (defaults)", "FAT 0, pan .2, drive .3", "FAT .5, pan .2, drive .3" };
    for (int k = 0; k < 3; ++k)
    {
        const double mg = k == 0 ? benchType (3, 48000.0, 0.5, 15, 0.0f, 0.0f, 0.0f)
                        : benchType (3, 48000.0, 0.5, 15, 0.2f, 0.3f, k == 1 ? 0.0f : 0.5f);
        const double cpu = mg * 1e-9 * 48000.0 * 32.0 * 100.0;
        std::printf ("  MG Low 24 [%s]: %.1f ns/sample stereo -> 16 voices x 2 filters at 48 kHz = %.2f%% of one core\n", label[k], mg, cpu);
    }
}
} // namespace

int main()
{
    ScopedNoDenormals noDenormals;
    const auto t0 = std::chrono::steady_clock::now();
    testLabels();
    testStability();
    testCutoffAccuracy();
    testSlopes();
    testSelfOsc();
    testMagnitude();
    testSwitching();
    testPanCleanMix();
    testMono();
    benchmark();
    const double sec = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    std::printf ("\n%d checks, %d failures (%.1f s)\n", checks, failures, sec);
    return failures == 0 ? 0 : 1;
}
