// Formula.h — the Wavetable Editor's formula parser (spec E §5) and the
// formula presets (§5.4).
//
// A formula compiles to a small stack bytecode (with constant folding) that
// is evaluated a whole block at a time: every instruction runs one tight loop
// over 2048 samples (or 512 FFT bins), so evaluation does no allocation and
// no per-sample dispatch. Grammar (muParser-like):
//   ternary  := or-expr [ '?' ternary ':' ternary ]           (lowest, right assoc)
//   binary   := && (1)  || (2)  < > <= >= == != (4)  + - (5)  * / % (6)  ^ (7, right assoc)
//   unary    := '-' | '+' | '!'  applied to a power expression  (-x^2 = -(x^2))
//   primary  := number | variable | constant | func '(' args ')' | '(' ternary ')'
// Functions: sin cos tan asin acos atan sinh cosh tanh asinh acosh atanh log2
//   log10 log (=log10) ln exp sqrt sign rint abs, variadic min max sum avg.
// Constants/variables: pi e x w y z q in sel rand (names are case-insensitive).
// Comparisons and logic give 1/0; non-finite results become 0.
//
// Variables (per sample i of 2048):  x = -1 + 2i/2048 (-1..1),  w = i/2048 (0..1),
// y = table index 0..1, z = 2y-1, rand = -1..1 per position (same in every frame),
// in = the frame's existing sample, sel = the sample of the frame selected when
// evaluation began. If `q` appears, the formula runs over FFT bins q = 1..512
// instead: its value v sets harmonic q to amplitude v*|v| (the FFT area's
// sqrt display scale, so (1/q)^0.5 is a normal saw), `in`/`sel` read bins the
// same way, phases are kept from the existing frame, bins above 512 and DC are
// cleared and the result is scaled down if it would clip.
// If `y` or `z` appears, all 256 frames are generated (y = i/255); otherwise
// only the current frame.
#pragma once

#include "WaveEdit.h"

#include <string>
#include <vector>

namespace tincture::wtedit
{
struct FormulaError
{
    int position = -1;            // character index into the formula, -1 = none
    std::string message;
    bool ok() const noexcept { return message.empty(); }
};

class Formula
{
public:
    enum Var : uint8_t { X = 0, W, Y, Z, Q, In, Sel, Rand, NumVars };

    Formula() = default;
    bool compile (const std::string& text, FormulaError& error);
    bool valid() const noexcept { return ! code.empty(); }
    const std::string& text() const noexcept { return source; }

    bool uses (Var v) const noexcept { return (usedVars >> v) & 1u; }
    bool multiFrame() const noexcept { return uses (Y) || uses (Z); }  // regenerates the whole table
    bool spectral() const noexcept { return uses (Q); }                // writes FFT bins
    bool constant() const noexcept { return isConstant; }

    // Block evaluation. Arrays that the formula doesn't use may be null.
    struct Inputs
    {
        int n = kFrameSize;                     // 2048 (time) or 512 (bins)
        const float* x = nullptr;               // these four are filled in by evaluate()
        const float* w = nullptr;               //   when null (standard ramps / bin numbers)
        const float* q = nullptr;
        const float* rand = nullptr;
        const float* in = nullptr;
        const float* sel = nullptr;
        float y = 0.0f;
    };
    void evaluate (const Inputs& in, float* out) const noexcept;
    float evaluateScalar (float x, float y = 0.0f) const noexcept;      // convenience (tests, UI preview)

private:
    friend struct FormulaCompiler;
    struct Ins { uint8_t op = 0, arg = 0; uint16_t argc = 0; float value = 0.0f; };
    std::vector<Ins> code;
    mutable std::vector<float> stack;       // depth * 2048 scratch, sized at compile time
    int depth = 0;
    uint32_t usedVars = 0;
    bool isConstant = false;
    std::string source;
};

// Evaluate into the table: returns false (and the error) if the formula
// doesn't compile. Uses the rules above; `seed` sets the rand table.
// result.frames = how many frames were written.
struct FormulaResult
{
    bool ok = false;
    FormulaError error;
    int frames = 0;
    bool spectral = false;
};
FormulaResult applyFormula (const std::string& text, FrameSet& set, Selection& sel, uint32_t seed = 1);
FormulaResult applyFormula (const Formula& f, FrameSet& set, Selection& sel, uint32_t seed = 1);

// ---------------------------------------------------------------------------
// Formula presets (§5.4). Menu: Singles > / Multis > / User Singles > /
// User Multis > / ---- / Save Formula...
// Files hold one entry per line: [formula][name]. The plugin layer owns the
// paths (FormulaFactorySingles.txt, FormulaUserSingles.txt, ...).
// ---------------------------------------------------------------------------
struct FormulaPreset { std::string name, formula; };

const std::vector<FormulaPreset>& factorySingles();
const std::vector<FormulaPreset>& factoryMultis();
const std::vector<const char*>& formulaMenuItems();     // the five entries above + "-" separator
std::vector<FormulaPreset> parseFormulaFile (const std::string& text);
std::string writeFormulaFile (const std::vector<FormulaPreset>& presets);
// "Save Formula...": which user list it goes to (true = User Multis).
bool isMultiFormula (const std::string& formula);
// Add (or replace by name) a preset in a user list; returns the new file text.
std::string addToFormulaFile (const std::string& fileText, const FormulaPreset& p);
} // namespace tincture::wtedit
