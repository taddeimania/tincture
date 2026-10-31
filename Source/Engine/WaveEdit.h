// WaveEdit.h — the data side of the Wavetable Editor (spec E §3): a set of raw
// single-cycle frames and every editing operation behind the editor's brush
// tools, FFT area, frame buttons and the REMOVE / SORT / PROCESS / MORPH
// menus. Everything works on raw frames; the oscillator's band-limited
// Wavetable (Wavetable.h) is built from FrameSet::frames afterwards.
//
// Message thread only: these functions allocate. Nothing here runs on the
// audio thread.
//
// Conventions
// - A frame is one cycle of 2048 float samples (std::array, so a FrameSet is
//   a std::vector of fixed-size arrays: contiguous, cheap to copy/move).
// - Harmonic n (1..1023) of a frame is  mag[n] * sin (2*pi*n*t/2048 + phase[n]),
//   i.e. phase 0 = sine phase and mag is in sample units (a full-scale sine
//   has mag 1). mag[0] is the signed DC value, mag[1024] the signed Nyquist
//   (cos) value; their phases are always 0.
// - The FFT area shows bins 1..512 as bars 0..1 with bar = sqrt (mag) (so
//   formula `(1/q)^0.5` draws a normal saw, see Formula.h); the phase strip
//   shows -pi..+pi as 0..1 (50% = phase 0).
// - Selection: `current` is the frame being viewed/edited; first..last is the
//   contiguous (shift-click) selection that contains it.
#pragma once

#include "Core.h"
#include "../Model/Tree.h"

#include <array>
#include <complex>
#include <string>
#include <vector>

namespace tincture::wtedit
{
constexpr int kFrameSize = 2048;
constexpr int kMaxFrames = 256;
constexpr int kNyquist = kFrameSize / 2;   // spectrum bins 0..1024
constexpr int kFftBins = 512;              // bins shown / edited in the FFT area, q = 1..512

using Frame = std::array<float, kFrameSize>;

// ---------------------------------------------------------------------------
// Data
// ---------------------------------------------------------------------------
struct FrameSet
{
    std::vector<Frame> frames;     // 1..256 frames (never empty after any edit)
    std::string name = "Default Shapes";
    // After a MORPH: indices of the original (thumbnail) frames, ascending;
    // the frames in between are generated. Empty = not morphed. Structural
    // edits (insert/remove/move/sort/paste) make every frame real again.
    std::vector<int> keyFrames;
    bool interpolate = true;       // crossfade between frames when playing ([no interp] in a .txt)
    bool modified = false;         // UI shows "Custom" + tinted name when set

    int size() const noexcept { return (int) frames.size(); }
    bool morphed() const noexcept { return ! keyFrames.empty(); }
    bool isKeyFrame (int i) const noexcept;

    static FrameSet initSaw();     // Init All (Default): one saw frame
    void save (Node& n) const;     // "name", "frames" (concatenated floats), "keys", "interp"
    static FrameSet load (const Node& n);
};

struct Selection
{
    int current = 0, first = 0, last = 0;

    void select (int i) noexcept { current = first = last = i; }
    void extendTo (int i) noexcept;                 // shift-click: range current..i
    void clamp (int numFrames) noexcept;            // keep everything inside the table
    int count() const noexcept { return last - first + 1; }
    bool contains (int i) const noexcept { return i >= first && i <= last; }
};

// ---------------------------------------------------------------------------
// FFT and spectra
// ---------------------------------------------------------------------------
class Fft
{
public:
    explicit Fft (int size);                                       // power of two
    int size() const noexcept { return n; }
    void forward (std::complex<float>* data) const noexcept;       // in place, unscaled
    void inverse (std::complex<float>* data) const noexcept;       // in place, scaled by 1/N
private:
    void run (std::complex<float>* d, bool inv) const noexcept;
    int n = 0;
    std::vector<std::complex<float>> tw;
    std::vector<int> rev;
};
const Fft& frameFft();                                             // shared size-2048 instance

struct Spectrum
{
    std::array<float, kNyquist + 1> mag {}, phase {};
};
void analyse (const Frame& f, Spectrum& s);
void synthesise (const Spectrum& s, Frame& f);
float harmonicMag (const Frame& f, int n);                          // 0..1024
float harmonicPhase (const Frame& f, int n);
bool setHarmonic (Frame& f, int n, float mag, float phase);

// FFT-area drawing state (the four toggles at the bottom of the FFT menus).
struct FftDrawOptions
{
    enum Flag { EvenOnly = 0, OddOnly, SnapQuarters, ScaleByBin, NumFlags };
    bool flags[NumFlags] {};
    bool on (Flag f) const noexcept { return flags[f]; }
};
float magToBar (float mag, int bin, const FftDrawOptions& o) noexcept;   // display 0..1
float barToMag (float bar, int bin, const FftDrawOptions& o) noexcept;
inline float phaseToBar (float ph) noexcept { return (ph + kPi) / kTwoPi; }
inline float barToPhase (float b) noexcept { return b * kTwoPi - kPi; }
bool binDrawable (int bin, const FftDrawOptions& o) noexcept;          // even/odd-only filter

// Mouse drawing in the FFT area. `bar` 0..1 as displayed. Return false when
// nothing changed (bin filtered out, same value). Working on a Spectrum lets
// the UI keep it during a drag and synthesise once per mouse event.
bool drawMagnitude (Spectrum& s, int bin, float bar, const FftDrawOptions& o);
bool drawPhase (Spectrum& s, int bin, float bar, const FftDrawOptions& o);
bool drawMagnitude (Frame& f, int bin, float bar, const FftDrawOptions& o);
bool drawPhase (Frame& f, int bin, float bar, const FftDrawOptions& o);
bool nudgePhase (Frame& f, int bin, float radians);                   // Shift-Cmd/Ctrl-click

// ---------------------------------------------------------------------------
// Resampling helpers (also used by WaveImport)
// ---------------------------------------------------------------------------
// Windowed-sinc read at fractional `pos`. cutoff 0..1 is relative to the
// source Nyquist (use < 1 when reading faster than 1 sample per output).
// periodic: indices wrap modulo len; otherwise outside samples are 0.
float sincRead (const float* x, int len, double pos, float cutoff, bool periodic) noexcept;
// Exact band-limited resample of one periodic cycle of any integer length
// (DFT; keeps harmonics up to min (len/2, 1023)).
void resampleCycle (const float* in, int len, Frame& out);
// Read one cycle starting at fractional `start` of fractional length `len`
// from a longer signal into a frame (non-periodic source).
void readCycle (const float* x, int xlen, double start, double len, Frame& out);

// ---------------------------------------------------------------------------
// BRUSH tools (spec §3.3)
// ---------------------------------------------------------------------------
enum class Brush : int
{
    FlatLine = 0, Sine, SlopeUp, SlopeDown, CurveUp, CurveDown, HalfSinePeak, HalfSineValley,
    InterpolateLinear, InterpolateCurved, Nudge, Noise, NumBrushes
};
const char* brushName (Brush b) noexcept;                // tooltip / accessibility names

// Grid: gridX cells across the frame (<= 0 = off: cells are 1 sample wide).
int gridCell (float x01, int gridX) noexcept;
void cellRange (int cell, int gridX, int& start, int& end) noexcept;   // [start, end)
float snapY (float y, int gridY) noexcept;               // to gridY steps over -1..1 (<= 0 = off)

// Apply a tool to the grid cell under x01 (0..1 across the frame). `y` is the
// mouse value -1..1 for shape tools; for Nudge it's the vertical drag delta
// and for Noise the drag amount (+ adds noise, - smooths). mirror = the Mirror
// toggle: the cell is also written point-mirrored about the frame centre
// (out[N-1-i] = -out[i]), giving symmetric, DC-free shapes.
bool applyBrush (Frame& f, Brush b, float x01, float y, int gridX, bool mirror, Rng& rng);
// Freehand pencil between two mouse points (grid off).
bool drawLine (Frame& f, float x0, float y0, float x1, float y1, bool mirror);
// GRID `<` `>` (shift by one grid step, dir -1/+1) and `^` `v` (nudge by dy).
bool shiftByGrid (Frame& f, int gridX, int dir);
bool nudgeVertical (Frame& f, float dy);

// ---------------------------------------------------------------------------
// Frame buttons (spec §3.2, §3.5)
// ---------------------------------------------------------------------------
bool insertFrame (FrameSet& s, Selection& sel);                 // `+`: copy of current, after it
bool appendFrame (FrameSet& s, Selection& sel);                 // thumbnail-strip `+`
bool removeFrame (FrameSet& s, Selection& sel);                 // `–`
bool moveFrame (FrameSet& s, Selection& sel, int from, int insertBefore);
bool copyFrames (const FrameSet& s, const Selection& sel, std::vector<Frame>& clipboard);
bool pasteFrames (FrameSet& s, Selection& sel, const std::vector<Frame>& clipboard);
bool replaceFrame (FrameSet& s, int index, const Frame& f);    // single-cycle drop on a thumbnail

// ---------------------------------------------------------------------------
// Single-frame processing primitives (used by the menus, usable directly)
// ---------------------------------------------------------------------------
bool normalise (Frame& f, float peak = 1.0f);
bool removeDc (Frame& f);
bool invert (Frame& f);
bool reverse (Frame& f);
bool shiftToZeroCrossing (Frame& f);
bool fadeEdges (Frame& f, int len);
bool crossfadeEdges (Frame& f, int len);
bool lowpassHarmonics (Frame& f, int keep);              // keep harmonics 1..keep, DC removed
bool sampleRedux (Frame& f, int hold);
float peakOf (const Frame& f) noexcept;

// ---------------------------------------------------------------------------
// MORPH (spec §3.10)
// ---------------------------------------------------------------------------
enum class MorphMode : int { Crossfade = 0, Spectral, SpectralZeroFund, SpectralZeroAll };
// Fills the table to 256 frames; originals land at round (i*255/(n-1)).
// Needs 1 < key frames < 256 (if already morphed, re-morphs from the keys).
bool morph (FrameSet& s, MorphMode mode);
bool removeMorph (FrameSet& s);

// ---------------------------------------------------------------------------
// Menus, table driven. The UI builds each menu from waveMenu() in order.
// ---------------------------------------------------------------------------
enum class WaveMenu : int { FftMags = 0, FftPhases, Remove, Sort, ProcessSingle, ProcessAll, Morph, NumMenus };

struct EditContext
{
    FrameSet& set;
    Selection& sel;
    int gridX = 8, gridY = 8;                 // GRID steppers
    int bin = 1;                              // FFT menus: the right-clicked bin
    FftDrawOptions* fftOptions = nullptr;     // FFT menu toggles
    const FrameSet* otherOsc = nullptr;       // "... from Other Osc" (A<->B, C uses A)
    std::vector<float>* noiseOut = nullptr;   // "Send to Noise Oscillator" target
    uint32_t seed = 0x1234567u;               // randomize / sort-random
};

using WaveOpFn = bool (*) (EditContext&);

struct WaveOp
{
    enum Kind : uint8_t { Action = 0, Title, Separator, Toggle };
    Kind kind = Action;
    const char* label = "";        // exact menu text; '#' is replaced by the clicked bin
    WaveOpFn fn = nullptr;         // Action / Toggle (toggle flips a flag, returns false)
    const char* submenu = nullptr; // non-null: item lives in this submenu (e.g. "Reduce to")
    int toggleFlag = -1;           // Toggle: FftDrawOptions::Flag
};

struct WaveOpList { const WaveOp* items; int count; const char* title; };
WaveOpList waveMenu (WaveMenu m) noexcept;
std::string menuLabel (const WaveOp& op, int bin);     // '#' -> bin number
bool isChecked (const WaveOp& op, const EditContext& ctx) noexcept;
// Run by label (tests, scripting); false if not found or nothing changed.
bool runOp (WaveMenu m, const std::string& label, EditContext& ctx);
} // namespace tincture::wtedit
