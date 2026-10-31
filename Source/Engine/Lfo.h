// Lfo.h — LFO 1-10: the shape-playing LFOs, their patch data, the factory
// shape library and the wavetable <-> LFO shape conversions.
//
// Output is unipolar 0..1 (the matrix makes it bipolar per row).
//
// Instances: a per-voice LFO is one `Lfo` per voice. A MONO LFO is one `Lfo`
// owned by the synth (ticked once per tick, read by every voice). A FREE LFO
// that is not MONO also runs as one shared instance (every voice sees the same
// phase, as the spec asks); each voice applies only its own DELAY/RISE to the
// shared value with Lfo::follow(). ModMatrix.h's ModVoice / ModGlobal do all
// of this; the core normally doesn't call Lfo directly.
//
// Timing semantics (spec §2.3, proposed parts decided here):
// - RATE: Hz mode = RATE (x10 option); BPM mode = syncRates()[RATE (Sync)]
//   with TRIP (x2/3) and DOT (x1.5). One cycle = one graph width.
// - Ping Pong: one forward pass then one backward pass, each one cycle long.
// - RETRIG / ENVELOPE: phase resets to PHASE at a trigger. The phase is
//   frozen during DELAY, then RISE crossfades linearly from the graph's
//   left-most value to the live output. FREE: DELAY/RISE also restart at
//   note-on, but the phase keeps running.
// - ENVELOPE: plays one cycle and holds the end. With a loopback point,
//   loops [loopback, end] while the note is held; after note-off it plays on
//   to the end. Ping Pong in ENVELOPE mode plays forward+back once (no loop).
// - HOST + BPM: phase = transport position / cycle length (FREE), or
//   (position - position at trigger - DELAY) / cycle length (RETRIG), so the
//   same ppq always gives the same phase and RATE changes jump to the grid.
//   ENVELOPE mode advances incrementally. Transport stopped = free running.
// - HOST + Hz, FREE: at transport start the phase is set from the transport
//   time (deterministic playback); HOST off = never reset (keeps running).
// - SWING (BPM mode, "Swing" ticked): cycles are taken in pairs and the first
//   one is lengthened to 0.5 + 0.25 * global SWING of the pair (100 % = 3:1).
// - SMOOTH: one-pole on the output, time constant SMOOTH x 1/4 cycle.
// - S&H: a new random value at every ||| grid step (once per cycle at grid 1).
// - Chaos: Lorenz (s=10, r=28, b=8/3) / Rossler (0.2, 0.2, 5.7), x axis,
//   about one orbit per cycle, clamped into 0..1.
// - Path: the path points are drawn in order (x may go backwards); output is
//   the path's y sampled by arc length (equal time for equal drawn length).
#pragma once

#include "Module.h"
#include "Model/Curve.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace tincture
{
enum class LfoType : uint8_t { Normal = 0, Path, Lorenz, Rossler, SampleHold };
enum class LfoMode : uint8_t { Free = 0, Retrig, Envelope };
enum class LfoDirection : uint8_t { Forward = 0, Reverse, PingPong };

constexpr int kMaxLfoBuses = 4;
constexpr int kMaxLfoPoints = 256;

// ---------------------------------------------------------------------------
// LFO point modulation ("LFO busses", spec §2.8). A bus is one source with a
// depth; each point/axis subscribes to a bus with its own range (dx, dy in
// graph units, dc in curve units). Offset = bus value x bus amount x range.
// Sources are ModSource indices (ModMatrix.h), -1 = unused.
// ---------------------------------------------------------------------------
struct LfoBus
{
    int source = -1;
    float amount = 1.0f;
};

struct LfoPointMod
{
    int point = 0, bus = 0;
    float dx = 0.0f, dy = 0.0f, dc = 0.0f;
};

// Patch data of one LFO (everything that isn't a parameter).
struct LfoData
{
    Curve shape = Curve::triangle();
    std::vector<CurvePoint> path;       // Path type, in drawing order; empty = use `shape`
    int loopback = -1;                  // shape point index (ENVELOPE mode); -1 or the last point = none
    std::string shapeName = "Default";  // shown in the shape menu
    LfoBus buses[kMaxLfoBuses];
    std::vector<LfoPointMod> pointMods;

    int nextFreeBus() const noexcept;   // -1 when all are used
    void save (Node& n) const;
    void load (const Node& n);
    bool operator== (const LfoData& o) const;
};

// An LFO's shape, baked on the message thread for the audio thread.
struct BakedLfo
{
    BakedCurve shape;
    BakedCurve path;
    float first = 0.0f;                 // value of the left-most point (DELAY hold value)
    float loopX = -1.0f;                // loopback x, or -1
    // Point modulation (empty when there is none).
    std::vector<CurvePoint> points;
    std::vector<LfoPointMod> mods;
    LfoBus buses[kMaxLfoBuses];

    BakedLfo() { Curve::triangle().bake (shape.lut); Curve::triangle().bake (path.lut); }
    static BakedLfo build (const LfoData& d);

    // Shape value at x (0..1); `src` = SourceValues array for point modulation (may be null).
    float evaluate (float x, const float* src) const noexcept;
};

struct LfoShapeSet
{
    BakedLfo lfo[kNumLfos];
    uint64_t busSources = 0;            // bit per ModSource used by any LFO bus
    static std::shared_ptr<const LfoShapeSet> build (const LfoData* data /* kNumLfos */);
};

// ---------------------------------------------------------------------------
// Per-tick settings and clock.
// ---------------------------------------------------------------------------
struct LfoSettings
{
    LfoType type = LfoType::Normal;
    LfoMode mode = LfoMode::Retrig;
    LfoDirection direction = LfoDirection::Forward;
    int gridX = 8;                      // ||| time divisions
    bool mono = false, host = false, bpm = true, swing = false;
    double cycleBeats = 1.0;            // BPM mode: quarter notes per cycle
    float hz = 2.0f;                    // cycles per second (both modes)
    float rise = 0.0f, delay = 0.0f;    // seconds
    float smooth = 0.0f;                // 0..1
    float phase = 0.0f;                 // 0..1 (snapped to the grid when Snap to Grid is on)
    float swingAmount = 0.0f;           // global SWING 0..1

    // `swing` = the global SWING parameter value (0..1).
    static LfoSettings read (const ParamView& p, const LfoIds& id, double bpm, float swing) noexcept;
};

// Rate helpers (also for the RATE readout).
double lfoSyncBeats (int syncIndex, bool trip, bool dot) noexcept;

struct LfoClock
{
    float sampleRate = 48000.0f;
    double bpm = 120.0;
    double ppq = 0.0;                   // transport position at the start of this tick
    bool playing = false;
    // Clock for the tick starting `sampleOffset` samples into the block.
    static LfoClock at (const Context& c, int sampleOffset) noexcept
    {
        LfoClock k;
        k.sampleRate = (float) c.sampleRate;
        k.bpm = c.bpm;
        k.playing = c.playing;
        k.ppq = c.ppq + (double) sampleOffset * c.bpm / (60.0 * c.sampleRate);
        return k;
    }
};

struct LfoTelemetry
{
    std::atomic<float> phase { 0.0f }, value { 0.0f };
};

class Lfo
{
public:
    void reset (uint32_t seed) noexcept;

    // Note-on (or a MONO retrigger). Resets phase (RETRIG / ENVELOPE),
    // DELAY / RISE (all modes) and the S&H / chaos state.
    void trigger (const LfoSettings& s, const BakedLfo& b, const LfoClock& clk) noexcept;
    void release() noexcept { released = true; }

    // Runs the full LFO for n samples; returns the value at the end of the tick.
    float process (const LfoSettings& s, const BakedLfo& b, const LfoClock& clk, int n, const float* src) noexcept;
    // Per-voice DELAY / RISE over a shared (FREE) LFO's value.
    float follow (const LfoSettings& s, const BakedLfo& b, float sharedValue, int n, float sampleRate) noexcept;

    float value() const noexcept { return out; }
    float previous() const noexcept { return prevOut; }
    float phase() const noexcept { return x; }          // graph position 0..1 (UI)
    bool envelopeDone() const noexcept { return done; }
    void publish (LfoTelemetry& tel) const noexcept
    {
        tel.phase.store (x, std::memory_order_relaxed);
        tel.value.store (out, std::memory_order_relaxed);
    }

private:
    float riseDelay (const LfoSettings& s, float first, float v) const noexcept;
    void chaosInit() noexcept;
    float chaosAdvance (LfoType type, double cycles) noexcept;

    double pos = 0.0;           // cycles, wrapped to 0..2 (FREE / RETRIG)
    double envPos = 0.0;        // ENVELOPE mode position (0..1, or 0..2 for Ping Pong)
    double t = 0.0;             // seconds since the trigger
    double trigPpq = 0.0;
    bool wasPlaying = false, released = false, done = false, fresh = true;
    float x = 0.0f, out = 0.0f, prevOut = 0.0f, smoothed = 0.0f;
    int shStep = -1;
    float shValue = 0.0f;
    float cx = 1.0f, cy = 1.0f, cz = 1.0f;
    Rng rng;
};

// ---------------------------------------------------------------------------
// Shape library (shape menu, §2.5).
// ---------------------------------------------------------------------------
namespace LfoShapes
{
    struct Shape { std::string name; Curve curve; };
    struct Folder { std::string name; std::vector<Shape> shapes; };

    Curve defaultShape();                                   // the triangle
    const std::vector<Folder>& factory();                   // Basic, Rhythmic, Sidechain
    const Shape* find (const std::string& folder, const std::string& name);

    // `Random (Curved)` / `Random (Flat)`: one value per ||| grid step.
    Curve randomCurved (int gridX, uint32_t seed);
    Curve randomFlat (int gridX, uint32_t seed);

    // A sampled function 0..1 -> 0..1 reduced to breakpoints (max error `tolerance`).
    Curve fromSamples (const std::vector<float>& ys01, bool cyclic, float tolerance = 0.002f, int maxPoints = 128);
    // `Wavetable X to LFO`: one frame (-1..1, any size, normally 2048),
    // peak-normalised into 0..1.
    Curve fromWavetableFrame (const std::vector<float>& frame, float tolerance = 0.002f, int maxPoints = 128);
    // LFO -> wavetable: one frame of `size` samples, -1..1.
    std::vector<float> toWavetableFrame (const Curve& c, int size = 2048);
}
} // namespace tincture
