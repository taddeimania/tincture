// Core.h — shared constants and small helpers for the Tincture sound engine.
// The engine has no JUCE dependency: plain C++20, built for speed.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>

namespace tincture
{
constexpr float kPi = 3.14159265358979323846f;
constexpr float kTwoPi = 2.0f * kPi;

// Modulation and parameter smoothing run at control rate: one tick is
// kTick samples. Audio inside a tick ramps linearly between tick values.
constexpr int kTick = 32;
constexpr int kMaxVoices = 32;
constexpr int kMaxUnison = 16;
constexpr int kNumOscs = 3;            // OSC A, B, C
constexpr int kNumFilters = 2;
constexpr int kNumEnvs = 4;
constexpr int kNumLfos = 10;
constexpr int kNumMacros = 8;
constexpr int kNumBuses = 2;

// Signal sources an oscillator's warp (FM/PD/AM/RM) can read.
enum class Source : int8_t { A = 0, B, C, Noise, Sub, Filter1, Filter2, Self, None };

// Where an oscillator / filter sends its output.
enum class Route : int8_t { Filter = 0, Main, Direct, None };

struct Context
{
    double sampleRate = 48000.0;
    float invSampleRate = 1.0f / 48000.0f;
    double bpm = 120.0;
    double ppq = 0.0;             // host position at the start of the block, quarter notes
    bool playing = false;
    int oversample = 1;           // 1, 2 or 4 (quality setting)
};

// ---------------------------------------------------------------------------
// Math
// ---------------------------------------------------------------------------
// Exact comparison, on purpose (change detection, sentinels). Spelled out so
// -Wfloat-equal can stay on for the comparisons that aren't deliberate.
template <typename T> constexpr bool same (T a, T b) noexcept { return std::equal_to<T>{} (a, b); }

inline float clamp01 (float x) noexcept { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
inline float clampf (float x, float lo, float hi) noexcept { return x < lo ? lo : (x > hi ? hi : x); }
inline float lerp (float a, float b, float t) noexcept { return a + (b - a) * t; }
inline float dbToGain (float db) noexcept { return db <= -120.0f ? 0.0f : std::pow (10.0f, db * 0.05f); }
inline float gainToDb (float g) noexcept { return g <= 1.0e-6f ? -120.0f : 20.0f * std::log10 (g); }
inline float noteToHz (float note) noexcept { return 440.0f * std::exp2 ((note - 69.0f) * (1.0f / 12.0f)); }
inline float hzToNote (float hz) noexcept { return 69.0f + 12.0f * std::log2 (hz / 440.0f); }

// Equal-power pan, -1..1 -> left/right gains (centre -3 dB each).
inline void panGains (float pan, float& l, float& r) noexcept
{
    const float a = (clampf (pan, -1.0f, 1.0f) + 1.0f) * (kPi * 0.25f);
    l = std::cos (a) * 1.41421356f;
    r = std::sin (a) * 1.41421356f;
}

// Fast approximations for the inner loops.
inline float fastTanh (float x) noexcept
{
    x = clampf (x, -4.97f, 4.97f);
    const float x2 = x * x;
    return x * (27.0f + x2) / (27.0f + 9.0f * x2);
}
inline float fastExp2 (float x) noexcept { return std::exp2 (x); }

// Fast, decent quality PRNG (xorshift32). Not for crypto.
struct Rng
{
    uint32_t s = 0x9E3779B9u;
    explicit Rng (uint32_t seed = 0x9E3779B9u) : s (seed ? seed : 1u) {}
    uint32_t next() noexcept { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
    float uni() noexcept { return (float) (next() >> 8) * (1.0f / 16777216.0f); }           // 0..1
    float bi() noexcept { return uni() * 2.0f - 1.0f; }                                      // -1..1
};

// Flush denormals for the lifetime of the object (audio thread).
struct ScopedNoDenormals
{
    ScopedNoDenormals() noexcept;
    ~ScopedNoDenormals() noexcept;
private:
    intptr_t saved = 0;
};

// One-pole smoother for parameters read once per tick.
struct Smoothed
{
    float current = 0.0f, target = 0.0f;
    void reset (float v) noexcept { current = target = v; }
    // Returns the value for this tick; `coeff` 0..1 (1 = jump).
    float tick (float t, float coeff) noexcept { target = t; current += (target - current) * coeff; return current; }
};
} // namespace tincture
