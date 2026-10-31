// Telemetry.h — what the audio thread reports for the panel to draw:
// voice counts, envelope and LFO positions, playheads, meters. Written at the
// end of each block with relaxed atomics; read by the UI timer. Nothing here
// feeds back into the sound.
#pragma once

#include "Core.h"

#include <atomic>

namespace tincture
{
struct Telemetry
{
    template <typename T> using A = std::atomic<T>;

    A<int> activeVoices { 0 };          // voices sounding
    A<int> activeUnison { 0 };          // total oscillator voices (the voice counter)
    A<int> lastNote { -1 };
    A<int> newestVoice { -1 };          // index of the newest sounding voice (Synth::sampleTelemetry)
    A<float> lastVelocity { 0.0f };

    // The newest voice's modulators.
    A<float> env[kNumEnvs] {};          // 0..1 output
    A<int> envStage[kNumEnvs] {};       // 0 idle, 1 delay/attack, 2 hold, 3 decay, 4 sustain, 5 release
    A<float> envProgress[kNumEnvs] {};  // 0..1 through the current stage
    A<float> lfo[kNumLfos] {};          // 0..1 output
    A<float> lfoPhase[kNumLfos] {};     // 0..1
    A<float> velocity { 0.0f }, note { 0.0f };
    A<double> bpm { 120.0 };               // host tempo (120 when the host gives none)
    A<bool> playing { false };

    // Sample-based oscillators: playhead 0..1 (-1 none).
    A<float> oscPlayhead[kNumOscs] {};
    A<float> wtPos[kNumOscs] {};        // modulated WT position 0..1 of the newest voice

    // Output meters (peak since the UI last read; the UI resets them).
    A<float> peakL { 0.0f }, peakR { 0.0f };

    // Mod matrix: each row's output for the newest voice (-1..1).
    A<float> rowOut[64] {};

    // Keys currently down (on-screen keyboard), as a bitset of 128 notes.
    A<uint64_t> keysLo { 0 }, keysHi { 0 };

    // Modulated parameter values of the newest voice for the "halo" rings:
    // the UI asks for a parameter, the audio thread publishes it.
    static constexpr int kHalo = 128;
    A<int> haloParam[kHalo] {};         // param index (-1 empty), set by the UI
    A<float> haloNorm[kHalo] {};        // modulated normalised value, set by the audio thread

    void pushPeak (float l, float r) noexcept
    {
        float o = peakL.load (std::memory_order_relaxed);
        if (l > o) peakL.store (l, std::memory_order_relaxed);
        o = peakR.load (std::memory_order_relaxed);
        if (r > o) peakR.store (r, std::memory_order_relaxed);
    }
};
} // namespace tincture
