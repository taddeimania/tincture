// EngineModTests.cpp — tests and benchmark for the modulation system:
// envelopes, LFOs (+ shape library), the mod matrix and the ModVoice /
// ModGlobal runtime. Returns non-zero on failure.
#include "Engine/ModMatrix.h"

#include <chrono>
#include <cstdio>
#include <iterator>

using namespace tincture;

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++checks; const double va_ = (a), vb_ = (b); if (! (std::abs (va_ - vb_) <= (tol))) { ++failures; \
    std::printf ("  FAIL %s:%d  %s = %g, expected %g (tol %g)\n", __FILE__, __LINE__, #a, va_, vb_, (double) (tol)); } } while (0)

constexpr float kSr = 48000.0f;

// A ParamView over plain values with nothing modulated.
struct PlainView
{
    ParamValues pv;
    std::vector<int16_t> noSlots;
    PlainView() { pv.resetToDefaults(); noSlots.assign ((size_t) params().size(), (int16_t) -1); }
    ParamView view() const { return { pv.v.data(), noSlots.data(), nullptr, nullptr }; }
};

// ---------------------------------------------------------------------------
// Envelopes
// ---------------------------------------------------------------------------
void testEnvelopes()
{
    std::printf ("envelopes\n");
    EnvSettings s;
    s.attack = 0.01f; s.hold = 0.02f; s.decay = 0.1f; s.sustain = dbToGain (-6.0f); s.release = 0.05f;
    s.attackCurve = 0.0f;

    // Stage timing: find the sample where each stage ends, at tick resolution.
    Envelope e;
    e.reset();
    e.trigger();
    int samples = 0, attackEnd = -1, holdEnd = -1, decayEnd = -1;
    float peak = 0.0f;
    while (samples < 48000 && decayEnd < 0)
    {
        e.process (s, kTick, kSr);
        samples += kTick;
        peak = std::max (peak, e.value());
        if (attackEnd < 0 && e.currentStage() != Envelope::Stage::Attack) attackEnd = samples;
        if (holdEnd < 0 && e.currentStage() != Envelope::Stage::Attack && e.currentStage() != Envelope::Stage::Hold) holdEnd = samples;
        if (e.currentStage() == Envelope::Stage::Sustain) decayEnd = samples;
    }
    CHECK (std::abs (attackEnd - 480) <= kTick);
    CHECK (std::abs (holdEnd - 1440) <= kTick);
    CHECK (std::abs (decayEnd - 6240) <= kTick);
    CHECK_NEAR (peak, 1.0, 1.0e-6);
    CHECK_NEAR (e.value(), dbToGain (-6.0f), 1.0e-6);

    // Release 50 ms from sustain.
    e.noteOff();
    CHECK (e.released());
    int rel = 0;
    while (! e.finished() && rel < 48000) { e.process (s, kTick, kSr); rel += kTick; }
    CHECK (std::abs (rel - 2400) <= kTick);
    CHECK (e.value() == 0.0f);

    // An odd tick size still hits the targets on the exact sample.
    e.reset(); e.trigger();
    int t = 0;
    while (t + 7 <= 480) { e.process (s, 7, kSr); t += 7; }
    CHECK (e.value() < 1.0f);
    e.process (s, 480 - t, kSr);
    CHECK_NEAR (e.value(), 1.0, 1.0e-6);

    // Curves: attack +0.6 bows up (above linear at the midpoint), decay -0.6 falls fast.
    EnvSettings c = s;
    c.attackCurve = 0.6f;
    e.reset(); e.trigger();
    e.process (c, 240, kSr);
    CHECK (e.value() > 0.6f);
    c.decayCurve = -0.6f;
    e.process (c, 240 + 960 + 2400, kSr);        // end of attack + hold + half the decay
    const float linearMid = 1.0f + (c.sustain - 1.0f) * 0.5f;
    CHECK (e.value() < linearMid - 0.05f);

    // Sustain dB mapping.
    CHECK_NEAR (envSustainGain (0.0f), 1.0, 1.0e-6);
    CHECK_NEAR (envSustainGain (-20.0f), 0.1, 1.0e-6);
    CHECK (envSustainGain (-60.0f) == 0.0f);

    // Reading from parameters, BPM mode: 1 beat at 120 BPM = 0.5 s.
    PlainView v;
    const auto& id = ids().env[1];
    v.pv[id.attack] = 1.0f; v.pv[id.decay] = 2.0f; v.pv[id.sustain] = -12.0f;
    EnvSettings r = EnvSettings::read (v.view(), id, 120.0);
    CHECK_NEAR (r.attack, 1.0, 1.0e-6);
    v.pv[id.bpm] = 1.0f;
    r = EnvSettings::read (v.view(), id, 120.0);
    CHECK_NEAR (r.attack, 0.5, 1.0e-6);
    CHECK_NEAR (r.decay, 1.0, 1.0e-6);
    CHECK_NEAR (r.sustain, dbToGain (-12.0f), 1.0e-6);
    e.reset(); e.trigger();
    int bs = 0;
    while (e.currentStage() == Envelope::Stage::Attack && bs < 96000) { e.process (r, kTick, kSr); bs += kTick; }
    CHECK (std::abs (bs - 24000) <= kTick);
    CHECK_NEAR (envSnapSecondsToBeats (0.5f, 120.0), 1.0, 1.0e-6);
    CHECK_NEAR (envSnapSecondsToBeats (0.37f, 120.0), 0.75, 1.0e-6);       // dotted 1/8
    CHECK_NEAR (envSnapSecondsToBeats (0.17f, 120.0), 1.0 / 3.0, 1.0e-6);  // 1/8 triplet

    // Legato rules.
    e.reset(); e.trigger();
    e.process (s, 20000, kSr);
    CHECK (e.currentStage() == Envelope::Stage::Sustain);
    e.noteOn (true, false);
    CHECK (e.currentStage() == Envelope::Stage::Sustain);       // legato: no retrigger
    e.noteOn (true, true);
    CHECK (e.currentStage() == Envelope::Stage::Attack);        // Legato Inverted: retrigger
    e.process (s, 20000, kSr);
    e.noteOn (false, false);
    CHECK (e.currentStage() == Envelope::Stage::Attack);        // normal overlapped note: retrigger
    e.process (s, 20000, kSr);
    e.noteOn (false, true);
    CHECK (e.currentStage() == Envelope::Stage::Sustain);       // inverted: the opposite

    // Retrigger starts from the current level (no click).
    e.noteOff();
    e.process (s, 600, kSr);
    const float lv = e.value();
    e.trigger();
    const EnvTick tk = e.process (s, 1, kSr);
    CHECK_NEAR (tk.start, lv, 1.0e-6);
    CHECK (tk.end >= lv);

    // Drawing function agrees with the generator.
    e.reset(); e.trigger();
    bool agree = true;
    for (int i = 1; i <= 400; ++i)
    {
        e.process (s, kTick, kSr);
        if (std::abs (e.value() - envShapeAt (s, (float) (i * kTick) / kSr, 100.0f)) > 2.0e-3f) agree = false;
    }
    CHECK (agree);
    std::vector<float> xy;
    envShapePolyline (s, 64, xy);
    CHECK (xy.size() == 128);
    CHECK_NEAR (xy.back(), 0.0, 1.0e-6);
}

// ---------------------------------------------------------------------------
// LFOs
// ---------------------------------------------------------------------------
float runLfo (Lfo& l, const LfoSettings& s, const BakedLfo& b, int ticks, LfoClock clk = {}, int n = kTick)
{
    clk.sampleRate = kSr;
    float v = 0.0f;
    for (int i = 0; i < ticks; ++i)
    {
        v = l.process (s, b, clk, n, nullptr);
        clk.ppq += (double) n * clk.bpm / (60.0 * kSr);
    }
    return v;
}

double phaseErr (double a, double b) { double d = std::abs (a - b); return std::min (d, 1.0 - d); }

void testLfos()
{
    std::printf ("lfos\n");
    PlainView v;
    const auto& id = ids().lfo[0];
    const BakedLfo tri = BakedLfo::build (LfoData());
    LfoClock clk;
    clk.sampleRate = kSr;

    // Hz rate: 5 Hz, phase after 1.05 s = 0.25.
    v.pv[id.bpm] = 0.0f; v.pv[id.rate] = 5.0f;
    LfoSettings s = LfoSettings::read (v.view(), id, 120.0, 0.0f);
    CHECK_NEAR (s.hz, 5.0, 1.0e-5);
    Lfo l;
    l.reset (1);
    l.trigger (s, tri, clk);
    runLfo (l, s, tri, 1575);
    CHECK (phaseErr (l.phase(), 0.25) < 1.0e-4);
    CHECK_NEAR (l.value(), 0.5, 1.0e-3);           // triangle at 0.25
    v.pv[id.x10] = 1.0f;
    CHECK_NEAR (LfoSettings::read (v.view(), id, 120.0, 0.0f).hz, 50.0, 1.0e-4);
    v.pv[id.x10] = 0.0f;

    // Synced: 1/4 at 120 BPM = 2 Hz; TRIP = 3 Hz; DOT = 4/3 Hz. 0.55 s.
    v.pv[id.bpm] = 1.0f; v.pv[id.rateSync] = 5.0f;
    s = LfoSettings::read (v.view(), id, 120.0, 0.0f);
    CHECK_NEAR (s.hz, 2.0, 1.0e-5);
    l.reset (1); l.trigger (s, tri, clk); runLfo (l, s, tri, 825);
    CHECK (phaseErr (l.phase(), 0.1) < 1.0e-4);
    v.pv[id.trip] = 1.0f;
    s = LfoSettings::read (v.view(), id, 120.0, 0.0f);
    l.reset (1); l.trigger (s, tri, clk); runLfo (l, s, tri, 825);
    CHECK (phaseErr (l.phase(), 0.65) < 1.0e-4);
    v.pv[id.trip] = 0.0f; v.pv[id.dot] = 1.0f;
    s = LfoSettings::read (v.view(), id, 120.0, 0.0f);
    l.reset (1); l.trigger (s, tri, clk); runLfo (l, s, tri, 825);
    CHECK (phaseErr (l.phase(), 0.55 * 4.0 / 3.0 - 0.0) < 1.0e-4);
    v.pv[id.dot] = 0.0f;
    s = LfoSettings::read (v.view(), id, 120.0, 0.0f);

    // Frequency by counting rising crossings of a 7 Hz triangle over 10 s.
    {
        LfoSettings f = s; f.bpm = false; f.hz = 7.0f;
        l.reset (3); l.trigger (f, tri, clk);
        int cross = 0; float prev = l.process (f, tri, clk, kTick, nullptr);
        for (int i = 0; i < 15000; ++i)
        {
            const float x = l.process (f, tri, clk, kTick, nullptr);
            if (prev < 0.5f && x >= 0.5f) ++cross;
            prev = x;
        }
        CHECK (std::abs (cross - 70) <= 1);
    }

    // PHASE offset and snap.
    v.pv[id.phase] = 100.0f; v.pv[id.phaseSnap] = 1.0f;
    CHECK_NEAR (LfoSettings::read (v.view(), id, 120.0, 0.0f).phase, 0.25, 1.0e-6);     // 100 deg -> 2/8
    v.pv[id.phase] = 0.0f; v.pv[id.phaseSnap] = 0.0f;

    // RETRIG: phase restarts at each trigger; FREE: it doesn't.
    LfoSettings rs = s;
    l.reset (1); l.trigger (rs, tri, clk); runLfo (l, rs, tri, 100);
    l.trigger (rs, tri, clk); runLfo (l, rs, tri, 50);
    Lfo l2; l2.reset (2); l2.trigger (rs, tri, clk); runLfo (l2, rs, tri, 50);
    CHECK (phaseErr (l.phase(), l2.phase()) < 1.0e-6);
    LfoSettings fs = s; fs.mode = LfoMode::Free;
    l.reset (1); l.trigger (fs, tri, clk); runLfo (l, fs, tri, 100);
    const float before = l.phase();
    l.trigger (fs, tri, clk);
    runLfo (l, fs, tri, 1);
    CHECK (l.phase() > before);

    // ENVELOPE mode: one cycle then hold the end.
    LfoData sawD; sawD.shape = Curve::line (0.0f, 1.0f);
    const BakedLfo saw = BakedLfo::build (sawD);
    LfoSettings es = s; es.mode = LfoMode::Envelope;          // 2 Hz: 0.5 s cycle
    l.reset (1); l.trigger (es, saw, clk);
    runLfo (l, es, saw, 700);                                   // 0.47 s
    CHECK (! l.envelopeDone());
    runLfo (l, es, saw, 100);
    CHECK (l.envelopeDone());
    CHECK_NEAR (l.value(), 1.0, 1.0e-6);
    // ...with a loopback point at x = 0.5: keeps looping while held, ends after release.
    LfoData loopD; loopD.shape.points = { { 0, 0, 0, false }, { 0.5f, 0.5f, 0, false }, { 1, 1, 0, false } };
    loopD.loopback = 1;
    const BakedLfo loop = BakedLfo::build (loopD);
    CHECK_NEAR (loop.loopX, 0.5, 1.0e-6);
    l.reset (1); l.trigger (es, loop, clk);
    bool inLoop = true;
    for (int i = 0; i < 3000; ++i) { runLfo (l, es, loop, 1); if (i > 800 && l.phase() < 0.499f) inLoop = false; }
    CHECK (inLoop && ! l.envelopeDone());
    l.release();
    runLfo (l, es, loop, 800);
    CHECK (l.envelopeDone());
    // Reverse plays the shape backwards.
    LfoSettings revS = rs; revS.direction = LfoDirection::Reverse;
    l.reset (1); l.trigger (revS, saw, clk); runLfo (l, revS, saw, 150);          // 0.1 s = 0.2 cycle
    CHECK_NEAR (l.value(), 0.8, 2.0e-3);
    LfoSettings pp = rs; pp.direction = LfoDirection::PingPong;
    l.reset (1); l.trigger (pp, saw, clk); runLfo (l, pp, saw, 937);              // 0.6245 s = 1.249 cycles
    CHECK_NEAR (l.value(), 0.751, 3.0e-3);

    // HOST anchoring (BPM): the same ppq gives the same phase, whatever came before.
    LfoSettings hs = fs; hs.host = true;
    LfoClock pc; pc.playing = true; pc.bpm = 120.0;
    Lfo a, b; a.reset (1); b.reset (2);
    pc.ppq = 0.0; runLfo (a, hs, tri, 333, pc);
    pc.ppq = 7.77; runLfo (b, hs, tri, 17, pc);
    LfoClock at = pc; at.ppq = 3.3;
    runLfo (a, hs, tri, 1, at);
    runLfo (b, hs, tri, 1, at);
    CHECK (phaseErr (a.phase(), b.phase()) < 1.0e-6);
    const double expect = (3.3 + kTick * 2.0 / kSr) / 1.0;                         // 1/4 = 1 beat cycles
    CHECK (phaseErr (a.phase(), expect - std::floor (expect)) < 1.0e-5);
    // RETRIG + HOST: phase measured from the trigger position.
    LfoSettings hr = rs; hr.host = true;
    at.ppq = 10.0; a.trigger (hr, tri, at);
    at.ppq = 10.25; runLfo (a, hr, tri, 1, at);
    CHECK (phaseErr (a.phase(), 0.25 + kTick * 2.0 / kSr) < 1.0e-5);
    // HOST + Hz, FREE: phase set from transport time at play start.
    LfoSettings hz = fs; hz.host = true; hz.bpm = false; hz.hz = 3.0f;
    a.reset (5); b.reset (6);
    runLfo (b, hz, tri, 77);                                    // stopped: free running
    at.ppq = 5.0; at.playing = true;
    runLfo (a, hz, tri, 1, at); runLfo (b, hz, tri, 1, at);
    CHECK (phaseErr (a.phase(), b.phase()) < 1.0e-6);

    // Swing: synced 1/4, full swing, after one beat the pair is 2/3 through its first cycle.
    LfoSettings sw = rs; sw.swing = true; sw.swingAmount = 1.0f;
    l.reset (1); l.trigger (sw, saw, clk); runLfo (l, sw, saw, 750);
    CHECK_NEAR (l.phase(), 2.0 / 3.0, 1.0e-4);
    v.pv[id.swing] = 1.0f;
    CHECK (LfoSettings::read (v.view(), id, 120.0, 0.5f).swing);
    v.pv[id.swing] = 0.0f;

    // DELAY / RISE: shape 0 at x = 0, 1 elsewhere.
    LfoData stepD; stepD.shape.points = { { 0, 0, 0, false }, { 0.01f, 1, 0, false }, { 1, 1, 0, false } };
    const BakedLfo step = BakedLfo::build (stepD);
    LfoSettings dr = rs; dr.delay = 0.1f; dr.rise = 0.1f;
    l.reset (1); l.trigger (dr, step, clk);
    runLfo (l, dr, step, 140);                                  // 93 ms
    CHECK (l.value() == 0.0f);
    CHECK (l.phase() == 0.0f);                                  // phase frozen during DELAY
    runLfo (l, dr, step, 85);                                   // 150 ms: halfway through RISE
    CHECK_NEAR (l.value(), 0.5, 0.03);
    runLfo (l, dr, step, 80);                                   // 203 ms
    CHECK_NEAR (l.value(), 1.0, 1.0e-6);
    v.pv[id.rise] = 0.5f; v.pv[id.delay] = 1.0f;                // BPM mode: beats
    const LfoSettings rd = LfoSettings::read (v.view(), id, 120.0, 0.0f);
    CHECK_NEAR (rd.rise, 0.25, 1.0e-6);
    CHECK_NEAR (rd.delay, 0.5, 1.0e-6);
    v.pv[id.rise] = 0.0f; v.pv[id.delay] = 0.0f;

    // Chaos types stay in 0..1 and move, at slow and very fast rates.
    for (LfoType ty : { LfoType::Lorenz, LfoType::Rossler })
        for (float hzv : { 0.1f, 5.0f, 200.0f })
        {
            LfoSettings cs = rs; cs.type = ty; cs.bpm = false; cs.hz = hzv;
            l.reset (9); l.trigger (cs, tri, clk);
            float lo = 1.0f, hi = 0.0f;
            bool bounded = true;
            for (int i = 0; i < 15000; ++i)
            {
                const float x = l.process (cs, tri, clk, kTick, nullptr);
                bounded = bounded && x >= 0.0f && x <= 1.0f && std::isfinite (x);
                lo = std::min (lo, x); hi = std::max (hi, x);
            }
            CHECK (bounded);
            CHECK (hi - lo > (hzv < 1.0f ? 0.05f : 0.3f));
        }

    // S&H: a new value per grid step (4 per cycle at grid 4, 1 Hz).
    LfoSettings sh = rs; sh.type = LfoType::SampleHold; sh.gridX = 4; sh.bpm = false; sh.hz = 1.0f;
    l.reset (4); l.trigger (sh, tri, clk);
    int changes = 0; float last = l.process (sh, tri, clk, kTick, nullptr); float maxStep = 0.0f;
    for (int i = 0; i < 1500 * 4; ++i)
    {
        const float x = l.process (sh, tri, clk, kTick, nullptr);
        if (x != last) ++changes;
        maxStep = std::max (maxStep, std::abs (x - last));
        last = x;
    }
    CHECK (changes >= 15 && changes <= 16);
    LfoSettings shs = sh; shs.smooth = 1.0f;
    l.reset (4); l.trigger (shs, tri, clk);
    float maxStepSmooth = 0.0f; last = l.process (shs, tri, clk, kTick, nullptr);
    for (int i = 0; i < 1500 * 4; ++i) { const float x = l.process (shs, tri, clk, kTick, nullptr); maxStepSmooth = std::max (maxStepSmooth, std::abs (x - last)); last = x; }
    CHECK (maxStepSmooth < maxStep * 0.1f);

    // Path: a path that goes there and back is sampled by arc length.
    LfoData pathD;
    pathD.path = { { 0, 0, 0, false }, { 1, 1, 0, false }, { 0, 0, 0, false } };
    const BakedLfo pb = BakedLfo::build (pathD);
    CHECK_NEAR (pb.path (0.25f), 0.5, 2.0e-3);
    CHECK_NEAR (pb.path (0.5f), 1.0, 2.0e-3);
    CHECK_NEAR (pb.path (0.75f), 0.5, 2.0e-3);

    // Point modulation: bus 0 = Macro 1 moves the triangle's peak down by 0.5.
    LfoData pm;
    pm.buses[0].source = (int) ModSource::Macro1;
    pm.pointMods.push_back ({ 1, 0, 0.0f, -0.5f, 0.0f });
    CHECK (pm.nextFreeBus() == 1);
    const BakedLfo pmb = BakedLfo::build (pm);
    SourceValues sv;
    CHECK_NEAR (pmb.evaluate (0.5f, sv.v), 1.0, 1.0e-6);
    sv[ModSource::Macro1] = 1.0f;
    CHECK_NEAR (pmb.evaluate (0.5f, sv.v), 0.5, 1.0e-6);
    CHECK_NEAR (pmb.evaluate (0.25f, sv.v), 0.25, 1.0e-6);
}

void testShapes()
{
    std::printf ("shapes\n");
    const auto& lib = LfoShapes::factory();
    CHECK (lib.size() == 3);
    CHECK (lib[0].name == "Basic" && lib[1].name == "Rhythmic" && lib[2].name == "Sidechain");
    bool valid = true;
    for (const auto& f : lib)
    {
        CHECK (f.shapes.size() == 8);
        for (const auto& s : f.shapes)
        {
            const auto& p = s.curve.points;
            if (p.size() < 2 || p.front().x != 0.0f || p.back().x != 1.0f) valid = false;
            for (size_t i = 1; i < p.size(); ++i) if (p[i].x < p[i - 1].x) valid = false;
            for (const auto& q : p) if (q.y < 0.0f || q.y > 1.0f) valid = false;
        }
    }
    CHECK (valid);
    const auto* sine = LfoShapes::find ("Basic", "Sine");
    CHECK (sine != nullptr);
    if (sine != nullptr)
    {
        float err = 0.0f;
        for (int i = 0; i <= 100; ++i) { const float x = (float) i / 100.0f; err = std::max (err, std::abs (sine->curve.evaluate (x) - (0.5f - 0.5f * std::cos (kTwoPi * x)))); }
        CHECK (err < 0.005f);
    }
    // Rhythmic gate: 8ths, first step open, second closed.
    const auto* g8 = LfoShapes::find ("Rhythmic", "Straight 8ths");
    CHECK (g8 && g8->curve.evaluate (0.02f) == 1.0f && g8->curve.evaluate (0.1f) == 0.0f && g8->curve.evaluate (0.13f) == 1.0f);

    const Curve rf = LfoShapes::randomFlat (4, 77);
    CHECK (rf.points.size() == 5);
    CHECK (rf.evaluate (0.3f) == rf.points[1].y && rf.evaluate (0.6f) == rf.points[2].y);
    const Curve rc = LfoShapes::randomCurved (8, 77);
    CHECK (rc.points.size() == 17 && rc.points.back().x == 1.0f);
    CHECK_NEAR (rc.points.front().y, rc.points.back().y, 1.0e-6);                 // seamless loop
    CHECK (! (LfoShapes::randomFlat (4, 77) == LfoShapes::randomFlat (4, 78)));

    // Wavetable <-> LFO.
    std::vector<float> frame (2048);
    for (int i = 0; i < 2048; ++i) frame[(size_t) i] = 0.8f * std::sin (kTwoPi * (float) i / 2048.0f);
    const Curve fromWt = LfoShapes::fromWavetableFrame (frame);
    CHECK (fromWt.points.size() <= 128);
    const auto back = LfoShapes::toWavetableFrame (fromWt);
    CHECK (back.size() == 2048);
    float e = 0.0f;
    for (int i = 0; i < 2048; ++i) e = std::max (e, std::abs (back[(size_t) i] - frame[(size_t) i] / 0.8f));
    CHECK (e < 0.01f);
}

// ---------------------------------------------------------------------------
// Matrix
// ---------------------------------------------------------------------------
struct Rig
{
    ParamValues base;
    ModMatrixData data;
    ModGraphs graphs;
    std::shared_ptr<const CompiledMatrix> cm;
    MatrixBlock blk;
    MatrixVoiceState st, gst;
    SourceValues src;
    float modded[kMaxModRows] {};
    Rig() { base.resetToDefaults(); }
    void compile() { cm = CompiledMatrix::build (data, graphs); }
    float value (int param, float seconds = 1.0f / 1500.0f)
    {
        cm->beginBlock (base.v.data(), blk);
        cm->evalGlobal (src, gst, blk, seconds);
        blk.applyMacros (src);
        cm->evalModulators (src, st, blk, modded, seconds);
        cm->evalMain (src, st, blk, modded, seconds);
        const ParamView pv { base.v.data(), cm->slots(), modded, modded };
        return pv (param);
    }
};

ModRow row (ModSource s, int dest, float amount, bool bi = false)
{
    ModRow r; r.source = s; r.dest = dest; r.amount = amount; r.bipolar = bi;
    return r;
}

void testMatrix()
{
    std::printf ("matrix\n");
    const auto& I = ids();
    const int res = I.filter[0].res;          // 0..1, default 0.1

    // Sources: names, tree.
    CHECK (std::string (modSourceInfo (ModSource::PolyAftertouch).menuName) == "Poly Aftertch");
    CHECK (modSourceFromId ("lfo10") == ModSource::Lfo10);
    CHECK (modSourceFromId ("nope") == ModSource::None);
    int treeCount = 0;
    for (const auto& c : modSourceTree()) treeCount += (int) c.sources.size();
    CHECK (treeCount == kNumModSources);
    CHECK (modSourceTree().size() == 11);

    // Unipolar row.
    {
        Rig r;
        r.data.add (row (ModSource::Lfo1, res, 0.5f));
        r.compile();
        r.src[ModSource::Lfo1] = 1.0f;
        CHECK_NEAR (r.value (res), 0.6, 1.0e-6);
        r.src[ModSource::Lfo1] = 0.0f;
        CHECK_NEAR (r.value (res), 0.1, 1.0e-6);
        CHECK (r.cm->slots()[I.filter[0].cutoff] == -1);
        CHECK_NEAR (r.value (I.filter[0].cutoff), 22000.0, 1.0e-3);
    }
    // Bipolar POL, and clamping at the bottom.
    {
        Rig r;
        r.data.add (row (ModSource::Lfo1, res, 0.5f, true));
        r.compile();
        r.src[ModSource::Lfo1] = 0.5f; CHECK_NEAR (r.value (res), 0.1, 1.0e-6);
        r.src[ModSource::Lfo1] = 1.0f; CHECK_NEAR (r.value (res), 0.6, 1.0e-6);
        r.src[ModSource::Lfo1] = 0.0f; CHECK_NEAR (r.value (res), 0.0, 1.0e-6);
    }
    // Bipolar source (pitch bend) on a uni row: both directions.
    {
        Rig r;
        r.data.add (row (ModSource::PitchBend, res, 0.2f));
        r.compile();
        r.src[ModSource::PitchBend] = 0.5f;  CHECK_NEAR (r.value (res), 0.2, 1.0e-6);
        r.src[ModSource::PitchBend] = -0.25f; CHECK_NEAR (r.value (res), 0.05, 1.0e-6);
    }
    // Aux scaling and invert.
    {
        Rig r;
        const int lvl = I.osc[0].level;   // default 0.75
        ModRow x = row (ModSource::Lfo1, lvl, -0.5f);
        x.aux = ModSource::ModWheel;
        r.data.add (x);
        r.compile();
        r.src[ModSource::Lfo1] = 1.0f;
        r.src[ModSource::ModWheel] = 0.0f; CHECK_NEAR (r.value (lvl), 0.75, 1.0e-6);
        r.src[ModSource::ModWheel] = 1.0f; CHECK_NEAR (r.value (lvl), 0.25, 1.0e-6);
        r.src[ModSource::ModWheel] = 0.5f; CHECK_NEAR (r.value (lvl), 0.5, 1.0e-6);
        r.data.rows[0].auxInvert = true;
        r.compile();
        r.src[ModSource::ModWheel] = 0.0f; CHECK_NEAR (r.value (lvl), 0.25, 1.0e-6);
        r.src[ModSource::ModWheel] = 1.0f; CHECK_NEAR (r.value (lvl), 0.75, 1.0e-6);
        // Aux curve: bend 1 -> x^(1/4).
        r.data.rows[0].auxInvert = false;
        r.data.rows[0].auxCurve = 1.0f;
        r.compile();
        r.src[ModSource::ModWheel] = 0.0625f; CHECK_NEAR (r.value (lvl), 0.5, 3.0e-3);
    }
    // Source curve: simple bend and custom curve; output scaling.
    {
        Rig r;
        const int wt = I.osc[0].wtpos;    // 0..1, default 0
        ModRow x = row (ModSource::Lfo1, wt, 1.0f);
        x.curve = 1.0f;
        r.data.add (x);
        r.compile();
        r.src[ModSource::Lfo1] = 0.0625f; CHECK_NEAR (r.value (wt), 0.5, 3.0e-3);
        r.data.rows[0].curve = 0.0f;      // x^4
        r.compile();
        r.src[ModSource::Lfo1] = 0.5f; CHECK_NEAR (r.value (wt), 0.0625, 3.0e-3);
        r.data.rows[0].customCurve = true;
        r.data.rows[0].srcCurve = Curve::line (1.0f, 0.0f);
        r.compile();
        r.src[ModSource::Lfo1] = 0.25f; CHECK_NEAR (r.value (wt), 0.75, 3.0e-3);
        r.data.rows[0].output = 0.5f;
        r.compile();
        CHECK_NEAR (r.value (wt), 0.375, 3.0e-3);
    }
    // Slew: RISE 0.1 s, FALL 0.2 s, ticks of 1/1500 s.
    {
        Rig r;
        const int wt = I.osc[1].wtpos;
        ModRow x = row (ModSource::Lfo1, wt, 1.0f);
        x.customCurve = true; x.rise = 0.1f; x.fall = 0.2f;
        r.data.add (x);
        r.compile();
        r.src[ModSource::Lfo1] = 0.0f;
        r.value (wt);                                   // first value initialises the slew
        r.src[ModSource::Lfo1] = 1.0f;
        float y = 0.0f;
        for (int i = 0; i < 75; ++i) y = r.value (wt);  // 50 ms
        CHECK_NEAR (y, 0.5, 1.0e-3);
        for (int i = 0; i < 100; ++i) y = r.value (wt);
        CHECK_NEAR (y, 1.0, 1.0e-6);
        r.src[ModSource::Lfo1] = 0.0f;
        for (int i = 0; i < 150; ++i) y = r.value (wt); // 100 ms of a 200 ms fall
        CHECK_NEAR (y, 0.5, 1.0e-3);
        r.st.noteOn (5);                                // note-on restarts the slew
        CHECK_NEAR (r.value (wt), 0.0, 1.0e-6);
    }
    // Several rows sum on one destination, then clamp at the top. Log and power destinations.
    {
        Rig r;
        r.data.add (row (ModSource::Lfo1, res, 0.2f));
        r.data.add (row (ModSource::Lfo2, res, 0.2f));
        const int cut = I.filter[1].cutoff;             // log 8..22000, default 22000
        r.data.add (row (ModSource::Lfo1, cut, -0.5f));
        const int atk = I.env[2].attack;                // power skew 4, 0..32 s
        r.data.add (row (ModSource::Fixed, atk, 0.5f));
        const int sync = I.lfo[3].rateSync;             // discrete
        r.data.add (row (ModSource::Fixed, sync, 0.25f));
        r.compile();
        r.src[ModSource::Lfo1] = 1.0f; r.src[ModSource::Lfo2] = 1.0f; r.src[ModSource::Fixed] = 1.0f;
        CHECK_NEAR (r.value (res), 0.5, 1.0e-6);
        CHECK_NEAR (r.value (cut), params()[cut].fromNorm (0.5f), params()[cut].fromNorm (0.5f) * 1.0e-4);
        const float atkNorm = params()[atk].toNorm (0.001f) + 0.5f;
        CHECK_NEAR (r.value (atk), params()[atk].fromNorm (atkNorm), 1.0e-4);
        CHECK_NEAR (r.value (sync), 8.0, 1.0e-6);       // 5 + 0.25 * 11 = 7.75 -> 8
        CHECK (r.cm->phaseOfSlot (r.cm->slots()[atk]) == CompiledMatrix::kModulators);
        CHECK (r.cm->phaseOfSlot (r.cm->slots()[res]) == CompiledMatrix::kMain);
        r.data.add (row (ModSource::Lfo3, res, 0.9f));
        r.compile();
        r.src[ModSource::Lfo3] = 1.0f;
        CHECK_NEAR (r.value (res), 1.0, 1.0e-6);
        // Bypassed rows do nothing.
        r.data.rows.back().bypass = true;
        r.compile();
        CHECK_NEAR (r.value (res), 0.5, 1.0e-6);
    }
    // Global destinations, macro chains, per-voice view of a global slot.
    {
        Rig r;
        const int mix = I.fx[0].mix;                    // global
        const int pan = I.osc[0].pan;                   // per voice, bipolar
        r.data.add (row (ModSource::Macro2, pan, 0.25f));
        r.data.add (row (ModSource::Fixed, I.macro[1], 0.5f));   // modulates Macro 2 itself
        r.data.add (row (ModSource::Lfo1, mix, -0.5f));
        r.compile();
        CHECK (r.cm->phaseOfSlot (r.cm->slots()[mix]) == CompiledMatrix::kGlobal);
        CHECK (r.cm->phaseOfSlot (r.cm->slots()[I.macro[1]]) == CompiledMatrix::kGlobalMacros);
        r.src[ModSource::Fixed] = 1.0f;
        r.src[ModSource::Lfo1] = 1.0f;
        CHECK_NEAR (r.value (pan), 0.25, 1.0e-5);       // macro 2 = 0.5 -> +0.125 norm -> pan 0.25
        CHECK_NEAR (r.blk.macro[1], 0.5, 1.0e-6);
        CHECK_NEAR (r.value (mix), 0.5, 1.0e-6);
        CHECK_NEAR (r.blk.globalModded[r.cm->slots()[mix]], 0.5, 1.0e-6);
        CHECK (ModMatrixData::defaultBipolar (pan) && ! ModMatrixData::defaultBipolar (res));
    }
    // Note-On Rand (Discrete): each destination gets its own value.
    {
        Rig r;
        r.data.add (row (ModSource::NoteOnRandDiscrete, I.osc[0].wtpos, 1.0f));
        r.data.add (row (ModSource::NoteOnRandDiscrete, I.osc[1].wtpos, 1.0f));
        r.data.add (row (ModSource::NoteOnRand1, I.osc[2].wtpos, 1.0f));
        r.compile();
        NoteInfo n;
        r.cm->noteOnSources (n, 1234, true, r.src, r.st);
        const float a = r.value (I.osc[0].wtpos), b = r.value (I.osc[1].wtpos);
        CHECK (a != b && a > 0.0f && b > 0.0f);
        CHECK_NEAR (r.value (I.osc[2].wtpos), r.src[ModSource::NoteOnRand1], 1.0e-6);
        CHECK (r.src[ModSource::NoteOnAlt1] == 1.0f && r.src[ModSource::NoteOnAlt2] == 0.0f);
        MatrixVoiceState st2; SourceValues s2;
        r.cm->noteOnSources (n, 1234, true, s2, st2);
        CHECK (st2.discrete[0] == r.st.discrete[0] && s2[ModSource::NoteOnRand2] == r.src[ModSource::NoteOnRand2]);
    }
    // VELO / NOTE graphs.
    {
        Rig r;
        r.graphs.velo.points[1].curve = 0.5f;
        r.graphs.note = Curve::line (1.0f, 0.0f);
        r.compile();
        CHECK_NEAR (r.cm->velocity (0.5f), r.graphs.velo.evaluate (0.5f), 1.0e-4);
        CHECK_NEAR (r.cm->note (127.0f), 0.0, 1.0e-6);
        NoteInfo n; n.velocity = 0.3f; n.note = 0;
        r.cm->noteOnSources (n, 1, false, r.src, r.st);
        CHECK_NEAR (r.src[ModSource::Velocity], r.graphs.velo.evaluate (0.3f), 1.0e-4);
        CHECK_NEAR (r.src[ModSource::Note], 1.0, 1.0e-6);
    }
    // Telemetry.
    {
        Rig r;
        r.data.add (row (ModSource::Lfo1, res, 0.5f));
        r.data.add (row (ModSource::Lfo1, I.fx[0].mix, 0.25f));
        r.compile();
        r.src[ModSource::Lfo1] = 0.5f;
        r.value (res);
        MatrixTelemetry t;
        r.cm->publish (&r.st, r.gst, t);
        CHECK_NEAR (t.rowOut[0].load(), 0.25, 1.0e-6);
        CHECK_NEAR (t.rowOut[1].load(), 0.125, 1.0e-6);
        CHECK (t.rowOut[2].load() == 0.0f);
    }
}

void testMatrixData()
{
    std::printf ("matrix data\n");
    const auto& I = ids();
    ModMatrixData d;
    CHECK (d.add (row (ModSource::Lfo3, I.osc[0].level, 0.1f)) == 0);
    CHECK (d.add (row (ModSource::Env1, I.filter[0].cutoff, 0.2f)) == 1);
    CHECK (d.add (ModRow()) == 2);                                   // empty row
    CHECK (d.add (row (ModSource::Macro1, I.osc[0].wtpos, 0.3f)) == 3);
    CHECK (d.add (row (ModSource::Lfo1, I.global.poly, 0.3f)) == -1); // not modulatable
    d.sortBySource();
    CHECK (d.rows[0].source == ModSource::Env1 && d.rows[1].source == ModSource::Lfo3
           && d.rows[2].source == ModSource::Macro1 && d.rows[3].source == ModSource::None);
    d.sortByDestination();
    CHECK (d.rows[0].dest == I.osc[0].level && d.rows[1].dest == I.osc[0].wtpos
           && d.rows[2].dest == I.filter[0].cutoff && d.rows[3].dest == -1);
    CHECK (d.move (0, 2));
    CHECK (d.rows[2].dest == I.osc[0].level && d.rows[0].dest == I.osc[0].wtpos);
    CHECK (d.move (2, 0));
    CHECK (d.rows[0].dest == I.osc[0].level);
    CHECK (d.findRows (I.filter[0].cutoff).size() == 1);
    CHECK (d.assign (ModSource::Env1, I.filter[0].cutoff) == d.findRows (I.filter[0].cutoff)[0]);
    const int pi = d.assign (ModSource::Lfo2, I.osc[1].pan);
    CHECK (pi >= 0 && d.rows[(size_t) pi].bipolar && d.rows[(size_t) pi].amount == 1.0f);
    CHECK (d.rowsForSource (ModSource::Lfo2).size() == 1);
    d.setSourceBypass (ModSource::Lfo2, true);
    CHECK (d.rows[(size_t) pi].bypass);
    d.removeSource (ModSource::Lfo2);
    CHECK (d.rowsForSource (ModSource::Lfo2).empty());
    d.removeRowsForGroup ("FILTER 1");
    CHECK (d.findRows (I.filter[0].cutoff).empty());
    while ((int) d.rows.size() < kMaxModRows) d.add (ModRow());
    CHECK (d.add (ModRow()) == -1);

    // Apply and Delete Macros.
    {
        ModMatrixData m;
        ParamValues pv; pv.resetToDefaults();
        pv[I.macro[0]] = 0.5f;
        pv[I.macro[1]] = 1.0f;
        m.add (row (ModSource::Macro1, I.filter[0].res, 0.4f));      // 0.1 + 0.2
        m.add (row (ModSource::Macro2, I.filter[0].res, 0.1f));      // + 0.1
        m.add (row (ModSource::Macro1, I.osc[0].pan, 0.5f, true));   // bi: macro 0.5 -> 0
        m.add (row (ModSource::Lfo1, I.filter[0].res, 0.4f));
        m.applyAndDeleteMacros (pv);
        CHECK_NEAR (pv[I.filter[0].res], 0.4, 1.0e-5);
        CHECK_NEAR (pv[I.osc[0].pan], 0.0, 1.0e-5);
        CHECK (m.rows.size() == 1 && m.rows[0].source == ModSource::Lfo1);
    }
    // Create Vibrato / Velo->Amp.
    {
        ModMatrixData m;
        ParamValues pv; pv.resetToDefaults();
        LfoData lfos[kNumLfos];
        m.add (row (ModSource::Lfo1, I.osc[0].level, 0.5f));
        const int l = m.createVibrato (&pv, lfos);
        CHECK (l == 1);
        const auto& r = m.rows.back();
        CHECK (r.source == ModSource::Lfo2 && r.dest == I.global.tuning && r.aux == ModSource::ModWheel && r.bipolar);
        CHECK_NEAR (r.amount * 48.0f, 0.5, 1.0e-5);
        CHECK (pv[I.lfo[1].bpm] == 0.0f && pv[I.lfo[1].rate] == 5.0f && lfos[1].shapeName == "Sine");
        const int va = m.createVeloAmp (pv);
        CHECK (va >= 0 && m.rows[(size_t) va].source == ModSource::Velocity && m.rows[(size_t) va].dest == I.global.amp);
        CHECK (pv[I.global.amp] == 0.0f);
        m.initModulations();
        CHECK (m.rows.empty());
    }
    // Save / load round trips (through the binary format).
    {
        ModMatrixData m;
        ModRow x = row (ModSource::Lfo7, I.filter[1].cutoff, -0.37f, true);
        x.curve = 0.8f; x.aux = ModSource::PolyAftertouch; x.auxInvert = true; x.auxCurve = 0.2f;
        x.output = 0.6f; x.bypass = true; x.rise = 0.25f; x.fall = 0.5f;
        x.customCurve = true; x.srcCurve.insert (0.3f, 0.9f); x.srcCurve.points[1].curve = 0.4f;
        x.auxCustom = true; x.auxCurveData = Curve::line (1.0f, 0.2f);
        m.add (x);
        m.add (ModRow());
        m.add (row (ModSource::NoteOnRandDiscrete, I.macro[3], 1.0f));
        m.locked = true;
        Node n ("matrix");
        m.save (n);
        const auto bin = n.toBinary();
        Node back;
        CHECK (Node::fromBinary (bin.data(), bin.size(), back));
        ModMatrixData m2;
        m2.load (back);
        CHECK (m2.rows.size() == 3 && m2.locked);
        CHECK (m2.rows[0] == m.rows[0] && m2.rows[1] == m.rows[1] && m2.rows[2] == m.rows[2]);
        // A locked matrix ignores preset loads.
        ModMatrixData other;
        Node empty ("matrix");
        other.save (empty);
        CHECK (! m2.loadPreset (empty) && m2.rows.size() == 3);
        m2.locked = false;
        CHECK (m2.loadPreset (n) && m2.rows.size() == 3 && ! m2.locked);
        // Missing keys = defaults.
        ModRow def; def.load (Node ("row"));
        CHECK (def == ModRow());

        ModGraphs g;
        g.velo.insert (0.5f, 0.8f); g.noteLegato = true;
        Node gn ("graphs");
        g.save (gn);
        ModGraphs g2; g2.load (gn);
        CHECK (g2.velo == g.velo && g2.note == g.note && g2.noteLegato && ! g2.veloLegato);

        LfoData ld;
        ld.shape = LfoShapes::randomCurved (8, 3);
        ld.path = { { 0, 0, 0, false }, { 0.8f, 1, 0.3f, false }, { 0.2f, 0.4f, 0, true } };
        ld.loopback = 2; ld.shapeName = "Random";
        ld.buses[1] = { (int) ModSource::Env3, 0.7f };
        ld.pointMods.push_back ({ 3, 1, 0.1f, -0.2f, 0.3f });
        Node ln ("lfo");
        ld.save (ln);
        const auto lbin = ln.toBinary();
        Node lback;
        CHECK (Node::fromBinary (lbin.data(), lbin.size(), lback));
        LfoData ld2; ld2.load (lback);
        CHECK (ld2 == ld);
        LfoData ld3; ld3.load (Node ("lfo"));
        CHECK (ld3 == LfoData());
    }
}

// ---------------------------------------------------------------------------
// Runtime (ModVoice / ModGlobal)
// ---------------------------------------------------------------------------
void testRuntime()
{
    std::printf ("runtime\n");
    const auto& I = ids();
    ParamValues base; base.resetToDefaults();
    Context ctx;
    LfoData lfoData[kNumLfos];
    auto shapes = LfoShapeSet::build (lfoData);

    // LFO 1 -> LFO 2 RATE (modulator order), ENV 2 -> cutoff, LFO 2 -> res.
    ModMatrixData d;
    ModGraphs g;
    base[I.lfo[0].bpm] = 0.0f; base[I.lfo[0].rate] = 1.0f;
    base[I.lfo[1].bpm] = 0.0f; base[I.lfo[1].rate] = 1.0f;
    d.add (row (ModSource::Lfo1, I.lfo[1].rate, 0.5f));
    d.add (row (ModSource::Env2, I.filter[0].cutoff, -0.5f));
    d.add (row (ModSource::Lfo2, I.filter[0].res, 0.5f));
    auto cm = CompiledMatrix::build (d, g);

    ModGlobal glob; glob.reset (1);
    ModVoice v; v.reset (7);
    NoteInfo n; n.velocity = 0.9f;
    glob.beginBlock (*cm, base, nullptr, 0.01f);
    v.noteOn (*cm, *shapes, glob, n, base, ctx, 0, 99, false, true);
    for (int i = 0; i < 300; ++i)
    {
        if (i % 16 == 0) glob.beginBlock (*cm, base, &v, 16.0f * kTick / kSr);
        glob.tick (*cm, *shapes, v.view (*cm, base), ctx, (i % 16) * kTick, kTick);
        v.tick (*cm, *shapes, glob, base, ctx, (i % 16) * kTick, kTick);
    }
    const ParamView pv = v.view (*cm, base);
    // LFO 2 rate was modulated by the previous tick's LFO 1 value (log 0.01..20 Hz).
    const auto& rate = params()[I.lfo[1].rate];
    const float prevLfo1 = v.lfoStart (0);
    CHECK_NEAR (pv (I.lfo[1].rate), rate.fromNorm (rate.toNorm (1.0f) + 0.5f * prevLfo1), 1.0e-3);
    // ENV 2 sustain (0 dB) -> cutoff at norm 0.5.
    CHECK_NEAR (pv (I.filter[0].cutoff), params()[I.filter[0].cutoff].fromNorm (0.5f), 1.0);
    CHECK_NEAR (pv (I.filter[0].res), 0.1 + 0.5 * v.lfoEnd (1), 1.0e-5);
    CHECK (pv.previous (I.filter[0].res) != pv (I.filter[0].res));
    CHECK (! v.finished());
    v.noteOff (0.4f);
    CHECK (v.src[ModSource::ReleaseVelocity] == 0.4f);
    for (int i = 0; i < 200 && ! v.finished(); ++i)
    {
        glob.tick (*cm, *shapes, v.view (*cm, base), ctx, 0, kTick);
        v.tick (*cm, *shapes, glob, base, ctx, 0, kTick);
    }
    CHECK (v.finished());           // ENV 1 release 15 ms

    // FREE (not MONO) LFO: two voices started at different times share the phase.
    base[I.lfo[2].mode] = 0.0f;
    ModMatrixData d2;
    d2.add (row (ModSource::Lfo3, I.osc[0].wtpos, 1.0f));
    auto cm2 = CompiledMatrix::build (d2, g);
    ModVoice a, b; a.reset (1); b.reset (2);
    glob.reset (3);
    glob.beginBlock (*cm2, base, nullptr, 0.0f);
    a.noteOn (*cm2, *shapes, glob, n, base, ctx, 0, 1, false, true);
    for (int i = 0; i < 37; ++i) { glob.tick (*cm2, *shapes, a.view (*cm2, base), ctx, 0, kTick); a.tick (*cm2, *shapes, glob, base, ctx, 0, kTick); }
    b.noteOn (*cm2, *shapes, glob, n, base, ctx, 0, 2, true, true);
    for (int i = 0; i < 10; ++i)
    {
        glob.tick (*cm2, *shapes, a.view (*cm2, base), ctx, 0, kTick);
        a.tick (*cm2, *shapes, glob, base, ctx, 0, kTick);
        b.tick (*cm2, *shapes, glob, base, ctx, 0, kTick);
    }
    CHECK_NEAR (a.lfoEnd (2), b.lfoEnd (2), 1.0e-6);
    CHECK (a.lfoEnd (2) > 0.0f);
    // MONO RETRIG LFO: retriggered by each note-on, identical for all voices.
    base[I.lfo[2].mode] = 1.0f; base[I.lfo[2].mono] = 1.0f;
    glob.tick (*cm2, *shapes, a.view (*cm2, base), ctx, 0, kTick);
    glob.noteOn (*shapes, a.view (*cm2, base), ctx, 0, false);
    a.tick (*cm2, *shapes, glob, base, ctx, 0, kTick);
    glob.tick (*cm2, *shapes, a.view (*cm2, base), ctx, 0, kTick);
    a.tick (*cm2, *shapes, glob, base, ctx, 0, kTick);
    b.tick (*cm2, *shapes, glob, base, ctx, 0, kTick);
    CHECK (glob.isShared (2));
    CHECK_NEAR (a.lfoEnd (2), b.lfoEnd (2), 1.0e-6);
    CHECK (glob.sharedLfo (2).phase() < 0.01f);
    MatrixTelemetry mt;
    LfoTelemetry lt[kNumLfos];
    glob.publish (*cm2, &a, mt, lt);
    CHECK_NEAR (lt[2].value.load(), glob.sharedLfo (2).value(), 1.0e-6);
}

// ---------------------------------------------------------------------------
// Benchmark: 16 voices x (4 envs + 10 LFOs + 64 rows) per tick.
// ---------------------------------------------------------------------------
void benchmark()
{
    std::printf ("benchmark\n");
    const auto& I = ids();
    ParamValues base; base.resetToDefaults();
    Context ctx;
    LfoData lfoData[kNumLfos];
    for (int l = 0; l < kNumLfos; ++l)
        lfoData[l].shape = LfoShapes::factory()[(size_t) (l % 3)].shapes[(size_t) (l % 8)].curve;
    auto shapes = LfoShapeSet::build (lfoData);
    for (int l = 0; l < kNumLfos; ++l)
    {
        base[I.lfo[l].smooth] = l % 2 ? 0.2f : 0.0f;
        base[I.lfo[l].rateSync] = (float) (3 + l % 6);
    }

    // 64 rows: every env and LFO used, a mix of sources, curves, aux and dests.
    std::vector<int> dests;
    for (int o = 0; o < kNumOscs; ++o)
        for (int p : { I.osc[o].level, I.osc[o].pan, I.osc[o].wtpos, I.osc[o].detune, I.osc[o].warp1, I.osc[o].fine, I.osc[o].sem, I.osc[o].phase })
            dests.push_back (p);
    for (int f = 0; f < kNumFilters; ++f)
        for (int p : { I.filter[f].cutoff, I.filter[f].res, I.filter[f].drive, I.filter[f].mix, I.filter[f].pan })
            dests.push_back (p);
    for (int l = 0; l < 4; ++l) dests.push_back (I.lfo[l + 4].rate);
    for (int e = 1; e < 4; ++e) dests.push_back (I.env[e].decay);
    dests.push_back (I.global.tuning);
    dests.push_back (I.fx[0].mix);
    dests.push_back (I.macro[2]);
    const ModSource srcs[] { ModSource::Env1, ModSource::Env2, ModSource::Env3, ModSource::Env4, ModSource::Lfo1, ModSource::Lfo2,
                             ModSource::Lfo3, ModSource::Lfo4, ModSource::Lfo5, ModSource::Lfo6, ModSource::Lfo7, ModSource::Lfo8,
                             ModSource::Lfo9, ModSource::Lfo10, ModSource::Velocity, ModSource::ModWheel, ModSource::Macro1,
                             ModSource::Note, ModSource::NoteOnRand1, ModSource::NoteOnRandDiscrete, ModSource::PitchBend };
    ModMatrixData d;
    for (int i = 0; i < kMaxModRows; ++i)
    {
        ModRow r = row (srcs[i % (int) std::size (srcs)], dests[(size_t) i % dests.size()], (i % 2 ? 0.3f : -0.2f), i % 3 == 0);
        if (i % 4 == 0) r.curve = 0.7f;
        if (i % 5 == 0) r.aux = ModSource::ModWheel;
        if (i % 9 == 0) { r.customCurve = true; r.rise = 0.05f; }
        d.add (r);
    }
    ModGraphs g;
    auto cm = CompiledMatrix::build (d, g);
    std::printf ("  compiled: %d rows, %d slots\n", (int) d.rows.size(), cm->numSlots());

    constexpr int kVoices = 16;
    ModGlobal glob; glob.reset (1);
    glob.globals[ModSource::ModWheel] = 0.6f;
    ModVoice voices[kVoices];
    glob.beginBlock (*cm, base, nullptr, 0.0f);
    for (int vi = 0; vi < kVoices; ++vi)
    {
        voices[vi].reset ((uint32_t) vi + 1);
        NoteInfo n; n.note = 40 + vi; n.velocity = 0.5f + 0.02f * (float) vi;
        voices[vi].noteOn (*cm, *shapes, glob, n, base, ctx, 0, (uint32_t) vi * 17, vi % 2, true);
    }

    constexpr int kBlock = 512, kTicksPerBlock = kBlock / kTick;
    const double seconds = 10.0;
    const int blocks = (int) (seconds * kSr / kBlock);
    float sink = 0.0f;
    const auto t0 = std::chrono::steady_clock::now();
    for (int b = 0; b < blocks; ++b)
    {
        ctx.ppq += kBlock * ctx.bpm / (60.0 * kSr);
        glob.beginBlock (*cm, base, &voices[kVoices - 1], (float) kBlock / kSr);
        for (int t = 0; t < kTicksPerBlock; ++t)
        {
            glob.tick (*cm, *shapes, voices[kVoices - 1].view (*cm, base), ctx, t * kTick, kTick);
            for (auto& v : voices)
            {
                v.tick (*cm, *shapes, glob, base, ctx, t * kTick, kTick);
                sink += v.view (*cm, base) (I.filter[0].cutoff);
            }
        }
    }
    const double el = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    const double ticks = (double) blocks * kTicksPerBlock;
    std::printf ("  16 voices x (4 env + 10 LFO + 64 rows): %.1f ns per voice-tick, %.2f %% of one core at 48 kHz  (sink %g)\n",
                 el * 1.0e9 / (ticks * kVoices), 100.0 * el / seconds, (double) sink);

    // Matrix evaluation alone (both voice phases), for reference.
    SourceValues src = voices[0].src;
    MatrixVoiceState st;
    MatrixBlock blk;
    cm->beginBlock (base.v.data(), blk);
    float out[kMaxModRows];
    const int reps = 200000;
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < reps; ++i)
    {
        src.v[4] = (float) (i & 255) * (1.0f / 255.0f);
        cm->evalModulators (src, st, blk, out, 1.0f / 1500.0f);
        cm->evalMain (src, st, blk, out, 1.0f / 1500.0f);
        sink += out[3];
    }
    const double el2 = std::chrono::duration<double> (std::chrono::steady_clock::now() - t1).count();
    std::printf ("  64-row matrix eval alone: %.1f ns per voice-tick  (sink %g)\n", el2 * 1.0e9 / reps, (double) sink);
}
} // namespace

int main()
{
    testEnvelopes();
    testLfos();
    testShapes();
    testMatrix();
    testMatrixData();
    testRuntime();
    benchmark();
    std::printf ("%s: %d checks, %d failures\n", failures ? "FAILED" : "OK", checks, failures);
    return failures ? 1 : 0;
}
