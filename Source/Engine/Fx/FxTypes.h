// FxTypes.h — the 16 FX module types (in "+ FX" menu order), their names and
// colours, and what each of a module's 16 normalised parameter slots means.
//
// A module instance in a rack owns one FX unit (ids().fx[u]); its controls
// ARE that unit's slots p[0..15] (all 0..1) plus the unit's MIX. This file
// describes, per type, how each slot maps to a real value, how it is shown
// and what its default is. Slot 15 is always LEVEL (output gain).
//
// Some slots change meaning with another slot ("variants"): e.g. the Reverb
// type decides which knobs are shown and what they are called, and a BPM
// toggle turns a time/rate knob into a note-value selector. fxVariant() and
// fxFormat() / fxSlotName() take all 16 values to resolve that.
#pragma once

#include "../ParamIds.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace tincture
{
enum class FxType : uint8_t
{
    Bode = 0, Chorus, Compressor, Convolve, Delay, Distortion, Equalizer, Filter, Flanger,
    Hyper, Phaser, Reverb, SplitterLH, SplitterLMH, SplitterMS, Utility
};
constexpr int kNumFxTypes = 16;
constexpr int kFxLevelSlot = 15;            // every type: LEVEL (−inf..+12 dB, 0.5 = 0 dB)

enum class FxKind : uint8_t { Unused, Knob, Toggle, Choice };

// How a knob's 0..1 maps to its plain value.
enum class FxMap : uint8_t
{
    Linear,     // min..max
    Log,        // min * (max/min)^n
    Power,      // min + (max-min) * n^skew
    Level,      // gain = 4 n^2  (0 = −inf, 0.5 = 0 dB, 1 = +12 dB); plain = dB
    Ratio,      // compressor ratio: 1/(1-t), t = n/0.985; n >= 0.985 = Limit (plain = +inf)
    EqQ,        // 0.6 -> 0.707, two-slope exponential (0.088 .. 11.3)
    Index       // Choice / Toggle / integer: round (n * (count-1))
};

enum class FxUnitKind : uint8_t { None, Percent, Hz, Ms, Db, Ratio, Semis, Times, Octaves };

// What one of a module's slots means.
struct FxParamDesc
{
    std::string name;                   // tooltip / automation name, e.g. "Feedback"
    std::string shortName;              // knob caption, e.g. "FEEDBACK"
    float def = 0.0f;                   // default, normalised
    FxKind kind = FxKind::Unused;
    std::vector<std::string> choices;   // Choice: items in menu order (index = round (n * (size-1)))
    bool bipolar = false;               // drawn from the centre
    bool modulatable = true;            // Knob slots; toggles/choices are not (proposed)

    FxMap map = FxMap::Linear;
    float min = 0.0f, max = 1.0f, skew = 1.0f;
    FxUnitKind unit = FxUnitKind::None;
    float displayScale = 1.0f;          // e.g. 100 for 0..1 shown as %
    int decimals = 1;

    // When >= 0: the slot index of a BPM toggle. With it on, this knob picks
    // a note value from choices::syncRates() [syncFirst, syncFirst+syncCount),
    // slowest at n = 0 (rates) or fastest at n = 0 (times, syncIsTime).
    int syncToggle = -1;
    int syncFirst = 0, syncCount = 0;
    bool syncIsTime = false;

    // Per-variant captions (see fxVariant); empty = always shortName; an
    // entry "" means the slot is hidden in that variant.
    std::vector<std::string> variantNames;

    float toPlain (float norm) const noexcept;          // in the slot's units (dB, Hz, ms, %, index...)
    float toNorm (float plain) const noexcept;
    std::string format (float norm) const;              // context-free; prefer fxFormat()
    int index (float norm) const noexcept;              // Choice/Toggle/Index
    int count() const noexcept { return kind == FxKind::Toggle ? 2 : (int) choices.size(); }
};

struct FxTypeInfo
{
    FxType type {};
    std::string name;                   // menu text, e.g. "Hyper/Dimension"
    std::string caps;                   // list/rack caption, e.g. "HYPER/DIMENSION"
    uint32_t colour = 0xff888888;       // ARGB
    bool splitter = false;
    int numBands = 0;                   // splitters: 2 or 3
    std::vector<std::string> bandNames; // "LOWS", "HIGHS" ...
    bool hasMix = true;                 // shows the unit's MIX knob
    std::string mixName = "MIX";        // caption for the unit's MIX (Hyper: "HYPER MIX")
    float defaultMix = 1.0f;            // the unit's MIX default
    int numVariants = 1;
    std::array<FxParamDesc, kFxParams> params;
};

const FxTypeInfo& fxInfo (FxType t);
inline const FxTypeInfo& fxInfo (int t) { return fxInfo ((FxType) t); }
const std::vector<std::string>& fxTypeNames();              // the + FX menu, in order
std::array<float, kFxParams> fxDefaults (FxType t);         // normalised slot defaults
inline float fxDefaultMix (FxType t) { return fxInfo (t).defaultMix; }

// Context-dependent views of a slot (all take the unit's 16 values).
int fxVariant (FxType t, const float* norm16);
bool fxSlotVisible (FxType t, int slot, const float* norm16);
std::string fxSlotName (FxType t, int slot, const float* norm16);  // caption in the current variant
std::string fxFormat (FxType t, int slot, const float* norm16);    // value text incl. sync / Limit
bool fxSlotSynced (FxType t, int slot, const float* norm16);

// Note length in beats for a synced slot at `norm`.
double fxSyncBeats (const FxParamDesc& d, float norm);
// Rate in Hz (rates) or time in seconds (times) of a slot, honouring its BPM toggle.
float fxRateHz (FxType t, int slot, const float* norm16, double bpm);
float fxTimeSeconds (FxType t, int slot, const float* norm16, double bpm);

// Plain value of a slot (as FxParamDesc::toPlain), for the DSP code.
inline float fxPlain (FxType t, int slot, float norm) { return fxInfo (t).params[(size_t) slot].toPlain (norm); }
inline float levelGain (float norm) noexcept { return 4.0f * norm * norm; }

// Choice lists used by the module code (index = stored value).
namespace fxchoices
{
    enum DistMode { Tube = 0, SoftClip, HardClip, Diode1, Diode2, LinFold, SinFold, ZeroSquare, Downsample, Asym, Rectify, XShaper, XShaperAsym };
    enum DelayMode { Normal = 0, PingPong, TapDelay };
    enum ReverbType { Plate = 0, Hall, Vintage, Nitrous, Basin };
    enum FilterKind { FMgLow6 = 0, FMgLow12, FMgLow18, FMgLow24, FLow12, FLow24, FHigh12, FHigh24, FBand12, FBand24,
                      FPeak12, FNotch12, FNotch24, FAcidLadder, FCombPlus, FCombMinus, FNumTypes };
}
} // namespace tincture
