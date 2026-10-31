// Warp.h — the oscillator warp modes (index = choices::warpModes()).
//
// Two kinds of warp:
//   - PHASE warps (Sync, Bend, PWM, Asym, Flip, Mirror, Remap, Quantize,
//     Odd/Even, FM, PD) change where in the waveform an oscillator reads.
//     They only make sense inside the wavetable oscillator, which applies
//     them itself (WavetableOsc.cpp).
//   - POST warps (LPF/HPF, the Distortion modes, AM, RM) process audio, so
//     any oscillator mode can use them: the sample modes call processPost().
#pragma once

#include "Core.h"
#include "../Model/Curve.h"

namespace tincture::warp
{
enum Mode : int
{
    Off = 0, Sync = 1,
    BendPlus = 2, BendMinus, BendPlusMinus, Pwm, AsymPlus, AsymMinus, AsymPlusMinus, Flip, Mirror,
    Remap1, Remap2, Remap3, Remap4, Quantize, OddEven,
    Lpf = 17, Hpf = 18,
    DistFirst = 19, Tube = 19, SoftClip, HardClip, Diode1, Diode2, LinearFold, SineFold, ZeroSquare, Asym,
    Rectify, SineShaper, StompBox, TapeSat, SoftSat, DistLast = 32,
    FmFirst = 33,   // FM (A), (B), (C), (Noise), (Sub), (Filter 1), (Filter 2)
    PdFirst = 40,   // PD (A) .. (Filter 2), PD (Self) = 47
    PdSelf = 47,
    AmFirst = 48,   // AM (A) .. (Filter 2)
    RmFirst = 55,   // RM (A) .. (Filter 2)
    NumModes = 62
};

enum class Kind { None, Sync, Phase, Filter, Distortion, Fm, Pd, Am, Rm };

Kind kindOf (int mode) noexcept;
// The source buffer (SourceBuffer in Module.h) a FM/PD/AM/RM mode reads, or -1 (incl. PD (Self)).
int sourceOf (int mode) noexcept;
bool isPost (int mode) noexcept;        // Filter, Distortion, AM, RM

// Per-stream memory for the post warps (one per unison voice per slot).
struct PostState
{
    float z1 = 0.0f, z2 = 0.0f;         // filter state
    float dc = 0.0f;                    // DC blocker for the asymmetric shapers
    void reset() noexcept { z1 = z2 = dc = 0.0f; }
};

// Process a POST warp in place on `buf` (n samples). `src` is the AM/RM
// source audio (may be null: then AM/RM do nothing). `pitchHz` lets LPF/HPF
// track the note.
void processPost (int mode, float amount, float* buf, int n, const float* src,
                  PostState& state, float sampleRate, float pitchHz) noexcept;

// The distortion transfer function for a mode at a drive amount (UI drawing
// and the oscillator's 2D display).
float shape (int mode, float amount, float x) noexcept;
} // namespace tincture::warp
