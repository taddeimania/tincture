// WavetableOsc.h — the per-voice wavetable oscillator (OSC A/B/C in
// WAVETABLE mode): up to 16 unison voices, two warp slots with all 62 warp
// modes, phase memory, band-limited mip-mapped playback and an optional
// 2x / 4x oversampled path for the warps that create new harmonics.
//
// Threading: render() and noteOn() run on the audio thread. The Shared
// struct (one per oscillator, owned by the synth) holds the cross-voice
// phase memory and the scratch buffers, so the voices of one oscillator must
// render one at a time (which the voice loop does).
#pragma once

#include "Module.h"
#include "Warp.h"
#include "Wavetable.h"
#include "../Model/Curve.h"

#include <memory>

namespace tincture
{
// Patch data of a wavetable oscillator that isn't a parameter.
struct WavetableOscData
{
    std::shared_ptr<const Wavetable> table;     // null = silent
    Curve remap[2];                              // the Remap 1/2/4 graph of each warp slot

    // Audio-thread copies of `remap`. Call bakeRemaps() after editing remap
    // (message thread); load() does it.
    BakedCurve remapLut[2];
    float remapSlope[2] { 1.0f, 1.0f };          // steepest slope of each curve (mip choice)
    void bakeRemaps();

    // Factory tables are stored by reference (category + name), others with
    // their frames. A missing table loads the default ("Default Shapes").
    void save (Node& n) const;
    void load (const Node& n);
};

// Polyphase IIR half-band decimator (2 -> 1), 8 allpass coefficients,
// ~-75 dB stop band, passband to 0.46 fs(out). Used by the oversampled path.
class Decimator2x
{
public:
    static constexpr int kCoefs = 8;
    void reset() noexcept { for (int i = 0; i < kCoefs; ++i) x[i] = y[i] = 0.0f; }
    // in: 2n samples, out: n samples (may not alias `in`).
    void process (const float* in, float* out, int n) noexcept;
    static const float* coefficients() noexcept;
private:
    float x[kCoefs] {}, y[kCoefs] {};
};

class WavetableOsc
{
public:
    static constexpr int kMaxOs = 4;
    static constexpr int kMaxBlock = kTick * kMaxOs;

    // Shared by all voices of one oscillator (the synth owns one per osc).
    struct Shared
    {
        // Phase memory.
        uint32_t phase[kMaxUnison] {};          // newest voice's running phases (Contiguous)
        uint32_t stored[kMaxUnison] {};         // the Per Voice start phases
        bool phaseValid = false, storedValid = false;
        uint32_t counter = 0, newest = 0;       // note-on stamps
        int lastMemMode = 0;
        // Scratch for render() (kept here so all voices reuse the same cache lines).
        alignas (32) float ph[kMaxBlock];
        alignas (32) float ph2[kMaxBlock];
        alignas (32) float gain[kMaxBlock];
        alignas (32) float out[kMaxBlock];
        alignas (32) float out2[kMaxBlock];
        alignas (32) float fmMod[kMaxBlock];
        alignas (32) float srcUp[2][kMaxBlock];
        alignas (32) float accL[kMaxBlock];
        alignas (32) float accR[kMaxBlock];
        alignas (32) float mid[2][kMaxBlock / 2];
        void resetMemory() noexcept { phaseValid = storedValid = false; counter = newest = 0; lastMemMode = 0; }
    };

    void prepare (const Context& ctx);
    void reset() noexcept;

    void noteOn (const NoteInfo& note, const OscTick& tick, const OscIds& ids, const WavetableOscData& data, Shared& shared);
    // Writes tick.n samples to outL/outR (and outMono if set); sets tick.activeUnison.
    void render (OscTick& tick, const OscIds& ids, const WavetableOscData& data, Shared& shared);

    // Read phase of the first unison voice, 0..1 (UI).
    float currentPhase() const noexcept { return (float) acc[0] * (1.0f / 4294967296.0f); }
    bool isActive() const noexcept { return active; }

private:
    float sampleRate = 48000.0f;
    int numUni = 1;
    bool active = false;
    uint32_t stamp = 0;
    uint32_t acc[kMaxUnison] {};                // phase accumulators (2^32 = one cycle)
    float randDetune[kMaxUnison] {};            // Random unison mode offsets, -1..1
    float gL[kMaxUnison] {}, gR[kMaxUnison] {}; // last tick's per-voice output gains
    bool gainsValid = false;
    float fb[kMaxUnison] {};                    // PD (Self) feedback
    warp::PostState post[2][kMaxUnison];
    float lastSrc[2] {};                        // previous source sample per slot (upsampling)
    Decimator2x dec[2][2];                      // [stage][channel]
    int lastOs = 1;
    Rng rng;
};
} // namespace tincture
