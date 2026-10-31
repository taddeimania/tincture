// EngineWaveEditTests.cpp — tests + benchmarks for the Wavetable Editor data
// side: WaveEdit (FFT, brushes, frame ops, menus, morph), Formula (parser,
// evaluation, presets) and WaveImport (pitch detection, import modes,
// overrides, images, export).
#include "Engine/Formula.h"
#include "Engine/WaveEdit.h"
#include "Engine/WaveImport.h"

#include <chrono>
#include <cstdio>
#include <string>

using namespace tincture;
using namespace tincture::wtedit;

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++checks; const double va_ = (a), vb_ = (b); if (! (std::abs (va_ - vb_) <= (tol))) { ++failures; \
    std::printf ("  FAIL %s:%d  %s = %g, expected %g (tol %g)\n", __FILE__, __LINE__, #a, va_, vb_, (double) (tol)); } } while (0)

constexpr double kPiD = 3.14159265358979323846;
constexpr int N = kFrameSize;

double nowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli> (steady_clock::now().time_since_epoch()).count();
}

float evalF (const std::string& s, float x = 0.0f, float y = 0.0f)
{
    Formula f;
    FormulaError e;
    if (! f.compile (s, e)) { std::printf ("  compile failed: %s (%s at %d)\n", s.c_str(), e.message.c_str(), e.position); return -12345.0f; }
    return f.evaluateScalar (x, y);
}

int errPos (const std::string& s, std::string* msg = nullptr)
{
    Formula f;
    FormulaError e;
    if (f.compile (s, e)) return -100;
    if (msg) *msg = e.message;
    return e.position;
}

double corr (const Frame& a, const Frame& b)
{
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < N; ++i) { ab += (double) a[(size_t) i] * b[(size_t) i]; aa += (double) a[(size_t) i] * a[(size_t) i]; bb += (double) b[(size_t) i] * b[(size_t) i]; }
    return aa > 0 && bb > 0 ? ab / std::sqrt (aa * bb) : 0.0;
}

Frame sineFrame (int h, float amp = 1.0f, float ph = 0.0f)
{
    Frame f;
    for (int i = 0; i < N; ++i) f[(size_t) i] = amp * std::sin ((float) (2.0 * kPiD * h * i / N) + ph);
    return f;
}

Frame randomFrame (uint32_t seed)
{
    Rng r (seed);
    Frame f;
    for (auto& v : f) v = r.bi();
    return f;
}

double maxDiff (const Frame& a, const Frame& b)
{
    double m = 0;
    for (int i = 0; i < N; ++i) m = std::max (m, (double) std::abs (a[(size_t) i] - b[(size_t) i]));
    return m;
}

// ---------------------------------------------------------------------------
void testFormulaParser()
{
    std::printf ("formula parser\n");
    CHECK_NEAR (evalF ("1+2*3"), 7, 1e-6);
    CHECK_NEAR (evalF ("(1+2)*3"), 9, 1e-6);
    CHECK_NEAR (evalF ("2^3^2"), 512, 1e-3);           // right assoc
    CHECK_NEAR (evalF ("-2^2"), -4, 1e-6);             // unary minus looser than ^
    CHECK_NEAR (evalF ("2^-1"), 0.5, 1e-6);
    CHECK_NEAR (evalF ("-3*2"), -6, 1e-6);
    CHECK_NEAR (evalF ("10-4-3"), 3, 1e-6);            // left assoc
    CHECK_NEAR (evalF ("12/3/2"), 2, 1e-6);
    CHECK_NEAR (evalF ("7%3"), 1, 1e-6);
    CHECK_NEAR (evalF ("2+7%3*2"), 4, 1e-6);           // % at * level
    CHECK_NEAR (evalF ("1+2<4"), 1, 1e-6);             // + binds tighter than <
    CHECK_NEAR (evalF ("3>=3"), 1, 1e-6);
    CHECK_NEAR (evalF ("3<=2"), 0, 1e-6);
    CHECK_NEAR (evalF ("3==3"), 1, 1e-6);
    CHECK_NEAR (evalF ("3!=3"), 0, 1e-6);
    CHECK_NEAR (evalF ("1<2&&3>2"), 1, 1e-6);
    CHECK_NEAR (evalF ("1||0&&0"), 0, 1e-6);           // spec: || (2) binds tighter than && (1)
    CHECK_NEAR (evalF ("0&&1||1"), 0, 1e-6);
    CHECK_NEAR (evalF ("!0"), 1, 1e-6);
    CHECK_NEAR (evalF ("1?2:3"), 2, 1e-6);
    CHECK_NEAR (evalF ("0?1:0?2:3"), 3, 1e-6);         // right-assoc ternary
    CHECK_NEAR (evalF ("1+1?5:6"), 5, 1e-6);           // ternary lowest
    CHECK_NEAR (evalF ("x<0?-1:1", -0.5f), -1, 1e-6);
    CHECK_NEAR (evalF ("x<0?-1:1", 0.5f), 1, 1e-6);
    CHECK_NEAR (evalF ("sin(x*pi)", 0.5f), 1, 1e-6);
    CHECK_NEAR (evalF ("pi"), kPiD, 1e-6);
    CHECK_NEAR (evalF ("e"), 2.718281828, 1e-6);
    CHECK_NEAR (evalF ("2e-1"), 0.2, 1e-7);
    CHECK_NEAR (evalF ("2*e"), 5.436563657, 1e-5);
    CHECK_NEAR (evalF (".5+1.25"), 1.75, 1e-7);
    CHECK_NEAR (evalF ("w", 0.0f), 0.5, 1e-7);
    CHECK_NEAR (evalF ("y", 0, 0.25f), 0.25, 1e-7);
    CHECK_NEAR (evalF ("z", 0, 0.25f), -0.5, 1e-7);
    CHECK_NEAR (evalF ("SIN(X*PI)", 0.5f), 1, 1e-6);   // case-insensitive
    CHECK_NEAR (evalF ("1/0"), 0, 0);                  // non-finite -> 0
    CHECK_NEAR (evalF ("sqrt(-1)"), 0, 0);

    // Every function.
    const float v = 0.37f;
    struct F { const char* name; double ref; };
    const F fs[] {
        { "sin", std::sin (v) }, { "cos", std::cos (v) }, { "tan", std::tan (v) },
        { "asin", std::asin (v) }, { "acos", std::acos (v) }, { "atan", std::atan (v) },
        { "sinh", std::sinh (v) }, { "cosh", std::cosh (v) }, { "tanh", std::tanh (v) },
        { "asinh", std::asinh (v) }, { "acosh", std::acosh (1.0 + v) }, { "atanh", std::atanh (v) },
        { "log2", std::log2 (v) }, { "log10", std::log10 (v) }, { "log", std::log10 (v) }, { "ln", std::log (v) },
        { "exp", std::exp (v) }, { "sqrt", std::sqrt (v) }, { "sign", 1.0 }, { "rint", 0.0 }, { "abs", v },
    };
    for (auto& f : fs)
    {
        const std::string arg = std::string (f.name) == "acosh" ? "1.37" : "0.37";
        CHECK_NEAR (evalF (std::string (f.name) + "(" + arg + ")"), f.ref, 1e-5);
        // Also through the block path (not folded).
        CHECK_NEAR (evalF (std::string (f.name) + "(x*0+" + arg + ")", 0.3f), f.ref, 1e-5);
    }
    CHECK_NEAR (evalF ("sign(-3)"), -1, 0);
    CHECK_NEAR (evalF ("sign(0)"), 0, 0);
    CHECK_NEAR (evalF ("rint(2.5)"), 3, 0);
    CHECK_NEAR (evalF ("rint(-1.4)"), -1, 0);
    CHECK_NEAR (evalF ("abs(-2)"), 2, 0);
    CHECK_NEAR (evalF ("min(3,1,2)"), 1, 0);
    CHECK_NEAR (evalF ("max(3,1,2,x)", 5.0f), 5, 0);
    CHECK_NEAR (evalF ("sum(1,2,3,4)"), 10, 0);
    CHECK_NEAR (evalF ("avg(1,2,3,4)"), 2.5, 1e-6);
    CHECK_NEAR (evalF ("min(x)", 0.25f), 0.25, 0);
    CHECK_NEAR (evalF ("max(min(1,2),avg(4,x))", 2.0f), 3, 1e-6);

    // Flags.
    {
        Formula f;
        FormulaError e;
        CHECK (f.compile ("(1/q)^z", e) && f.spectral() && f.multiFrame());
        CHECK (f.compile ("sin(x*pi)", e) && ! f.spectral() && ! f.multiFrame());
        CHECK (f.compile ("2*3+pi", e) && f.constant());
        CHECK (f.compile ("in*0.5+sel*rand", e) && f.uses (Formula::In) && f.uses (Formula::Sel) && f.uses (Formula::Rand));
    }

    // Errors with positions.
    std::string msg;
    CHECK (errPos ("") == 0);
    CHECK (errPos ("sin(x", &msg) == 5); CHECK (msg.find ("')'") != std::string::npos);
    CHECK (errPos ("1+") == 2);
    CHECK (errPos ("foo(1)", &msg) == 0); CHECK (msg.find ("foo") != std::string::npos);
    CHECK (errPos ("x $ 2", &msg) == 2); CHECK (msg.find ('$') != std::string::npos);
    CHECK (errPos ("sin(1,2)", &msg) == 0); CHECK (msg.find ("1 argument") != std::string::npos);
    CHECK (errPos ("(1+2") == 4);
    CHECK (errPos ("1 ? 2", &msg) == 5); CHECK (msg.find ("':'") != std::string::npos);
    CHECK (errPos ("1 2") == 2);
    CHECK (errPos ("x+*2") == 2);
    CHECK (errPos ("sin x") == 4);
    CHECK (errPos ("min()") == 0);
    CHECK (errPos ("1.2.3") == 0);
    CHECK (errPos ("max(1,)") == 6);
}

void testFormulaFrames()
{
    std::printf ("formula frames\n");
    FrameSet s = FrameSet::initSaw();
    Selection sel;

    auto r = applyFormula ("sin(x*pi)", s, sel);
    CHECK (r.ok && r.frames == 1 && ! r.spectral && s.size() == 1);
    double err = 0;
    for (int i = 0; i < N; ++i) err = std::max (err, std::abs (s.frames[0][(size_t) i] - std::sin (kPiD * (-1.0 + 2.0 * i / N))));
    CHECK (err < 2e-6);

    applyFormula ("x<0?-1:1", s, sel);
    CHECK (s.frames[0][0] == -1.0f && s.frames[0][N / 2 - 1] == -1.0f && s.frames[0][N / 2] == 1.0f && s.frames[0][N - 1] == 1.0f);

    applyFormula ("(rint(16*abs(x))/8.0)-1", s, sel);
    CHECK_NEAR (s.frames[0][0], 1.0, 1e-6);           // x = -1 -> rint(16)/8-1 = 1
    CHECK_NEAR (s.frames[0][N / 2], -1.0, 1e-6);      // x = 0

    // Spectral: first 16 harmonics, equal level.
    r = applyFormula ("q<17", s, sel);
    CHECK (r.ok && r.spectral);
    {
        Spectrum sp;
        analyse (s.frames[0], sp);
        CHECK (sp.mag[1] > 0.01f);
        bool flat = true;
        for (int k = 2; k <= 16; ++k) flat &= std::abs (sp.mag[(size_t) k] - sp.mag[1]) < 1e-4f * sp.mag[1] + 1e-6f;
        CHECK (flat);
        float hi = 0;
        for (int k = 17; k <= kNyquist; ++k) hi = std::max (hi, std::abs (sp.mag[(size_t) k]));
        CHECK (hi < 1e-5f);
        CHECK (peakOf (s.frames[0]) <= 1.0001f);
    }
    // (1/q)^0.5 = normal saw (amplitude 1/q); ^0.25 brighter.
    applyFormula ("(1/q)^0.5", s, sel);
    {
        Spectrum sp;
        analyse (s.frames[0], sp);
        CHECK_NEAR (sp.mag[2] / sp.mag[1], 0.5, 1e-3);
        CHECK_NEAR (sp.mag[10] / sp.mag[1], 0.1, 1e-3);
        CHECK (std::abs (sp.mag[600]) < 1e-5f);        // bins > 512 cleared
    }
    // Squarify keeps odd harmonics of a saw.
    s = FrameSet::initSaw();
    applyFormula ("((q%2)==1)?in:0", s, sel);
    {
        Spectrum sp;
        analyse (s.frames[0], sp);
        CHECK (sp.mag[2] < 1e-5f && sp.mag[4] < 1e-5f);
        CHECK_NEAR (sp.mag[3] / sp.mag[1], 1.0 / 3.0, 1e-3);
    }
    // `in` is the identity (time domain).
    s = FrameSet::initSaw();
    const Frame saw = s.frames[0];
    applyFormula ("in", s, sel);
    CHECK (maxDiff (s.frames[0], saw) == 0.0);

    // Multi-frame: y across 256 frames; rand identical across frames.
    r = applyFormula ("y", s, sel);
    CHECK (r.frames == 256 && s.size() == 256);
    CHECK_NEAR (s.frames[0][5], 0.0, 1e-7);
    CHECK_NEAR (s.frames[255][5], 1.0, 1e-7);
    CHECK_NEAR (s.frames[51][5], 51.0 / 255.0, 1e-6);
    applyFormula ("rand+0*y", s, sel);
    CHECK (maxDiff (s.frames[0], s.frames[200]) == 0.0 && peakOf (s.frames[0]) > 0.9f);
    // Spectral multi: (1/q)^z gets darker with frame index.
    applyFormula ("(1/q)^z", s, sel);
    {
        Spectrum a, b;
        analyse (s.frames[64], a);
        analyse (s.frames[250], b);
        CHECK (b.mag[8] / b.mag[1] < a.mag[8] / a.mag[1]);
    }
    // q<z*256: frame i has max(0, round-ish(z*256)-1) harmonics.
    applyFormula ("q<z*256", s, sel);
    {
        Spectrum sp;
        analyse (s.frames[255], sp);
        CHECK (sp.mag[255] > 1e-4f && sp.mag[256] < 1e-5f);
        CHECK (peakOf (s.frames[0]) < 1e-6f);           // z = -1: nothing
    }
    // sel = frame selected at the start (constant across tables).
    s.frames.assign (2, Frame {});
    s.frames[1] = sineFrame (1);
    sel.select (1);
    applyFormula ("sel*(1-y)", s, sel);
    CHECK (s.size() == 256 && maxDiff (s.frames[0], sineFrame (1)) < 1e-6 && peakOf (s.frames[255]) < 1e-6);

    // Bad formula leaves the table alone.
    s = FrameSet::initSaw();
    r = applyFormula ("sin(", s, sel);
    CHECK (! r.ok && r.error.position == 4 && maxDiff (s.frames[0], saw) == 0.0);

    // Presets.
    for (auto& p : factorySingles())
    {
        Formula f;
        FormulaError e;
        const bool ok = f.compile (p.formula, e);
        CHECK (ok && ! f.multiFrame());
        if (! ok) std::printf ("    single '%s': %s\n", p.name.c_str(), e.message.c_str());
    }
    for (auto& p : factoryMultis())
    {
        Formula f;
        FormulaError e;
        CHECK (f.compile (p.formula, e) && f.multiFrame());
    }
    CHECK (formulaMenuItems().size() == 6 && std::string (formulaMenuItems()[5]) == "Save Formula...");
    const auto parsed = parseFormulaFile ("[sin(x*pi)][Sine]\n  \n[q<17][First 16]\r\n[x]\nnot a preset\n");
    CHECK (parsed.size() == 3 && parsed[0].name == "Sine" && parsed[1].formula == "q<17" && parsed[2].name == "x");
    CHECK (parseFormulaFile (writeFormulaFile (parsed)).size() == 3);
    const std::string file = addToFormulaFile (writeFormulaFile (parsed), { "Sine", "sin(2*x*pi)" });
    CHECK (parseFormulaFile (file).size() == 3 && parseFormulaFile (file)[0].formula == "sin(2*x*pi)");
    CHECK (isMultiFormula ("(1/q)^z") && ! isMultiFormula ("q<17"));
}

void testFft()
{
    std::printf ("fft / harmonics\n");
    const Frame f = randomFrame (7);
    Spectrum s;
    analyse (f, s);
    Frame g;
    synthesise (s, g);
    const double rt = maxDiff (f, g);
    std::printf ("    round trip max error %.2e\n", rt);
    CHECK (rt < 1e-5);

    // A known sine reads back as the right harmonic/phase.
    const Frame sn = sineFrame (5, 0.3f, 1.0f);
    CHECK_NEAR (harmonicMag (sn, 5), 0.3, 1e-5);
    CHECK_NEAR (harmonicPhase (sn, 5), 1.0, 1e-4);
    CHECK (harmonicMag (sn, 4) < 1e-5f);

    Frame z {};
    CHECK (setHarmonic (z, 5, 0.3f, 1.0f));
    CHECK (maxDiff (z, sn) < 1e-5);
    CHECK (setHarmonic (z, 0, 0.25f, 0.0f));          // DC
    CHECK_NEAR (harmonicMag (z, 0), 0.25, 1e-6);
    CHECK (! setHarmonic (z, 5, 0.3f, 1.0f) || maxDiff (z, sn) > 0.2);   // setting the same value is (nearly) a no-op

    // FFT-area drawing scale: bar = sqrt(mag), ScaleByBin flattens a saw.
    FftDrawOptions o;
    CHECK_NEAR (magToBar (0.25f, 3, o), 0.5, 1e-6);
    o.flags[FftDrawOptions::ScaleByBin] = true;
    CHECK_NEAR (magToBar (0.1f, 10, o), 1.0, 1e-6);
    CHECK_NEAR (barToMag (1.0f, 10, o), 0.1, 1e-6);
    o = {};
    o.flags[FftDrawOptions::SnapQuarters] = true;
    CHECK_NEAR (barToMag (0.6f, 1, o), 0.25, 1e-6);    // bar snaps to 0.5
    o = {};
    o.flags[FftDrawOptions::OddOnly] = true;
    Frame d {};
    CHECK (! drawMagnitude (d, 2, 1.0f, o));
    CHECK (drawMagnitude (d, 3, 1.0f, o));
    CHECK_NEAR (harmonicMag (d, 3), 1.0, 1e-5);
    CHECK (drawPhase (d, 3, 0.75f, o));
    CHECK_NEAR (harmonicPhase (d, 3), 0.5 * kPiD, 1e-4);
    CHECK (nudgePhase (d, 3, 0.25f));
    CHECK_NEAR (harmonicPhase (d, 3), 0.5 * kPiD + 0.25, 1e-4);
}

void testBrushes()
{
    std::printf ("brushes\n");
    Rng rng (3);
    Frame f {};
    CHECK (gridCell (0.3f, 8) == 2);
    int a, b;
    cellRange (2, 8, a, b);
    CHECK (a == 512 && b == 768);
    cellRange (5, 6, a, b);
    CHECK (a == 1706 && b == 2048);
    CHECK_NEAR (snapY (0.3f, 8), 0.25, 1e-6);
    CHECK_NEAR (snapY (-0.9f, 8), -1.0, 1e-6);
    CHECK_NEAR (snapY (0.3f, 0), 0.3, 1e-6);

    CHECK (applyBrush (f, Brush::FlatLine, 0.3f, 0.5f, 8, false, rng));
    CHECK (f[512] == 0.5f && f[767] == 0.5f && f[511] == 0.0f && f[768] == 0.0f);
    CHECK (! applyBrush (f, Brush::FlatLine, 0.3f, 0.5f, 8, false, rng));     // unchanged

    // Sine brush on every cell of grid 4 = harmonic 4.
    f.fill (0.0f);
    for (int c = 0; c < 4; ++c) applyBrush (f, Brush::Sine, (c + 0.5f) / 4.0f, 1.0f, 4, false, rng);
    CHECK (maxDiff (f, sineFrame (4)) < 1e-4);

    // Shapes hit their endpoints.
    f.fill (0.0f);
    applyBrush (f, Brush::SlopeUp, 0.05f, 0.8f, 8, false, rng);
    CHECK_NEAR (f[0], -0.8, 1e-6);
    CHECK (f[255] > 0.79f);
    applyBrush (f, Brush::HalfSineValley, 0.2f, 1.0f, 8, false, rng);
    CHECK (f[256 + 128] < -0.99f);
    applyBrush (f, Brush::CurveDown, 0.4f, 1.0f, 8, false, rng);
    CHECK_NEAR (f[768], 1.0, 1e-6);

    // Interpolate linear joins neighbours.
    f.fill (0.0f);
    f[255] = 0.0f; f[512] = 1.0f;
    applyBrush (f, Brush::InterpolateLinear, 0.2f, 0.0f, 8, false, rng);
    CHECK (f[256] > 0.0f && f[511] < 1.0f && f[300] < f[400]);

    // Mirror: odd symmetry about the centre.
    f.fill (0.0f);
    applyBrush (f, Brush::FlatLine, 0.1f, 0.7f, 8, true, rng);
    CHECK (f[100] == 0.7f && f[N - 1 - 100] == -0.7f);

    // Nudge / noise.
    f.fill (0.0f);
    applyBrush (f, Brush::Nudge, 0.1f, 0.2f, 8, false, rng);
    CHECK_NEAR (f[10], 0.2, 1e-6);
    applyBrush (f, Brush::Noise, 0.1f, 0.5f, 8, false, rng);
    float dev = 0;
    for (int i = 0; i < 256; ++i) dev = std::max (dev, std::abs (f[(size_t) i] - 0.2f));
    CHECK (dev > 0.1f);
    const Frame noisy = f;
    applyBrush (f, Brush::Noise, 0.1f, -1.0f, 8, false, rng);
    double eN = 0, eS = 0;
    for (int i = 10; i < 246; ++i) { eN += std::abs (noisy[(size_t) i + 1] - noisy[(size_t) i]); eS += std::abs (f[(size_t) i + 1] - f[(size_t) i]); }
    CHECK (eS < eN * 0.6);

    f.fill (0.0f);
    CHECK (drawLine (f, 0.0f, -1.0f, 0.5f, 1.0f, false));
    CHECK_NEAR (f[0], -1.0, 1e-6);
    CHECK_NEAR (f[1024], 1.0, 1e-6);
    CHECK_NEAR (f[512], 0.0, 0.01);

    f = sineFrame (1);
    CHECK (shiftByGrid (f, 4, 1));
    CHECK_NEAR (f[512], 0.0, 1e-5);      // sine moved right by 512
    CHECK (nudgeVertical (f, 0.1f));
}

void testFrameOps()
{
    std::printf ("frame ops\n");
    FrameSet s;
    for (int i = 0; i < 9; ++i) s.frames.push_back (sineFrame (i + 1));
    Selection sel;
    sel.select (0);
    sel.extendTo (2);
    CHECK (sel.first == 0 && sel.last == 2 && sel.count() == 3);
    std::vector<Frame> clip;
    copyFrames (s, sel, clip);
    CHECK (clip.size() == 3);
    sel.select (8);
    CHECK (pasteFrames (s, sel, clip));
    CHECK (s.size() == 11);                               // spec example: 9 frames, paste 3 at 9 -> 11
    CHECK (maxDiff (s.frames[10], sineFrame (3)) == 0.0);

    sel.select (0);
    CHECK (insertFrame (s, sel) && s.size() == 12 && sel.current == 1);
    CHECK (removeFrame (s, sel) && s.size() == 11);
    CHECK (moveFrame (s, sel, 0, 3));
    CHECK (maxDiff (s.frames[2], sineFrame (1)) == 0.0 && sel.current == 2);
    CHECK (appendFrame (s, sel) && s.size() == 12);
    CHECK (replaceFrame (s, 0, sineFrame (20)) && ! replaceFrame (s, 0, sineFrame (20)));

    // REMOVE menu.
    EditContext c { s, sel };
    sel.select (3);
    sel.extendTo (5);
    CHECK (runOp (WaveMenu::Remove, "Remove: multiselection", c) && s.size() == 9);
    sel.select (4);
    CHECK (runOp (WaveMenu::Remove, "Remove: beginning->selected", c) && s.size() == 5 && sel.current == 0);
    sel.select (1);
    CHECK (runOp (WaveMenu::Remove, "Remove: selected->end", c) && s.size() == 2);
    s.frames.resize (16, sineFrame (2));
    CHECK (runOp (WaveMenu::Remove, "Every 4th Frame", c) && s.size() == 4);
    sel.select (1); sel.extendTo (2);
    CHECK (runOp (WaveMenu::Remove, "Remove All (except Selected)", c) && s.size() == 2);
    CHECK (runOp (WaveMenu::Remove, "Init All (Default)", c) && s.size() == 1);
    CHECK (! runOp (WaveMenu::Remove, "Remove (current index)", c));    // never empties

    // SORT.
    s.frames = { sineFrame (5, 0.2f), sineFrame (1, 0.9f), sineFrame (3, 0.5f) };
    CHECK (runOp (WaveMenu::Sort, "Sort by spectrum (Peak Spect)", c));
    CHECK (harmonicMag (s.frames[0], 1) > 0.8f && harmonicMag (s.frames[2], 5) > 0.1f);
    CHECK (runOp (WaveMenu::Sort, "Sort by spectrum (Peak Amount)", c));
    CHECK (harmonicMag (s.frames[0], 5) > 0.1f);
    CHECK (runOp (WaveMenu::Sort, "Sort by spectrum (Highest w/Spect)", c));
    CHECK (harmonicMag (s.frames[2], 5) > 0.1f);
    CHECK (runOp (WaveMenu::Sort, "Reverse Entire Table Order", c));
    CHECK (harmonicMag (s.frames[0], 5) > 0.1f);
    CHECK (runOp (WaveMenu::Sort, "Sort by spectrum (Average Spect)", c));
    CHECK (harmonicMag (s.frames[0], 1) > 0.8f);
    CHECK (runOp (WaveMenu::Sort, "Sort by spectrum (Fundamental Amt.)", c));
    CHECK (harmonicMag (s.frames[2], 1) > 0.8f);
    CHECK (! runOp (WaveMenu::Sort, "Sort by spectrum (Num w/Spect)", c));    // all have one bin: stable, unchanged
    CHECK (runOp (WaveMenu::Sort, "Sort Randomize", c));
}

void testProcess()
{
    std::printf ("process / fft menus\n");
    FrameSet s;
    Selection sel;
    EditContext c { s, sel };

    Frame f = sineFrame (1, 0.5f);
    for (auto& v : f) v += 0.2f;
    s.frames = { f };
    CHECK (runOp (WaveMenu::ProcessSingle, "Remove DC Offset", c));
    double mean = 0;
    for (float v : s.frames[0]) mean += v;
    CHECK (std::abs (mean / N) < 1e-6);
    CHECK (runOp (WaveMenu::ProcessSingle, "Normalize", c));
    CHECK_NEAR (peakOf (s.frames[0]), 1.0, 1e-6);
    CHECK (! runOp (WaveMenu::ProcessSingle, "Normalize", c));         // already normalised
    const Frame before = s.frames[0];
    CHECK (runOp (WaveMenu::ProcessSingle, "Flip Vertical (Polarity Invert)", c));
    CHECK (s.frames[0][100] == -before[100]);
    s.frames[0] = randomFrame (11);
    const Frame rnd = s.frames[0];
    CHECK (runOp (WaveMenu::ProcessSingle, "Flip Horizontal (Reverse)", c));
    CHECK (s.frames[0][0] == rnd[0] && s.frames[0][1] == rnd[N - 1] && s.frames[0][N - 1] == rnd[1]);
    s.frames[0] = sineFrame (1);
    CHECK (runOp (WaveMenu::ProcessSingle, "Flip Horizontal (Reverse)", c));
    for (auto& v : s.frames[0]) v = -v;
    CHECK (maxDiff (s.frames[0], sineFrame (1)) < 1e-6);               // reversed sine = -sine

    s.frames[0] = sineFrame (1, 1.0f, 1.0f);
    CHECK (runOp (WaveMenu::ProcessSingle, "Shift Horizontal to Zero Crossing", c));
    CHECK (s.frames[0][0] >= 0.0f && s.frames[0][N - 1] < 0.0f);
    CHECK (! runOp (WaveMenu::ProcessSingle, "Shift Horizontal to Zero Crossing", c));

    s.frames[0] = sineFrame (1, 1.0f, 0.5f);
    CHECK (runOp (WaveMenu::ProcessSingle, "Fade Edges (Grid Size)", c));
    CHECK (s.frames[0][0] == 0.0f && std::abs (s.frames[0][N - 1]) < 0.01f);
    s.frames[0] = randomFrame (5);
    c.gridX = 8;
    CHECK (runOp (WaveMenu::ProcessSingle, "X-Fade Edges (Grid Size)", c));
    CHECK_NEAR (s.frames[0][N - 1], s.frames[0][0], 1e-6);
    s.frames[0] = randomFrame (5);
    CHECK (runOp (WaveMenu::ProcessSingle, "Filter (Grid Size)", c));
    {
        Spectrum sp;
        analyse (s.frames[0], sp);
        float hi = 0;
        for (int k = 9; k <= kNyquist; ++k) hi = std::max (hi, std::abs (sp.mag[(size_t) k]));
        CHECK (hi < 1e-5f && sp.mag[3] > 1e-3f && std::abs (sp.mag[0]) < 1e-6f);
    }
    s.frames[0] = randomFrame (5);
    CHECK (runOp (WaveMenu::ProcessSingle, "Sample Redux at Grid Size", c));
    CHECK (s.frames[0][1] == s.frames[0][0] && s.frames[0][7] == s.frames[0][0] && s.frames[0][8] != s.frames[0][0]);
    std::vector<float> noise;
    c.noiseOut = &noise;
    runOp (WaveMenu::ProcessSingle, "Send to Noise Oscillator", c);
    CHECK (noise.size() == (size_t) N && noise[0] == s.frames[0][0]);
    CHECK (runOp (WaveMenu::ProcessSingle, "Init (Silence)", c) && peakOf (s.frames[0]) == 0.0f);

    // ALL.
    s.frames = { sineFrame (1, 0.5f), sineFrame (2, 0.25f), sineFrame (3, 0.1f) };
    CHECK (runOp (WaveMenu::ProcessAll, "Normalize Same (Max From All Frames)", c));
    CHECK_NEAR (peakOf (s.frames[0]), 1.0, 1e-5);
    CHECK_NEAR (peakOf (s.frames[1]), 0.5, 1e-5);
    CHECK (runOp (WaveMenu::ProcessAll, "Normalize Each (Gained Separately)", c));
    CHECK_NEAR (peakOf (s.frames[2]), 1.0, 1e-5);
    s.frames = { FrameSet::initSaw().frames[0], FrameSet::initSaw().frames[0] };
    CHECK (runOp (WaveMenu::ProcessAll, "Remove Fundamental (HPF)", c));
    CHECK (harmonicMag (s.frames[1], 1) < 1e-5f && harmonicMag (s.frames[1], 2) > 0.1f);
    c.gridX = 4;
    CHECK (runOp (WaveMenu::ProcessAll, "Remove Low Spectra (Grid Size)", c));
    CHECK (harmonicMag (s.frames[0], 4) < 1e-5f && harmonicMag (s.frames[0], 5) > 0.05f);
    c.gridX = 8;

    s.frames = { sineFrame (1), sineFrame (2), sineFrame (3) };
    CHECK (runOp (WaveMenu::ProcessAll, "Resize Tables to be Half (2x Total Number)", c) && s.size() == 6);
    s.frames = { sineFrame (2), sineFrame (2) };
    CHECK (runOp (WaveMenu::ProcessAll, "Resize Tables to be Double (\xC2\xBD Total Number)", c) && s.size() == 1);
    CHECK_NEAR (harmonicMag (s.frames[0], 4), 1.0, 0.01);           // two cycles of h2 in one frame = h4
    s.frames = { FrameSet::initSaw().frames[0] };
    sel.select (0);
    CHECK (runOp (WaveMenu::ProcessAll, "Create PWM from This Table to All", c) && s.size() == 256);
    CHECK (harmonicMag (s.frames[0], 2) < 0.01f && harmonicMag (s.frames[0], 3) > 0.1f);   // 50% = square
    s.frames = { sineFrame (1, 1.0f, 0.7f), sineFrame (1, 1.0f, -2.0f) };
    CHECK (runOp (WaveMenu::ProcessAll, "Nudge All Phases for Fundamental to 50%", c));
    CHECK (maxDiff (s.frames[0], sineFrame (1)) < 1e-4 && maxDiff (s.frames[1], sineFrame (1)) < 1e-4);
    s.frames = { sineFrame (3), sineFrame (1, 0.5f) };
    sel.select (0);
    CHECK (runOp (WaveMenu::ProcessAll, "Set Spectra (This Frame to All)", c));
    CHECK_NEAR (harmonicMag (s.frames[1], 3), 1.0, 1e-4);
    CHECK (harmonicMag (s.frames[1], 1) < 1e-4f);
    FrameSet other;
    other.frames = { sineFrame (7, 0.3f) };
    c.otherOsc = &other;
    CHECK (runOp (WaveMenu::ProcessAll, "Set Spectra from Other Osc", c));
    CHECK_NEAR (harmonicMag (s.frames[1], 7), 0.3, 1e-4);
    s.frames = { FrameSet::initSaw().frames[0] };
    other.frames = { FrameSet::initSaw().frames[0] };
    CHECK (runOp (WaveMenu::ProcessAll, "Subtract Spectra from Other Osc", c));
    CHECK (peakOf (s.frames[0]) < 1e-4f);
    c.otherOsc = nullptr;
    s.frames = { sineFrame (10), sineFrame (10, 0.0f) };
    CHECK (runOp (WaveMenu::ProcessAll, "Blur Spectra - Adjacent Bins (Grid Size)", c));
    CHECK (harmonicMag (s.frames[0], 11) > 0.05f && harmonicMag (s.frames[0], 10) < 0.5f);
    s.frames = { sineFrame (10), Frame {} };
    CHECK (runOp (WaveMenu::ProcessAll, "Blur Spectra - Adjacent Frames (Grid Size)", c));
    CHECK_NEAR (harmonicMag (s.frames[1], 10), 0.5, 1e-3);

    // Smoke: every op in every menu runs on a 4-frame table.
    std::vector<float> nz;
    FrameSet o2;
    o2.frames = { sineFrame (2) };
    FftDrawOptions fo;
    for (int m = 0; m < (int) WaveMenu::NumMenus; ++m)
    {
        const auto list = waveMenu ((WaveMenu) m);
        for (int i = 0; i < list.count; ++i)
        {
            if (list.items[i].fn == nullptr) continue;
            FrameSet t;
            for (int k = 0; k < 4; ++k) t.frames.push_back (randomFrame ((uint32_t) (k + 1)));
            Selection ts;
            ts.select (1);
            ts.extendTo (2);
            EditContext tc { t, ts };
            tc.bin = 42;
            tc.otherOsc = &o2;
            tc.noiseOut = &nz;
            tc.fftOptions = &fo;
            list.items[i].fn (tc);
            bool finite = t.size() >= 1 && t.size() <= kMaxFrames;
            for (auto& fr : t.frames) for (float v : fr) finite &= std::isfinite (v);
            CHECK (finite);
            if (! finite) std::printf ("    op '%s' broke the table\n", list.items[i].label);
        }
    }

    // Menu wording/order spot checks.
    auto pa = waveMenu (WaveMenu::ProcessAll);
    CHECK (pa.count == 27 && std::string (pa.items[0].label) == "Normalize Each (Gained Separately)"
           && std::string (pa.items[26].label) == "Shift Horizontal to Zero-Crossing");
    auto ps = waveMenu (WaveMenu::ProcessSingle);
    CHECK (ps.count == 11 && std::string (ps.items[10].label) == "Send to Noise Oscillator");
    CHECK (waveMenu (WaveMenu::Sort).count == 8 && waveMenu (WaveMenu::Morph).count == 5);
    auto fm = waveMenu (WaveMenu::FftMags);
    CHECK (fm.items[0].kind == WaveOp::Title && std::string (fm.items[0].label) == "FFT Mags");
    CHECK (menuLabel (fm.items[3], 42) == "Clear HF (Bin 42 to End)" && menuLabel (fm.items[4], 42) == "Clear LF (Start to 42)");

    // FFT menu behaviour.
    s.frames = { randomFrame (9) };
    sel.select (0);
    c.bin = 42;
    CHECK (runOp (WaveMenu::FftMags, "Clear HF (Bin # to End)", c));
    CHECK (harmonicMag (s.frames[0], 42) < 1e-5f && harmonicMag (s.frames[0], 41) > 1e-4f);
    CHECK (runOp (WaveMenu::FftMags, "Clear LF (Start to #)", c));
    CHECK (harmonicMag (s.frames[0], 41) < 1e-5f && peakOf (s.frames[0]) < 1e-4f);
    CHECK (runOp (WaveMenu::FftMags, "Generate Saw", c));
    CHECK_NEAR (harmonicMag (s.frames[0], 2) / harmonicMag (s.frames[0], 1), 0.5, 1e-3);
    CHECK (peakOf (s.frames[0]) <= 1.0001f);
    CHECK (runOp (WaveMenu::FftMags, "Shift Octave Up", c));
    CHECK (harmonicMag (s.frames[0], 1) < 1e-5f && harmonicMag (s.frames[0], 3) < 1e-5f && harmonicMag (s.frames[0], 2) > 0.1f);
    CHECK (runOp (WaveMenu::FftMags, "Shift Octave Down", c));
    CHECK (harmonicMag (s.frames[0], 1) > 0.1f && harmonicMag (s.frames[0], 2) > 0.05f);
    c.bin = 3;
    s.frames[0] = Frame {};
    setHarmonic (s.frames[0], 1, 0.3f, 0.0f);
    CHECK (runOp (WaveMenu::FftMags, "Repeat Bin Group (Start to Cursor Pos)", c));
    CHECK_NEAR (harmonicMag (s.frames[0], 4) / harmonicMag (s.frames[0], 1), 1.0, 1e-3);
    CHECK (harmonicMag (s.frames[0], 5) < 1e-6f && harmonicMag (s.frames[0], 1021) > 1e-4f);
    CHECK (runOp (WaveMenu::FftMags, "Randomize Low 16 Bins", c));
    FftDrawOptions opts;
    c.fftOptions = &opts;
    runOp (WaveMenu::FftMags, "Draw Even Harmonics Only", c);
    CHECK (opts.on (FftDrawOptions::EvenOnly) && isChecked (fm.items[20], c));
    runOp (WaveMenu::FftMags, "Draw Odd Harmonics Only", c);
    CHECK (opts.on (FftDrawOptions::OddOnly) && ! opts.on (FftDrawOptions::EvenOnly));
    s.frames[0] = sineFrame (3, 0.5f, 1.0f);
    CHECK (runOp (WaveMenu::FftPhases, "Clear All", c));
    CHECK (maxDiff (s.frames[0], sineFrame (3, 0.5f)) < 1e-5);
}

void testMorph()
{
    std::printf ("morph\n");
    FrameSet s;
    s.frames = { sineFrame (1), sineFrame (1, 1.0f, 2.0f), sineFrame (3, 0.5f) };
    FrameSet orig = s;
    CHECK (morph (s, MorphMode::Crossfade));
    CHECK (s.size() == 256 && s.keyFrames == std::vector<int> ({ 0, 128, 255 }));
    CHECK (maxDiff (s.frames[128], orig.frames[1]) == 0.0 && maxDiff (s.frames[255], orig.frames[2]) == 0.0);
    {
        Frame mid;
        for (int i = 0; i < N; ++i) mid[(size_t) i] = 0.5f * (orig.frames[0][(size_t) i] + orig.frames[1][(size_t) i]);
        CHECK (maxDiff (s.frames[64], mid) < 1e-6);
    }
    CHECK (s.isKeyFrame (128) && ! s.isKeyFrame (127));
    // Spectral morph between two phase-shifted sines keeps the level (a crossfade would dip).
    CHECK (morph (s, MorphMode::Spectral));
    CHECK (s.size() == 256);
    CHECK_NEAR (harmonicMag (s.frames[64], 1), 1.0, 1e-3);
    CHECK_NEAR (harmonicPhase (s.frames[64], 1), 1.0, 1e-3);
    CHECK (removeMorph (s) && s.size() == 3 && maxDiff (s.frames[1], orig.frames[1]) == 0.0);
    CHECK (morph (s, MorphMode::SpectralZeroAll));
    CHECK (std::abs (harmonicPhase (s.frames[128], 1)) < 1e-3);      // sources altered
    CHECK (removeMorph (s) && ! removeMorph (s));
    CHECK (morph (s, MorphMode::SpectralZeroFund) && s.size() == 256);
    // Limits.
    FrameSet one;
    one.frames = { sineFrame (1) };
    CHECK (! morph (one, MorphMode::Crossfade));
    FrameSet full;
    full.frames.assign (256, sineFrame (1));
    CHECK (! morph (full, MorphMode::Crossfade));
    // 9 frames: keys 0, 32, 64 ... (32 apart), 256 frames.
    FrameSet nine;
    for (int i = 0; i < 9; ++i) nine.frames.push_back (sineFrame (i + 1));
    CHECK (morph (nine, MorphMode::Spectral) && nine.size() == 256 && nine.keyFrames[1] == 32 && nine.keyFrames[8] == 255);
    // Save / load round trip keeps frames, keys and flags.
    Node n ("wt");
    nine.interpolate = false;
    nine.save (n);
    auto bin = n.toBinary();
    Node back;
    CHECK (Node::fromBinary (bin.data(), bin.size(), back));
    const FrameSet l = FrameSet::load (back);
    CHECK (l.size() == 256 && l.keyFrames == nine.keyFrames && ! l.interpolate && maxDiff (l.frames[77], nine.frames[77]) == 0.0);
    CHECK (FrameSet::load (Node ("empty")).size() == 1);
}

std::vector<float> bandLimitedSaw (double hz, double sr, int len, double phase0 = 0.0)
{
    std::vector<float> x ((size_t) len, 0.0f);
    const int harmonics = (int) (sr * 0.5 / hz);
    for (int k = 1; k <= harmonics; ++k)
    {
        const double a = 2.0 / (kPiD * k) * 0.5;
        const double w = 2.0 * kPiD * hz * k / sr;
        for (int i = 0; i < len; ++i) x[(size_t) i] += (float) (a * std::sin (w * i + k * phase0));
    }
    return x;
}

void testImport()
{
    std::printf ("import\n");
    const double sr = 48000.0;
    const int len = (int) (sr * 2.0);
    const auto saw = bandLimitedSaw (220.0, sr, len, 0.3);

    PitchDetector det (sr);
    const PitchEstimate pe = det.detect (saw.data(), len, 1000);
    std::printf ("    YIN period %.4f (exact %.4f), confidence %.3f\n", pe.period, sr / 220.0, pe.confidence);
    CHECK_NEAR (pe.period, sr / 220.0, 0.05);
    CHECK (pe.confidence > 0.9f);

    const ImportMethod methods[] { ImportMethod::PitchAverage, ImportMethod::DynamicZeroSnap, ImportMethod::DynamicFollow,
                                   ImportMethod::FrequencyEstimation };
    const char* names[] { "pitch average", "zero snap", "follow", "freq estimation" };
    for (int m = 0; m < 4; ++m)
    {
        const double t0 = nowMs();
        const auto r = importAudio (saw.data(), len, sr, methods[m], {}, "saw");
        const double ms = nowMs() - t0;
        CHECK (r.ok && r.set.size() > 100 && r.set.size() <= 256);
        double minCorr = 1.0;
        for (int i = 1; i < r.set.size(); ++i) minCorr = std::min (minCorr, corr (r.set.frames[(size_t) i - 1], r.set.frames[(size_t) i]));
        std::printf ("    %-16s frames %3d  cycle %.3f  %.2f Hz  min consecutive corr %.5f  (%.1f ms)\n", names[m], r.set.size(),
                     r.samplesPerCycle, r.detectedHz, minCorr, ms);
        CHECK (minCorr > 0.99);
        CHECK_NEAR (r.detectedHz, 220.0, 0.05);
        // The frame really is one saw cycle: energy in harmonic 1, 1/k slope.
        const Frame& f = r.set.frames[(size_t) r.set.size() / 2];
        CHECK_NEAR (harmonicMag (f, 2) / harmonicMag (f, 1), 0.5, 0.02);
        CHECK_NEAR (harmonicMag (f, 1), 1.0 / kPiD, 0.01);
        if (m == 0) CHECK (r.fieldText == "218");
        if (m == 3) CHECK (r.nearestNote == 57);        // A3 = 220 Hz (MIDI 57)
    }

    auto r = importAudio (saw.data(), len, sr, ImportMethod::FixedFrameSize);
    CHECK (r.ok && r.set.size() == len / 2048 && maxDiff (r.set.frames[1], [&] { Frame f; std::copy (saw.begin() + 2048, saw.begin() + 4096, f.begin()); return f; }()) == 0.0);
    ImportOverride ov;
    ov.samplesPerCycle = 1000;
    ov.interpolate = 0;
    r = importAudio (saw.data(), len, sr, ImportMethod::DynamicZeroSnap, ov);
    CHECK (r.ok && r.set.size() == 96 && ! r.set.interpolate && r.samplesPerCycle == 1000.0);
    for (auto m : { ImportMethod::Fft256, ImportMethod::Fft512, ImportMethod::Fft1024, ImportMethod::Fft2048 })
    {
        r = importAudio (saw.data(), len, sr, m);
        CHECK (r.ok && r.set.size() >= 1 && r.set.size() <= 256);
        float pk = 0;
        for (auto& fr : r.set.frames) pk = std::max (pk, peakOf (fr));
        CHECK_NEAR (pk, 1.0, 1e-5);
    }
    r = importAudio (saw.data(), len, sr, ImportMethod::Fft2048);
    CHECK (r.set.size() == 1 + (len - 2048) / 1024 || r.set.size() == 256);
    // Silence: no pitch -> plain fixed frames, no crash.
    std::vector<float> silence (20000, 0.0f);
    r = importAudio (silence.data(), (int) silence.size(), sr, ImportMethod::DynamicFollow);
    CHECK (r.ok && r.set.size() == 9);
    CHECK (! importAudio (nullptr, 0, sr, ImportMethod::PitchAverage).ok);
    // Long file: capped at 256 frames.
    const auto longSaw = bandLimitedSaw (440.0, sr, (int) (sr * 3.0));
    r = importAudio (longSaw.data(), (int) longSaw.size(), sr, ImportMethod::DynamicFollow);
    CHECK (r.ok && r.set.size() == 256);

    // Single-cycle resampling accuracy (odd and even lengths, up and down).
    for (int L : { 300, 301, 1000, 2048, 4410 })
    {
        std::vector<float> cyc ((size_t) L);
        auto ref = [] (double t) { return std::sin (2 * kPiD * t) + 0.5 * std::sin (2 * kPiD * 3 * t + 0.4) + 0.25 * std::cos (2 * kPiD * 7 * t); };
        for (int i = 0; i < L; ++i) cyc[(size_t) i] = (float) ref ((double) i / L);
        Frame out;
        importSingleCycle (cyc.data(), L, out);
        double e = 0;
        for (int i = 0; i < N; ++i) e = std::max (e, std::abs (out[(size_t) i] - ref ((double) i / N)));
        std::printf ("    single-cycle %4d -> 2048: max error %.2e\n", L, e);
        CHECK (e < 1e-4);
    }
    const FrameSet sc = importSingleCycles ({ std::vector<float> (100, 0.5f), std::vector<float> (600, 0.0f) }, "sc");
    CHECK (sc.size() == 2 && std::abs (sc.frames[0][77] - 0.5f) < 1e-5f);

    // Images.
    std::vector<float> img (32 * 64);
    for (int r2 = 0; r2 < 32; ++r2) for (int c = 0; c < 64; ++c) img[(size_t) (r2 * 64 + c)] = (float) r2 / 31.0f;
    FrameSet im = importImage (img.data(), 64, 32);
    CHECK (im.size() == 64);
    CHECK_NEAR (im.frames[10][0], 0.0, 1e-6);
    CHECK_NEAR (im.frames[10][N - 1], 1.0, 1e-6);
    im = importImage (img.data(), 64, 32, ImageMode::Bipolar);
    CHECK_NEAR (im.frames[10][0], -1.0, 1e-6);
    std::vector<float> wide (300 * 4, 0.0f);
    CHECK (importImage (wide.data(), 300, 4).size() == 256);
    std::vector<float> spec (16 * 3, 0.0f);
    for (int c = 0; c < 3; ++c) spec[(size_t) (15 * 3 + c)] = 1.0f;          // bottom row = fundamental
    im = importImage (spec.data(), 3, 16, ImageMode::Spectral);
    CHECK (im.size() == 3 && harmonicMag (im.frames[1], 1) > 0.9f && harmonicMag (im.frames[1], 2) < 1e-5f);

    // Overrides.
    ImportOverride to;
    CHECK (parseTextOverride ("[1024]\n[no interp]\n", to) && to.samplesPerCycle == 1024 && to.interpolate == 0);
    ImportOverride t2;
    CHECK (! parseTextOverride ("[12]", t2) && t2.samplesPerCycle == 0);   // below 32
    CHECK (parseTextOverride ("[Interp]", t2) && t2.interpolate == 1);
    int smp = 0;
    CHECK (parseFieldOverride ("2048", smp) && smp == 2048);
    CHECK (parseFieldOverride (" 512 ", smp) && smp == 512);
    CHECK (! parseFieldOverride ("12345", smp));
    CHECK (parseFieldOverride ("F1", smp) && smp == 2020);
    CHECK (parseFieldOverride ("C#2", smp) && smp == 1273);
    CHECK (parseFieldOverride ("Db2", smp) && smp == 1273);
    CHECK (! parseFieldOverride ("H2", smp) && ! parseFieldOverride ("sin(x)", smp));
    const std::string echo = splitEcho (2048);
    std::printf ("    %s\n", echo.c_str());
    CHECK (echo.find ("2048 samples (22 Hz, note: F1 and -2") != std::string::npos);
    CHECK (parseClmChunk (makeClmChunk (2048)) == 2048 && parseClmChunk ("junk") == 0);

    // Menus.
    CHECK (importMenu().size() == 9 && importMenu()[7].singleCycle && exportMenu().size() == 5);
    CHECK (std::string (importMenu()[8].label) == "Audio Frequency Estimation...");

    // Export.
    FrameSet ex;
    ex.frames = { sineFrame (1), sineFrame (2), sineFrame (3) };
    CHECK (exportAudio (ex).size() == 3u * N && exportAudio (ex, 1, 1).size() == (size_t) N);
    CHECK (exportAudio (ex, 1, 2)[0] == ex.frames[1][0]);
    CHECK (exportSingleCycles (ex).size() == 3);
    const auto b256 = export256 (ex);
    CHECK (b256.size() == 3 * 256 && b256[0] == 128 && b256[64] == 255 && b256[192] == 1);
    CHECK (singleCycleFileName ("Pad", 0) == "Pad_001.wav");
}

void benchmarks()
{
    std::printf ("benchmarks\n");
    FrameSet s;
    for (int i = 0; i < 2; ++i) s.frames.push_back (randomFrame ((uint32_t) i + 1));
    double t0 = nowMs();
    const int reps = 5;
    for (int r = 0; r < reps; ++r) { removeMorph (s); morph (s, MorphMode::Spectral); }
    std::printf ("    spectral morph -> 256 frames:      %.2f ms\n", (nowMs() - t0) / reps);

    t0 = nowMs();
    for (int r = 0; r < reps; ++r) { removeMorph (s); morph (s, MorphMode::Crossfade); }
    std::printf ("    crossfade morph -> 256 frames:     %.2f ms\n", (nowMs() - t0) / reps);

    Selection sel;
    t0 = nowMs();
    for (int r = 0; r < reps; ++r) applyFormula ("sin(x*pi*(1+7*y))*tanh(3*w)+0.1*rand", s, sel);
    std::printf ("    formula, 256 frames (time):        %.2f ms\n", (nowMs() - t0) / reps);
    t0 = nowMs();
    for (int r = 0; r < reps; ++r) applyFormula ("(1/q)^(1.5-y)", s, sel);
    std::printf ("    formula, 256 frames (spectral):    %.2f ms\n", (nowMs() - t0) / reps);

    Formula f;
    FormulaError e;
    f.compile ("sin(x*pi)+0.5*sin(2*x*pi)*(x<0?1:-1)", e);
    std::array<float, N> out {};
    Formula::Inputs in;
    t0 = nowMs();
    const int evals = 2000;
    for (int r = 0; r < evals; ++r) { in.y = (float) r / evals; f.evaluate (in, out.data()); }
    const double ms = nowMs() - t0;
    std::printf ("    block eval 2048 samples:           %.2f us (%.1f ns/sample)\n", ms * 1000.0 / evals, ms * 1.0e6 / evals / N);

    EditContext c { s, sel };
    t0 = nowMs();
    runOp (WaveMenu::ProcessAll, "Blur Spectra - Adjacent Frames (Grid Size)", c);
    std::printf ("    blur spectra across 256 frames:    %.2f ms\n", nowMs() - t0);

    const auto saw = bandLimitedSaw (110.0, 48000.0, 48000 * 4);
    t0 = nowMs();
    const auto r = importAudio (saw.data(), (int) saw.size(), 48000.0, ImportMethod::DynamicZeroSnap);
    std::printf ("    import 4 s zero-snap (%d frames):  %.2f ms\n", r.set.size(), nowMs() - t0);
}
} // namespace

int main()
{
    testFormulaParser();
    testFormulaFrames();
    testFft();
    testBrushes();
    testFrameOps();
    testProcess();
    testMorph();
    testImport();
    benchmarks();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
