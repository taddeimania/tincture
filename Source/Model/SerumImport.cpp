// SerumImport.cpp — Serum 2 preset -> Tincture patch. See SerumImport.h.
//
// Facts about the Serum 2 format used here were read off real presets
// (module names, which keys appear, value ranges) and cross-checked with the
// public notes on the format (serum2-preset-loader, serum2vital's source-id
// and unit tables). Where a unit law is a measurement rather than something
// stored, the comment says so.
#include "SerumImport.h"
#include "../Engine/Fx/FxTypes.h"
#include "../Engine/ParamIds.h"
#include "../Engine/Synth.h"
#include "../Engine/Wavetable.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>

namespace tincture::serum
{
// ---------------------------------------------------------------------------
// Report
// ---------------------------------------------------------------------------
void Report::note (const std::string& s)
{
    if (std::find (notes.begin(), notes.end(), s) == notes.end()) notes.push_back (s);
}

std::string Report::summary() const
{
    std::string s = "Imported \"" + presetName + "\" from Serum 2.";
    if (modRows > 0 || modRowsDropped > 0)
        s += "\nMod matrix: " + std::to_string (modRows) + " routings" + (modRowsDropped > 0 ? " (" + std::to_string (modRowsDropped) + " left out)" : std::string());
    if (fxModules > 0 || fxDropped > 0)
        s += "\nFX: " + std::to_string (fxModules) + " modules" + (fxDropped > 0 ? " (" + std::to_string (fxDropped) + " left out)" : std::string());
    if (! missingFiles.empty())
    {
        s += "\nNot found (Tincture tables used instead):";
        for (const auto& f : missingFiles) s += "\n  " + f;
    }
    if (! notes.empty())
    {
        s += "\nApproximated or left out:";
        for (const auto& n : notes) s += "\n  " + n;
    }
    return s;
}

namespace
{
// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
std::string squash (const std::string& s)
{
    std::string o;
    for (const char c : s)
        if (std::isalnum ((unsigned char) c)) o += (char) std::tolower ((unsigned char) c);
    return o;
}

int indexOf (const std::vector<std::string>& list, const std::string& name)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == name) return (int) i;
    return -1;
}

bool startsWith (const std::string& s, const char* p) { return s.rfind (p, 0) == 0; }
bool endsWith (const std::string& s, const std::string& e) { return s.size() >= e.size() && s.compare (s.size() - e.size(), e.size(), e) == 0; }

const char* const kOscNames[] { "OSC A", "OSC B", "OSC C" };

// plainParams of a module, or nullptr when it is "default" / missing.
const Value* plain (const Value& module)
{
    const Value* p = module.find ("plainParams");
    return p != nullptr && p->isMap() ? p : nullptr;
}

double num (const Value* pp, const char* key, double fallback)
{
    if (pp == nullptr) return fallback;
    const Value* v = pp->find (key);
    return v != nullptr ? v->num (fallback) : fallback;
}

bool has (const Value* pp, const char* key)
{
    return pp != nullptr && pp->find (key) != nullptr;
}

std::string text (const Value* pp, const char* key)
{
    if (pp == nullptr) return {};
    const Value* v = pp->find (key);
    return v != nullptr && v->isString() ? v->text : std::string();
}

bool on (const Value* pp, const char* key, bool fallback)
{
    return num (pp, key, fallback ? 1.0 : 0.0) > 0.5;
}

double clampd (double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Serum's cutoff knobs: 8 Hz .. 22.05 kHz, log-linear in the stored 0..1.
double cutoffHz (double n) { return 8.0 * std::pow (22050.0 / 8.0, clampd (n, 0.0, 1.0)); }

// Serum stores sustain as a 0..1 amplitude.
double sustainDb (double s) { return s <= 0.001 ? -60.0 : std::max (-60.0, 20.0 * std::log10 (s)); }

// The Serum 1 / Serum 2 FX and filter LEVEL law: gain = (2n)^2 (0.5 = 0 dB),
// which is also Tincture's FX level slot.
double levelDb (double n) { return n <= 1.0e-4 ? -60.0 : std::max (-60.0, 40.0 * std::log10 (2.0 * n)); }

// Master volume: cubic amplitude (measured). Tincture's 0 dB master is
// matched to Serum's default 70 %, so presets keep their level relative to
// the init sound.
double masterDb (double n) { return n <= 1.0e-4 ? -60.0 : 60.0 * std::log10 (n / 0.7); }

// ---------------------------------------------------------------------------
// The importer
// ---------------------------------------------------------------------------
struct Importer
{
    const Value& s;             // the CBOR module map
    Patch& patch;
    ParamValues& v;
    const Assets& assets;
    Report& report;
    const ParamIdTable& I = ids();

    // Serum FX instances in rack order, for mod destinations (type key, rack, index in rack, unit).
    struct FxRef { std::string key; int rack = 0, index = 0, unit = -1; FxType type = FxType::Utility; };
    std::vector<FxRef> fxRefs;
    bool lfoBpm[kNumLfos] {};

    Importer (const Value& state, Patch& p, ParamValues& values, const Assets& a, Report& r)
        : s (state), patch (p), v (values), assets (a), report (r) {}

    void set (int id, double plainValue)
    {
        if (id < 0) return;
        const auto& pi = params()[id];
        float x = (float) plainValue;
        x = std::clamp (x, std::min (pi.min, pi.max), std::max (pi.min, pi.max));
        v[id] = pi.snap (x);
    }

    const Value& module (const std::string& name) const { return s[name]; }
    const Value* pp (const std::string& name) const { return plain (s[name]); }

    // ---- assets ------------------------------------------------------------
    static std::string baseName (const std::string& rel)
    {
        std::string b = rel;
        const size_t slash = b.find_last_of ("/\\");
        if (slash != std::string::npos) b = b.substr (slash + 1);
        const size_t dot = b.find_last_of ('.');
        if (dot != std::string::npos) b = b.substr (0, dot);
        return b;
    }

    std::shared_ptr<const Wavetable> fallbackTable (const std::string& rel) const
    {
        // Same name in Tincture's factory set first, then a few known equivalents.
        const std::string want = squash (baseName (rel));
        for (const auto& cat : FactoryTables::categories())
            for (const auto& n : FactoryTables::names (cat))
                if (squash (n) == want) return FactoryTables::get (cat, n);
        static const std::map<std::string, std::pair<const char*, const char*>> near {
            { "basicshapes", { "Analog", "Basic Shapes" } }, { "basicmini", { "Analog", "Basic Shapes" } },
            { "basicmdc", { "Analog", "Basic Shapes" } }, { "basicwrd", { "Analog", "Basic Shapes" } },
            { "analogsaw", { "Analog", "Analog Saw" } }, { "mbsaw", { "Analog", "Analog Saw" } },
            { "msaw", { "Analog", "Analog Saw" } }, { "jno", { "Analog", "Analog Saw" } },
            { "pwmrestricted", { "Analog", "PWM Sweep" } }, { "sawroundedtosquare", { "Analog", "Soft Square" } },
            { "sqrsawwrd", { "Analog", "Soft Square" } }, { "bsodsquare", { "Analog", "Soft Square" } },
            { "analogbdsin", { "Analog", "Basic Shapes" } }, { "reese", { "S2 Tables", "Saw Stack" } },
            { "fmdirtysquare", { "Digital", "FM 2-1" } }, { "softestgrowl", { "S2 Tables", "Resonant Saw" } },
            { "harmonicsubtle", { "S2 Tables", "Harmonic Series" } }, { "fftadd2nds", { "Spectral", "Additive Sweep" } },
            { "fftadd4ths", { "Spectral", "Additive Sweep" } }, { "additivetalk", { "Spectral", "Additive Sweep" } },
        };
        if (const auto it = near.find (want); it != near.end())
            if (auto t = FactoryTables::get (it->second.first, it->second.second)) return t;
        return FactoryTables::defaultTable();
    }

    std::shared_ptr<const Wavetable> table (const std::string& rel)
    {
        if (rel.empty()) return FactoryTables::defaultTable();
        if (assets.wavetable)
            if (auto t = assets.wavetable (rel)) return t;
        // Serum's built-in default needs no file.
        if (squash (baseName (rel)) == "defaultshapes") return FactoryTables::defaultTable();
        report.missingFiles.push_back (rel);
        return fallbackTable (rel);
    }

    std::shared_ptr<const SampleData> sampleFile (const std::string& rel)
    {
        if (rel.empty()) return nullptr;
        if (assets.sample)
            if (auto smp = assets.sample (rel)) return smp;
        report.missingFiles.push_back (rel);
        return nullptr;
    }

    // ---- init differences --------------------------------------------------
    void serumInit()
    {
        set (I.filter[0].enable, 0.0);            // Serum's filters start off
        set (I.filter[1].enable, 0.0);
        set (I.noise.level, 0.75);
        set (I.noise.colour, 0.0);                // the noise oscillator plays a sample
        for (int e = 0; e < kNumEnvs; ++e)
        {
            set (I.env[e].attackCurve, attackCurveFor (50.0));
            set (I.env[e].decayCurve, fallCurveFor (66.6));
            set (I.env[e].releaseCurve, fallCurveFor (66.6));
        }
    }

    // ---- oscillators -------------------------------------------------------
    void routing (int slot, int routeId, int bus1, int bus2)
    {
        const Value* r = pp ("RoutingSlot" + std::to_string (slot));
        const std::string to = text (r, "kParamRoutingDest");
        if (to == "kRoutingDestFilter")      set (routeId, 0);
        else if (to == "kRoutingDestMaster") set (routeId, 1);
        else if (to == "kRoutingDestDirect") set (routeId, 2);
        else if (to == "kRoutingDestNone")   set (routeId, 3);
        else if (! to.empty()) report.note ("routing '" + to + "' not recognised");
        if (has (r, "kParamFXBus1Level")) set (bus1, num (r, "kParamFXBus1Level", 0.0) / 100.0);
        if (has (r, "kParamFXBus2Level")) set (bus2, num (r, "kParamFXBus2Level", 0.0) / 100.0);
    }

    void unisonStack (int id, const std::string& st)
    {
        if (st.empty()) return;
        if (startsWith (st, "kOctaveFifth")) set (id, 2);
        else if (startsWith (st, "kOctave")) set (id, 1);
        else if (st == "kCenter12") set (id, 3);
        else if (st == "kCenter24") set (id, 4);
        else if (st == "kOff" || st == "kNone") set (id, 0);
        else report.note ("unison stack '" + st + "' not recognised");
    }

    void unisonMode (int id, const std::string& m)
    {
        if (m.empty()) return;
        static const std::pair<const char*, int> map[] { { "kDetuneLinear", 0 }, { "kDetuneSuper", 1 }, { "kDetuneExp", 2 },
                                                         { "kDetuneInv", 3 }, { "kDetuneInverse", 3 }, { "kDetuneRandom", 4 } };
        for (const auto& [name, idx] : map)
            if (m == name) { set (id, idx); return; }
        report.note ("unison mode '" + m + "' not recognised");
    }

    void warp (int o, const Value* w, const char* menuKey, const char* amountKey, int modeId, int amountId)
    {
        const std::string name = text (w, menuKey);
        if (! name.empty())
        {
            int fm = -1;
            const int mode = warpModeFor (name, o, fm);
            if (mode < 0) report.note (std::string (kOscNames[o]) + ": warp '" + name + "' has no Tincture equivalent (left off)");
            else
            {
                set (modeId, mode);
                if (fm >= 0) set (I.osc[o].fmType, fm);
            }
        }
        // Serum stores the warp amount 0..1.
        if (has (w, amountKey)) set (amountId, num (w, amountKey, 0.0));
    }

    void oscillator (int o)
    {
        const std::string mname = "Oscillator" + std::to_string (o);
        const Value& m = module (mname);
        const Value* p = plain (m);
        const auto& id = I.osc[o];

        set (id.enable, on (p, "kParamEnable", o == 0) ? 1.0 : 0.0);
        routing (o, id.route, id.bus1, id.bus2);
        if (has (p, "kParamVolume")) set (id.level, num (p, "kParamVolume", 0.75));
        if (has (p, "kParamPan")) set (id.pan, num (p, "kParamPan", 0.0) / 50.0);
        if (has (p, "kParamOctave")) set (id.oct, num (p, "kParamOctave", 0.0));
        if (has (p, "kParamPitch")) set (id.sem, num (p, "kParamPitch", 0.0));
        if (has (p, "kParamSemi")) set (id.sem, num (p, "kParamSemi", 0.0));
        if (has (p, "kParamFine")) set (id.fine, num (p, "kParamFine", 0.0));
        if (has (p, "kParamCoarsePit"))
        {
            const double c = num (p, "kParamCoarsePit", 0.0);
            if (std::abs (c) > 48.0) report.note (std::string (kOscNames[o]) + ": coarse pitch beyond +/-48 st clamped");
            set (id.coarse, c);
        }
        if (has (p, "kParamUnison")) set (id.unison, num (p, "kParamUnison", 1.0));
        if (has (p, "kParamDetune")) set (id.detune, num (p, "kParamDetune", 0.25));
        if (has (p, "kParamUnisonBlend")) set (id.blend, num (p, "kParamUnisonBlend", 75.0) / 100.0);
        if (has (p, "kParamDetuneWid")) set (id.uniWidth, num (p, "kParamDetuneWid", 100.0) / 100.0);
        if (has (p, "kParamUnisonRange")) set (id.uniRange, num (p, "kParamUnisonRange", 2.0));
        unisonStack (id.uniStack, text (p, "kParamUnisonStack"));
        unisonMode (id.uniMode, text (p, "kParamDetuneMode"));
        if (has (p, "kParamPitchTrack")) set (id.pitchTrack, on (p, "kParamPitchTrack", true) ? 1.0 : 0.0);

        const std::string type = text (p, "kParamType");
        const Value& wt = m["WTOsc" + std::to_string (o)];
        const Value* w = plain (wt);

        if (type.empty() || type == "kOsc_Wavetable")
        {
            set (id.type, 0);
            patch.wavetable[(size_t) o].table = table (wt["relativePathToWT"].str());
            // WT POS: 1..256 in Serum's display, 0..1 here.
            if (has (w, "kParamTablePos")) set (id.wtpos, (num (w, "kParamTablePos", 1.0) - 1.0) / 255.0);
            if (has (w, "kParamInitialPhase")) set (id.phase, num (w, "kParamInitialPhase", 180.0));
            if (has (w, "kParamRandomPhase")) set (id.rand, num (w, "kParamRandomPhase", 100.0) / 100.0);
            const std::string mem = text (w, "kParamPhaseMemory");
            if (mem == "kPerVoice") set (id.phaseMem, 2);
            else if (mem == "kContiguous") set (id.phaseMem, 1);
            warp (o, w, "kParamWarpMenu", "kParamWarp", id.warp1Mode, id.warp1);
            warp (o, w, "kParamWarpMenu2", "kParamWarp2", id.warp2Mode, id.warp2);
        }
        else if (type == "kOsc_Sample" || type == "kOsc_Granular" || type == "kOsc_Spectral")
        {
            const int t = type == "kOsc_Sample" ? 2 : (type == "kOsc_Granular" ? 3 : 4);
            set (id.type, t);
            const char* subKey = t == 2 ? "SampleOsc" : (t == 3 ? "GranularOsc" : "SpectralOsc");
            const Value& so = m[subKey + std::to_string (o)];
            auto& sd = patch.sample[(size_t) o];
            sd.type = t;
            sd.sample = sampleFile (so["samplePathRelative"].str());
            const Value* sp = plain (so);
            if (has (p, "kParamLoopStart")) set (id.loopStart, num (p, "kParamLoopStart", 0.0) / 100.0);
            if (has (p, "kParamLoopEnd")) set (id.loopEnd, num (p, "kParamLoopEnd", 100.0) / 100.0);
            if (has (sp, "kParamStart")) set (id.start, num (sp, "kParamStart", 0.0) / 100.0);
            if (t != 2) report.note (std::string (kOscNames[o]) + ": " + (t == 3 ? "granular" : "spectral") + " settings beyond the sample itself were not imported");
        }
        else if (type == "kOsc_MultiSample")
        {
            // Multisample instruments (SFZ) aren't read; keep a wavetable in its place.
            set (id.type, 0);
            patch.wavetable[(size_t) o].table = FactoryTables::defaultTable();
            report.note (std::string (kOscNames[o]) + ": multisample instrument not imported (a wavetable plays instead)");
        }
        else
            report.note (std::string (kOscNames[o]) + ": oscillator type '" + type + "' not recognised");
    }

    void noise()
    {
        const Value& m = module ("Oscillator3");
        const Value* p = plain (m);
        const auto& id = I.noise;
        set (id.enable, on (p, "kParamEnable", false) ? 1.0 : 0.0);
        routing (3, id.route, id.bus1, id.bus2);
        if (has (p, "kParamVolume")) set (id.level, num (p, "kParamVolume", 0.75));
        if (has (p, "kParamPan")) set (id.pan, num (p, "kParamPan", 0.0) / 50.0);

        const Value& n = m["NoiseOsc3"];
        const Value* np = plain (n);
        const std::string type = text (np, "kParamNoiseType");
        if (type == "White") set (id.colour, 1);
        else if (type == "Pink") set (id.colour, 2);
        else if (type == "Brown") set (id.colour, 3);
        else
        {
            if (! type.empty()) report.note ("noise type '" + type + "' not recognised");
            const std::string rel = n["relativePathToNoiseSample"].str();
            if (! rel.empty())
            {
                if (auto smp = sampleFile (rel)) patch.noise.sample = smp;
                else set (id.colour, 1);                        // white noise rather than silence
            }
        }
        if (has (np, "kParamOneShot")) set (id.oneShot, on (np, "kParamOneShot", false) ? 1.0 : 0.0);
        if (has (np, "kParamInitialPhase")) set (id.start, num (np, "kParamInitialPhase", 0.0) / 100.0);
        if (has (np, "kParamRandomPhase")) set (id.rand, num (np, "kParamRandomPhase", 0.0) / 100.0);
        if (has (np, "kParamPitch")) set (id.pitch, num (np, "kParamPitch", 50.0) / 100.0);
        if (has (np, "kParamPitchTrack")) set (id.pitchTrack, on (np, "kParamPitchTrack", false) ? 1.0 : 0.0);
        // COLOR is Serum's tilt filter (0..1, 0.5 = flat; measured as the knob centre).
        if (has (np, "kParamColor")) set (id.tilt, num (np, "kParamColor", 0.5) * 2.0 - 1.0);
    }

    void sub()
    {
        const Value& m = module ("Oscillator4");
        const Value* p = plain (m);
        const auto& id = I.sub;
        set (id.enable, on (p, "kParamEnable", false) ? 1.0 : 0.0);
        routing (4, id.route, id.bus1, id.bus2);
        if (has (p, "kParamVolume")) set (id.level, num (p, "kParamVolume", 0.75));
        if (has (p, "kParamPan")) set (id.pan, num (p, "kParamPan", 0.0) / 50.0);
        if (has (p, "kParamOctave")) set (id.oct, num (p, "kParamOctave", 0.0));
        if (has (p, "kParamCoarsePit")) set (id.coarse, num (p, "kParamCoarsePit", 0.0));
        const Value* sp = plain (m["SubOsc4"]);
        const std::string shape = text (sp, "kParamShape");
        static const std::pair<const char*, int> shapes[] { { "kSine", 0 }, { "kRoundRect", 1 }, { "kTriangle", 2 },
                                                            { "kSaw", 3 }, { "kSquare", 4 }, { "kPulse", 5 } };
        for (const auto& [name, idx] : shapes)
            if (shape == name) set (id.shape, idx);
        if (has (sp, "kParamInitialPhase")) set (id.phase, num (sp, "kParamInitialPhase", 0.0));
    }

    // ---- filters -----------------------------------------------------------
    void filter (int f)
    {
        const Value* p = pp ("VoiceFilter" + std::to_string (f));
        const auto& id = I.filter[f];
        set (id.enable, on (p, "kParamEnable", false) ? 1.0 : 0.0);
        const std::string type = text (p, "kParamType");
        if (! type.empty())
        {
            bool exact = true;
            set (id.type, filterTypeFor (type, exact));
            if (! exact) report.note ("Filter " + std::to_string (f + 1) + ": type '" + type + "' approximated by " + choices::filterTypes()[(size_t) filterTypeFor (type, exact)]);
        }
        set (id.cutoff, cutoffHz (num (p, "kParamFreq", 1.0)));
        if (has (p, "kParamReso")) set (id.res, num (p, "kParamReso", 10.0) / 100.0);
        if (has (p, "kParamDrive")) set (id.drive, num (p, "kParamDrive", 0.0) / 100.0);
        if (has (p, "kParamVar")) set (id.var, num (p, "kParamVar", 0.0) / 100.0);
        if (has (p, "kParamWet")) set (id.mix, num (p, "kParamWet", 100.0) / 100.0);
        if (has (p, "kParamStereo")) set (id.pan, (num (p, "kParamStereo", 50.0) - 50.0) / 50.0);
        if (has (p, "kParamLevelOut")) set (id.level, levelDb (num (p, "kParamLevelOut", 0.5)));
        if (has (p, "kParamKeyTrack")) set (id.keytrack, on (p, "kParamKeyTrack", false) ? 1.0 : 0.0);

        // Filter outputs: slot 5 = Filter 1, 6 = Filter 2. Serum's default is Main.
        const Value* r = pp ("RoutingSlot" + std::to_string (5 + f));
        const std::string to = text (r, "kParamRoutingDest");
        if (to == "kRoutingDestFilter")      set (id.route, f == 0 ? 0 : 1);   // F1 -> F2 (serial); F2 -> F1 would loop
        else if (to == "kRoutingDestMaster") set (id.route, 1);
        else if (to == "kRoutingDestDirect") set (id.route, 2);
        else if (to == "kRoutingDestNone")   set (id.route, 3);
        if (has (r, "kParamFXBus1Level")) set (id.bus1, num (r, "kParamFXBus1Level", 0.0) / 100.0);
        if (has (r, "kParamFXBus2Level")) set (id.bus2, num (r, "kParamFXBus2Level", 0.0) / 100.0);
    }

    // ---- envelopes ---------------------------------------------------------
    void envelope (int e)
    {
        const Value* p = pp ("Env" + std::to_string (e));
        const auto& id = I.env[e];
        if (has (p, "kParamAttack"))  set (id.attack, num (p, "kParamAttack", 0.001));
        if (has (p, "kParamHold"))    set (id.hold, num (p, "kParamHold", 0.0));
        if (has (p, "kParamDecay"))   set (id.decay, num (p, "kParamDecay", 1.0));
        if (has (p, "kParamSustain")) set (id.sustain, sustainDb (num (p, "kParamSustain", 1.0)));
        if (has (p, "kParamRelease")) set (id.release, num (p, "kParamRelease", 0.015));
        if (has (p, "kParamCurve1"))  set (id.attackCurve, attackCurveFor (num (p, "kParamCurve1", 50.0)));
        if (has (p, "kParamCurve2"))  set (id.decayCurve, fallCurveFor (num (p, "kParamCurve2", 66.6)));
        if (has (p, "kParamCurve3"))  set (id.releaseCurve, fallCurveFor (num (p, "kParamCurve3", 66.6)));
    }

    // ---- LFOs --------------------------------------------------------------
    static bool readCurve (const Value& c, Curve& out)
    {
        const Value& xs = c["xVals"];
        const Value& ys = c["yVals"];
        const Value& cs = c["curveVals"];
        if (! xs.isArray() || ! ys.isArray() || xs.size() < 2) return false;
        std::vector<CurvePoint> pts;
        double lastX = -1.0;
        for (size_t i = 0; i < xs.size() && i < ys.size(); ++i)
        {
            CurvePoint pt;
            double x = clampd (xs[i].num (0.0), 0.0, 1.0);
            if (x < lastX) x = lastX;
            // Serum's y runs down the display (0 = top).
            pt.y = (float) clampd (1.0 - ys[i].num (0.0), 0.0, 1.0);
            // curveVals[i] bends the segment that starts at point i (0.5 = straight);
            // Tincture puts the bend on the point that ends the segment.
            if (i > 0) pt.curve = (float) clampd ((cs[i - 1].num (0.5) - 0.5) * 2.0, -1.0, 1.0);
            // Vertical steps: nudge so x stays strictly increasing.
            if (! pts.empty() && x <= lastX + 1.0e-5)
            {
                if (x >= 1.0) { pts.back().y = pt.y; break; }
                x = std::min (1.0, lastX + 1.0e-4);
            }
            pt.x = (float) x;
            pts.push_back (pt);
            lastX = x;
            if (x >= 1.0) break;            // points after the end are padding
        }
        if (pts.size() < 2) return false;
        pts.front().x = 0.0f;
        if (pts.back().x < 1.0f) { CurvePoint end = pts.back(); end.x = 1.0f; end.curve = 0.0f; pts.push_back (end); }
        out.points = std::move (pts);
        return true;
    }

    void lfo (int l)
    {
        const Value& m = module ("LFO" + std::to_string (l));
        const Value* p = plain (m);
        const auto& id = I.lfo[l];

        const std::string mode = text (p, "kParamMode");
        if (mode == "Free") set (id.mode, 0);
        else if (mode == "Envelope") set (id.mode, 2);
        else if (mode == "Retrig" || mode == "Trig") set (id.mode, 1);
        else if (! mode.empty()) report.note ("LFO " + std::to_string (l + 1) + ": mode '" + mode + "' not recognised");

        const std::string type = text (p, "kParamType");
        const bool chaos = type == "Lorenz" || type == "Rossler";
        if (type == "Lorenz") set (id.type, 2);
        else if (type == "Rossler") set (id.type, 3);
        else if (type == "SH" || type == "S&H" || type == "SampleAndHold") set (id.type, 4);
        else if (type == "Path") { set (id.type, 0); report.note ("LFO " + std::to_string (l + 1) + ": path LFO converted to a normal shape"); }
        else if (! type.empty() && type != "Normal") report.note ("LFO " + std::to_string (l + 1) + ": type '" + type + "' not recognised");

        // Rate: both modes store 100 * knob^4 (measured); in Hz mode that is the rate in Hz.
        const bool bpm = on (p, "kParamBeatSync", true);
        lfoBpm[l] = bpm;
        set (id.bpm, bpm ? 1.0 : 0.0);
        const double rate = num (p, "kParamRate", 6.25);
        if (bpm) set (id.rateSync, syncRateForKnob (rate));
        else
        {
            const bool x10 = rate > 20.0;
            set (id.x10, x10 ? 1.0 : 0.0);
            set (id.rate, x10 ? rate / 10.0 : rate);
        }
        if (! chaos && bpm)
        {
            const bool trip = on (p, "kParamTriplets", false), dot = on (p, "kParamDotted", false);
            if (trip && ! dot) set (id.trip, 1.0);
            if (dot && ! trip) set (id.dot, 1.0);
        }
        if (has (p, "kParamRise"))   set (id.rise, num (p, "kParamRise", 0.0));
        if (has (p, "kParamDelay"))  set (id.delay, num (p, "kParamDelay", 0.0));
        if (has (p, "kParamSmooth")) set (id.smooth, num (p, "kParamSmooth", 0.0) / 100.0);
        if (has (p, "kParamPhase"))  set (id.phase, std::fmod (num (p, "kParamPhase", 0.0), 360.0));
        if (has (p, "kParamAnchored")) set (id.host, on (p, "kParamAnchored", false) ? 1.0 : 0.0);

        Curve c;
        if (readCurve (m["curveData"], c))
        {
            patch.lfo[(size_t) l].shape = c;
            patch.lfo[(size_t) l].shapeName = "Serum";
        }
    }

    // ---- macros, global ----------------------------------------------------
    void macros()
    {
        for (int i = 0; i < kNumMacros; ++i)
        {
            const Value& m = module ("Macro" + std::to_string (i));
            const Value* p = plain (m);
            if (has (p, "kParamValue")) set (I.macro[i], num (p, "kParamValue", 0.0) / 100.0);
            const std::string name = m["name"].str();
            const std::string def = "Macro " + std::to_string (i + 1);
            if (! name.empty() && name != def) patch.macroNames[(size_t) i] = name;
        }
    }

    void global()
    {
        const Value* p = pp ("Global0");
        const auto& g = I.global;
        set (g.master, masterDb (num (p, "kParamMasterVolume", 0.7)));
        if (has (p, "kParamPolyCount")) set (g.poly, num (p, "kParamPolyCount", 8.0));
        if (has (p, "kParamMonoToggle")) set (g.mono, on (p, "kParamMonoToggle", false) ? 1.0 : 0.0);
        if (has (p, "kParamLegato")) set (g.legato, on (p, "kParamLegato", false) ? 1.0 : 0.0);
        if (has (p, "kParamPortamentoTime")) set (g.porta, num (p, "kParamPortamentoTime", 0.0));
        if (has (p, "kParamPortamentoCurve")) set (g.portaCurve, num (p, "kParamPortamentoCurve", 0.0) / 100.0);
        if (has (p, "kParamPortaAlways")) set (g.portaAlways, on (p, "kParamPortaAlways", false) ? 1.0 : 0.0);
        if (has (p, "kParamPortaScaled")) set (g.portaScaled, on (p, "kParamPortaScaled", false) ? 1.0 : 0.0);
        if (has (p, "kParamBendRangeUp")) set (g.bendUp, num (p, "kParamBendRangeUp", 2.0));
        if (has (p, "kParamBendRangeDn")) set (g.bendDown, num (p, "kParamBendRangeDn", -2.0));
        if (has (p, "kParamMasterTuning")) set (g.tuning, num (p, "kParamMasterTuning", 0.0));
        if (has (p, "kParamTranspose")) set (g.transpose, num (p, "kParamTranspose", 0.0));
        patch.limitSameNote = on (p, "kParamLimitSameNotePolyphony", false);
        if (has (p, "kParamModWheel")) report.note ("stored mod wheel position not imported");
    }

    // ---- FX ------------------------------------------------------------------
    struct FxSlotMap { const char* key; int slot; double (*conv) (double); };

    FxModuleSlot* lastModule (int rack)
    {
        auto& mods = patch.fx.racks[(size_t) rack].modules;
        return mods.empty() ? nullptr : &mods.back();
    }

    // Serum's synced delay knob (229 steps) -> index into choices::syncRates() (measured runs).
    static int delaySyncIndex (double knob)
    {
        const int step = (int) std::lround (clampd (knob, 0.0, 1.0) * 228.0);
        static const int lastStep[] { 10, 31, 51, 72, 93, 114, 134, 155, 176, 196, 217, 228 };
        static const int index[]    { 11, 11, 10,  9,  8,   7,   6,   5,   4,   3,   2,   1 };
        //                           fast 1/256 1/128 1/64 1/32 1/16 1/8 1/4 1/2 bar 2bar 4bar
        for (int i = 0; i < 12; ++i)
            if (step <= lastStep[i]) return index[i];
        return 1;
    }

    static double pct (double x)     { return x / 100.0; }
    static double same (double x)    { return x; }
    static double bip50 (double x)   { return (x - 50.0) / 50.0; }
    static double bip100 (double x)  { return x / 100.0; }

    // Writes a plain value into an FX slot through its own mapping.
    void fxPlain (int unit, FxType t, int slot, double plainValue)
    {
        if (unit < 0) return;
        v[I.fx[unit].p[slot]] = fxInfo (t).params[(size_t) slot].toNorm ((float) plainValue);
    }
    void fxNorm (int unit, int slot, double n)
    {
        if (unit >= 0) v[I.fx[unit].p[slot]] = (float) clampd (n, 0.0, 1.0);
    }
    void fxMix (int unit, double m)
    {
        if (unit >= 0) v[I.fx[unit].mix] = (float) clampd (m, 0.0, 1.0);
    }

    static bool fxTypeFor (const std::string& key, FxType& t)
    {
        static const std::pair<const char*, FxType> map[] {
            { "FXReverb", FxType::Reverb }, { "FXDelay", FxType::Delay }, { "FXChorus", FxType::Chorus },
            { "FXComp", FxType::Compressor }, { "FXEQ", FxType::Equalizer }, { "FXHyperD", FxType::Hyper },
            { "FXDistortion", FxType::Distortion }, { "FXFilter", FxType::Filter }, { "FXFlanger", FxType::Flanger },
            { "FXPhaser", FxType::Phaser }, { "FXConv", FxType::Convolve }, { "FXBode", FxType::Bode },
            { "FXUtility", FxType::Utility },
        };
        for (const auto& [k, type] : map)
            if (key == k) { t = type; return true; }
        return false;
    }

    // Serum's default WET for each module (Serum 1 values, which Serum 2 kept).
    static double defaultWet (FxType t)
    {
        switch (t)
        {
            case FxType::Reverb:    return 33.0;
            case FxType::Delay:     return 30.0;
            case FxType::Chorus:    return 50.0;
            case FxType::Hyper:     return 50.0;
            case FxType::Compressor:return 100.0;
            case FxType::Convolve:  return 30.0;
            case FxType::Bode: case FxType::Distortion: case FxType::Equalizer: case FxType::Filter:
            case FxType::Flanger: case FxType::Phaser: case FxType::SplitterLH: case FxType::SplitterLMH:
            case FxType::SplitterMS: case FxType::Utility:
                return 100.0;
        }
        return 100.0;
    }

    void fxModule (int rack, int index, const std::string& key, const Value& fx)
    {
        FxType t;
        if (! fxTypeFor (key, t))
        {
            report.note ("FX module '" + key.substr (key.rfind ("FX", 0) == 0 ? 2 : 0) + "' has no Tincture equivalent");
            ++report.fxDropped;
            fxRefs.push_back ({ key, rack, index, -1, FxType::Utility });
            return;
        }
        const int unit = patch.fx.add (rack, t);
        fxRefs.push_back ({ key, rack, index, unit, t });
        if (unit < 0) { ++report.fxDropped; report.note ("too many FX modules; some were left out"); return; }
        ++report.fxModules;
        const auto d = fxDefaults (t);
        for (int k = 0; k < kFxParams; ++k) v[I.fx[unit].p[k]] = d[(size_t) k];

        const Value* p = plain (fx);
        fxMix (unit, num (p, "kParamWet", defaultWet (t)) / 100.0);
        if (has (p, "kParamLevelOut")) fxNorm (unit, kFxLevelSlot, num (p, "kParamLevelOut", 0.5));

        auto apply = [&] (std::initializer_list<FxSlotMap> list)
        {
            for (const auto& e : list)
                if (has (p, e.key)) fxPlain (unit, t, e.slot, e.conv (num (p, e.key, 0.0)));
        };

        switch (t)
        {
            case FxType::Reverb:
            {
                const std::string type = text (p, "kParamType");
                static const std::pair<const char*, int> types[] { { "kPlate", 0 }, { "kHall", 1 }, { "kVintage", 2 }, { "kNitrous", 3 }, { "kBasin", 4 } };
                int variant = 0;                // Serum's default reverb is the plate
                for (const auto& [n, i] : types)
                    if (type == n) variant = i;
                if (! type.empty() && variant == 0 && type != "kPlate") report.note ("reverb type '" + type + "' not recognised");
                fxPlain (unit, t, 0, variant);
                // Serum: LOW CUT / HIGH CUT (FreqB / Freq on the hall & vintage), SIZE, PRE-DELAY (ms).
                apply ({ { "kParamFreq", 2, pct }, { "kParamFreqB", 1, pct }, { "kParamSize", 3, pct },
                         { "kParamDelay", 4, same }, { "kParamFeedback", 8, pct } });
                if (has (p, "kParamPreDelay")) fxPlain (unit, t, 4, num (p, "kParamPreDelay", 0.0) * 1000.0);
                if (has (p, "kParamWidth"))
                {
                    if (variant == 0) fxPlain (unit, t, 7, num (p, "kParamWidth", 100.0) / 100.0);
                    else fxPlain (unit, t, 10, num (p, "kParamWidth", 20.0) / 100.0);   // spin / chorus depth
                }
                if (has (p, "kParamDamp")) fxPlain (unit, t, 6, num (p, "kParamDamp", 40.0) / 100.0);
                break;
            }
            case FxType::Delay:
            {
                // Serum's MODE menu: 0 Normal, 1 Ping-Pong, 2 Tap->Delay.
                if (has (p, "kParamMode")) fxPlain (unit, t, 0, std::round (num (p, "kParamMode", 0.0)));
                // BPM sync is Serum's default. The times are stored as knob^4 (the same
                // law as the LFO rate); synced, the knob picks a note value.
                const bool sync = on (p, "kParamBeatSync", true);
                fxPlain (unit, t, 2, sync ? 1.0 : 0.0);
                fxPlain (unit, t, 3, on (p, "kParamLink", false) ? 1.0 : 0.0);
                for (const auto& [timeKey, timeSlot] : { std::pair<const char*, int> { "kParamTimeL", 4 }, { "kParamTimeR", 6 } })
                {
                    if (! has (p, timeKey)) continue;
                    const double knob = std::pow (std::max (num (p, timeKey, 0.0), 0.0), 0.25);
                    if (sync) fxNorm (unit, timeSlot, (11.0 - delaySyncIndex (knob)) / 11.0);
                    else fxPlain (unit, t, timeSlot, 1.0 + 500.0 * std::pow (knob, 4.0));     // 1..501 ms (measured)
                }
                apply ({ { "kParamOffsetL", 5, same }, { "kParamOffsetR", 7, same }, { "kParamFeedback", 8, pct },
                         { "kParamFreq", 9, same } });
                // BW: 0.8..8.2 octaves in Serum (measured), the filter width here.
                if (has (p, "kParamBW")) fxNorm (unit, 10, (num (p, "kParamBW", 6.8) - 0.8) / 7.4);
                break;
            }
            case FxType::Chorus:
                if (has (p, "kParamBeatSync")) fxPlain (unit, t, 1, on (p, "kParamBeatSync", false) ? 1.0 : 0.0);
                apply ({ { "kParamRate", 0, same }, { "kParamDelay", 2, same }, { "kParamDelay2", 3, same },
                         { "kParamFeedback", 5, pct }, { "kParamFilt", 6, same } });
                // DEPTH is in ms (0..~5 ms on the knob, measured).
                if (has (p, "kParamDepth")) fxNorm (unit, 4, num (p, "kParamDepth", 1.0) / 5.0);
                break;
            case FxType::Compressor:
            {
                if (has (p, "kParamMultiband")) fxPlain (unit, t, 0, on (p, "kParamMultiband", false) ? 1.0 : 0.0);
                // THRESHOLD is stored as a linear amplitude.
                if (has (p, "kParamThresh"))
                {
                    const double th = num (p, "kParamThresh", 0.125);
                    fxPlain (unit, t, 1, th <= 1.0e-6 ? -120.0 : std::max (-120.0, std::min (0.0, 20.0 * std::log10 (th))));
                }
                apply ({ { "kParamRatio", 2, same }, { "kParamAttack", 3, same }, { "kParamRelease", 4, same },
                         { "kParamMakeup", 5, same } });
                if (has (p, "kParamThreshUD0") || has (p, "kParamThreshUD1") || has (p, "kParamThreshUD2"))
                    report.note ("Compressor: multiband up/down thresholds not imported");
                break;
            }
            case FxType::Equalizer:
            {
                apply ({ { "kParamFreq1", 1, same }, { "kParamGain1", 3, same }, { "kParamFreq2", 5, same }, { "kParamGain2", 7, same } });
                // Q knobs: the same 0..100 % knob as Serum (60 % = 0.707).
                if (has (p, "kParamReso1")) fxNorm (unit, 2, num (p, "kParamReso1", 60.0) / 100.0);
                if (has (p, "kParamReso2")) fxNorm (unit, 6, num (p, "kParamReso2", 60.0) / 100.0);
                // TYPE: 0 Shelf, 1 Peak, 2 Pass.
                if (has (p, "kParamType1")) fxPlain (unit, t, 0, std::round (num (p, "kParamType1", 0.0)));
                if (has (p, "kParamType2")) fxPlain (unit, t, 4, std::round (num (p, "kParamType2", 0.0)));
                break;
            }
            case FxType::Distortion:
            {
                const std::string mode = text (p, "kParamMode");
                if (! mode.empty())
                {
                    static const std::pair<const char*, int> modes[] {
                        { "kTube", 0 }, { "kSoftClip", 1 }, { "kHardClip", 2 }, { "kDiode1", 3 }, { "kDiode2", 4 },
                        { "kLinFold", 5 }, { "kSinFold", 6 }, { "kSineFold", 6 }, { "kZeroSquare", 7 }, { "kDownsample", 8 },
                        { "kAsym", 9 }, { "kRectify", 10 }, { "kXShaper", 11 }, { "kXShaperAsym", 12 },
                        // Serum 2 additions, nearest here:
                        { "kOverdrive", 0 }, { "kStompBox", 0 }, { "kTapeSat", 1 }, { "kSineShaper", 6 }, { "kSoftSat", 1 },
                    };
                    int idx = -1;
                    for (const auto& [n, i] : modes)
                        if (mode == n) idx = i;
                    if (idx < 0) { idx = 0; report.note ("Distortion: mode '" + mode + "' approximated by Tube"); }
                    else if (mode == "kOverdrive" || mode == "kStompBox" || mode == "kTapeSat" || mode == "kSineShaper" || mode == "kSoftSat")
                        report.note ("Distortion: mode '" + mode.substr (1) + "' approximated");
                    fxPlain (unit, t, 0, idx);
                }
                if (has (p, "kParamPrePost")) fxPlain (unit, t, 1, std::round (num (p, "kParamPrePost", 0.0)));
                if (has (p, "kParamLPHP")) fxPlain (unit, t, 2, num (p, "kParamLPHP", 0.0) / 100.0);
                if (has (p, "kParamFreq")) fxPlain (unit, t, 3, cutoffHz (num (p, "kParamFreq", 0.5)));
                // BW (0..~1.2 stored): narrower = higher Q.
                if (has (p, "kParamBW")) fxNorm (unit, 4, 1.0 - clampd (num (p, "kParamBW", 0.5), 0.0, 1.0));
                apply ({ { "kParamDrive", 5, pct } });
                break;
            }
            case FxType::Filter:
            {
                const std::string type = text (p, "kParamType");
                if (! type.empty())
                {
                    bool exact = true;
                    const int ft = fxFilterTypeFor (type, exact);
                    fxPlain (unit, t, 0, ft);
                    if (! exact)
                    {
                        // A stand-in filter type can silence the sound (a formant filter at
                        // 0 Hz is a vowel; a low-pass there is silence): keep it, bypassed.
                        if (FxModuleSlot* slotPtr = lastModule (rack)) slotPtr->bypass = true;
                        report.note ("FX Filter: type '" + type + "' has no exact match (module added, bypassed)");
                    }
                }
                if (has (p, "kParamFreq")) fxPlain (unit, t, 1, cutoffHz (num (p, "kParamFreq", 0.5)));
                apply ({ { "kParamReso", 2, pct }, { "kParamDrive", 3, pct }, { "kParamVar", 4, pct } });
                if (has (p, "kParamStereo")) fxPlain (unit, t, 5, (num (p, "kParamStereo", 50.0) - 50.0) / 50.0);
                break;
            }
            case FxType::Flanger:
            case FxType::Phaser:
            {
                const bool sync = on (p, "kParamBeatSync", false);
                fxPlain (unit, t, 1, sync ? 1.0 : 0.0);
                if (! sync && has (p, "kParamRate")) fxPlain (unit, t, 0, num (p, "kParamRate", 0.5));
                // Synced: stored as 20 * knob^4 like the free rate; the knob picks the note value.
                if (sync && has (p, "kParamRate")) fxNorm (unit, 0, std::pow (std::max (num (p, "kParamRate", 0.0), 0.0) / 20.0, 0.25));
                if (t == FxType::Flanger)
                    apply ({ { "kParamDepth", 2, pct }, { "kParamFeedback", 3, pct } });
                else
                {
                    apply ({ { "kParamDepth", 3, pct }, { "kParamFreq", 5, same }, { "kParamFeedback", 6, pct } });
                    if (has (p, "kParamDepth2")) fxNorm (unit, 4, num (p, "kParamDepth2", 0.0));
                    if (has (p, "kParamWidth")) fxNorm (unit, 7, num (p, "kParamWidth", 0.0) / 360.0);
                }
                break;
            }
            case FxType::Hyper:
                // RATE is the same 0..100 % knob as Tincture's.
                if (has (p, "kParamRate")) fxNorm (unit, 0, num (p, "kParamRate", 40.0) / 100.0);
                if (has (p, "kParamUnison")) fxPlain (unit, t, 1, std::round (num (p, "kParamUnison", 4.0)));
                apply ({ { "kParamDetune", 2, pct }, { "kParamDimESize", 4, pct }, { "kParamDimEWet", 5, pct } });
                if (has (p, "kParamRetrig")) fxPlain (unit, t, 3, on (p, "kParamRetrig", false) ? 1.0 : 0.0);
                break;
            case FxType::Bode:
            {
                if (has (p, "kParamShift"))
                {
                    const double sh = num (p, "kParamShift", 0.0);
                    fxPlain (unit, t, 0, std::abs (sh) / 100.0);
                    fxPlain (unit, t, 2, sh < 0.0 ? -1.0 : 1.0);
                }
                if (has (p, "kParamDelayTime")) fxPlain (unit, t, 4, num (p, "kParamDelayTime", 0.0) * 1000.0);
                apply ({ { "kParamFeedback", 6, pct }, { "kParamDelayBalance", 7, bip50 }, { "kParamBlur", 8, pct } });
                if (has (p, "kParamRange")) report.note ("Bode: range not imported");
                break;
            }
            case FxType::Convolve:
            {
                apply ({ { "kParamTone", 1, bip100 }, { "kParamDamping", 7, pct } });
                const std::string ir = fx["relativePathToIR"].str();
                if (! ir.empty()) report.note ("Convolve: impulse '" + baseName (ir) + "' replaced by Tincture's default IR");
                break;
            }
            case FxType::Utility:
            case FxType::SplitterLH:
            case FxType::SplitterLMH:
            case FxType::SplitterMS:
                break;
        }
    }

    void fxRacks()
    {
        for (int r = 0; r < kNumFxRacks; ++r)
        {
            const Value& rack = module ("FXRack" + std::to_string (r));
            const Value& list = rack["FX"];
            for (size_t i = 0; i < list.size(); ++i)
            {
                const Value& entry = list[i];
                std::string key;
                for (const auto& e : entry.entries)
                    if (startsWith (e.first, "FX") && e.second.isMap()) key = e.first;
                if (key.empty()) continue;
                fxModule (r, (int) i, key, entry[key]);
            }
            const std::string dn = rack["displayName"].str();
            if (! dn.empty()) patch.fx.racks[(size_t) r].presetName = dn;
        }
    }

    // ---- mod matrix ----------------------------------------------------------
    static ModSource source (int sid)
    {
        if (sid == 1) return ModSource::ModWheel;
        if (sid >= 2 && sid <= 5) return envSource (sid - 2);
        if (sid >= 6 && sid <= 15) return lfoSource (sid - 6);
        if (sid >= 25 && sid <= 32) return macroSource (sid - 25);
        switch (sid)
        {
            case 16: return ModSource::Velocity;
            case 17: return ModSource::Note;
            case 18: return ModSource::Aftertouch;
            case 19: return ModSource::PolyAftertouch;
            case 20: return ModSource::Noise;
            case 21: return ModSource::NoteOnRand1;
            case 22: return ModSource::NoteOnRand2;
            case 23: return ModSource::NoteOnAlt1;
            case 24: return ModSource::NoteOnAlt2;
            case 33: return ModSource::PitchBend;
            case 34: return ModSource::ExprX;
            case 35: return ModSource::ExprY;
            case 36: return ModSource::ExprZ;
            case 37: return ModSource::ReleaseVelocity;
            case 38: return ModSource::Fixed;
            case 49: return ModSource::OscA;
            case 50: return ModSource::OscB;
            case 51: return ModSource::OscC;
            case 52: return ModSource::Sub;
            case 53: return ModSource::Filter1;
            case 54: return ModSource::Filter2;
            case 55: return ModSource::ActiveVoices;
            case 56: return ModSource::VoiceMod1;
            case 57: return ModSource::VoiceMod2;
            case 58: return ModSource::VoiceIndex;
            case 59: return ModSource::NoteOnRandDiscrete;
            default: return ModSource::None;
        }
    }

    // A destination and how a Serum amount scales onto it.
    struct Dest { int param = -1; double scale = 1.0; };

    Dest fxDest (const std::string& type, int moduleId, const std::string& name)
    {
        // destModuleID counts modules within their rack; match by type too.
        const FxRef* ref = nullptr;
        for (const auto& f : fxRefs)
            if (f.key == type && f.index == moduleId) { ref = &f; break; }
        if (ref == nullptr)
            for (const auto& f : fxRefs)
                if (f.key == type) { ref = &f; break; }
        if (ref == nullptr || ref->unit < 0) return {};
        const auto& u = I.fx[ref->unit];
        if (name == "kParamWet") return { u.mix, 1.0 };
        if (name == "kParamLevelOut") return { u.p[kFxLevelSlot], 1.0 };
        static const std::map<std::string, std::map<std::string, int>> slots {
            { "FXReverb", { { "kParamSize", 3 }, { "kParamDelay", 4 }, { "kParamFreq", 2 }, { "kParamFreqB", 1 }, { "kParamFeedback", 8 } } },
            { "FXDelay", { { "kParamFeedback", 8 }, { "kParamFreq", 9 }, { "kParamBW", 10 }, { "kParamTimeL", 4 }, { "kParamTimeR", 6 } } },
            { "FXChorus", { { "kParamRate", 0 }, { "kParamDepth", 4 }, { "kParamFeedback", 5 }, { "kParamFilt", 6 } } },
            { "FXComp", { { "kParamThresh", 1 }, { "kParamRatio", 2 }, { "kParamMakeup", 5 } } },
            { "FXEQ", { { "kParamFreq1", 1 }, { "kParamReso1", 2 }, { "kParamGain1", 3 }, { "kParamFreq2", 5 }, { "kParamReso2", 6 }, { "kParamGain2", 7 } } },
            { "FXDistortion", { { "kParamDrive", 5 }, { "kParamFreq", 3 }, { "kParamBW", 4 } } },
            { "FXFilter", { { "kParamFreq", 1 }, { "kParamReso", 2 }, { "kParamDrive", 3 }, { "kParamVar", 4 } } },
            { "FXFlanger", { { "kParamRate", 0 }, { "kParamDepth", 2 }, { "kParamFeedback", 3 } } },
            { "FXPhaser", { { "kParamRate", 0 }, { "kParamDepth", 3 }, { "kParamFreq", 5 }, { "kParamFeedback", 6 } } },
            { "FXHyperD", { { "kParamRate", 0 }, { "kParamDetune", 2 }, { "kParamDimESize", 4 }, { "kParamDimEWet", 5 } } },
            { "FXBode", { { "kParamShift", 0 }, { "kParamFeedback", 6 } } },
            { "FXConv", { { "kParamTone", 1 } } },
        };
        if (const auto t = slots.find (type); t != slots.end())
            if (const auto s2 = t->second.find (name); s2 != t->second.end())
                return { u.p[s2->second], 1.0 };
        return {};
    }

    Dest dest (const std::string& type, int mid, const std::string& name)
    {
        auto pick = [] (std::initializer_list<std::pair<const char*, Dest>> l, const std::string& n) -> Dest
        {
            for (const auto& e : l)
                if (n == e.first) return e.second;
            return {};
        };
        if (type == "Oscillator")
        {
            if (mid >= 0 && mid < kNumOscs)
            {
                const auto& o = I.osc[mid];
                // Serum COARSE spans +/-64 st, Tincture +/-48.
                return pick ({ { "kParamVolume", { o.level } }, { "kParamPan", { o.pan } }, { "kParamOctave", { o.oct } },
                               { "kParamPitch", { o.sem } }, { "kParamFine", { o.fine } }, { "kParamCoarsePit", { o.coarse, 64.0 / 48.0 } },
                               { "kParamDetune", { o.detune } }, { "kParamDetuneWid", { o.uniWidth } }, { "kParamUnisonBlend", { o.blend } } }, name);
            }
            if (mid == 3) return pick ({ { "kParamVolume", { I.noise.level } }, { "kParamPan", { I.noise.pan } } }, name);
            if (mid == 4) return pick ({ { "kParamVolume", { I.sub.level } }, { "kParamPan", { I.sub.pan } },
                                        { "kParamOctave", { I.sub.oct } }, { "kParamCoarsePit", { I.sub.coarse, 64.0 / 48.0 } } }, name);
            return {};
        }
        if (type == "WTOsc" && mid >= 0 && mid < kNumOscs)
        {
            const auto& o = I.osc[mid];
            return pick ({ { "kParamTablePos", { o.wtpos } }, { "kParamWarp", { o.warp1 } }, { "kParamWarp2", { o.warp2 } },
                           { "kParamRandomPhase", { o.rand } }, { "kParamInitialPhase", { o.phase } } }, name);
        }
        if ((type == "SampleOsc" || type == "GranularOsc" || type == "SpectralOsc") && mid >= 0 && mid < kNumOscs)
        {
            const auto& o = I.osc[mid];
            return pick ({ { "kParamStart", { o.start } }, { "kParamScan", { o.scan } }, { "kParamDensity", { o.density } } }, name);
        }
        if (type == "NoiseOsc")
            return pick ({ { "kParamColor", { I.noise.tilt } }, { "kParamPitch", { I.noise.pitch } } }, name);
        if (type == "VoiceFilter" && mid >= 0 && mid < kNumFilters)
        {
            const auto& f = I.filter[mid];
            return pick ({ { "kParamFreq", { f.cutoff } }, { "kParamReso", { f.res } }, { "kParamDrive", { f.drive } },
                           { "kParamVar", { f.var } }, { "kParamWet", { f.mix } }, { "kParamLevelOut", { f.level } },
                           { "kParamStereo", { f.pan } } }, name);
        }
        if (type == "Env" && mid >= 0 && mid < kNumEnvs)
        {
            const auto& e = I.env[mid];
            return pick ({ { "kParamAttack", { e.attack } }, { "kParamHold", { e.hold } }, { "kParamDecay", { e.decay } },
                           { "kParamSustain", { e.sustain } }, { "kParamRelease", { e.release } } }, name);
        }
        if (type == "LFO" && mid >= 0 && mid < kNumLfos)
        {
            const auto& l = I.lfo[mid];
            if (name == "kParamRate") return { lfoBpm[mid] ? l.rateSync : l.rate };
            return pick ({ { "kParamSmooth", { l.smooth } }, { "kParamRise", { l.rise } }, { "kParamDelay", { l.delay } },
                           { "kParamPhase", { l.phase } } }, name);
        }
        if (type == "Macro" && mid >= 0 && mid < kNumMacros && name == "kParamValue") return { I.macro[mid] };
        if (type == "Global")
            return pick ({ { "kParamMasterVolume", { I.global.master } }, { "kParamMasterTuning", { I.global.tuning } },
                           { "kParamVoiceAmp", { I.global.amp } }, { "kParamPortamentoTime", { I.global.porta } } }, name);
        if (type == "RoutingSlot")
        {
            int b1 = -1, b2 = -1;
            if (mid >= 0 && mid < kNumOscs) { b1 = I.osc[mid].bus1; b2 = I.osc[mid].bus2; }
            else if (mid == 3) { b1 = I.noise.bus1; b2 = I.noise.bus2; }
            else if (mid == 4) { b1 = I.sub.bus1; b2 = I.sub.bus2; }
            else if (mid == 5 || mid == 6) { b1 = I.filter[mid - 5].bus1; b2 = I.filter[mid - 5].bus2; }
            if (name == "kParamFXBus1Level") return { b1 };
            if (name == "kParamFXBus2Level") return { b2 };
            return {};
        }
        if (startsWith (type, "FX")) return fxDest (type, mid, name);
        return {};
    }

    void matrix()
    {
        // Slots in numeric order (the map is sorted by text: ModSlot0, ModSlot1, ModSlot10...).
        for (int slot = 0; slot < 256; ++slot)
        {
            const Value* m = s.find ("ModSlot" + std::to_string (slot));
            if (m == nullptr)
            {
                if (slot >= 64) break;
                continue;
            }
            const Value& src = (*m)["source"];
            if (! src.isArray() || src.size() == 0) continue;
            const int sid = (int) src[0].num (0.0);
            const int aux = src.size() > 1 ? (int) src[1].num (0.0) : 0;
            const Value* p = plain (*m);
            const double amount = num (p, "kParamAmount", 0.0);
            if (sid <= 0) continue;

            const std::string type = (*m)["destModuleTypeString"].str();
            const std::string pname = (*m)["destModuleParamName"].str();
            const int mid = (int) (*m)["destModuleID"].num (0.0);
            const ModSource ms = source (sid);
            if (ms == ModSource::None)
            {
                ++report.modRowsDropped;
                report.note ("mod source #" + std::to_string (sid) + " not recognised");
                continue;
            }
            if (type.empty() && pname.empty()) continue;            // a source with no destination
            const Dest d = dest (type, mid, pname);
            if (d.param < 0 || (params()[d.param].flags & kModulatable) == 0)
            {
                ++report.modRowsDropped;
                report.note ("mod destination " + type + " " + (pname.size() > 6 ? pname.substr (6) : pname) + " not available");
                continue;
            }
            ModRow r;
            r.source = ms;
            r.dest = d.param;
            r.amount = (float) clampd (amount / 100.0 * d.scale, -1.0, 1.0);
            r.bipolar = on (p, "kParamBipolar", false);
            r.output = (float) clampd (num (p, "kParamOut", 100.0) / 100.0, 0.0, 1.0);
            if (aux > 0)
            {
                r.aux = source (aux);
                if (r.aux == ModSource::None) report.note ("aux source #" + std::to_string (aux) + " not recognised");
            }
            if (has (p, "kParamCurveIn") || has (p, "kParamMainCurveData"))
                report.note ("mod matrix curves not imported (routings are linear)");
            if (patch.matrix.add (r) >= 0) ++report.modRows;
            else ++report.modRowsDropped;
        }
    }

    void info (const Value& header)
    {
        auto& in = patch.info;
        const std::string name = header["presetName"].str();
        in.name = name.empty() ? std::string ("Serum Import") : name;
        in.author = header["presetAuthor"].str();
        in.description = header["presetDescription"].str();
        in.tags.clear();
        const Value& tags = header["tags"];
        for (size_t i = 0; i < tags.size(); ++i)
            if (tags[i].isString()) in.tags.push_back (tags[i].text);
        // Category from the usual name prefix ("BS - Atom").
        static const std::pair<const char*, const char*> prefixes[] {
            { "BS", "Bass" }, { "LD", "Lead" }, { "PL", "Pluck" }, { "PD", "Pad" }, { "SY", "Synth" },
            { "FX", "FX" }, { "KY", "Keys" }, { "AR", "Arp" }, { "SQ", "Sequence" }, { "DR", "Drum" }, { "VX", "Vocal" },
        };
        for (const auto& [pre, cat] : prefixes)
            if (startsWith (in.name, pre) && in.name.size() > 3 && (in.name[2] == ' ' || in.name[2] == '_'))
                in.category = cat;
        report.presetName = in.name;
    }

    void run (const Value& header)
    {
        info (header);
        serumInit();
        for (int o = 0; o < kNumOscs; ++o) oscillator (o);
        noise();
        sub();
        for (int f = 0; f < kNumFilters; ++f) filter (f);
        for (int e = 0; e < kNumEnvs; ++e) envelope (e);
        for (int l = 0; l < kNumLfos; ++l) lfo (l);
        macros();
        global();
        fxRacks();
        matrix();

        if (plain (module ("Arp0")) != nullptr) report.note ("arpeggiator settings not imported");
        for (int c = 0; c < 12; ++c)
        {
            const Value& clip = module ("MidiClip" + std::to_string (c))["clip"];
            if (clip.size() > 0) { report.note ("MIDI clips not imported"); break; }
        }
        if (plain (module ("PitchQuantizer0")) != nullptr) report.note ("pitch quantiser not imported");
    }
};
} // namespace

// ---------------------------------------------------------------------------
// Mapping helpers
// ---------------------------------------------------------------------------
int filterTypeFor (const std::string& raw, bool& exact)
{
    exact = true;
    const auto& list = choices::filterTypes();
    auto idx = [&] (const std::string& n) { const int i = indexOf (list, n); return i < 0 ? 1 : i; };
    const std::string k = squash (raw);

    static const std::map<std::string, const char*> special {
        { "combs", "Combs" }, { "allpasses", "Allpasses" }, { "reverb", "Reverb" }, { "reverb1", "Reverb" },
        { "diffuser", "Diffusor" }, { "diffusor", "Diffusor" }, { "bandreject", "Bandreject" }, { "addbass", "Add Bass" },
        { "french", "French LP" }, { "frenchlp", "French LP" }, { "german", "German LP" }, { "germanlp", "German LP" },
        { "scream", "Scream LP" }, { "screamlp", "Scream LP" }, { "screambp", "Scream BP" }, { "rm", "Ring Mod" },
        { "ringmod", "Ring Mod" }, { "rmx2", "Ring Modx2" }, { "rmt", "Ring Modx2" }, { "samphold", "SampHold" },
        { "snh1", "SampHold" }, { "snh", "SampHold" }, { "sampholdn", "SampHold-" }, { "snh2", "SampHold-" },
        { "snhn", "SampHold-" }, { "wsp", "Wsp" }, { "djmixer", "DJ Mixer" }, { "laddermg", "MG Ladder" },
        { "ladderacid", "Acid Ladder" }, { "ladderems", "EMS Ladder" }, { "dirtymg", "MG Dirty" }, { "pzsvf", "PZ SVF" },
        { "comb2", "Comb 2" }, { "exp", "Exp MM" }, { "expmm", "Exp MM" }, { "expbpf", "Exp BPF" },
        { "formantone", "Formant-I" }, { "formanttwo", "Formant-II" }, { "formantthree", "Formant-III" },
        { "formant1", "Formant-I" }, { "formant2", "Formant-II" }, { "formant3", "Formant-III" },
        { "distcomb1lp", "Dist.Comb 1 LP" }, { "distcomb1bp", "Dist.Comb 1 BP" }, { "distcomb2lp", "Dist.Comb 2 LP" },
        { "distcomb2bp", "Dist.Comb 2 BP" }, { "leq6", "Low EQ 6" }, { "leq12", "Low EQ 12" }, { "beq12", "Band EQ 12" },
        { "heq6", "High EQ 6" }, { "heq12", "High EQ 12" },
    };
    if (const auto it = special.find (k); it != special.end())
    {
        if (k == "scream3lp" || k == "rmt" || k == "snhn") exact = false;
        return idx (it->second);
    }
    if (k == "scream3lp") { exact = false; return idx ("Scream LP"); }

    // MgL6 .. MgL24
    if (startsWith (k, "mgl"))
    {
        const std::string n = k.substr (3);
        if (n == "6" || n == "12" || n == "18" || n == "24") return idx ("MG Low " + n);
    }
    // Comb / flange / phase families: (Comb|Flange|[Flange]PhaseNN)(L6|H6|HL6)?(P|N)
    {
        std::string body = k;
        bool neg = false;
        if (endsWith (body, "p") || endsWith (body, "n"))
        {
            neg = body.back() == 'n';
            const std::string stem = body.substr (0, body.size() - 1);
            std::string fam, var;
            if (startsWith (stem, "comb")) { fam = "Cmb"; var = stem.substr (4); }
            else if (startsWith (stem, "flangephase")) { fam = "Phs"; var = stem.substr (11); }
            else if (startsWith (stem, "flange")) { fam = "Flg"; var = stem.substr (6); }
            else if (startsWith (stem, "phase")) { fam = "Phs"; var = stem.substr (5); }
            if (! fam.empty())
            {
                // Drop the phaser's pole count.
                size_t d = 0;
                while (d < var.size() && std::isdigit ((unsigned char) var[d])) ++d;
                var = var.substr (d);
                std::string side;
                if (var.empty()) { side = "L"; exact = false; }        // no damping filter: L with VAR at max
                else if (var == "l6") side = "L";
                else if (var == "h6") side = "H";
                else if (var == "hl6") side = "HL";
                if (! side.empty())
                {
                    if (neg) exact = false;                            // Tincture has one polarity
                    if (fam == "Phs") exact = false;
                    return idx (fam + " " + side);
                }
            }
        }
    }
    // Morphing multi types: LBH12, LPH24, LNH12, BPN24
    for (const char* mm : { "lbh", "lph", "lnh", "bpn" })
        if (startsWith (k, mm))
        {
            const std::string n = k.substr (3);
            std::string up = mm;
            for (auto& c : up) c = (char) std::toupper ((unsigned char) c);
            if (n == "12" || n == "24") return idx (up + " " + n);
        }
    // Dual types: LH12 .. NN12
    if (k.size() >= 3 && std::isalpha ((unsigned char) k[0]) && std::isalpha ((unsigned char) k[1]) && std::isdigit ((unsigned char) k[2]))
    {
        std::string up = k.substr (0, 2);
        for (auto& c : up) c = (char) std::toupper ((unsigned char) c);
        const int i = indexOf (list, up + " 12");
        if (i >= 0) { if (k.substr (2) != "12") exact = false; return i; }
    }
    // Single types: L6 .. N24
    if (k.size() >= 2 && std::isalpha ((unsigned char) k[0]) && std::isdigit ((unsigned char) k[1]))
    {
        const std::string n = k.substr (1);
        const char c = k[0];
        const char* base = c == 'l' ? "Low " : c == 'h' ? "High " : c == 'b' ? "Band " : c == 'p' ? "Peak " : c == 'n' ? "Notch " : nullptr;
        if (base != nullptr)
        {
            const int i = indexOf (list, base + n);
            if (i >= 0) return i;
            exact = false;
            return idx (std::string (base) + (c == 'l' || c == 'h' ? "12" : "12"));
        }
    }
    exact = false;
    return idx ("MG Low 12");
}

int fxFilterTypeFor (const std::string& raw, bool& exact)
{
    exact = true;
    const std::string k = squash (raw);
    // { "MG Low 6", "MG Low 12", "MG Low 18", "MG Low 24", "Low 12", "Low 24", "High 12", "High 24",
    //   "Band 12", "Band 24", "Peak 12", "Notch 12", "Notch 24", "Acid Ladder", "Comb +", "Comb -" }
    static const std::map<std::string, int> direct {
        { "mgl6", 0 }, { "mgl12", 1 }, { "mgl18", 2 }, { "mgl24", 3 }, { "l12", 4 }, { "l24", 5 }, { "h12", 6 }, { "h24", 7 },
        { "b12", 8 }, { "b24", 9 }, { "p12", 10 }, { "n12", 11 }, { "n24", 12 }, { "ladderacid", 13 }, { "combp", 14 }, { "combn", 15 },
    };
    if (const auto it = direct.find (k); it != direct.end()) return it->second;
    exact = false;
    if (startsWith (k, "comb") || startsWith (k, "flange") || startsWith (k, "phase")) return endsWith (k, "n") ? 15 : 14;
    if (startsWith (k, "ladder") || startsWith (k, "dirty")) return 13;
    if (startsWith (k, "formant") || startsWith (k, "bpn")) return 8;
    if (k.size() >= 2 && k[0] == 'l' && std::isdigit ((unsigned char) k[1])) return k[1] == '6' ? 4 : 5;
    if (k.size() >= 2 && k[0] == 'h' && std::isdigit ((unsigned char) k[1])) return 6;
    if (k.size() >= 2 && k[0] == 'b' && std::isdigit ((unsigned char) k[1])) return 8;
    if (k.size() >= 2 && k[0] == 'n' && std::isdigit ((unsigned char) k[1])) return 11;
    return 1;
}

int warpModeFor (const std::string& raw, int osc, int& fmType)
{
    fmType = -1;
    const auto& list = choices::warpModes();
    std::string n = raw;
    if (startsWith (n, "k")) n = n.substr (1);

    // Modulation warps: FM_OSC, PD_SUB, RM_FILT2, ...
    const size_t us = n.find ('_');
    if (us != std::string::npos)
    {
        std::string kind = n.substr (0, us), from = n.substr (us + 1);
        if (kind == "Remap") return indexOf (list, "Remap " + from);
        if (kind == "FMP") { kind = "FM"; fmType = 0; }
        else if (kind == "FMX") { kind = "FM"; fmType = 2; }
        if (kind != "FM" && kind != "PD" && kind != "AM" && kind != "RM") return -1;
        // OSC / OSC2 are "the other oscillators" in order (for OSC A: B then C).
        int others[2], k = 0;
        for (int o = 0; o < kNumOscs && k < 2; ++o)
            if (o != osc) others[k++] = o;
        std::string target;
        if (from == "OSC") target = std::string (1, (char) ('A' + others[0]));
        else if (from == "OSC2") target = std::string (1, (char) ('A' + others[1]));
        else if (from == "SUB") target = "Sub";
        else if (from == "NOISE") target = "Noise";
        else if (from == "FILT1" || from == "FILT") target = "Filter 1";
        else if (from == "FILT2") target = "Filter 2";
        else if (from == "SELF") target = "Self";
        else return -1;
        return indexOf (list, kind + " (" + target + ")");
    }
    static const std::map<std::string, const char*> plainNames {
        { "Off", "Off" }, { "None", "Off" }, { "Sync", "Sync" }, { "BendPos", "Bend +" }, { "BendNeg", "Bend -" },
        { "BendPosNeg", "Bend +/-" }, { "PWM", "PWM" }, { "ASYMPos", "Asym +" }, { "ASYMNeg", "Asym -" },
        { "ASYMPosNeg", "Asym +/-" }, { "Flip", "Flip" }, { "Mirror", "Mirror" }, { "Quantize", "Quantize" },
        { "OddEven", "Odd/Even" }, { "LPF", "LPF" }, { "HPF", "HPF" },
    };
    if (const auto it = plainNames.find (n); it != plainNames.end()) return indexOf (list, it->second);
    // Distortion warps: kDistTube, kDistSoftClip, ...
    if (startsWith (n, "Dist"))
    {
        const std::string want = squash (n.substr (4));
        static const std::map<std::string, const char*> alias {
            { "linfold", "Linear Fold" }, { "sinfold", "Sine Fold" }, { "sinefold", "Sine Fold" }, { "tapesat", "Tape Sat." },
            { "softsat", "Soft Sat." }, { "diode", "Diode 1" },
        };
        if (const auto a = alias.find (want); a != alias.end()) return indexOf (list, a->second);
        for (size_t i = 0; i < list.size(); ++i)
            if (squash (list[i]) == want) return (int) i;
    }
    return -1;
}

int syncRateForKnob (double rate)
{
    // Serum's synced rate knob: stored 100 * knob^4, 229 steps (knob * 228),
    // grouped into note values (measured). Tincture's list starts at 8 bars.
    const double knob = std::pow (std::max (rate, 0.0) / 100.0, 0.25);
    const int step = (int) std::lround (clampd (knob, 0.0, 1.0) * 228.0);
    static const int lastStep[] { 13, 29, 46, 62, 78, 94, 111, 127, 143, 160, 176, 192, 208, 225, 228 };
    //                            32b 16b 8b  4b  2b  1b  1/2  1/4  1/8  1/16 1/32 1/64 1/128 1/256 fast
    int run = 14;
    for (int i = 0; i < 15; ++i)
        if (step <= lastStep[i]) { run = i; break; }
    // runs 0..2 (32/16/8 bars) -> "8 bars"; 3 -> "4 bars" ... 13 -> "1/256"; fast -> "1/256".
    return std::clamp (run - 2, 0, 11);
}

// Serum's curve knobs are 0..100 with the default (attack 50, decay/release
// 66.6) drawn the way Tincture's defaults (+0.2 / -0.6) are; the ends map to
// the ends.
float attackCurveFor (double c)
{
    c = clampd (c, 0.0, 100.0);
    return (float) (c >= 50.0 ? 0.2 + (c - 50.0) / 50.0 * 0.8 : -1.0 + c / 50.0 * 1.2);
}

float fallCurveFor (double c)
{
    c = clampd (c, 0.0, 100.0);
    constexpr double d = 66.6;
    return (float) (c >= d ? -0.6 - (c - d) / (100.0 - d) * 0.4 : 1.0 - c / d * 1.6);
}

// ---------------------------------------------------------------------------
namespace
{
// Peak (dBFS) of a test note: middle C (a chord for pads) held 2 s, or 6 s for
// FX sounds (risers build up), then 1 s of tail.
double renderPeak (const Patch& patch, const ParamValues& values)
{
    constexpr double rate = 48000.0;
    constexpr int block = 256;
    // Serum basses usually drop their oscillators an octave or more, so middle C
    // suits every category (pads get a chord).
    const std::vector<int> notes = patch.info.category == "Pad" ? std::vector<int> { 48, 55, 60, 64 } : std::vector<int> { 60 };
    const int held = (int) ((patch.info.category == "FX" ? 6.0 : 2.0) * rate), total = held + (int) rate;

    Synth synth;
    synth.prepare (rate, block);
    synth.setKeyboardSettings (patch.keyboard);
    synth.setPatch (CompiledPatch::compile (std::make_shared<Patch> (patch)));
    std::vector<float> L ((size_t) block), R ((size_t) block);
    MidiBuffer out;
    HostInfo host;
    host.playing = true;
    float peak = 0.0f;
    for (int pos = 0; pos < total; pos += block)
    {
        std::vector<MidiEvent> ev;
        if (pos == 0)
            for (int n : notes) ev.push_back (MidiEvent::noteOn (0, n, 100));
        if (pos <= held && held < pos + block)
            for (int n : notes) ev.push_back (MidiEvent::noteOff (held - pos, n));
        out.clear();
        synth.process (L.data(), R.data(), block, ev.data(), (int) ev.size(), values, host, out);
        host.ppq += block / rate * host.bpm / 60.0;
        for (size_t i = 0; i < (size_t) block; ++i)
            if (std::isfinite (L[i]) && std::isfinite (R[i]))
                peak = std::max (peak, std::max (std::abs (L[i]), std::abs (R[i])));
    }
    synth.collectGarbage();
    return peak < 1.0e-5f ? -INFINITY : 20.0 * std::log10 ((double) peak);
}

// Adds `db` of gain: master first, then the MAIN and DIRECT levels together.
// Returns what couldn't be applied.
double addGain (ParamValues& values, double db)
{
    auto bump = [&] (int id, double amount)
    {
        const auto& pi = params()[id];
        const float before = values[id];
        values[id] = std::clamp ((float) (before + amount), pi.min, pi.max);
        return (double) (values[id] - before);
    };
    const auto& g = ids().global;
    db -= bump (g.master, db);
    if (db > 0.01)
    {
        const double room = std::min ((double) params()[g.mainLevel].max - values[g.mainLevel],
                                      (double) params()[g.directLevel].max - values[g.directLevel]);
        const double extra = std::clamp (db, 0.0, std::max (room, 0.0));
        bump (g.mainLevel, extra);
        bump (g.directLevel, extra);
        db -= extra;
    }
    return db;
}
} // namespace

double matchLevel (const Patch& patch, ParamValues& values, double targetPeakDb)
{
    const double first = renderPeak (patch, values);
    if (! std::isfinite (first)) return first;
    double peak = first;
    // Twice: compressors and drive make the response to gain non-linear.
    for (int pass = 0; pass < 2 && std::abs (peak - targetPeakDb) > 0.5; ++pass)
    {
        if (addGain (values, targetPeakDb - peak) > 0.5 && pass > 0) break;     // out of range
        peak = renderPeak (patch, values);
        if (! std::isfinite (peak)) break;
    }
    return first;
}

bool importState (const File& file, Patch& patch, ParamValues& values, const Assets& assets, Report& report)
{
    patch = Patch();
    values.resetToDefaults();
    report = Report {};
    Importer im { file.state, patch, values, assets, report };
    im.run (file.header);
    // Duplicates in the missing list (one table used twice).
    std::sort (report.missingFiles.begin(), report.missingFiles.end());
    report.missingFiles.erase (std::unique (report.missingFiles.begin(), report.missingFiles.end()), report.missingFiles.end());
    report.peakBeforeDb = matchLevel (patch, values);
    if (! std::isfinite (report.peakBeforeDb))
        report.note ("the test note (middle C) was silent, so the level wasn't matched");
    else if (renderPeak (patch, values) < -12.0)
        report.note ("still quiet after level matching (master and output levels at maximum)");
    return true;
}

bool importPreset (const uint8_t* data, size_t size, Patch& patch, ParamValues& values,
                   const Assets& assets, Report& report, std::string& error)
{
    File f;
    if (! readFile (data, size, f, error)) return false;
    return importState (f, patch, values, assets, report);
}
} // namespace tincture::serum
