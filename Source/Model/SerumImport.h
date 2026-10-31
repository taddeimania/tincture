// SerumImport.h — turning a Serum 2 preset (.SerumPreset) into a Tincture
// patch.
//
// The importer starts from Tincture's init patch, applies the places where
// Serum 2's init differs (Filter 1 off, noise level, ...), then maps every
// module the preset edits: oscillators, sub and noise, routing and bus sends,
// both filters, ENV 1-4, LFO 1-10 (rate, mode, shape), macros and their
// names, the mod matrix, the voicing/global settings and the three FX racks.
//
// Serum only stores parameters that differ from its defaults; anything it
// leaves out keeps Tincture's default, which follows Serum's for nearly every
// control. Units are converted where they differ (cutoff law, sustain level,
// master volume, LFO sync divisions, curve knobs, ...). It is an
// approximation: the sound will be close, not identical.
//
// Wavetables, noise and sample files are only referenced by Serum (a path
// relative to its Tables / Samples folders), so the caller supplies the
// lookups (`Assets`). A table that can't be found falls back to the nearest
// Tincture factory table and is listed in the report.
#pragma once

#include "Patch.h"
#include "SerumPreset.h"
#include "../Engine/Params.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tincture::serum
{
struct Assets
{
    // relPath as Serum stores it, e.g. "S2 Tables/Default Shapes.wav" or
    // "/Analog/Basic Shapes.wav". Return nullptr if not found.
    std::function<std::shared_ptr<const Wavetable> (const std::string& relPath)> wavetable;
    // Noise and sample-oscillator files ("Organics/AC hum1.wav", "Factory/Plucked/x.flac").
    std::function<std::shared_ptr<const SampleData> (const std::string& relPath)> sample;
};

struct Report
{
    std::string presetName;
    std::vector<std::string> notes;          // what was approximated or left out, one line each
    std::vector<std::string> missingFiles;   // referenced files that weren't found
    int modRows = 0, modRowsDropped = 0;
    int fxModules = 0, fxDropped = 0;
    double peakBeforeDb = 0.0;               // test render before the level match (-inf = silent)

    void note (const std::string& s);        // de-duplicated
    std::string summary() const;             // a few lines for the UI
};

// Fills `patch` and `values` (both reset to init first). Returns false only
// when the file can't be read at all; partial imports return true with notes.
bool importPreset (const uint8_t* data, size_t size, Patch& patch, ParamValues& values,
                   const Assets& assets, Report& report, std::string& error);
bool importState (const File& file, Patch& patch, ParamValues& values, const Assets& assets, Report& report);

// Renders a short note (a chord for pads) and sets the master volume so the
// sound peaks at `targetPeakDb`, like Tincture's own presets. Serum's gain
// staging (and its master law) differs from Tincture's, so this is what
// makes imports come out at a usable level. Returns the measured peak before
// the change (dBFS), or -inf when the patch is silent (then nothing changes).
double matchLevel (const Patch& patch, ParamValues& values, double targetPeakDb = -6.0);

// The mapping helpers, exposed for the tests.
int filterTypeFor (const std::string& serumName, bool& exact);       // index into choices::filterTypes()
int fxFilterTypeFor (const std::string& serumName, bool& exact);     // index into the FX filter's type list
int warpModeFor (const std::string& serumName, int osc, int& fmType);// index into choices::warpModes(), -1 = unknown
int syncRateForKnob (double storedRate);                             // index into choices::syncRates()
float attackCurveFor (double serumCurve);                            // Serum 0..100 -> Tincture -1..1
float fallCurveFor (double serumCurve);
} // namespace tincture::serum
