// FxTypes.cpp — the FX module type table: names, colours and the meaning of
// each module's 16 parameter slots (ranges, defaults, formatting).
#include "FxTypes.h"

#include <cmath>
#include <cstdio>

namespace tincture
{
// ---------------------------------------------------------------------------
// FxParamDesc
// ---------------------------------------------------------------------------
int FxParamDesc::index (float n) const noexcept
{
    const int c = count();
    if (c <= 1) return 0;
    n = clamp01 (n);
    return (int) std::lround (n * (float) (c - 1));
}

float FxParamDesc::toPlain (float n) const noexcept
{
    n = clamp01 (n);
    switch (map)
    {
        case FxMap::Linear: return min + (max - min) * n;
        case FxMap::Log:    return min * std::pow (max / min, n);
        case FxMap::Power:  return min + (max - min) * std::pow (n, skew);
        case FxMap::Level:  return gainToDb (levelGain (n));
        case FxMap::Ratio:
        {
            if (n >= 0.985f) return 1.0e9f;                     // Limit
            const float t = std::min (n / 0.985f, 0.999f);
            return 1.0f / (1.0f - t);
        }
        case FxMap::EqQ:    return 0.70710678f * std::exp2 ((n <= 0.6f ? 5.0f : 10.0f) * (n - 0.6f));
        case FxMap::Index:  return (float) index (n);
    }
    return n;
}

float FxParamDesc::toNorm (float x) const noexcept
{
    switch (map)
    {
        case FxMap::Linear: return max > min ? clamp01 ((x - min) / (max - min)) : 0.0f;
        case FxMap::Log:    return clamp01 (std::log (std::max (x, min) / min) / std::log (max / min));
        case FxMap::Power:  return std::pow (clamp01 ((x - min) / (max - min)), 1.0f / skew);
        case FxMap::Level:  return clamp01 (std::sqrt (dbToGain (x) * 0.25f));
        case FxMap::Ratio:
        {
            if (x >= 1.0e8f) return 1.0f;
            const float t = 1.0f - 1.0f / std::max (x, 1.0f);
            return clamp01 (t * 0.985f);
        }
        case FxMap::EqQ:
        {
            const float e = std::log2 (std::max (x, 1.0e-3f) / 0.70710678f);
            return clamp01 (0.6f + e / (e <= 0.0f ? 5.0f : 10.0f));
        }
        case FxMap::Index:
        {
            const int c = count();
            return c > 1 ? clamp01 (x / (float) (c - 1)) : 0.0f;
        }
    }
    return clamp01 (x);
}

namespace
{
std::string fmt (const char* f, double v)
{
    char b[64];
    std::snprintf (b, sizeof (b), f, v);
    return b;
}

std::string formatNumber (float v, int decimals)
{
    char f[16];
    std::snprintf (f, sizeof (f), "%%.%df", decimals < 0 ? 0 : decimals);
    std::string s = fmt (f, (double) v);
    if (s == "-0" || s == "-0.0" || s == "-0.00") s = s.substr (1);
    return s;
}
} // namespace

std::string FxParamDesc::format (float n) const
{
    if (kind == FxKind::Unused) return {};
    if (kind == FxKind::Toggle) return index (n) ? "On" : "Off";
    if (kind == FxKind::Choice) return choices.empty() ? std::string() : choices[(size_t) index (n)];
    const float v = toPlain (n);
    switch (unit)
    {
        case FxUnitKind::Percent: return formatNumber (v * displayScale, decimals) + "%";
        case FxUnitKind::Hz:      return v >= 1000.0f ? formatNumber (v * 0.001f, 2) + " kHz" : formatNumber (v, v < 10.0f ? 2 : decimals) + " Hz";
        case FxUnitKind::Ms:      return v >= 1000.0f ? formatNumber (v * 0.001f, 2) + " s" : formatNumber (v, v < 10.0f ? 2 : decimals) + " ms";
        case FxUnitKind::Db:      return v <= -119.0f ? "-inf dB" : formatNumber (v, decimals) + " dB";
        case FxUnitKind::Ratio:   return v >= 1.0e8f ? "Limit" : (v >= 100.0f ? "Inf:1" : formatNumber (v, 1) + ":1");
        case FxUnitKind::Semis:   return formatNumber (v, decimals) + " st";
        case FxUnitKind::Times:   return "x" + formatNumber (v, 2);
        case FxUnitKind::Octaves: return formatNumber (v, decimals) + " oct";
        case FxUnitKind::None:    break;
    }
    return formatNumber (v * displayScale, decimals);
}

// ---------------------------------------------------------------------------
// The table
// ---------------------------------------------------------------------------
namespace
{
FxParamDesc knob (const char* name, const char* sh, FxMap m, float mn, float mx, float defPlain,
                  FxUnitKind u, int decimals = 1, float skew = 1.0f)
{
    FxParamDesc d;
    d.name = name; d.shortName = sh; d.kind = FxKind::Knob;
    d.map = m; d.min = mn; d.max = mx; d.skew = skew; d.unit = u; d.decimals = decimals;
    d.def = d.toNorm (defPlain);
    return d;
}
FxParamDesc pct (const char* name, const char* sh, float def01)
{
    auto d = knob (name, sh, FxMap::Linear, 0.0f, 1.0f, def01, FxUnitKind::Percent, 0);
    d.displayScale = 100.0f;
    return d;
}
FxParamDesc bip (const char* name, const char* sh, float def = 0.0f)
{
    auto d = knob (name, sh, FxMap::Linear, -1.0f, 1.0f, def, FxUnitKind::Percent, 0);
    d.displayScale = 100.0f;
    d.bipolar = true;
    return d;
}
FxParamDesc hz (const char* name, const char* sh, float mn, float mx, float def)
{
    return knob (name, sh, FxMap::Log, mn, mx, def, FxUnitKind::Hz, 0);
}
FxParamDesc ms (const char* name, const char* sh, float mn, float mx, float def, float skew)
{
    return knob (name, sh, FxMap::Power, mn, mx, def, FxUnitKind::Ms, 1, skew);
}
FxParamDesc db (const char* name, const char* sh, float mn, float mx, float def)
{
    auto d = knob (name, sh, FxMap::Linear, mn, mx, def, FxUnitKind::Db, 1);
    d.bipolar = mn < 0.0f && mx > 0.0f && std::abs (mn + mx) < 1.0e-3f;
    return d;
}
FxParamDesc toggle (const char* name, const char* sh, bool def)
{
    FxParamDesc d;
    d.name = name; d.shortName = sh; d.kind = FxKind::Toggle; d.map = FxMap::Index;
    d.def = def ? 1.0f : 0.0f; d.modulatable = false;
    return d;
}
FxParamDesc choice (const char* name, const char* sh, std::vector<std::string> items, int def)
{
    FxParamDesc d;
    d.name = name; d.shortName = sh; d.kind = FxKind::Choice; d.map = FxMap::Index;
    d.choices = std::move (items); d.modulatable = false;
    d.def = d.toNorm ((float) def);
    return d;
}
FxParamDesc level()
{
    auto d = knob ("Level", "LEVEL", FxMap::Level, 0.0f, 1.0f, 0.0f, FxUnitKind::Db, 1);
    d.def = 0.5f;
    return d;
}
// Note-value sync: rates use 8 bars..1/32, times 1/256 (n = 0) .. `slowest`.
FxParamDesc& syncRate (FxParamDesc& d, int toggleSlot)
{
    d.syncToggle = toggleSlot; d.syncFirst = 0; d.syncCount = 9; d.syncIsTime = false;
    return d;
}
FxParamDesc& syncTime (FxParamDesc& d, int toggleSlot, int slowestIndex)
{
    d.syncToggle = toggleSlot; d.syncFirst = slowestIndex; d.syncCount = 12 - slowestIndex; d.syncIsTime = true;
    return d;
}
std::vector<std::string> numbers (int from, int to)
{
    std::vector<std::string> v;
    for (int i = from; i <= to; ++i) v.push_back (std::to_string (i));
    return v;
}

FxTypeInfo make (FxType t, const char* name, const char* caps, uint32_t colour, float defMix = 1.0f)
{
    FxTypeInfo i;
    i.type = t; i.name = name; i.caps = caps; i.colour = colour; i.defaultMix = defMix;
    i.params[kFxLevelSlot] = level();
    return i;
}

std::vector<FxTypeInfo> buildTable()
{
    std::vector<FxTypeInfo> v;

    // Bode -------------------------------------------------------------------
    {
        auto i = make (FxType::Bode, "Bode", "BODE", 0xffe5484d);
        auto& p = i.params;
        p[0] = pct ("Shift", "SHIFT", 0.0f);
        p[1] = hz ("Range", "RANGE", 1.0f, 4000.0f, 100.0f);
        p[2] = bip ("Direction", "DIR", 1.0f);
        p[3] = pct ("Width", "WIDTH", 0.0f);
        p[4] = ms ("Delay", "DELAY", 0.0f, 1000.0f, 125.0f, 2.0f);
        syncTime (p[4], 5, 3);
        p[5] = toggle ("Delay BPM Sync", "BPM", false);
        p[6] = pct ("Feedback", "FEED", 0.0f);
        p[7] = bip ("Balance", "BALANCE");
        p[8] = pct ("Blur", "BLUR", 0.0f);
        p[9] = toggle ("Mono Input", "MONO INPUT", false);
        p[10] = toggle ("Retrig", "RETRIG", false);
        v.push_back (i);
    }
    // Chorus -----------------------------------------------------------------
    {
        auto i = make (FxType::Chorus, "Chorus", "CHORUS", 0xff3cc8e0, 0.5f);
        auto& p = i.params;
        p[0] = knob ("Rate", "RATE", FxMap::Power, 0.0f, 20.0f, 0.5f, FxUnitKind::Hz, 2, 3.0f);
        syncRate (p[0], 1);
        p[1] = toggle ("Rate BPM Sync", "BPM", false);
        p[2] = ms ("Delay 1", "DELAY 1", 0.1f, 40.0f, 8.0f, 2.0f);
        p[3] = ms ("Delay 2", "DELAY 2", 0.1f, 40.0f, 13.0f, 2.0f);
        p[4] = pct ("Depth", "DEPTH", 0.25f);
        p[5] = pct ("Feedback", "FEEDBACK", 0.0f);
        p[6] = hz ("Filter Freq", "FREQ", 20.0f, 20000.0f, 20000.0f);
        p[7] = choice ("Filter Type", "LPF", { "LPF", "HPF" }, 0);
        v.push_back (i);
    }
    // Compressor -------------------------------------------------------------
    {
        auto i = make (FxType::Compressor, "Compressor", "COMPRESSOR", 0xfff2a477);
        i.numVariants = 4;              // SINGLE, MULTIBAND, SINGLE+Limit, MULTIBAND+Limit
        auto& p = i.params;
        p[0] = choice ("Mode", "MODE", { "Single", "Multiband" }, 0);
        p[1] = knob ("Threshold", "THRESH", FxMap::Power, 0.0f, -120.0f, -18.1f, FxUnitKind::Db, 1, 2.0f);
        p[1].def = std::sqrt (18.1f / 120.0f);
        p[2] = knob ("Ratio", "RATIO", FxMap::Ratio, 1.0f, 1.0e9f, 4.0f, FxUnitKind::Ratio, 1);
        p[3] = knob ("Attack", "ATTACK", FxMap::Log, 0.1f, 1000.0f, 90.1f, FxUnitKind::Ms, 1);
        p[4] = knob ("Release", "RELEASE", FxMap::Log, 1.0f, 1000.0f, 90.1f, FxUnitKind::Ms, 1);
        p[5] = db ("Makeup Gain", "GAIN", 0.0f, 30.0f, 0.0f);
        p[6] = hz ("Crossover Low", "X-LOW", 20.0f, 1000.0f, 88.0f);
        p[7] = pct ("Below Ratio", "BELOW", 0.0f);              // upward ratio 1 + 19 n^2 (fxFormat shows it)
        p[8] = hz ("Crossover High", "X-HIGH", 500.0f, 16000.0f, 2500.0f);
        p[9] = db ("High Band Gain", "H", -24.0f, 24.0f, 0.0f);
        p[10] = db ("Mid Band Gain", "M", -24.0f, 24.0f, 0.0f);
        p[11] = db ("Low Band Gain", "L", -24.0f, 24.0f, 0.0f);
        p[12] = toggle ("Limiter Latency Comp", "LAT COMP", false);
        v.push_back (i);
    }
    // Convolve ---------------------------------------------------------------
    {
        auto i = make (FxType::Convolve, "Convolve", "CONVOLVE", 0xff4cc56a, 0.3f);
        auto& p = i.params;
        p[0] = knob ("Size", "SIZE", FxMap::Log, 0.25f, 4.0f, 1.0f, FxUnitKind::Percent, 0);
        p[0].displayScale = 100.0f;
        p[1] = bip ("Tone", "TONE");
        p[2] = toggle ("Minimum Phase", "MIN", false);
        p[3] = ms ("Pre-Delay", "PRE-DLY", 0.0f, 500.0f, 0.0f, 2.0f);
        syncTime (p[3], 4, 5);
        p[4] = toggle ("Pre-Delay BPM Sync", "BPM", false);
        p[5] = pct ("Attack", "ATTACK", 0.0f);
        p[6] = pct ("Decay", "DECAY", 1.0f);
        p[7] = pct ("Damp", "DAMP", 0.0f);
        p[8] = db ("IR Gain", "IR GAIN", -24.0f, 24.0f, 0.0f);
        v.push_back (i);
    }
    // Delay ------------------------------------------------------------------
    {
        auto i = make (FxType::Delay, "Delay", "DELAY", 0xff9b6ff2, 0.3f);
        auto& p = i.params;
        p[0] = choice ("Mode", "MODE", { "Normal", "Ping-Pong", "Tap->Delay" }, 0);
        p[1] = toggle ("High Quality", "HQ", false);
        p[2] = toggle ("BPM", "BPM", true);
        p[3] = toggle ("Link", "LINK", true);
        p[4] = ms ("Time L", "L", 1.0f, 2000.0f, 0.0f, 2.0f);
        syncTime (p[4], 2, 0);
        p[4].def = 6.0f / 11.0f;                                // 1/4 when synced
        p[5] = knob ("Scalar L", "L x", FxMap::Linear, 0.0f, 2.0f, 1.0f, FxUnitKind::Times, 2);
        p[6] = p[4]; p[6].name = "Time R"; p[6].shortName = "R";
        p[7] = p[5]; p[7].name = "Scalar R"; p[7].shortName = "R x";
        p[8] = pct ("Feedback", "FEEDBACK", 0.4f);
        p[9] = hz ("Filter Freq", "FREQ", 40.0f, 18000.0f, 1200.0f);
        p[10] = pct ("Filter Width", "Q", 0.75f);               // max = widest band = least filtering
        v.push_back (i);
    }
    // Distortion -------------------------------------------------------------
    {
        auto i = make (FxType::Distortion, "Distortion", "DISTORTION", 0xfff25c2c);
        auto& p = i.params;
        p[0] = choice ("Mode", "MODE", { "Tube", "SoftClip", "HardClip", "Diode 1", "Diode 2", "Lin.Fold", "Sin.Fold",
                                         "Zero-Square", "Downsample", "Asym", "Rectify", "X-Shaper", "X-Shaper (Asym)" }, 0);
        p[1] = choice ("Filter Position", "FILTER", { "Off", "Pre", "Post" }, 0);
        p[2] = pct ("Filter Type", "TYPE", 0.0f);              // LP -> BP -> HP morph
        p[3] = hz ("Filter Freq", "FREQ", 20.0f, 20000.0f, 425.0f);
        p[4] = knob ("Filter Q", "Q", FxMap::Log, 0.5f, 20.0f, 1.9f, FxUnitKind::None, 1);
        p[5] = pct ("Drive", "DRIVE", 0.25f);
        p[6] = toggle ("Filter Key Track", "KEY", false);
        v.push_back (i);
    }
    // Equalizer --------------------------------------------------------------
    {
        auto i = make (FxType::Equalizer, "Equalizer", "EQUALIZER", 0xff8cc8ff);
        i.hasMix = false;
        auto& p = i.params;
        auto q = [] (const char* n, const char* s)
        {
            auto d = knob (n, s, FxMap::EqQ, 0.0f, 1.0f, 0.70710678f, FxUnitKind::Percent, 0);
            d.def = 0.6f;
            return d;
        };
        p[0] = choice ("Low Type", "LOW TYPE", { "Shelf", "Peak", "High Pass" }, 0);
        p[1] = hz ("Low Freq", "FREQ", 20.0f, 20000.0f, 210.0f);
        p[2] = q ("Low Q", "Q");
        p[3] = db ("Low Gain", "GAIN", -24.0f, 24.0f, 0.0f);
        p[4] = choice ("High Type", "HIGH TYPE", { "Shelf", "Peak", "Low Pass" }, 0);
        p[5] = hz ("High Freq", "FREQ", 20.0f, 20000.0f, 2041.0f);
        p[6] = q ("High Q", "Q");
        p[7] = db ("High Gain", "GAIN", -24.0f, 24.0f, 0.0f);
        v.push_back (i);
    }
    // Filter -----------------------------------------------------------------
    {
        auto i = make (FxType::Filter, "Filter", "FILTER", 0xff3f7cf2);
        i.numVariants = fxchoices::FNumTypes;
        auto& p = i.params;
        p[0] = choice ("Type", "TYPE", { "MG Low 6", "MG Low 12", "MG Low 18", "MG Low 24", "Low 12", "Low 24", "High 12",
                                         "High 24", "Band 12", "Band 24", "Peak 12", "Notch 12", "Notch 24", "Acid Ladder",
                                         "Comb +", "Comb -" }, 0);
        p[1] = hz ("Cutoff", "CUTOFF", 20.0f, 20000.0f, 1262.0f);
        p[2] = pct ("Resonance", "RES", 0.1f);
        p[3] = pct ("Drive", "DRIVE", 0.0f);
        p[4] = pct ("Var", "VAR", 0.0f);
        p[4].variantNames = { "FAT", "FAT", "FAT", "FAT", "", "", "", "", "", "", "", "", "", "FAT", "DAMP", "DAMP" };
        p[5] = bip ("Stereo", "PAN");
        p[6] = toggle ("Cutoff Key Track", "KEY", false);
        p[7] = toggle ("Clean Drive", "CLEAN", false);
        v.push_back (i);
    }
    // Flanger ----------------------------------------------------------------
    {
        auto i = make (FxType::Flanger, "Flanger", "FLANGER", 0xffef6fb4, 0.5f);
        auto& p = i.params;
        p[0] = knob ("Rate", "RATE", FxMap::Power, 0.0f, 20.0f, 0.3f, FxUnitKind::Hz, 2, 3.0f);
        syncRate (p[0], 1);
        p[1] = toggle ("Rate BPM Sync", "BPM", false);
        p[2] = pct ("Depth", "DEPTH", 0.5f);
        p[3] = pct ("Feedback", "FEEDBACK", 0.5f);
        p[4] = pct ("Stereo Phase", "PHASE", 0.0f);
        v.push_back (i);
    }
    // Hyper/Dimension --------------------------------------------------------
    {
        auto i = make (FxType::Hyper, "Hyper/Dimension", "HYPER/DIMENSION", 0xfff2d43c, 0.5f);
        i.mixName = "HYPER MIX";
        auto& p = i.params;
        p[0] = knob ("Hyper Rate", "RATE", FxMap::Log, 0.05f, 10.0f, 0.8f, FxUnitKind::Hz, 2);
        p[1] = choice ("Hyper Unison", "UNISON", numbers (0, 7), 4);
        p[1].modulatable = true;
        p[2] = pct ("Hyper Detune", "DETUNE", 0.25f);
        p[3] = toggle ("Hyper Retrig", "RETRIG", false);
        p[4] = pct ("Dimension Size", "SIZE", 0.5f);
        p[5] = pct ("Dimension Mix", "MIX", 0.0f);
        v.push_back (i);
    }
    // Phaser -----------------------------------------------------------------
    {
        auto i = make (FxType::Phaser, "Phaser", "PHASER", 0xff2fbfa8, 0.5f);
        auto& p = i.params;
        p[0] = knob ("Rate", "RATE", FxMap::Power, 0.0f, 20.0f, 0.5f, FxUnitKind::Hz, 2, 3.0f);
        syncRate (p[0], 1);
        p[1] = toggle ("Rate BPM Sync", "BPM", false);
        p[2] = choice ("Poles", "POLES", numbers (1, 12), 3);
        p[2].modulatable = true;
        p[3] = pct ("Depth", "DEPTH", 0.5f);
        p[4] = pct ("Depth 2 (Spread)", "DEPTH 2", 0.0f);
        p[5] = hz ("Frequency", "FREQ", 20.0f, 18000.0f, 800.0f);
        p[6] = pct ("Feedback", "FEEDBACK", 0.0f);
        p[7] = pct ("Stereo Phase", "PHASE", 0.0f);
        v.push_back (i);
    }
    // Reverb -----------------------------------------------------------------
    {
        auto i = make (FxType::Reverb, "Reverb", "REVERB", 0xffa6d93a, 0.3f);
        i.numVariants = 5;              // Plate, Hall, Vintage, Nitrous, Basin
        auto& p = i.params;
        p[0] = choice ("Type", "TYPE", { "Plate", "Hall", "Vintage", "Nitrous", "Basin" }, 0);
        p[1] = pct ("Low Cut", "LO", 0.0f);
        p[2] = pct ("High Cut", "HI", 0.35f);
        p[3] = pct ("Size", "SIZE", 0.5f);
        p[4] = ms ("Pre-Delay", "PRE-DLY", 0.0f, 500.0f, 0.0f, 2.0f);
        syncTime (p[4], 5, 5);
        p[5] = toggle ("Pre-Delay BPM Sync", "BPM", false);
        p[6] = pct ("Damp", "DAMP", 0.4f);
        p[6].variantNames = { "DAMP", "", "DAMP", "", "" };
        p[7] = pct ("Width", "WIDTH", 1.0f);
        p[7].variantNames = { "WIDTH", "", "", "", "" };
        p[8] = pct ("Decay / Feedback", "DECAY", 0.5f);
        p[8].variantNames = { "", "DECAY", "DECAY", "FEEDBACK", "FEEDBACK" };
        p[9] = pct ("Spin / Chorus Rate", "RATE", 0.3f);
        p[9].variantNames = { "", "SPIN RATE", "CHORUS RATE", "CHORUS RATE", "CHORUS RATE" };
        p[10] = pct ("Spin / Chorus Depth", "DEPTH", 0.3f);
        p[10].variantNames = { "", "SPIN DEPTH", "CHORUS DEPTH", "CHORUS DEPTH", "CHORUS DEPTH" };
        p[11] = pct ("Early Reflection Size", "ER SIZE", 0.5f);
        p[11].variantNames = { "", "", "ER SIZE", "", "" };
        p[12] = pct ("Diffusion", "DIFF A", 0.7f);
        p[12].variantNames = { "", "", "DIFF A", "DIFFUSION", "" };
        p[13] = pct ("Diffusion Damping", "DIFF B", 0.3f);
        p[13].variantNames = { "", "", "DIFF B", "", "" };
        p[14] = choice ("Nitrous Mode", "MODE", { "Space", "Marble", "Rectangle", "Hexagon", "Box" }, 0);
        p[14].variantNames = { "", "", "", "MODE", "" };
        v.push_back (i);
    }
    // Splitters --------------------------------------------------------------
    {
        auto i = make (FxType::SplitterLH, "Splitter L/H", "SPLITTER L/H", 0xff9a9a9a);
        i.splitter = true; i.numBands = 2; i.bandNames = { "LOWS", "HIGHS" }; i.hasMix = false;
        i.params[0] = hz ("Split Freq", "SPLIT FREQ", 20.0f, 20000.0f, 210.0f);
        v.push_back (i);
    }
    {
        auto i = make (FxType::SplitterLMH, "Splitter L/M/H", "SPLITTER L/M/H", 0xff9a9a9a);
        i.splitter = true; i.numBands = 3; i.bandNames = { "LOWS", "MIDS", "HIGHS" }; i.hasMix = false;
        i.params[0] = hz ("Split Freq Low", "SPLIT FREQ", 20.0f, 20000.0f, 210.0f);
        i.params[1] = hz ("Split Freq High", "SPLIT FREQ", 20.0f, 20000.0f, 2100.0f);
        v.push_back (i);
    }
    {
        auto i = make (FxType::SplitterMS, "Splitter M/S", "SPLITTER M/S", 0xff9a9a9a);
        i.splitter = true; i.numBands = 2; i.bandNames = { "MID", "SIDE" }; i.hasMix = false;
        v.push_back (i);
    }
    // Utility ----------------------------------------------------------------
    {
        auto i = make (FxType::Utility, "Utility", "UTILITY", 0xffb4b4b4);
        auto& p = i.params;
        p[0] = toggle ("Invert Left", "L", false);
        p[1] = toggle ("Invert Right", "R", false);
        p[2] = hz ("Low Pass", "LPF", 20.0f, 20000.0f, 20000.0f);
        p[3] = hz ("High Pass", "HPF", 20.0f, 20000.0f, 20.0f);
        p[4] = toggle ("Mono Bass", "MONO BASS", false);
        p[5] = hz ("Mono Bass Freq", "FREQ", 20.0f, 500.0f, 120.0f);
        p[6] = knob ("Width", "WIDTH", FxMap::Linear, 0.0f, 2.0f, 1.0f, FxUnitKind::Percent, 0);
        p[6].displayScale = 100.0f;
        p[7] = bip ("Pan", "PAN");
        v.push_back (i);
    }
    return v;
}

const std::vector<FxTypeInfo>& table()
{
    static const std::vector<FxTypeInfo> t = buildTable();
    return t;
}
} // namespace

const FxTypeInfo& fxInfo (FxType t)
{
    const int i = (int) t;
    return table()[(size_t) (i < 0 || i >= kNumFxTypes ? 0 : i)];
}

const std::vector<std::string>& fxTypeNames()
{
    static const std::vector<std::string> n = []
    {
        std::vector<std::string> v;
        for (auto& i : table()) v.push_back (i.name);
        return v;
    }();
    return n;
}

std::array<float, kFxParams> fxDefaults (FxType t)
{
    std::array<float, kFxParams> d {};
    const auto& info = fxInfo (t);
    for (int k = 0; k < kFxParams; ++k)
        d[(size_t) k] = info.params[(size_t) k].kind == FxKind::Unused ? 0.0f : info.params[(size_t) k].def;
    return d;
}

int fxVariant (FxType t, const float* v)
{
    const auto& p = fxInfo (t).params;
    switch (t)
    {
        case FxType::Compressor: return p[0].index (v[0]) + (v[2] >= 0.985f ? 2 : 0);
        case FxType::Filter:     return p[0].index (v[0]);
        case FxType::Reverb:     return p[0].index (v[0]);
        default:                 return 0;
    }
}

bool fxSlotVisible (FxType t, int slot, const float* v)
{
    if (slot < 0 || slot >= kFxParams) return false;
    const auto& d = fxInfo (t).params[(size_t) slot];
    if (d.kind == FxKind::Unused) return false;
    if (d.variantNames.empty()) return true;
    const int var = fxVariant (t, v);
    return var < (int) d.variantNames.size() && ! d.variantNames[(size_t) var].empty();
}

std::string fxSlotName (FxType t, int slot, const float* v)
{
    if (slot < 0 || slot >= kFxParams) return {};
    const auto& d = fxInfo (t).params[(size_t) slot];
    if (d.variantNames.empty()) return d.shortName;
    const int var = fxVariant (t, v);
    return var < (int) d.variantNames.size() ? d.variantNames[(size_t) var] : std::string();
}

bool fxSlotSynced (FxType t, int slot, const float* v)
{
    const auto& d = fxInfo (t).params[(size_t) slot];
    return d.syncToggle >= 0 && v[d.syncToggle] >= 0.5f;
}

double fxSyncBeats (const FxParamDesc& d, float n)
{
    const int c = std::max (d.syncCount, 1);
    int k = (int) std::lround (clamp01 (n) * (float) (c - 1));
    if (d.syncIsTime) k = c - 1 - k;            // n = 0: shortest
    return syncRateBeats (d.syncFirst + k);
}

float fxRateHz (FxType t, int slot, const float* v, double bpm)
{
    const auto& d = fxInfo (t).params[(size_t) slot];
    if (fxSlotSynced (t, slot, v))
        return (float) (bpm / (60.0 * fxSyncBeats (d, v[slot])));
    return d.toPlain (v[slot]);
}

float fxTimeSeconds (FxType t, int slot, const float* v, double bpm)
{
    const auto& d = fxInfo (t).params[(size_t) slot];
    if (fxSlotSynced (t, slot, v))
        return (float) (fxSyncBeats (d, v[slot]) * 60.0 / std::max (bpm, 1.0));
    return d.toPlain (v[slot]) * 0.001f;
}

std::string fxFormat (FxType t, int slot, const float* v)
{
    if (slot < 0 || slot >= kFxParams) return {};
    const auto& d = fxInfo (t).params[(size_t) slot];
    if (fxSlotSynced (t, slot, v))
    {
        const int c = std::max (d.syncCount, 1);
        int k = (int) std::lround (clamp01 (v[slot]) * (float) (c - 1));
        if (d.syncIsTime) k = c - 1 - k;
        return choices::syncRates()[(size_t) (d.syncFirst + k)];
    }
    if (t == FxType::Compressor && v[2] >= 0.985f)
    {
        if (slot == 3) return formatNumber (clamp01 (v[3]) * 10.0f, 2) + " ms";    // look-ahead
        if (slot == 5) return formatNumber (clamp01 (v[5]) * 36.0f, 1) + " dB";
    }
    if (t == FxType::Compressor && slot == 7)
    {
        const float r = 1.0f + 19.0f * clamp01 (v[7]) * clamp01 (v[7]);
        return formatNumber (r, 1) + ":1";
    }
    if (t == FxType::Reverb && slot == 8)
    {
        const int type = fxInfo (t).params[0].index (v[0]);
        if (type == fxchoices::Hall || type == fxchoices::Vintage)
            return formatNumber (0.2f * std::exp2 (clamp01 (v[8]) * 7.0f), 2) + " s";       // RT60
    }
    if (t == FxType::Delay && (slot == 5 || slot == 7))
    {
        const float x = d.toPlain (v[slot]);
        if (std::abs (x - 1.0f) < 0.005f) return "---";
        if (std::abs (x - 4.0f / 3.0f) < 0.005f) return "Trip";
        if (std::abs (x - 1.5f) < 0.005f) return "Dot";
    }
    return d.format (v[slot]);
}
} // namespace tincture
