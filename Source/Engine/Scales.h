// Scales.h — interval sets for the 51 scales of the SCALE menu
// (choices::scales(), same order), key/scale quantising and the "in scale"
// test used by the piano roll shading, FOLD and Conform to Scale.
//
// Keys: the G.key parameter is a choice where 0 = "-" (off) and 1..12 =
// C..B; keyFromParam() turns it into a pitch class (0..11) or -1 for off.
#pragma once

#include <cstdint>

namespace tincture::scales
{
constexpr int kNumScales = 51;
constexpr int kChromatic = 50;

// Bit i set = the scale contains the degree i semitones above the root.
uint16_t mask (int scale) noexcept;
// Number of notes per octave.
int size (int scale) noexcept;

// G.key parameter value (0 = off, 1..12 = C..B) -> pitch class, or -1.
inline int keyFromParam (float v) noexcept { const int k = (int) (v + 0.5f); return k >= 1 && k <= 12 ? k - 1 : -1; }

// key: pitch class 0..11, or -1 = off (every note is "in scale").
bool inScale (int note, int key, int scale) noexcept;
bool isRoot (int note, int key) noexcept;

// Moves a note to the nearest scale degree; ties go up. Key -1 = unchanged.
// The result stays in 0..127 (it goes down instead of up past 127).
int quantise (int note, int key, int scale) noexcept;
// The semitone offset quantise() would add for this pitch class (precomputed table).
int quantiseOffset (int pitchClassFromRoot, int scale) noexcept;
} // namespace tincture::scales
