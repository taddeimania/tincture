// Filters.h — the per-voice filter module (FILTER 1 / FILTER 2), all 83 types of
// choices::filterTypes() (spec C §1). One FilterVoice per voice per filter slot.
//
// Processing is stereo and in place, one control tick (<= kTick samples) per call.
// Cutoff ramps exponentially from cutoffHzPrev to cutoffHz across the tick;
// coefficients are recomputed every 8 samples. Most types are zero-delay-feedback
// (TPT) structures: SVFs, a transistor ladder with tanh input stage, a diode ladder
// solved as a tridiagonal system per sample. Comb/flanger/reverb types use delay
// lines preallocated in prepare() for 8 Hz at the prepared sample rate.
//
// Signal chain per tick:  dry copy -> [clean: x/16] -> drive -> filter type ->
// [clean: x16] -> type-switch declick -> dry/wet MIX (ramped).
// PAN offsets the cutoff by up to +-1 octave: pan < 0 raises the left cutoff and
// lowers the right one; pan > 0 the opposite.
#pragma once

#include "Module.h"

#include <vector>

namespace tincture
{
class FilterVoice
{
public:
    static constexpr int kNumTypes = 83;

    // Allocates the delay lines (for 8 Hz at ctx.sampleRate) and resets.
    // ctx.sampleRate is the rate process() runs at (i.e. after oversampling).
    void prepare (const Context&);
    // Clears all filter state (note-on). Cheap unless a delay type was used.
    void reset();
    // Processes t.L / t.R in place (t.n samples; any n is accepted, kTick is typical).
    void process (FilterTick&);
    // Mono fast path: processes t.L only (t.R is ignored, may be null), for a
    // mono input with PAN 0. Output equals process() on L == R. Mono and stereo
    // ticks can be mixed freely: R's state is resynced from L on the next stereo tick.
    // process() itself also takes this path when pan is 0, L and R are bitwise
    // equal and R's state still equals L's (it then copies L's output to R).
    void processMono (FilterTick&);

    // Small-signal magnitude response in dB of the wet path (MIX ignored) at
    // freqHz, for drawing the filter curve. Exact for the linear TPT types
    // (same coefficients as process()); an approximation for the
    // time-variant ones (Ring Mod, SampHold) and where feedback runs away.
    static float magnitudeDb (int type, float cutoffHz, float res, float drive, float var,
                              float freqHz, double sampleRate);
    // Whether the VAR knob does anything for this type (choices::filterVarLabel is non-empty).
    static bool hasVar (int type);

private:
    friend struct FilterImpl;

    static constexpr int kStateSize = 72;   // per channel; the type decides the layout

    double fs = 48000.0;
    float invFs = 1.0f / 48000.0f;
    int curType = -1;                       // -1: nothing processed since reset()

    alignas (16) float st[2][kStateSize] {};
    float smoothLc[2] {};                   // SMOOTH types: slewed log2 cutoff
    bool smoothInit = false;

    std::vector<float> dl[2];               // delay lines, power-of-two size
    int dlSize = 0;
    uint32_t dlPos = 0;
    bool dlDirty = false;                   // a delay type wrote into dl since the last clear

    float lastWet[2] {};                    // last wet output (for the declick on type change)
    float declick[2] {};                    // decaying offset added to the wet signal
    float declickCoef = 0.99f;
    float mixPrev = -1.0f;                  // -1: snap on next tick
    Rng rng { 0x2545F491u };
    bool rStale = true;                     // R's state logically equals L's but isn't stored (mono ticks)
    float scratchR[kTick] {};               // mono delay types: R runs on a copy of L

    float cacheHz[2] { -1.0f, -1.0f }, cacheLog2[2] {};    // cutoffHzPrev is usually last tick's cutoffHz
    int cacheNext = 0;
    float log2Hz (float hz) noexcept
    {
        hz = hz > 1.0f ? hz : 1.0f;
        if (same (hz, cacheHz[0])) return cacheLog2[0];
        if (same (hz, cacheHz[1])) return cacheLog2[1];
        cacheHz[cacheNext] = hz;
        cacheLog2[cacheNext] = std::log2 (hz);
        cacheNext ^= 1;
        return cacheLog2[cacheNext ^ 1];
    }

    void clearState();
    void processChunk (FilterTick&, int offset, int n, float frac0, float frac1, bool mono);
};
} // namespace tincture
