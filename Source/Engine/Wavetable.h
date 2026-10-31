// Wavetable.h — immutable wavetables for the wavetable oscillator, their
// band-limited mip-maps, the FFT used to build them, and the factory table
// library. Also a few fast-math helpers shared by the oscillators.
//
// A table holds up to 256 frames of 2048 samples. Frames may be interpolated
// at build time (crossfade or spectral morph); the patch stores the source
// frames and the morph type, not the generated frames.
//
// Mip-maps: level m keeps harmonics 1..(1024 >> m). Higher levels are stored
// in shorter tables (2048 .. 256 samples), sized so that linear
// interpolation on playback keeps its images low (the length is at least 4x
// the top harmonic, and 16x or more for the small high levels).
// Each stored mip frame has kGuard extra samples (a copy of the start) so
// the reader never wraps an index.
#pragma once

#include "Core.h"
#include "../Model/Tree.h"

#include <memory>
#include <string>
#include <vector>

namespace tincture
{
// ---------------------------------------------------------------------------
// Fast math for the oscillator inner loops (no libm calls).
// ---------------------------------------------------------------------------
namespace fm
{
// x - floor(x), for |x| < 2^31.
inline float wrap01 (float x) noexcept
{
    float t = (float) (int) x;
    t -= t > x ? 1.0f : 0.0f;
    return x - t;
}

// sin (2 pi x) for any moderate x; max error ~4e-6.
inline float sin2pi (float x) noexcept
{
    x -= (float) (int) x;                                   // (-1, 1)
    x = x >= 0.5f ? x - 1.0f : (x < -0.5f ? x + 1.0f : x);  // [-0.5, 0.5)
    x = x > 0.25f ? 0.5f - x : (x < -0.25f ? -0.5f - x : x);
    const float z = x * kTwoPi, z2 = z * z;
    return z * (1.0f + z2 * (-1.0f / 6.0f + z2 * (1.0f / 120.0f + z2 * (-1.0f / 5040.0f + z2 * (1.0f / 362880.0f)))));
}

// 2^x, relative error ~2e-5, x clamped to +-60.
inline float exp2 (float x) noexcept
{
    x = clampf (x, -60.0f, 60.0f);
    float fl = (float) (int) x;
    fl -= fl > x ? 1.0f : 0.0f;
    const float f = x - fl;
    const float p = 1.0f + f * (0.69314718f + f * (0.24022651f + f * (0.05550411f + f * (0.00961813f + f * (0.00133336f + f * 0.00015404f)))));
    const uint32_t bits = (uint32_t) ((int) fl + 127) << 23;
    float s;
    std::memcpy (&s, &bits, sizeof (s));
    return p * s;
}
} // namespace fm

// In-place iterative radix-2 complex FFT; n must be a power of two.
// Forward: X[k] = sum x[n] e^(-2 pi i k n / N). Inverse includes the 1/N.
void fft (float* re, float* im, int n, bool inverse);

class Wavetable
{
public:
    static constexpr int kFrameSize = 2048;
    static constexpr int kMaxFrames = 256;
    static constexpr int kNumMips = 11;
    static constexpr int kGuard = 4;

    // Frame interpolation done when the table is built (the editor's MORPH menu).
    enum class Morph : int { None = 0, Crossfade, Spectral, SpectralZeroFund, SpectralZeroAll };

    std::string name, category;
    bool factory = false;               // a FactoryTables entry (patches store a reference)

    // Builds a table from source frames (each any length; resampled to 2048).
    // With a morph and 2+ sources the table is filled to 256 frames.
    // normalise: scale the whole table to a peak of 1 (relative frame levels kept).
    static std::shared_ptr<Wavetable> create (std::string name, std::string category,
                                              std::vector<std::vector<float>> frames,
                                              Morph morph = Morph::None, bool normalise = true);

    // Frames as the user sees them (WT POS range, frame number display).
    int numFrames() const noexcept { return logicalFrames; }
    int numSourceFrames() const noexcept { return (int) sources.size(); }
    const std::vector<float>& sourceFrame (int i) const { return sources[(size_t) i]; }
    bool isSourceFrame (int logical) const noexcept;
    Morph morph() const noexcept { return morphType; }
    // The full-band frame at a logical index (UI / editor; interpolated for crossfade morphs).
    void renderFrame (int logical, float* out2048) const;

    // ---- Playback (audio thread) ------------------------------------------
    // Frames actually stored. A crossfade morph is not expanded: blending the
    // sources at playback is the same thing, so stored = sources.
    int storedFrames() const noexcept { return stored; }
    // WT POS 0..1 -> position in stored frames (0 .. storedFrames-1).
    // smooth = false snaps to the nearest logical frame.
    float storedPos (float wtpos, bool smooth) const noexcept
    {
        if (logicalFrames <= 1) return 0.0f;
        float l = clamp01 (wtpos) * (float) (logicalFrames - 1);
        if (! smooth) l = (float) (int) (l + 0.5f);
        return l * posScale;
    }

    static constexpr int mipHarmonics (int level) noexcept { return 1024 >> level; }
    static constexpr int mipLength (int level) noexcept
    {
        constexpr int len[kNumMips] { 2048, 2048, 1024, 1024, 512, 512, 256, 256, 256, 256, 256 };
        return len[level];
    }
    static constexpr int mipStride (int level) noexcept { return mipLength (level) + kGuard; }
    // The mip level for a read rate (cycles per sample, including any warp
    // speed-up) so that no harmonic goes above maxCycles (cycles per sample;
    // 0.5 = Nyquist, more allows folding into the inaudible top octave).
    static int mipFor (float cyclesPerSample, float maxCycles) noexcept
    {
        int m = 0;
        while (m < kNumMips - 1 && (float) mipHarmonics (m) * cyclesPerSample > maxCycles) ++m;
        return m;
    }
    // mipStride(level) floats: mipLength(level) samples + kGuard wrap copies.
    const float* mipFrame (int level, int storedFrame) const noexcept
    {
        return data.data() + levelOffset[level] + (size_t) storedFrame * (size_t) mipStride (level);
    }

    size_t memoryBytes() const noexcept { return data.size() * sizeof (float); }

    // Saves the source frames (as one float array) and the morph type.
    void save (Node& n) const;
    static std::shared_ptr<const Wavetable> load (const Node& n);

private:
    std::vector<std::vector<float>> sources;      // 2048 each, as given (after resampling)
    std::vector<int> sourceAt;                    // logical index of each source
    Morph morphType = Morph::None;
    bool normalised = true;
    int logicalFrames = 1, stored = 1;
    float posScale = 1.0f;                        // logical -> stored position
    std::vector<float> data;
    size_t levelOffset[kNumMips] {};

    void build();
};

// ---------------------------------------------------------------------------
// Factory tables: procedurally generated, in the menu's folders. Built on
// first request (message thread), cached, thread-safe.
// ---------------------------------------------------------------------------
struct FactoryTables
{
    static const std::vector<std::string>& categories();     // Analog, Digital, S2 Tables, Spectral, Vowel
    static std::vector<std::string> names (const std::string& category);
    // nullptr if there is no such table.
    static std::shared_ptr<const Wavetable> get (const std::string& category, const std::string& name);
    static std::shared_ptr<const Wavetable> defaultTable();  // "S2 Tables" / "Default Shapes"
    static int count();
};
} // namespace tincture
