// ParamIds.h — indices of every parameter, filled in by registerAllParams().
//
// Engine and UI code look parameters up through these structs, e.g.
//     pv[ids().osc[1].level]        // OSC B level, plain value
// The registration order in ParamIds.cpp is the host's parameter order.
#pragma once

#include "Core.h"
#include "Params.h"

#include <string>
#include <vector>

namespace tincture
{
// Choice lists shared by the engine and the UI. Index = stored value.
namespace choices
{
    const std::vector<std::string>& oscTypes();       // Wavetable, Multisample, Sample, Granular, Spectral
    const std::vector<std::string>& routes();         // Filter, Main, Direct, None
    const std::vector<std::string>& unisonModes();    // Linear, Super, Exp, Inv, Random
    const std::vector<std::string>& unisonStacks();   // Off, 12 (1-3x), 12+7 (1-3x), Center-12, Center-24
    const std::vector<std::string>& phaseMemory();    // All Voices, Contiguous, Per Voice
    const std::vector<std::string>& pitchModes();     // Semitones, Harmonics, Ratio
    const std::vector<std::string>& subShapes();      // Sine, Rounded Rect, Triangle, Saw, Square, Pulse
    const std::vector<std::string>& noiseColours();   // Sample, White, Pink, Brown, Geiger
    const std::vector<std::string>& loopModes();      // One-shot, Fwd Loop, Rev Loop, Fwd/Rev Loop, Tailed
    const std::vector<std::string>& filterRoutes1();  // Filter 2, Main, Direct, None
    const std::vector<std::string>& filterRoutes2();  // Filter 1, Main, Direct, None
    const std::vector<std::string>& busRoutes1();     // Main, Direct, Bus 2
    const std::vector<std::string>& busRoutes2();     // Main, Direct, Bus 1
    const std::vector<std::string>& lfoTypes();       // Normal, Path, Chaos: Lorenz, Chaos: Rossler, S&H
    const std::vector<std::string>& lfoModes();       // Free, Retrig, Envelope
    const std::vector<std::string>& lfoDirections();  // Forward, Reverse, Ping Pong
    const std::vector<std::string>& syncRates();      // "8 bars" ... "1/256" (straight values)
    const std::vector<std::string>& scales();         // the 51 scales
    const std::vector<std::string>& keys();           // C, C#, ... B
    const std::vector<std::string>& winShapes();      // granular windows
    const std::vector<std::string>& voiceSteal();     // proposed: Oldest, Quietest, Lowest, Highest

    // Warp modes: one flat list; see Warp.h for what each index means.
    const std::vector<std::string>& warpModes();
    const std::vector<std::string>& fmTypes();        // Phase, Thru-Zero, Exp, Linear

    // Filter types, flat, in menu order; filterCategories() gives the
    // sub-menus as (name, first index, count). Defined in FilterTypes.cpp.
    const std::vector<std::string>& filterTypes();
    struct Category { const char* name; int first, count; };
    const std::vector<Category>& filterCategories();
    // The VAR knob's caption for a filter type ("FAT", "FREQ", ... or "" when unused).
    const char* filterVarLabel (int type);
}

// Length in quarter notes of syncRates()[i].
double syncRateBeats (int index);

struct OscIds
{
    int enable, type, route, filterBal, bus1, bus2, env1Gate;
    int oct, sem, fine, coarse, pitchMode, ratio, pitchTrack, bendTrack;
    int level, pan;
    int wtpos, wtSmooth, phase, rand, phaseMem, phaseLegato;
    int unison, detune, blend, uniMode, uniStack, uniWidth, uniRange, uniWtpos, uniWarp1, uniWarp2, uniStart, uniSpan;
    int warp1Mode, warp1, warp2Mode, warp2, warpVar, fmType;
    // Sample / Granular / Spectral / Multisample.
    int start, end, loopStart, loopEnd, loopMode, xfade, scan, timbre;
    int density, grainLen, grOffset, grDir, grPitch, grLenRand, grPanRand, grGainRand;
    int winAmt, winAmtRand, winSkew, winSkewRand, winShape;
    int specLo, specHi, specCut, specMix;
};

struct SubIds  { int enable, route, filterBal, bus1, bus2, env1Gate, oct, coarse, shape, phase, contiguous, pan, level, pitchTrack; };
struct NoiseIds { int enable, route, filterBal, bus1, bus2, env1Gate, colour, oneShot, start, rand, pitch, fine, pan, level, stereo, tilt, pitchTrack; };

struct FilterIds
{
    int enable, type, cutoff, res, drive, var, pan, mix, level, keytrack, cleanDrive;
    int route, bus1, bus2, chanPan, env1Gate;
};

struct EnvIds { int attack, hold, decay, sustain, release, attackCurve, decayCurve, releaseCurve, bpm, legatoInvert; };

struct LfoIds
{
    int type, mode, mono, direction, gridX, gridY, host, bpm, rate, rateSync, trip, dot, swing, x10;
    int rise, delay, smooth, phase, phaseSnap;
};

constexpr int kNumFxUnits = 24;      // FX module instances across all racks (and splitter bands)
constexpr int kFxParams = 16;        // normalised parameters per unit

struct FxUnitIds { int mix; int p[kFxParams]; };

struct GlobalIds
{
    int master, amp, mainLevel, directLevel;
    int bus1Level, bus1Route, bus2Level, bus2Route;
    int poly, mono, legato, porta, portaCurve, portaAlways, portaScaled, voiceSteal;
    int bendUp, bendDown, transpose, key, scale, swing, tuning, concertA;
    int velo, note;         // the VELO / NOTE source outputs can't be set; these are placeholders (-1)
    int arpOn, clipOn, midiOut;
};

struct ParamIdTable
{
    OscIds osc[kNumOscs];
    SubIds sub;
    NoiseIds noise;
    FilterIds filter[kNumFilters];
    EnvIds env[kNumEnvs];
    LfoIds lfo[kNumLfos];
    int macro[kNumMacros];
    FxUnitIds fx[kNumFxUnits];
    GlobalIds global;
};

const ParamIdTable& ids();

// Called once by params(); also fills ids().
void registerAllParams (ParamRegistry&);
} // namespace tincture
