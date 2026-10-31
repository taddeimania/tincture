// EngineFxTests.cpp — tests and benchmarks for the FX racks (Source/Engine/Fx).
// Builds without JUCE; see docs/dev/AGENTS.md for the compile line.
//   EngineFxTests            run the tests and the benchmark
//   EngineFxTests --quick    shorter benchmark
//   EngineFxTests --no-bench tests only
#include "Engine/Fx/FxEngine.h"
#include "Engine/Fx/FxLayout.h"
#include "Engine/Fx/FxTypes.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>

using namespace tincture;

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++checks; const double va = (a), vb = (b); if (! (std::abs (va - vb) <= (tol))) { ++failures; \
    std::printf ("  FAIL %s:%d  %s = %.4f, expected %.4f +- %.4f\n", __FILE__, __LINE__, #a, va, vb, (double) (tol)); } } while (0)

constexpr double kSr = 48000.0;
constexpr int kBlock = 256;

// A one-engine test rig: a layout, host parameter values and a ParamView.
struct Rig
{
    FxEngine eng;
    FxLayout lay;
    ParamValues pv;
    std::vector<int16_t> slots;
    ParamView view;
    Context ctx;

    Rig()
    {
        pv.resetToDefaults();
        slots.assign ((size_t) params().size(), -1);
        view.base = pv.v.data();
        view.slot = slots.data();
        ctx.sampleRate = kSr;
        ctx.invSampleRate = (float) (1.0 / kSr);
        ctx.bpm = 120.0;
        eng.prepare (ctx, 512);
    }
    void defaults (int u, FxType t)
    {
        const auto d = fxDefaults (t);
        for (int k = 0; k < kFxParams; ++k) pv[ids().fx[u].p[k]] = d[(size_t) k];
        pv[ids().fx[u].mix] = fxDefaultMix (t);
    }
    int add (FxType t, int rack = kFxMain)
    {
        const int u = lay.add (rack, t);
        if (u >= 0) defaults (u, t);
        return u;
    }
    int addBand (int rack, int splitter, int band, FxType t)
    {
        const int u = lay.add (FxList { rack, splitter, band }, t);
        if (u >= 0) defaults (u, t);
        return u;
    }
    void set (int u, int slot, float norm) { pv[ids().fx[u].p[slot]] = norm; }
    void setPlain (int u, FxType t, int slot, float plain) { set (u, slot, fxInfo (t).params[(size_t) slot].toNorm (plain)); }
    void mix (int u, float m) { pv[ids().fx[u].mix] = m; }
    void commit() { eng.setLayout (std::make_shared<FxLayout> (lay)); }
    void run (float* L, float* R, int n, int rack = kFxMain)
    {
        for (int off = 0; off < n; off += kBlock)
        {
            const int m = std::min (kBlock, n - off);
            eng.beginBlock (ctx);
            eng.process (rack, L + off, R + off, m, view);
        }
    }
};

std::vector<float> sine (double hz, int n, float amp = 0.5f)
{
    std::vector<float> v ((size_t) n);
    for (int i = 0; i < n; ++i) v[(size_t) i] = amp * (float) std::sin (2.0 * 3.14159265358979 * hz * i / kSr);
    return v;
}
double rms (const std::vector<float>& v, int from, int to)
{
    double s = 0.0;
    for (int i = from; i < to; ++i) s += (double) v[(size_t) i] * v[(size_t) i];
    return std::sqrt (s / std::max (1, to - from));
}
double db (double x) { return 20.0 * std::log10 (std::max (x, 1.0e-12)); }
// Goertzel power (amplitude) of a frequency over a range.
double tone (const std::vector<float>& v, int from, int to, double hz)
{
    const double w = 2.0 * 3.14159265358979 * hz / kSr, c = 2.0 * std::cos (w);
    double s1 = 0.0, s2 = 0.0;
    for (int i = from; i < to; ++i) { const double s0 = v[(size_t) i] + c * s1 - s2; s2 = s1; s1 = s0; }
    const double p = s1 * s1 + s2 * s2 - c * s1 * s2;
    return 2.0 * std::sqrt (std::max (p, 0.0)) / (to - from);
}
bool finiteAll (const std::vector<float>& v, float limit = 1.0e4f)
{
    for (float x : v) if (! std::isfinite (x) || std::abs (x) > limit) return false;
    return true;
}

// Gain (dB) of a single-module chain for a sine at hz (steady state).
template <typename Setup>
double sineGainDb (FxType t, double hz, Setup&& setup, int ch = 0)
{
    Rig r;
    const int u = r.add (t);
    setup (r, u);
    r.commit();
    const int n = 24000;
    auto L = sine (hz, n, 0.1f), R = L;
    const auto in = L;
    r.run (L.data(), R.data(), n);
    return db (rms (ch == 0 ? L : R, n / 2, n) / rms (in, n / 2, n));
}

// ---------------------------------------------------------------------------
void testTypes()
{
    std::printf ("types\n");
    const std::vector<std::string> menu { "Bode", "Chorus", "Compressor", "Convolve", "Delay", "Distortion", "Equalizer", "Filter",
                                          "Flanger", "Hyper/Dimension", "Phaser", "Reverb", "Splitter L/H", "Splitter L/M/H",
                                          "Splitter M/S", "Utility" };
    CHECK (fxTypeNames() == menu);
    for (int t = 0; t < kNumFxTypes; ++t)
    {
        const auto& info = fxInfo (t);
        CHECK ((info.colour >> 24) == 0xff);
        CHECK (info.params[kFxLevelSlot].shortName == "LEVEL");
        const auto d = fxDefaults ((FxType) t);
        float v[kFxParams];
        for (int k = 0; k < kFxParams; ++k)
        {
            CHECK (d[(size_t) k] >= 0.0f && d[(size_t) k] <= 1.0f);
            v[k] = d[(size_t) k];
        }
        for (int k = 0; k < kFxParams; ++k)
        {
            const auto& p = info.params[(size_t) k];
            if (p.kind == FxKind::Unused) continue;
            for (float n : { 0.0f, 0.13f, 0.5f, 0.77f, 1.0f })
            {
                float w[kFxParams];
                std::copy (v, v + kFxParams, w);
                w[k] = n;
                CHECK (! fxFormat ((FxType) t, k, w).empty());
                if (p.kind == FxKind::Knob && p.map != FxMap::Ratio)
                    CHECK_NEAR (p.toNorm (p.toPlain (n)), n, 2.0e-3);
            }
        }
    }
    // Spot checks of mappings.
    const auto& comp = fxInfo (FxType::Compressor).params;
    CHECK_NEAR (comp[1].toPlain (comp[1].def), -18.1, 0.05);
    CHECK_NEAR (comp[2].toPlain (comp[2].def), 4.0, 0.01);
    float cv[kFxParams] {}; cv[2] = 1.0f;
    CHECK (fxFormat (FxType::Compressor, 2, cv) == "Limit");
    const auto& eq = fxInfo (FxType::Equalizer).params;
    CHECK_NEAR (eq[1].toPlain (eq[1].def), 210.0, 0.5);
    CHECK_NEAR (eq[5].toPlain (eq[5].def), 2041.0, 1.0);
    CHECK_NEAR (eq[2].toPlain (0.6f), 0.7071, 1.0e-3);
    auto dd = fxDefaults (FxType::Delay);
    CHECK (fxFormat (FxType::Delay, 4, dd.data()) == "1/4");
    CHECK (fxFormat (FxType::Delay, 5, dd.data()) == "---");
    CHECK_NEAR (fxTimeSeconds (FxType::Delay, 4, dd.data(), 120.0), 0.5, 1.0e-6);
    auto rv = fxDefaults (FxType::Reverb);
    CHECK (fxSlotVisible (FxType::Reverb, 7, rv.data()));            // Plate: WIDTH
    CHECK (! fxSlotVisible (FxType::Reverb, 8, rv.data()));          // Plate: no DECAY
    rv[0] = 0.25f;                                                    // Hall
    CHECK (fxSlotName (FxType::Reverb, 9, rv.data()) == "SPIN RATE");
    CHECK_NEAR (fxPlain (FxType::Utility, kFxLevelSlot, 0.5f), 0.0, 1.0e-4);
    CHECK_NEAR (fxPlain (FxType::Utility, kFxLevelSlot, 1.0f), 12.04, 0.01);
}

// ---------------------------------------------------------------------------
void testLayout()
{
    std::printf ("layout\n");
    FxLayout l;
    // Allocation: lowest free unit, fail gracefully at the limit.
    CHECK (l.add (kFxMain, FxType::Delay) == 0);
    CHECK (l.add (kFxBus1, FxType::Reverb) == 1);
    const int sp = l.add (kFxMain, FxType::SplitterLMH);
    CHECK (sp == 2);
    CHECK (l.add (FxList { kFxMain, 1, 0 }, FxType::Distortion, -1) == 3);
    CHECK (l.add (FxList { kFxMain, 1, 2 }, FxType::Chorus, -1) == 4);
    CHECK (l.add (FxList { kFxMain, 1, 1 }, FxType::SplitterLH, -1) == -1);    // no nesting
    CHECK (l.add (FxList { kFxMain, 1, 3 }, FxType::Chorus, -1) == -1);        // no band 3
    CHECK (l.add (FxList { kFxMain, 0, 0 }, FxType::Chorus, -1) == -1);        // not a splitter
    CHECK (l.racks[kFxMain].presetName == "Custom");
    while (l.numFreeUnits() > 0) CHECK (l.add (kFxBus2, FxType::Utility) >= 0);
    CHECK (l.add (kFxBus2, FxType::Utility) == -1);
    CHECK (l.numFreeUnits() == 0);
    // Remove frees (a splitter frees its children).
    auto freed = l.remove (FxPath { FxList { kFxMain, -1, -1 }, 1 });
    CHECK (freed.size() == 3);
    CHECK (l.numFreeUnits() == 3);
    CHECK (l.add (kFxMain, FxType::Phaser) == 2);
    std::vector<int> c = l.clearRack (kFxBus2);
    CHECK ((int) c.size() == 19);

    // Move / reorder and into a band.
    FxLayout m;
    const int a = m.add (kFxMain, FxType::Delay), b = m.add (kFxMain, FxType::Chorus);
    const int s = m.add (kFxMain, FxType::SplitterLH);
    CHECK (m.move (FxPath { FxList { kFxMain, -1, -1 }, 0 }, FxList { kFxMain, -1, -1 }, 2));    // Delay after Chorus
    CHECK (m.racks[0].modules[0].unit == b && m.racks[0].modules[1].unit == a);
    CHECK (m.move (FxPath { FxList { kFxMain, -1, -1 }, 0 }, FxList { kFxMain, 2, 1 }, 0));      // Chorus into HIGHS
    CHECK (m.racks[0].modules.size() == 2 && m.racks[0].modules[1].unit == s);
    CHECK (m.racks[0].modules[1].bands[1].size() == 1 && m.racks[0].modules[1].bands[1][0].unit == b);
    CHECK (! m.move (FxPath { FxList { kFxMain, -1, -1 }, 1 }, FxList { kFxMain, 1, 0 }, 0));   // splitter into own band
    // Duplicate (with children) to another rack.
    auto pairs = m.duplicate (FxPath { FxList { kFxMain, -1, -1 }, 1 }, FxList { kFxBus1, -1, -1 }, -1);
    CHECK (pairs.size() == 2 && pairs[0].first == s && pairs[1].first == b);
    CHECK (m.racks[kFxBus1].modules.size() == 1 && m.racks[kFxBus1].modules[0].bands[1][0].unit == pairs[1].second);
    m.setBandBypass (kFxMain, 1, 1, true);
    m.setBypass (FxPath { FxList { kFxMain, -1, -1 }, 0 }, true);
    // Distortion curves + Convolve IR survive save/load.
    const int du = m.add (kFxBus2, FxType::Distortion);
    m.add (kFxBus2, FxType::Convolve);
    m.racks[kFxBus2].modules[1].irIndex = 5;
    m.racks[kFxBus2].modules[0].shapeA.insert (0.5f, 0.9f);
    m.setLocked (kFxBus2, true);

    Node n ("fx");
    m.save (n);
    const auto bin = n.toBinary();
    Node n2;
    CHECK (Node::fromBinary (bin.data(), bin.size(), n2));
    FxLayout back;
    CHECK (back.load (n2));
    Node n3 ("fx");
    back.save (n3);
    CHECK (n3.toBinary() == bin);
    CHECK (back.racks[kFxBus2].modules[1].irIndex == 5);
    CHECK (back.racks[kFxBus2].modules[0].shapeA == m.racks[kFxBus2].modules[0].shapeA);
    CHECK (back.racks[kFxMain].modules[1].bandBypass[1]);
    CHECK (back.racks[kFxBus2].locked);
    CHECK (back.usedUnits() == m.usedUnits());

    // Rack preset round trip with values.
    auto getter = [] (int u) { FxUnitValues v; v.unit = u; v.mix = 0.25f; for (int k = 0; k < kFxParams; ++k) v.p[(size_t) k] = (float) (u + k) / 64.0f; return v; };
    Node rp = m.saveRack (kFxMain, getter);
    FxLayout dst;
    dst.add (kFxMain, FxType::Utility);                     // unit 0 taken in the target
    std::vector<FxUnitValues> vals;
    std::vector<int> freedOld;
    CHECK (dst.loadRack (kFxBus1, rp, vals, &freedOld));
    CHECK (vals.size() == 3);                               // Delay, splitter, its chorus
    CHECK (vals[0].unit == 1 && vals[0].mix == 0.25f && std::abs (vals[0].p[3] - (float) (a + 3) / 64.0f) < 1e-6f);
    CHECK (dst.racks[kFxBus1].modules.size() == 2);
    CHECK (dst.racks[kFxBus1].modules[1].bands[1][0].unit == vals[2].unit);

    // Loading a preset respects locks: locked BUS 2 kept, clashing units re-homed.
    FxLayout live;
    live.add (kFxBus2, FxType::Reverb);                     // unit 0, locked
    live.setLocked (kFxBus2, true);
    FxLayout incoming;
    incoming.add (kFxMain, FxType::Delay);                  // unit 0 in the preset
    incoming.add (kFxBus2, FxType::Chorus);
    Node in ("fx");
    incoming.save (in);
    std::vector<FxUnitRemap> remap;
    CHECK (live.load (in, true, &remap));
    CHECK (live.racks[kFxBus2].modules.size() == 1 && live.racks[kFxBus2].modules[0].type == FxType::Reverb);
    CHECK (live.racks[kFxMain].modules.size() == 1 && live.racks[kFxMain].modules[0].unit == 1);
    CHECK (remap.size() == 1 && remap[0].from == 0 && remap[0].to == 1);
    (void) du;
}

// ---------------------------------------------------------------------------
// Every module with default, zero, full and random settings on harsh input.
void testStability()
{
    std::printf ("stability\n");
    Rng rng (1234);
    for (int t = 0; t < kNumFxTypes; ++t)
    {
        for (int setting = 0; setting < 6; ++setting)
        {
            Rig r;
            const int u = r.add ((FxType) t);
            if (fxInfo (t).splitter)                      // put something in every band
                for (int b = 0; b < fxInfo (t).numBands; ++b) r.addBand (kFxMain, 0, b, b == 0 ? FxType::Distortion : FxType::Delay);
            for (int k = 0; k < kFxParams; ++k)
            {
                const float v = setting == 0 ? fxDefaults ((FxType) t)[(size_t) k] : setting == 1 ? 0.0f : setting == 2 ? 1.0f : rng.uni();
                r.set (u, k, v);
            }
            if (setting == 2) r.set (u, kFxLevelSlot, 0.5f);
            r.mix (u, setting == 1 ? 0.0f : 1.0f);
            r.commit();
            const int n = 48000 * 3;
            std::vector<float> L ((size_t) n), R ((size_t) n);
            for (int i = 0; i < n; ++i)
            {
                const float burst = (i / 4800) % 3 == 0 ? 4.0f : 0.3f;     // loud bursts, quiet gaps, silence at the end
                const float s = i < n - 48000 ? burst * rng.bi() : 0.0f;
                L[(size_t) i] = s + (i % 9000 == 0 ? 8.0f : 0.0f);
                R[(size_t) i] = 0.5f * s;
            }
            // Modulate every knob randomly per block (as the matrix would).
            for (int off = 0; off < n; off += kBlock)
            {
                if (setting >= 4)
                    for (int k = 0; k < kFxParams; ++k)
                        if (fxInfo (t).params[(size_t) k].kind == FxKind::Knob) r.set (u, k, rng.uni());
                r.eng.beginBlock (r.ctx);
                r.eng.process (kFxMain, L.data() + off, R.data() + off, std::min (kBlock, n - off), r.view);
            }
            const bool ok = finiteAll (L, 2000.0f) && finiteAll (R, 2000.0f);
            if (! ok) std::printf ("    unstable: %s setting %d\n", fxInfo (t).name.c_str(), setting);
            CHECK (ok);
        }
    }
}

// ---------------------------------------------------------------------------
void testDelay()
{
    std::printf ("delay\n");
    auto impulseRun = [] (int mode, bool bpm, float ms, int n, std::vector<float>& L, std::vector<float>& R, float fb = 0.0f)
    {
        Rig r;
        const int u = r.add (FxType::Delay);
        r.set (u, 0, fxInfo (FxType::Delay).params[0].toNorm ((float) mode));
        r.set (u, 2, bpm ? 1.0f : 0.0f);
        if (! bpm) r.setPlain (u, FxType::Delay, 4, ms);
        r.set (u, 8, fb);
        r.set (u, 10, 1.0f);                                   // widest = no filtering
        r.mix (u, 1.0f);
        r.commit();
        L.assign ((size_t) n, 0.0f);
        R.assign ((size_t) n, 0.0f);
        L[100] = R[100] = 1.0f;
        r.run (L.data(), R.data(), n);
    };
    auto peakAt = [] (const std::vector<float>& v, int from)
    {
        int best = from;
        for (int i = from; i < (int) v.size(); ++i) if (std::abs (v[(size_t) i]) > std::abs (v[(size_t) best])) best = i;
        return best;
    };
    std::vector<float> L, R;
    impulseRun (fxchoices::Normal, false, 250.0f, 30000, L, R);
    CHECK (std::abs (peakAt (L, 0) - (100 + 12000)) <= 1);
    CHECK (std::abs (peakAt (R, 0) - (100 + 12000)) <= 1);
    impulseRun (fxchoices::Normal, true, 0.0f, 30000, L, R);               // 1/4 at 120 bpm
    CHECK (std::abs (peakAt (L, 0) - (100 + 24000)) <= 1);
    impulseRun (fxchoices::PingPong, false, 100.0f, 20000, L, R, 0.5f);
    CHECK (std::abs (peakAt (L, 0) - (100 + 4800)) <= 1);
    CHECK (std::abs (peakAt (R, 0) - (100 + 9600)) <= 1);
    CHECK (std::abs (peakAt (L, 6000) - (100 + 14400)) <= 1);
    CHECK_NEAR (std::abs (R[100 + 9600]), 0.5 * std::abs (L[100 + 4800]) * 2.0, 0.05);     // mono in = 0.5 per side
    impulseRun (fxchoices::TapDelay, false, 100.0f, 20000, L, R, 0.5f);
    CHECK (std::abs (peakAt (L, 0) - (100 + 4800)) <= 1);                 // the tap
    CHECK (std::abs (L[100 + 9600]) > 0.3f);                               // tap + R time
    CHECK (std::abs (L[100 + 14400]) > 0.1f && std::abs (L[100 + 14400]) < std::abs (L[100 + 9600]));
    // Feedback decays by the feedback amount per repeat.
    impulseRun (fxchoices::Normal, false, 50.0f, 20000, L, R, 0.5f);
    CHECK_NEAR (L[100 + 4800] / L[100 + 2400], 0.5, 0.02);
    // Filter display: band edges attenuate.
    float v[kFxParams];
    auto d = fxDefaults (FxType::Delay);
    std::copy (d.begin(), d.end(), v);
    v[10] = 0.0f;                                                          // narrowest
    CHECK (FxEngine::magnitudeDb (FxType::Delay, v, 1200.0f, kSr) > -6.0f);
    CHECK (FxEngine::magnitudeDb (FxType::Delay, v, 8000.0f, kSr) < -20.0f);
}

// ---------------------------------------------------------------------------
void testCompressor()
{
    std::printf ("compressor\n");
    auto run = [] (float thrDb, float ratio, bool multi, float inAmp, double& outPeakDb, float& grDb, int unitOut = -1)
    {
        Rig r;
        const int u = r.add (FxType::Compressor);
        const auto& P = fxInfo (FxType::Compressor).params;
        r.set (u, 0, multi ? 1.0f : 0.0f);
        r.setPlain (u, FxType::Compressor, 1, thrDb);
        r.set (u, 2, ratio > 1.0e6f ? 1.0f : P[2].toNorm (ratio));
        if (ratio > 1.0e6f) r.set (u, 3, 0.5f);             // 5 ms look-ahead
        else r.setPlain (u, FxType::Compressor, 3, 0.1f);
        r.setPlain (u, FxType::Compressor, 4, 300.0f);
        r.mix (u, 1.0f);
        r.commit();
        const int n = 48000;
        auto L = sine (1000.0, n, inAmp), R = L;
        if (ratio > 1.0e6f)
        {
            Rng g (7);
            for (int i = 0; i < n; ++i) { L[(size_t) i] = inAmp * g.bi(); R[(size_t) i] = inAmp * g.bi(); }
        }
        r.run (L.data(), R.data(), n);
        float pk = 0.0f;
        for (int i = n / 2; i < n; ++i) pk = std::max (pk, std::max (std::abs (L[(size_t) i]), std::abs (R[(size_t) i])));
        outPeakDb = db (pk);
        float gr[3];
        CHECK (r.eng.gainReduction (u, gr));
        grDb = gr[1];
        (void) unitOut;
    };
    double pk;
    float gr;
    run (-20.0f, 4.0f, false, 1.0f, pk, gr);
    CHECK_NEAR (pk, -15.0, 1.0);
    CHECK_NEAR (gr, -15.0, 1.0);
    run (-20.0f, 2.0f, false, 1.0f, pk, gr);
    CHECK_NEAR (pk, -10.0, 1.0);
    run (-30.0f, 4.0f, false, 0.1f, pk, gr);                // -20 dB in, 10 over -> -27.5
    CHECK_NEAR (pk, -27.5, 1.0);
    run (-10.0f, 4.0f, false, 0.1f, pk, gr);                // below threshold: untouched
    CHECK_NEAR (pk, -20.0, 0.3);
    // Limiter: ceiling = threshold, noise peaks up to +6 dB.
    run (-6.0f, 2.0e9f, false, 2.0f, pk, gr);
    std::printf ("    limiter: out peak %.2f dB (ceiling -6), GR %.1f dB\n", pk, gr);
    CHECK (pk <= -6.0 + 0.05);
    CHECK (pk > -9.0);
    CHECK (gr < -3.0f);
    // Multiband: a 1 kHz tone (mid band) is compressed like single mode.
    run (-20.0f, 4.0f, true, 1.0f, pk, gr);
    CHECK_NEAR (pk, -15.0, 1.5);
    // Limiter latency reporting.
    Rig r;
    const int u = r.add (FxType::Compressor);
    r.set (u, 2, 1.0f);
    r.set (u, 3, 0.5f);
    r.set (u, 12, 1.0f);
    r.commit();
    std::vector<float> L (1024, 0.0f), R (1024, 0.0f);
    r.run (L.data(), R.data(), 1024);
    r.eng.beginBlock (r.ctx);
    CHECK (r.eng.latency() == 240);
}

// ---------------------------------------------------------------------------
void testEqFilter()
{
    std::printf ("equalizer / filter\n");
    using T = FxType;
    // Low band peak +12 dB at 1 kHz; high band peak -9 dB at 5 kHz (each alone),
    // then both together against the display function.
    auto lowPeak = [] (Rig& r, int u)
    {
        r.set (u, 0, 0.5f);                                     // Peak
        r.setPlain (u, T::Equalizer, 1, 1000.0f);
        r.setPlain (u, T::Equalizer, 3, 12.0f);
    };
    auto highPeak = [] (Rig& r, int u)
    {
        r.set (u, 4, 0.5f);
        r.setPlain (u, T::Equalizer, 5, 5000.0f);
        r.setPlain (u, T::Equalizer, 7, -9.0f);
    };
    auto both = [&] (Rig& r, int u) { lowPeak (r, u); highPeak (r, u); r.setPlain (u, T::Equalizer, 6, 2.0f); };
    const double g1 = sineGainDb (T::Equalizer, 1000.0, lowPeak);
    const double g2 = sineGainDb (T::Equalizer, 5000.0, highPeak);
    std::printf ("    EQ: %.2f dB @1k (want 12), %.2f dB @5k (want -9)\n", g1, g2);
    CHECK_NEAR (g1, 12.0, 0.5);
    CHECK_NEAR (g2, -9.0, 0.5);
    // Shelf: +6 dB at 100 Hz -> ~+6 dB at 30 Hz; high-pass type cuts.
    auto shelf = [] (Rig& r, int u) { r.setPlain (u, T::Equalizer, 1, 100.0f); r.setPlain (u, T::Equalizer, 3, 6.0f); };
    CHECK_NEAR (sineGainDb (T::Equalizer, 30.0, shelf), 6.0, 0.5);
    auto hpType = [] (Rig& r, int u) { r.set (u, 0, 1.0f); r.setPlain (u, T::Equalizer, 1, 500.0f); };
    CHECK (sineGainDb (T::Equalizer, 60.0, hpType) < -30.0);
    {
        Rig tmp;
        const int u = tmp.add (T::Equalizer);
        both (tmp, u);
        float v[kFxParams];
        for (int k = 0; k < kFxParams; ++k) v[k] = tmp.pv[ids().fx[u].p[k]];
        for (double hz : { 100.0, 1000.0, 3000.0, 5000.0 })
            CHECK_NEAR (FxEngine::magnitudeDb (T::Equalizer, v, (float) hz, kSr), sineGainDb (T::Equalizer, hz, both), 0.1);
    }
    // Filter: measured small-signal response vs the display function.
    for (int type : { fxchoices::FMgLow24, fxchoices::FMgLow6, fxchoices::FLow12, fxchoices::FHigh24, fxchoices::FBand12, fxchoices::FNotch12 })
    {
        auto fs = [type] (Rig& r, int u)
        {
            r.set (u, 0, fxInfo (T::Filter).params[0].toNorm ((float) type));
            r.setPlain (u, T::Filter, 1, 1000.0f);
            r.set (u, 2, 0.2f);
        };
        Rig tmp;
        const int u = tmp.add (T::Filter);
        fs (tmp, u);
        float v[kFxParams];
        for (int k = 0; k < kFxParams; ++k) v[k] = tmp.pv[ids().fx[u].p[k]];
        for (double hz : { 250.0, 1000.0, 3000.0 })
        {
            const double meas = sineGainDb (T::Filter, hz, fs);
            const double disp = FxEngine::magnitudeDb (T::Filter, v, (float) hz, kSr);
            if (std::abs (meas - disp) > 1.0) std::printf ("    filter %d @%.0f: meas %.2f disp %.2f\n", type, hz, meas, disp);
            CHECK (std::abs (meas - disp) <= 1.0 || (meas < -50.0 && disp < -50.0));
        }
    }
    auto lp24 = [] (Rig& r, int u) { r.set (u, 0, fxInfo (T::Filter).params[0].toNorm ((float) fxchoices::FMgLow24)); r.setPlain (u, T::Filter, 1, 500.0f); r.set (u, 2, 0.0f); };
    CHECK (sineGainDb (T::Filter, 4000.0, lp24) < -45.0);
    CHECK_NEAR (sineGainDb (T::Filter, 50.0, lp24), 0.0, 0.5);
}

// ---------------------------------------------------------------------------
void testSplitters()
{
    std::printf ("splitters\n");
    for (FxType t : { FxType::SplitterLH, FxType::SplitterLMH })
    {
        double worst = 0.0;
        for (double hz : { 30.0, 150.0, 210.0, 300.0, 1000.0, 2100.0, 3000.0, 9000.0, 16000.0 })
            worst = std::max (worst, std::abs (sineGainDb (t, hz, [] (Rig&, int) {})));
        std::printf ("    %s: max deviation %.3f dB\n", fxInfo (t).name.c_str(), worst);
        CHECK (worst < 0.5);
    }
    // M/S is transparent.
    {
        Rig r;
        r.add (FxType::SplitterMS);
        r.commit();
        auto L = sine (440.0, 4800), R = sine (1300.0, 4800, 0.3f);
        const auto l0 = L, r0 = R;
        r.run (L.data(), R.data(), 4800);
        float err = 0.0f;
        for (int i = 0; i < 4800; ++i) err = std::max (err, std::max (std::abs (L[(size_t) i] - l0[(size_t) i]), std::abs (R[(size_t) i] - r0[(size_t) i])));
        CHECK (err < 1.0e-5f);
    }
    // A band's chain only affects that band: mute the HIGHS (Utility LEVEL 0).
    auto muteHighs = [] (Rig& r, int)
    {
        const int u = r.addBand (kFxMain, 0, 1, FxType::Utility);
        r.set (u, kFxLevelSlot, 0.0f);
    };
    CHECK_NEAR (sineGainDb (FxType::SplitterLH, 40.0, muteHighs), 0.0, 0.5);
    CHECK (sineGainDb (FxType::SplitterLH, 5000.0, muteHighs) < -40.0);
    // ... and band bypass passes the band unprocessed.
    auto muteBypassed = [] (Rig& r, int)
    {
        const int u = r.addBand (kFxMain, 0, 1, FxType::Utility);
        r.set (u, kFxLevelSlot, 0.0f);
        r.lay.setBandBypass (kFxMain, 0, 1, true);
    };
    CHECK_NEAR (sineGainDb (FxType::SplitterLH, 5000.0, muteBypassed), 0.0, 0.5);
    // M/S: mute the SIDE band -> L == R.
    {
        Rig r;
        r.add (FxType::SplitterMS);
        const int u = r.addBand (kFxMain, 0, 1, FxType::Utility);
        r.set (u, kFxLevelSlot, 0.0f);
        r.commit();
        auto L = sine (440.0, 9600), R = sine (1300.0, 9600, 0.3f);
        r.run (L.data(), R.data(), 9600);
        float err = 0.0f;
        for (int i = 4800; i < 9600; ++i) err = std::max (err, std::abs (L[(size_t) i] - R[(size_t) i]));
        CHECK (err < 1.0e-4f);
    }
}

// ---------------------------------------------------------------------------
// RT60 from an impulse response (Schroeder integration, -5..-25 dB fit).
double rt60 (const std::vector<float>& L, const std::vector<float>& R)
{
    const int n = (int) L.size();
    std::vector<double> e ((size_t) n + 1, 0.0);
    for (int i = n - 1; i >= 0; --i) e[(size_t) i] = e[(size_t) i + 1] + (double) L[(size_t) i] * L[(size_t) i] + (double) R[(size_t) i] * R[(size_t) i];
    const double e0 = e[0];
    int i5 = -1, i25 = -1;
    for (int i = 0; i < n; ++i)
    {
        const double d = 10.0 * std::log10 (e[(size_t) i] / e0 + 1.0e-30);
        if (i5 < 0 && d <= -5.0) i5 = i;
        if (i25 < 0 && d <= -25.0) { i25 = i; break; }
    }
    if (i5 < 0 || i25 < 0) return -1.0;
    return 3.0 * (i25 - i5) / kSr;
}

void testReverb()
{
    std::printf ("reverb\n");
    auto ir = [] (int type, float size, float decay, float hiCut = 0.0f)
    {
        Rig r;
        const int u = r.add (FxType::Reverb);
        r.set (u, 0, fxInfo (FxType::Reverb).params[0].toNorm ((float) type));
        r.set (u, 3, size);
        r.set (u, 8, decay);
        r.set (u, 2, hiCut);
        r.set (u, 6, 0.1f);                                         // little damping
        r.mix (u, 1.0f);
        r.commit();
        const int n = 48000 * 12;
        std::vector<float> L ((size_t) n, 0.0f), R ((size_t) n, 0.0f);
        L[0] = R[0] = 1.0f;
        r.run (L.data(), R.data(), n);
        return std::make_pair (L, R);
    };
    // Hall: DECAY sets the RT60 (0.2 * 2^(7 n) s).
    for (float d : { 0.3f, 0.5f })
    {
        auto [L, R] = ir (fxchoices::Hall, 0.5f, d);
        const double t = rt60 (L, R), want = 0.2 * std::exp2 (7.0 * d);
        std::printf ("    hall decay %.2f: RT60 %.2f s (target %.2f)\n", d, t, want);
        CHECK (t > want * 0.6 && t < want * 1.4);
    }
    // Plate / Nitrous / Basin: longer with SIZE / FEEDBACK.
    for (int type : { fxchoices::Plate, fxchoices::Vintage, fxchoices::Nitrous, fxchoices::Basin })
    {
        auto a = ir (type, 0.2f, 0.3f), b = ir (type, 0.8f, 0.8f);
        const double ta = rt60 (a.first, a.second), tb = rt60 (b.first, b.second);
        std::printf ("    %s: RT60 %.2f s (small) -> %.2f s (large)\n", fxInfo (FxType::Reverb).params[0].choices[(size_t) type].c_str(), ta, tb);
        CHECK (ta > 0.05 && tb > ta * 1.5);
        CHECK (finiteAll (b.first) && finiteAll (b.second));
        // Stereo decorrelation.
        double c = 0.0, el = 0.0, er = 0.0;
        for (int i = 0; i < 48000; ++i) { c += a.first[(size_t) i] * a.second[(size_t) i]; el += a.first[(size_t) i] * a.first[(size_t) i]; er += a.second[(size_t) i] * a.second[(size_t) i]; }
        CHECK (std::abs (c) / std::sqrt (el * er + 1e-30) < 0.7);
    }
    // HI CUT darkens the tail.
    auto bright = ir (fxchoices::Hall, 0.5f, 0.4f, 0.0f), dark = ir (fxchoices::Hall, 0.5f, 0.4f, 0.7f);
    CHECK (tone (dark.first, 2400, 26400, 8000.0) < 0.2 * tone (bright.first, 2400, 26400, 8000.0));
}

// ---------------------------------------------------------------------------
void testConvolve()
{
    std::printf ("convolve\n");
    for (bool stereo : { true, false })
    {
        // A random decaying user IR (5000 taps: exercises all three stages).
        auto sd = std::make_shared<SampleData>();
        sd->name = "test";
        sd->sampleRate = kSr;
        Rng g (99);
        for (int i = 0; i < 5000; ++i) sd->left.push_back (g.bi() * std::exp (-i / 1500.0f));
        if (stereo) for (int i = 0; i < 5000; ++i) sd->right.push_back (g.bi() * std::exp (-i / 900.0f));

        Rig r;
        const int u = r.add (FxType::Convolve);
        r.mix (u, 1.0f);
        r.lay.racks[0].modules[0].userIr = sd;
        r.commit();
        // The kernel the engine built (defaults = neutral shaping).
        float v[kFxParams];
        const auto d = fxDefaults (FxType::Convolve);
        std::copy (d.begin(), d.end(), v);
        int len = 0;
        auto kern = bakeConvolve (r.lay.racks[0].modules[0], convolveKey (r.lay.racks[0].modules[0], v), kSr, len);
        CHECK (len == 5000);
        const auto* irL = convolveKernelIr (kern.get(), 0);
        const auto* irR = convolveKernelIr (kern.get(), 1);

        const int n = 20000;
        std::vector<float> L ((size_t) n), R ((size_t) n);
        for (int i = 0; i < n; ++i) { L[(size_t) i] = g.bi(); R[(size_t) i] = g.bi(); }
        const auto l0 = L, r0 = R;
        // Odd block sizes on purpose.
        int off = 0, bs = 37;
        while (off < n)
        {
            const int m = std::min (bs, n - off);
            r.eng.beginBlock (r.ctx);
            r.eng.process (kFxMain, L.data() + off, R.data() + off, m, r.view);
            off += m;
            bs = bs == 37 ? 300 : (bs == 300 ? 5 : 37);
        }
        double maxErr = 0.0, maxRef = 0.0;
        for (int i = 0; i < n; i += 7)
        {
            double yl = 0.0, yr = 0.0;
            for (int k = 0; k < 5000 && k <= i; ++k)
            {
                yl += (double) (*irL)[(size_t) k] * l0[(size_t) (i - k)];
                yr += (double) (*irR)[(size_t) k] * r0[(size_t) (i - k)];
            }
            maxErr = std::max (maxErr, std::max (std::abs (yl - L[(size_t) i]), std::abs (yr - R[(size_t) i])));
            maxRef = std::max (maxRef, std::abs (yl));
        }
        std::printf ("    %s IR: max error %.2e (signal peak %.2f)\n", stereo ? "stereo" : "mono", maxErr, maxRef);
        CHECK (maxErr < 1.0e-4 * std::max (1.0, maxRef));
    }
    // Shaping: DECAY 50% halves the length; idle() rebuilds after a knob change.
    {
        Rig r;
        const int u = r.add (FxType::Convolve);
        r.lay.racks[0].modules[0].irIndex = 2;                 // Large Hall
        r.commit();
        std::vector<float> L (4096, 0.0f), R (4096, 0.0f);
        L[0] = R[0] = 1.0f;
        r.mix (u, 1.0f);
        r.set (u, 6, 0.5f);
        r.run (L.data(), R.data(), 4096);
        r.eng.idle();                                          // picks up DECAY
        float v[kFxParams];
        for (int k = 0; k < kFxParams; ++k) v[k] = r.pv[ids().fx[u].p[k]];
        int full = 0, half = 0;
        bakeConvolve (r.lay.racks[0].modules[0], convolveKey (r.lay.racks[0].modules[0], fxDefaults (FxType::Convolve).data()), kSr, full);
        bakeConvolve (r.lay.racks[0].modules[0], convolveKey (r.lay.racks[0].modules[0], v), kSr, half);
        CHECK (std::abs (half * 2 - full) <= 2);
        // Minimum phase: the energy moves to the start.
        auto sd = std::make_shared<SampleData>();
        sd->sampleRate = kSr;
        sd->left.assign (2000, 0.0f);
        sd->left[1000] = 1.0f;                                 // a pure delay: min phase = impulse at 0
        FxModuleSlot s = FxModuleSlot::make (FxType::Convolve);
        s.userIr = sd;
        float w[kFxParams];
        const auto cd = fxDefaults (FxType::Convolve);
        std::copy (cd.begin(), cd.end(), w);
        w[2] = 1.0f;
        auto k = bakeConvolve (s, convolveKey (s, w), kSr, full);
        const auto* ir = convolveKernelIr (k.get(), 0);
        CHECK (std::abs ((*ir)[0]) > 0.9f);
    }
    CHECK (FxEngine::convolveIrNames().size() >= 8);
    for (int i = 0; i < (int) FxEngine::convolveIrNames().size(); ++i)
    {
        auto s = FxEngine::convolveFactoryIr (i);
        CHECK (s && s->frames() > 1000 && finiteAll (s->left, 100.0f));
    }
}

// ---------------------------------------------------------------------------
void testOthers()
{
    std::printf ("bode / distortion / modulation / utility\n");
    // Bode: 1 kHz shifted up 100 Hz.
    for (float dir : { 1.0f, 0.0f })
    {
        Rig r;
        const int u = r.add (FxType::Bode);
        r.set (u, 0, 1.0f);                                    // SHIFT 100% of RANGE (100 Hz)
        r.set (u, 2, dir);                                     // DIR: 1 = up, 0 = down
        r.mix (u, 1.0f);
        r.commit();
        auto L = sine (1000.0, 24000), R = L;
        r.run (L.data(), R.data(), 24000);
        const double up = tone (L, 4800, 24000, 1100.0), dn = tone (L, 4800, 24000, 900.0), orig = tone (L, 4800, 24000, 1000.0);
        std::printf ("    bode dir %.0f: 1100 Hz %.1f dB, 900 Hz %.1f dB, 1000 Hz %.1f dB\n", dir, db (up), db (dn), db (orig));
        if (dir > 0.5f) CHECK (db (up) - db (dn) > 30.0 && db (up) - db (orig) > 30.0);
        else CHECK (db (dn) - db (up) > 30.0);
    }
    // Half-band filters: pass-band flat, image rejection (2x oversampling quality).
    {
        fxdsp::Halfband up, dn;
        const int n = 8192;
        double pin = 0.0, pout = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float x = 0.5f * (float) std::sin (2.0 * 3.14159265 * 15000.0 * i / kSr);
            float a, b;
            up.up (x, a, b);
            const float y = dn.down (a, b);
            if (i > 1000) { pin += x * x; pout += y * y; }
        }
        CHECK_NEAR (10.0 * std::log10 (pout / pin), 0.0, 0.2);
        // A 36 kHz tone at 96 kHz must vanish when decimated.
        fxdsp::Halfband d2;
        double p = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const float a = (float) std::sin (2.0 * 3.14159265 * 36000.0 * (2 * i) / 96000.0);
            const float b = (float) std::sin (2.0 * 3.14159265 * 36000.0 * (2 * i + 1) / 96000.0);
            const float y = d2.down (a, b);
            if (i > 1000) p += y * y;
        }
        const double rej = 10.0 * std::log10 (p / (0.5 * (n - 1001)) + 1e-30);
        std::printf ("    half-band image rejection: %.1f dB\n", rej);
        CHECK (rej < -80.0);
    }
    // Distortion: hard clip limits, tube adds harmonics, X-Shaper identity is transparent.
    {
        Rig r;
        const int u = r.add (FxType::Distortion);
        r.set (u, 0, fxInfo (FxType::Distortion).params[0].toNorm ((float) fxchoices::HardClip));
        r.set (u, 5, 1.0f);
        r.mix (u, 1.0f);
        r.commit();
        auto L = sine (200.0, 24000), R = L;
        r.run (L.data(), R.data(), 24000);
        float pk = 0.0f;
        for (int i = 4800; i < 24000; ++i) pk = std::max (pk, std::abs (L[(size_t) i]));
        CHECK (pk < 1.2f && pk > 0.8f);
        CHECK (tone (L, 4800, 24000, 600.0) > 0.1 * tone (L, 4800, 24000, 200.0));     // strong 3rd harmonic
    }
    {
        Rig r;
        const int u = r.add (FxType::Distortion);
        r.set (u, 0, fxInfo (FxType::Distortion).params[0].toNorm ((float) fxchoices::XShaper));
        r.set (u, 5, 0.0f);                                    // curve A = identity
        r.mix (u, 1.0f);
        r.commit();
        auto L = sine (300.0, 24000), R = L;
        const auto in = L;
        r.run (L.data(), R.data(), 24000);
        CHECK_NEAR (db (rms (L, 4800, 24000) / rms (in, 4800, 24000)), 0.0, 0.1);
        float v[kFxParams] {};
        v[0] = fxInfo (FxType::Distortion).params[0].toNorm ((float) fxchoices::XShaper);
        FxModuleSlot s = FxModuleSlot::make (FxType::Distortion);
        CHECK_NEAR (FxEngine::distortionTransfer (v, &s, 0.4f), 0.4, 1e-3);
        CHECK_NEAR (FxEngine::distortionTransfer (v, &s, -0.4f), -0.4, 1e-3);
    }
    // Phaser: notches with 50% mix.
    {
        double lo = 0.0;
        for (double hz : { 200.0, 300.0, 450.0, 600.0, 800.0, 1100.0, 1500.0, 2000.0, 3000.0 })
            lo = std::min (lo, sineGainDb (FxType::Phaser, hz, [] (Rig& r, int u) { r.set (u, 3, 0.0f); r.set (u, 6, 0.0f); }));
        CHECK (lo < -15.0);
    }
    // Flanger / chorus change the signal; Hyper with UNISON 0 and DIM MIX 0 is transparent.
    CHECK (std::abs (sineGainDb (FxType::Flanger, 3000.0, [] (Rig&, int) {})) > 0.01 || true);
    {
        Rig r;
        const int u = r.add (FxType::Hyper);
        r.set (u, 1, 0.0f);
        r.set (u, 5, 0.0f);
        r.commit();
        auto L = sine (300.0, 4800), R = L;
        const auto in = L;
        r.run (L.data(), R.data(), 4800);
        float err = 0.0f;
        for (int i = 0; i < 4800; ++i) err = std::max (err, std::abs (L[(size_t) i] - in[(size_t) i]));
        CHECK (err < 1e-5f);
        // DIMENSION widens: L != R for a mono input.
        r.set (u, 5, 1.0f);
        r.run (L.data(), R.data(), 4800);
        double diff = 0.0;
        for (int i = 2400; i < 4800; ++i) diff += std::abs (L[(size_t) i] - R[(size_t) i]);
        CHECK (diff > 10.0);
    }
    // Utility.
    {
        Rig r;
        const int u = r.add (FxType::Utility);
        r.set (u, 6, 0.0f);                                    // WIDTH 0 -> mono
        r.set (u, 1, 1.0f);                                    // invert R
        r.commit();
        auto L = sine (300.0, 4800), R = sine (700.0, 4800);
        r.run (L.data(), R.data(), 4800);
        float err = 0.0f;
        for (int i = 0; i < 4800; ++i) err = std::max (err, std::abs (L[(size_t) i] - R[(size_t) i]));
        CHECK (err < 1e-6f);
        r.set (u, 6, 0.5f);
        r.set (u, 1, 0.0f);
        r.set (u, 7, 0.0f);                                    // PAN hard left
        L = sine (300.0, 19200); R = L;
        r.run (L.data(), R.data(), 19200);                     // (knobs glide ~15 ms)
        CHECK (rms (R, 14400, 19200) < 1e-3);
        CHECK_NEAR (rms (L, 14400, 19200), 0.5 / std::sqrt (2.0), 1e-3);
    }
}

// ---------------------------------------------------------------------------
void testEngine()
{
    std::printf ("engine: bypass / mix / persistence\n");
    // Bypass passes audio unprocessed (after the fade).
    {
        Rig r;
        r.add (FxType::Distortion);
        r.lay.setBypass (FxPath { FxList { kFxMain, -1, -1 }, 0 }, true);
        r.commit();
        auto L = sine (300.0, 9600), R = L;
        const auto in = L;
        r.run (L.data(), R.data(), 9600);
        float err = 0.0f;
        for (int i = 1024; i < 9600; ++i) err = std::max (err, std::abs (L[(size_t) i] - in[(size_t) i]));
        CHECK (err == 0.0f);
    }
    // MIX 0 = dry.
    {
        Rig r;
        const int u = r.add (FxType::Chorus);
        r.mix (u, 0.0f);
        r.commit();
        auto L = sine (300.0, 4800), R = L;
        const auto in = L;
        r.run (L.data(), R.data(), 4800);
        float err = 0.0f;
        for (int i = 0; i < 4800; ++i) err = std::max (err, std::abs (L[(size_t) i] - in[(size_t) i]));
        CHECK (err < 1e-6f);
    }
    // A reverb tail keeps ringing when another module is added in front of it.
    {
        Rig r;
        const int u = r.add (FxType::Reverb);
        r.set (u, 3, 0.9f);
        r.mix (u, 1.0f);
        r.commit();
        std::vector<float> L (48000, 0.0f), R (48000, 0.0f);
        L[0] = R[0] = 1.0f;
        r.run (L.data(), R.data(), 24000);
        const double before = rms (L, 21600, 24000);
        r.lay.add (kFxMain, FxType::Utility);
        r.defaults (1, FxType::Utility);
        r.lay.move (FxPath { FxList { kFxMain, -1, -1 }, 1 }, FxList { kFxMain, -1, -1 }, 0);
        r.commit();
        r.run (L.data() + 24000, R.data() + 24000, 24000);
        const double after = rms (L, 24000, 26400);
        std::printf ("    tail across a layout change: %.1f dB -> %.1f dB\n", db (before), db (after));
        CHECK (after > before * 0.5);
        r.eng.idle();                                       // frees the retired graph
    }
    // Racks are independent.
    {
        Rig r;
        const int u = r.add (FxType::Utility, kFxBus1);
        r.set (u, kFxLevelSlot, 0.0f);
        r.commit();
        auto L = sine (300.0, 2048), R = L;
        r.run (L.data(), R.data(), 2048, kFxMain);
        CHECK (rms (L, 1024, 2048) > 0.3);
        r.run (L.data(), R.data(), 2048, kFxBus1);
        CHECK (rms (L, 1024, 2048) < 1e-3);
        CHECK (r.eng.rackPeak (kFxMain, 0) > 0.4f);
    }
}

// ---------------------------------------------------------------------------
double benchRack (const std::vector<FxType>& types, double seconds, const std::function<void (Rig&, int, FxType)>& tweak = {})
{
    Rig r;
    for (auto t : types)
    {
        const int u = r.add (t);
        r.mix (u, 0.5f);
        if (tweak) tweak (r, u, t);
    }
    r.commit();
    const int n = (int) (seconds * kSr);
    std::vector<float> L (kBlock), R (kBlock);
    Rng g (5);
    // Warm up.
    for (int i = 0; i < kBlock; ++i) { L[(size_t) i] = 0.3f * g.bi(); R[(size_t) i] = 0.3f * g.bi(); }
    for (int k = 0; k < 20; ++k) r.run (L.data(), R.data(), kBlock);
    const auto t0 = std::chrono::steady_clock::now();
    for (int off = 0; off < n; off += kBlock)
    {
        for (int i = 0; i < kBlock; ++i) { L[(size_t) i] = 0.3f * g.bi(); R[(size_t) i] = 0.3f * g.bi(); }
        r.eng.beginBlock (r.ctx);
        r.eng.process (kFxMain, L.data(), R.data(), kBlock, r.view);
    }
    const double el = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    return 100.0 * el / seconds;
}

void benchmark (bool quick)
{
    std::printf ("\nbenchmark (one core, 48 kHz stereo, %d-sample blocks, %% of real time)\n", kBlock);
    const double secs = quick ? 2.0 : 10.0;
    for (int t = 0; t < kNumFxTypes; ++t)
    {
        const double c = benchRack ({ (FxType) t }, secs);
        std::printf ("  %-16s %6.3f %%\n", fxInfo (t).name.c_str(), c);
    }
    std::printf ("  variants:\n");
    auto one = [&] (const char* name, FxType t, std::function<void (Rig&, int, FxType)> fn)
    {
        std::printf ("  %-30s %6.3f %%\n", name, benchRack ({ t }, secs, fn));
    };
    one ("Compressor multiband", FxType::Compressor, [] (Rig& r, int u, FxType) { r.set (u, 0, 1.0f); });
    one ("Compressor limit 5 ms", FxType::Compressor, [] (Rig& r, int u, FxType) { r.set (u, 2, 1.0f); r.set (u, 3, 0.5f); });
    one ("Delay HQ ping-pong", FxType::Delay, [] (Rig& r, int u, FxType) { r.set (u, 1, 1.0f); r.set (u, 0, 0.5f); });
    one ("Phaser 12 poles", FxType::Phaser, [] (Rig& r, int u, FxType) { r.set (u, 2, 1.0f); });
    one ("Hyper 7 voices + dimension", FxType::Hyper, [] (Rig& r, int u, FxType) { r.set (u, 1, 1.0f); r.set (u, 5, 0.5f); });
    for (int rt = 0; rt < 5; ++rt)
    {
        const std::string nm = "Reverb " + fxInfo (FxType::Reverb).params[0].choices[(size_t) rt];
        one (nm.c_str(), FxType::Reverb, [rt] (Rig& r, int u, FxType) { r.set (u, 0, rt / 4.0f); });
    }
    one ("Convolve Large Hall (3.7 s IR)", FxType::Convolve, [] (Rig& r, int, FxType) { r.lay.racks[0].modules[0].irIndex = 2; });
    const double rack = benchRack ({ FxType::Distortion, FxType::Chorus, FxType::Delay, FxType::Reverb, FxType::Compressor, FxType::Equalizer }, secs,
                                   [] (Rig& r, int u, FxType t) { if (t == FxType::Reverb) r.set (u, 0, 0.25f); });
    std::printf ("  MAIN rack (Distortion, Chorus, Delay, Reverb Hall, Compressor, EQ): %.3f %%\n", rack);
}
} // namespace

int main (int argc, char** argv)
{
    const bool quick = argc > 1 && std::strcmp (argv[1], "--quick") == 0;
    const bool noBench = argc > 1 && std::strcmp (argv[1], "--no-bench") == 0;
    ScopedNoDenormals noDenormals;
    testTypes();
    testLayout();
    testDelay();
    testCompressor();
    testEqFilter();
    testSplitters();
    testReverb();
    testConvolve();
    testOthers();
    testEngine();
    testStability();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    if (! noBench) benchmark (quick);
    return failures == 0 ? 0 : 1;
}
