// Scales.cpp — interval tables for the SCALE menu and key/scale quantising.
//
// The guide only names the scales; these are the usual textbook definitions.
// Where a name is ambiguous the choice is noted (semitones from the root):
//   Gypsy            Hungarian gypsy 0 2 3 6 7 8 10
//   Algerian         7-note form 0 2 3 6 7 8 11
//   Hindu            Mixolydian b6 0 2 4 5 7 8 10
//   Nat Bhairava     Nat Bhairav thaat = harmonic major 0 2 4 5 7 8 11
//   Acoustic         Lydian dominant 0 2 4 6 7 9 10
//   Arabian          major Locrian 0 2 4 5 6 8 10
//   Flamenco         8-note Phrygian with major 3rd 0 1 3 4 5 7 8 10
//   Klezmer          Freygish / Phrygian dominant 0 1 4 5 7 8 10
//   Neopolitan       Neapolitan minor 0 1 3 5 7 8 11
//   Purvi / Todi     the Purvi and Todi thaats
//   Japanese         In (Miyako-bushi) 0 1 5 7 8
//   Balinese         Pelog 0 1 3 7 8
//   Asian            "Oriental" 0 1 4 5 6 9 10
//   Egyptian         suspended pentatonic 0 2 5 7 10
//   Piongio          0 2 5 7 9 10
//   Hexatonic        major hexatonic (no 7th) 0 2 4 5 7 9
//   Ancient Chinese  Lydian-derived pentatonic 0 4 6 7 11
//   Whole Dim.       whole-half octatonic; Half Dim. = half-whole octatonic
//   Major+b7         bebop dominant 0 2 4 5 7 9 10 11
//   Eastern          double harmonic / Byzantine 0 1 4 5 7 8 11
//   Bebop Minor      bebop Dorian 0 2 3 4 5 7 9 10
//   Polymode         Messiaen mode 3 (9 notes) 0 2 3 4 6 7 8 10 11
//   Nine Tone        0 2 3 4 6 7 8 9 11
//   Major/Minor      union of major and natural minor (10 notes); the
//                    "Mixolydian b6" reading would duplicate Hindu.
#include "Scales.h"

#include <initializer_list>

namespace tincture::scales
{
namespace
{
constexpr uint16_t bits (std::initializer_list<int> degrees)
{
    uint16_t m = 0;
    for (int d : degrees) m = (uint16_t) (m | (1u << d));
    return m;
}

constexpr uint16_t kMasks[kNumScales] = {
    bits ({ 0, 2, 4, 5, 7, 9, 11 }),        // Major
    bits ({ 0, 2, 3, 5, 7, 9, 10 }),        // Dorian
    bits ({ 0, 1, 3, 5, 7, 8, 10 }),        // Phrygian
    bits ({ 0, 2, 4, 6, 7, 9, 11 }),        // Lydian
    bits ({ 0, 2, 4, 5, 7, 9, 10 }),        // Mixolydian
    bits ({ 0, 2, 3, 5, 7, 8, 10 }),        // Minor
    bits ({ 0, 1, 3, 5, 6, 8, 10 }),        // Locrian
    bits ({ 0, 2, 3, 5, 7, 8, 11 }),        // Minor (Harmonic)
    bits ({ 0, 2, 3, 6, 7, 9, 10 }),        // Romanian Minor
    bits ({ 0, 2, 3, 6, 7, 8, 10 }),        // Gypsy
    bits ({ 0, 2, 3, 6, 7, 8, 11 }),        // Algerian
    bits ({ 0, 2, 4, 5, 7, 8, 10 }),        // Hindu
    bits ({ 0, 2, 4, 5, 7, 8, 11 }),        // Nat Bhairava
    bits ({ 0, 2, 4, 6, 7, 9, 10 }),        // Acoustic
    bits ({ 0, 2, 4, 5, 6, 8, 10 }),        // Arabian
    bits ({ 0, 1, 3, 4, 5, 7, 8, 10 }),     // Flamenco
    bits ({ 0, 1, 4, 5, 7, 8, 10 }),        // Klezmer
    bits ({ 0, 1, 3, 5, 7, 8, 11 }),        // Neopolitan
    bits ({ 0, 1, 4, 5, 6, 8, 11 }),        // Persian
    bits ({ 0, 1, 4, 6, 7, 8, 11 }),        // Purvi Theta
    bits ({ 0, 1, 3, 6, 7, 8, 11 }),        // Todi Theta
    bits ({ 0, 1, 3, 4, 7, 8, 9 }),         // Ultra Phrygian
    bits ({ 0, 1, 5, 6, 10 }),              // Iwato
    bits ({ 0, 1, 5, 7, 10 }),              // Insen
    bits ({ 0, 1, 5, 7, 8 }),               // Japanese
    bits ({ 0, 1, 3, 7, 8 }),               // Balinese
    bits ({ 0, 2, 3, 7, 8 }),               // Hirajoshi
    bits ({ 0, 2, 3, 7, 9 }),               // Akebono
    bits ({ 0, 1, 4, 5, 6, 9, 10 }),        // Asian
    bits ({ 0, 2, 5, 7, 10 }),              // Egyptian
    bits ({ 0, 2, 4, 7, 9 }),               // Major Pentatonic
    bits ({ 0, 3, 5, 7, 10 }),              // Minor Pentatonic
    bits ({ 0, 2, 3, 4, 7, 9 }),            // Major Blues
    bits ({ 0, 3, 5, 6, 7, 10 }),           // Minor Blues
    bits ({ 0, 2, 5, 7, 9, 10 }),           // Piongio
    bits ({ 0, 2, 4, 5, 7, 9 }),            // Hexatonic
    bits ({ 0, 4, 6, 7, 11 }),              // Ancient Chinese
    bits ({ 0, 3, 4, 7, 8, 11 }),           // Augmented
    bits ({ 0, 2, 4, 6, 8, 10 }),           // Whole Tone
    bits ({ 0, 2, 3, 5, 6, 8, 9, 11 }),     // Whole Dim.
    bits ({ 0, 1, 3, 4, 6, 7, 9, 10 }),     // Half Dim.
    bits ({ 0, 2, 3, 5, 7, 9, 11 }),        // Minor (Melodic)
    bits ({ 0, 2, 4, 5, 7, 9, 10, 11 }),    // Major+b7
    bits ({ 0, 1, 4, 5, 7, 8, 11 }),        // Eastern
    bits ({ 0, 2, 3, 4, 5, 7, 9, 10 }),     // Bebop Minor
    bits ({ 0, 2, 4, 5, 7, 8, 9, 11 }),     // Bebop Major
    bits ({ 0, 2, 3, 4, 6, 7, 8, 10, 11 }), // Polymode
    bits ({ 0, 1, 2, 3, 5, 7, 9, 10 }),     // Adonai Malakh
    bits ({ 0, 2, 3, 4, 6, 7, 8, 9, 11 }),  // Nine Tone
    bits ({ 0, 2, 3, 4, 5, 7, 8, 9, 10, 11 }), // Major/Minor
    0x0FFF                                  // Chromatic
};

// offsets[scale][pc] = semitones to add to reach the nearest degree (ties up).
struct OffsetTable
{
    int8_t o[kNumScales][12] {};
    OffsetTable()
    {
        for (int s = 0; s < kNumScales; ++s)
            for (int pc = 0; pc < 12; ++pc)
                for (int d = 0; d <= 6; ++d)
                {
                    if (kMasks[s] & (1u << ((pc + d) % 12)))      { o[s][pc] = (int8_t) d; break; }
                    if (kMasks[s] & (1u << ((pc - d + 12) % 12))) { o[s][pc] = (int8_t) -d; break; }
                }
    }
};
const OffsetTable& table() { static const OffsetTable t; return t; }

inline int clampScale (int s) noexcept { return s < 0 ? 0 : (s >= kNumScales ? kNumScales - 1 : s); }
inline int pcOf (int note, int key) noexcept { return ((note - key) % 12 + 12) % 12; }
} // namespace

uint16_t mask (int scale) noexcept { return kMasks[clampScale (scale)]; }

int size (int scale) noexcept
{
    int n = 0;
    for (uint16_t m = mask (scale); m; m = (uint16_t) (m & (m - 1))) ++n;
    return n;
}

bool inScale (int note, int key, int scale) noexcept
{
    if (key < 0) return true;
    return (mask (scale) >> pcOf (note, key)) & 1u;
}

bool isRoot (int note, int key) noexcept { return key >= 0 && pcOf (note, key) == 0; }

int quantiseOffset (int pc, int scale) noexcept { return table().o[clampScale (scale)][((pc % 12) + 12) % 12]; }

int quantise (int note, int key, int scale) noexcept
{
    if (key < 0) return note;
    const int pc = pcOf (note, key);
    int out = note + quantiseOffset (pc, scale);
    if (out > 127)
    {
        // Nearest degree below instead.
        for (int d = 1; d <= 11; ++d)
            if (inScale (note - d, key, scale)) { out = note - d; break; }
    }
    if (out < 0)
    {
        for (int d = 1; d <= 11; ++d)
            if (inScale (note + d, key, scale)) { out = note + d; break; }
    }
    return out;
}
} // namespace tincture::scales
