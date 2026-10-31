// Patch.h — everything in a sound that isn't a parameter: wavetables,
// samples, LFO shapes, the mod matrix, the FX racks, arp and clip banks,
// oscillator key mapping, preset metadata. Edited on the message thread as a
// value; the audio thread gets an immutable compiled copy (CompiledPatch in
// Engine/Synth.h).
#pragma once

#include "Curve.h"
#include "Tree.h"
#include "../Engine/Arp.h"
#include "../Engine/Clip.h"
#include "../Engine/Fx/FxLayout.h"
#include "../Engine/ModMatrix.h"
#include "../Engine/NoteProcessor.h"
#include "../Engine/NoiseOsc.h"
#include "../Engine/SampleOsc.h"
#include "../Engine/WavetableOsc.h"

#include <array>
#include <string>
#include <vector>

namespace tincture
{
struct PresetInfo
{
    std::string name = "Init", author, description, category;
    std::vector<std::string> tags;
    int rating = 0;
    void save (Node&) const;
    void load (const Node&);
};

struct Patch
{
    PresetInfo info;
    std::array<WavetableOscData, kNumOscs> wavetable;
    std::array<SampleOscData, kNumOscs> sample;
    NoiseOscData noise;
    std::array<LfoData, kNumLfos> lfo;
    ModMatrixData matrix;
    ModGraphs graphs;                 // VELO and NOTE curves
    FxLayout fx;
    ArpBank arp;
    ClipBank clips;
    KeyboardSettings keyboard;        // swing division, OSC MAPPING
    std::array<std::string, kNumMacros> macroNames {};   // empty = "MACRO n"
    bool limitSameNote = false;       // POLY menu: Limit Same Note Poly to 1

    Patch();                          // the init patch
    void save (Node&) const;
    void load (const Node&);          // also rebuilds derived sample data
};
} // namespace tincture
