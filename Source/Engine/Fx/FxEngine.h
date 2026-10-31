// FxEngine.h — runs the three FX racks (MAIN, BUS 1, BUS 2) on the summed
// voice output, and the internals the FX module files share.
//
// Threads:
//   message thread: prepare(), setLayout(), idle(), telemetry readers.
//   audio thread:   beginBlock(), noteOn(), process() (x3 racks per block).
// setLayout() builds the processor graph off the audio thread and publishes
// it lock-free; modules that persist (same unit, same type) keep their
// processor object, so reverb tails, delay lines and LFO phases carry on.
// Old graphs are freed on the message thread once the audio thread has
// moved on (a single hazard pointer).
//
// Bypass (spec 3.6: "passes audio unprocessed"): a bypassed module outputs
// its input, with a ~5 ms crossfade; once fully bypassed it stops running
// and its state is cleared, so re-enabling starts clean (no stale tails).
#pragma once

#include "FxLayout.h"
#include "FxTypes.h"
#include "../Module.h"

#include <atomic>
#include <memory>
#include <vector>

namespace tincture
{
class FxModule;
class FxSplitter;

// Per-unit telemetry for the UI.
struct FxTelemetry
{
    std::atomic<float> gainReductionDb[3] {};   // Compressor: H, M, L bands (single band: [1]); <= 0
};

class FxEngine
{
public:
    FxEngine();
    ~FxEngine();

    // Message thread (audio stopped). Rebuilds every processor.
    void prepare (const Context& ctx, int maxBlock);
    // Message thread: publish a new rack layout.
    void setLayout (std::shared_ptr<const FxLayout> layout);
    // Message thread, ~10-30 Hz: frees retired graphs and rebuilds Convolve
    // impulses whose shaping knobs (SIZE, TONE, MIN, ATTACK, DECAY, DAMP)
    // changed. Those knobs therefore act at UI rate, not per block.
    void idle();

    // Audio thread, once per block before the racks.
    void beginBlock (const Context& ctx);
    void noteOn (int note) noexcept { lastNote = note; ++noteCount; }
    // Audio thread: process one rack in place (n <= maxBlock).
    void process (int rack, float* L, float* R, int n, const ParamView& global);
    void reset();                               // audio thread / stopped: clear all tails

    // Latency in samples of the MAIN rack (Limiter Latency Comp).
    int latency() const noexcept { return mainLatency.load (std::memory_order_relaxed); }

    // UI telemetry.
    bool gainReduction (int unit, float outDb[3]) const;          // false if unit isn't a compressor
    float rackPeak (int rack, int channel);                       // peak since the last call
    // Response curves for displays (dB at `hz`): Equalizer, Filter,
    // Distortion's filter, Delay's feedback filter; 0 for other types.
    static float magnitudeDb (FxType t, const float* norm16, float hz, double sampleRate);
    // Distortion transfer curve: output for input x in -1..1 (drive applied).
    static float distortionTransfer (const float* norm16, const FxModuleSlot* slot, float x);
    // Factory impulse responses for Convolve (index = FxModuleSlot::irIndex).
    static const std::vector<std::string>& convolveIrNames();
    static std::shared_ptr<const SampleData> convolveFactoryIr (int index);

    struct Node;
    struct Graph;

private:
    Graph* build (const std::shared_ptr<const FxLayout>& layout, bool fresh);
    void publish (Graph* g);
    void collectGarbage();
    void acquire() noexcept;
    void runChain (const std::vector<Node>& chain, float* L, float* R, int n, const ParamView& p, int depth);

    Context ctx;
    int maxBlock = 512;
    std::shared_ptr<const FxLayout> layout;
    std::vector<std::unique_ptr<Graph>> graphs;     // owned by the message thread
    std::atomic<Graph*> latest { nullptr };
    std::atomic<Graph*> inUse { nullptr };
    Graph* active = nullptr;                         // audio thread
    std::vector<float> dry[2][2];                    // [depth][channel]
    int lastNote = 60;
    uint32_t noteCount = 0;
    std::atomic<int> mainLatency { 0 };
    std::atomic<float> peaks[kNumFxRacks][2] {};
    FxTelemetry telemetry[kNumFxUnits];
    std::atomic<float> seen[kNumFxUnits][kFxParams] {};    // last values the audio thread used
    std::atomic<int> seenType[kNumFxUnits] {};
    int idleCount = 0;
};

// ===========================================================================
// Internals shared by the module files (Source/Engine/Fx/*.cpp).
// ===========================================================================

// Everything a module's process() call gets besides the audio.
struct FxBlock
{
    const float* v = nullptr;           // the unit's 16 slots, 0..1 (modulated)
    float mix = 1.0f;                   // the unit's MIX (for modules that mix themselves)
    const Context* ctx = nullptr;       // tempo for synced rates
    int note = 60;                      // last played note (key tracking)
    uint32_t noteCount = 0;             // increments on each note-on (retrigger)
    const void* extra = nullptr;        // baked patch data (curves, IR kernel), type-specific
    FxTelemetry* telemetry = nullptr;
};

class FxModule
{
public:
    virtual ~FxModule() = default;
    virtual void prepare (double sampleRate, int maxBlock) = 0;     // allocate here
    virtual void reset() = 0;                                       // clear state, no allocation
    virtual void process (float* L, float* R, int n, const FxBlock& b) = 0;
    virtual int latency() const noexcept { return 0; }
    virtual bool mixesItself() const noexcept { return false; }     // applies b.mix internally
    virtual FxSplitter* splitter() noexcept { return nullptr; }
    // Convolve only: kernel length (samples) to allocate for / that fits.
    virtual void reserve (int /*kernelSamples*/) {}
    virtual int kernelCapacity() const noexcept { return 0; }

    // Engine-owned runtime state (audio thread).
    float bypassGain = 1.0f, prevMix = -1.0f, prevLevel = -1.0f;
    bool cleared = true;
};

// Splitters: split() fills band buffers, the engine runs each band's chain
// on them, join() writes the sum back.
class FxSplitter : public FxModule
{
public:
    void process (float*, float*, int, const FxBlock&) override {}
    FxSplitter* splitter() noexcept override { return this; }
    virtual void split (const float* L, const float* R, int n, const FxBlock& b) = 0;
    virtual void join (float* L, float* R, int n) = 0;
    float* bandL (int b) noexcept { return buf[b][0].data(); }
    float* bandR (int b) noexcept { return buf[b][1].data(); }
protected:
    std::vector<float> buf[3][2];
};

// Module factories (one per .cpp).
std::unique_ptr<FxModule> makeBode();
std::unique_ptr<FxModule> makeChorus();
std::unique_ptr<FxModule> makeCompressor();
std::unique_ptr<FxModule> makeConvolve();
std::unique_ptr<FxModule> makeDelay();
std::unique_ptr<FxModule> makeDistortion();
std::unique_ptr<FxModule> makeEqualizer();
std::unique_ptr<FxModule> makeFilter();
std::unique_ptr<FxModule> makeFlanger();
std::unique_ptr<FxModule> makeHyper();
std::unique_ptr<FxModule> makePhaser();
std::unique_ptr<FxModule> makeReverb();
std::unique_ptr<FxModule> makeSplitter (FxType t);
std::unique_ptr<FxModule> makeUtility();

// Baked patch data (message thread).
std::shared_ptr<const void> bakeDistortion (const FxModuleSlot& s);
// Convolve: key = the shaping knobs + IR identity; kernel built for `sr`.
struct ConvolveKey
{
    const void* irId = nullptr;
    int irIndex = -1;
    float k[6] {};                      // SIZE, TONE, MIN, ATTACK, DECAY, DAMP (quantised)
    bool operator== (const ConvolveKey& o) const noexcept;
};
ConvolveKey convolveKey (const FxModuleSlot& s, const float* norm16);
std::shared_ptr<const void> bakeConvolve (const FxModuleSlot& s, const ConvolveKey& key, double sr, int& lengthOut);
const std::vector<float>* convolveKernelIr (const void* kernel, int channel);     // the shaped IR (tests, display)

// Display helpers implemented in the module files.
float equalizerMagnitudeDb (const float* v, float hz, double sr);
float filterMagnitudeDb (const float* v, float hz, double sr);
float distortionFilterMagnitudeDb (const float* v, float hz, double sr);
float delayFilterMagnitudeDb (const float* v, float hz, double sr);
float distortionShape (const float* v, const void* extraOrNull, const FxModuleSlot* slot, float x);

// ---------------------------------------------------------------------------
// Small DSP building blocks.
// ---------------------------------------------------------------------------
namespace fxdsp
{
constexpr float kDenorm = 1.0e-20f;

inline int nextPow2 (int n) { int p = 1; while (p < n) p <<= 1; return p; }

// Smooths a unit's 16 normalised slots once per tick (~15 ms), so modules
// can map them to plain values per tick without zipper noise.
struct SlotSmoother
{
    float cur[kFxParams] {};
    float coef = 0.1f;
    bool primed = false;
    void prepare (double sr) noexcept { coef = 1.0f - std::exp (-(float) kTick / (0.015f * (float) sr)); primed = false; }
    void reset() noexcept { primed = false; }
    const float* tick (const float* v) noexcept
    {
        if (! primed) { std::copy (v, v + kFxParams, cur); primed = true; }
        else for (int k = 0; k < kFxParams; ++k) cur[k] += (v[k] - cur[k]) * coef;
        return cur;
    }
};

// Mono delay line, power-of-two ring. push() writes the newest sample;
// read positions count back from it (0 = newest). In a feedback loop read
// before pushing: a delay of D samples is then at position D - 1.
struct DelayLine
{
    std::vector<float> buf;
    int mask = 0, pos = 0;

    void init (int maxSamples) { buf.assign ((size_t) nextPow2 (maxSamples + 4), 0.0f); mask = (int) buf.size() - 1; pos = 0; }
    void clear() { std::fill (buf.begin(), buf.end(), 0.0f); }
    int capacity() const noexcept { return mask - 3; }
    inline void push (float x) noexcept { pos = (pos + 1) & mask; buf[(size_t) pos] = x; }
    inline float tap (int d) const noexcept { return buf[(size_t) ((pos - d) & mask)]; }
    inline float linear (float d) const noexcept
    {
        const int i = (int) d;
        const float f = d - (float) i;
        const float a = tap (i), b = tap (i + 1);
        return a + (b - a) * f;
    }
    inline float hermite (float d) const noexcept
    {
        const int i = (int) d;
        const float f = d - (float) i;
        const float xm1 = tap (i - 1), x0 = tap (i), x1 = tap (i + 1), x2 = tap (i + 2);
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }
};

// Topology-preserving-transform state-variable filter (Simper / Zavalishin).
struct SvfCoefs
{
    float g = 0.0f, k = 1.414f, a1 = 1.0f, a2 = 0.0f, a3 = 0.0f;
    void set (float hz, float q, float sampleRate) noexcept
    {
        hz = clampf (hz, 1.0f, sampleRate * 0.49f);
        g = std::tan (kPi * hz / sampleRate);
        k = 1.0f / std::max (q, 0.025f);
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }
};
struct Svf
{
    float ic1 = 0.0f, ic2 = 0.0f;
    void clear() noexcept { ic1 = ic2 = 0.0f; }
    // Returns lp; bp/hp through references.
    inline float tick (const SvfCoefs& c, float x, float& bp, float& hp) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        bp = v1;
        hp = x - c.k * v1 - v2;
        return v2;
    }
    inline float lp (const SvfCoefs& c, float x) noexcept { float b, h; return tick (c, x, b, h); }
    inline float hp (const SvfCoefs& c, float x) noexcept { float b, h; tick (c, x, b, h); return h; }
    inline float bp (const SvfCoefs& c, float x) noexcept { float b, h; tick (c, x, b, h); return b; }
};

// Biquad (RBJ cookbook), transposed direct form II.
struct BiquadCoefs
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    void lowShelf (float hz, float q, float db, float sr) noexcept;
    void highShelf (float hz, float q, float db, float sr) noexcept;
    void peak (float hz, float q, float db, float sr) noexcept;
    void lowPass (float hz, float q, float sr) noexcept;
    void highPass (float hz, float q, float sr) noexcept;
    float magnitudeDb (float hz, float sr) const noexcept;
};
struct Biquad
{
    float z1 = 0.0f, z2 = 0.0f;
    void clear() noexcept { z1 = z2 = 0.0f; }
    inline float tick (const BiquadCoefs& c, float x) noexcept
    {
        const float y = c.b0 * x + z1;
        z1 = c.b1 * x - c.a1 * y + z2;
        z2 = c.b2 * x - c.a2 * y;
        return y;
    }
};

// Linkwitz-Riley 4th-order crossover for one channel: low + high sums to a
// 2nd-order all-pass (flat magnitude). allpass() gives that same all-pass,
// to keep a band that skips this split in phase (3-band splitting).
struct Lr4
{
    Svf s1, sLo, sHi;
    void clear() noexcept { s1.clear(); sLo.clear(); sHi.clear(); }
    inline void split (const SvfCoefs& c, float x, float& lo, float& hi) noexcept
    {
        float bp, hp;
        const float lp = s1.tick (c, x, bp, hp);
        lo = sLo.lp (c, lp);
        hi = sHi.hp (c, hp);
    }
    inline float allpass (const SvfCoefs& c, float x) noexcept
    {
        float bp, hp;
        s1.tick (c, x, bp, hp);
        return x - 2.0f * c.k * bp;
    }
};

// One-pole low-pass (smoothing, damping).
struct OnePole
{
    float z = 0.0f;
    inline float lp (float x, float a) noexcept { z += a * (x - z); return z; }
};
inline float onePoleCoef (float hz, float sr) noexcept { return 1.0f - std::exp (-kTwoPi * clampf (hz, 0.0f, sr * 0.49f) / sr); }

// Polyphase IIR half-band filters for 2x oversampling (two all-pass paths;
// coefficients designed for ~100 dB stop-band, transition 0.04).
struct Halfband
{
    static constexpr int kCoefs = 8;
    static const float* coefs();
    float x[kCoefs] {}, y[kCoefs] {};
    void clear() noexcept { for (int i = 0; i < kCoefs; ++i) x[i] = y[i] = 0.0f; }
    // Upsample one input sample to two output samples.
    inline void up (float in, float& o0, float& o1) noexcept
    {
        const float* c = coefs();
        float even = in, odd = in;
        for (int i = 0; i < kCoefs; i += 2)
        {
            const float ye = (even - y[i]) * c[i] + x[i];
            x[i] = even; y[i] = ye; even = ye;
            const float yo = (odd - y[i + 1]) * c[i + 1] + x[i + 1];
            x[i + 1] = odd; y[i + 1] = yo; odd = yo;
        }
        o0 = even; o1 = odd;
    }
    // Downsample two input samples (in0 earlier) to one.
    inline float down (float in0, float in1) noexcept
    {
        const float* c = coefs();
        float even = in1, odd = in0;
        for (int i = 0; i < kCoefs; i += 2)
        {
            const float ye = (even - y[i]) * c[i] + x[i];
            x[i] = even; y[i] = ye; even = ye;
            const float yo = (odd - y[i + 1]) * c[i + 1] + x[i + 1];
            x[i + 1] = odd; y[i + 1] = yo; odd = yo;
        }
        return 0.5f * (even + odd);
    }
};

// Sine/triangle LFO evaluated per tick (phase 0..1).
struct Lfo
{
    double phase = 0.0;
    inline void advance (float hz, int n, float sr) noexcept
    {
        phase += (double) hz * (double) n / (double) sr;
        phase -= std::floor (phase);
    }
};
inline float sine01 (double ph) noexcept { return std::sin ((float) (ph - std::floor (ph)) * kTwoPi); }
inline float tri01 (double ph) noexcept
{
    const float p = (float) (ph - std::floor (ph));
    return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p;
}
} // namespace fxdsp
} // namespace tincture
