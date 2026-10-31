// SubOsc.h — the SUB oscillator: six shapes, OCT / CRS pitch, start PHASE
// or Contiguous phase, PAN, LEVEL. The shapes play from small band-limited
// mip-mapped tables (one frame each, built once), which alias far less than
// polyBLEP at high notes for about the same cost.
#pragma once

#include "Module.h"

namespace tincture
{
class SubOsc
{
public:
    enum Shape { Sine = 0, RoundedRect, Triangle, Saw, Square, Pulse };

    // Shared by all voices of the sub (owned by the synth): Contiguous phase.
    struct Shared
    {
        uint32_t phase = 0;             // newest voice's running phase
        bool valid = false;
        uint32_t counter = 0, newest = 0;
    };

    void prepare (const Context& ctx);
    void reset() noexcept;
    void noteOn (const NoteInfo& note, const OscTick& tick, const SubIds& ids, Shared& shared);
    // Writes tick.n samples to outL/outR (and outMono if set); activeUnison = 1.
    void render (OscTick& tick, const SubIds& ids, Shared& shared);

    float currentPhase() const noexcept { return (float) acc * (1.0f / 4294967296.0f); }

    // One sample of a shape at phase t (0..1), full band (UI drawing).
    static float sample (int shape, float t) noexcept;

private:
    float sampleRate = 48000.0f;
    uint32_t acc = 0;
    uint32_t stamp = 0;
    float gL = 0.0f, gR = 0.0f;
    bool gainsValid = false;
    alignas (32) float buf[kTick] {};
};
} // namespace tincture
