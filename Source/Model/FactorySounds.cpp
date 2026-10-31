// FactorySounds.cpp — the factory preset library, made in code.
//
// Every preset starts from the init patch and default parameter values and
// changes what it needs. Levels are set so that each sound peaks around
// -6 dBFS in its natural use (one note for basses and leads, a 3-4 note chord
// for pads and keys at velocity 100); Tests/EnginePresetsTests.cpp renders
// and measures every preset.
//
// Macro convention (most presets): MACRO 1 = tone (filter / brightness),
// MACRO 2 = movement (LFO depth, wavetable position), MACRO 3 = space
// (reverb / delay amount), MACRO 4 = character (drive, warp, detune, glide).
#include "FactoryPresets.h"
#include "../Engine/Fx/FxTypes.h"
#include "../Engine/ParamIds.h"
#include "../Engine/Wavetable.h"

#include <cmath>

namespace tincture
{
namespace
{
using MS = ModSource;

int indexOf (const std::vector<std::string>& list, const char* name)
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i] == name) return (int) i;
    return 0;
}
int filterType (const char* name) { return indexOf (choices::filterTypes(), name); }
int warpMode (const char* name)   { return indexOf (choices::warpModes(), name); }
int syncRate (const char* name)   { return indexOf (choices::syncRates(), name); }

// Normalised modulation amounts for a few common destinations.
constexpr float kOct = 0.0875279f;          // one octave of filter cutoff (8 Hz .. 22 kHz, log)
inline float coarseSemis (float st) { return st / 96.0f; }
inline float tuneSemis (float st)   { return st / 48.0f; }

// One FX module in a rack: slots are set in their plain units (see FxTypes.cpp).
struct Fx
{
    ParamValues* v = nullptr;
    FxType type = FxType::Utility;
    int unit = -1;

    Fx& set (int slot, float plain)
    {
        if (unit >= 0) (*v)[ids().fx[unit].p[slot]] = fxInfo (type).params[(size_t) slot].toNorm (plain);
        return *this;
    }
    Fx& norm (int slot, float n)  { if (unit >= 0) (*v)[ids().fx[unit].p[slot]] = n; return *this; }
    Fx& mix (float m)             { if (unit >= 0) (*v)[ids().fx[unit].mix] = m; return *this; }
    Fx& level (float db)          { return set (kFxLevelSlot, db); }
    int param (int slot) const    { return unit >= 0 ? ids().fx[unit].p[slot] : -1; }
    int mixParam() const          { return unit >= 0 ? ids().fx[unit].mix : -1; }
};

// The preset builder: thin helpers over Patch + ParamValues.
struct B
{
    Patch& p;
    ParamValues& v;
    const ParamIdTable& I = ids();

    // ---- Oscillators -----------------------------------------------------
    void table (int o, const char* category, const char* name)
    {
        if (auto t = FactoryTables::get (category, name)) p.wavetable[(size_t) o].table = t;
    }
    // Enables an oscillator with a factory table.
    void osc (int o, const char* category, const char* name, float level = 0.75f, float wtpos = 0.0f)
    {
        v[I.osc[o].enable] = 1.0f;
        table (o, category, name);
        v[I.osc[o].level] = level;
        v[I.osc[o].wtpos] = wtpos;
    }
    void unison (int o, int voices, float detune, float blend = 0.75f, float width = 1.0f, int mode = 0)
    {
        v[I.osc[o].unison] = (float) voices;
        v[I.osc[o].detune] = detune;
        v[I.osc[o].blend] = blend;
        v[I.osc[o].uniWidth] = width;
        v[I.osc[o].uniMode] = (float) mode;
    }
    void warp (int o, int slot, const char* mode, float amount)
    {
        v[slot == 0 ? I.osc[o].warp1Mode : I.osc[o].warp2Mode] = (float) warpMode (mode);
        v[slot == 0 ? I.osc[o].warp1 : I.osc[o].warp2] = amount;
    }
    void pitch (int o, int octave, int semis = 0, float fine = 0.0f)
    {
        v[I.osc[o].oct] = (float) octave;
        v[I.osc[o].sem] = (float) semis;
        v[I.osc[o].fine] = fine;
    }
    void route (int o, int r) { v[I.osc[o].route] = (float) r; }      // 0 Filter, 1 Main, 2 Direct, 3 None

    void sub (float level, int octave = 0, int shape = 0, bool toFilter = false)
    {
        v[I.sub.enable] = 1.0f;
        v[I.sub.level] = level;
        v[I.sub.oct] = (float) octave;
        v[I.sub.shape] = (float) shape;
        v[I.sub.route] = toFilter ? 0.0f : 1.0f;
    }
    // Colour: 0 Sample, 1 White, 2 Pink, 3 Brown, 4 Geiger.
    void noise (int colour, float level, bool toFilter = true)
    {
        v[I.noise.enable] = 1.0f;
        v[I.noise.colour] = (float) colour;
        v[I.noise.level] = level;
        v[I.noise.route] = toFilter ? 0.0f : 1.0f;
    }
    void noiseSample (const char* category, const char* name, float level, bool toFilter = true)
    {
        noise (0, level, toFilter);
        p.noise.sample = FactoryNoises::get (category, name);
    }

    // ---- Filters -----------------------------------------------------------
    void filter (int f, const char* type, float cutoff, float res = 0.1f, float drive = 0.0f, float var = 0.0f)
    {
        v[I.filter[f].enable] = 1.0f;
        v[I.filter[f].type] = (float) filterType (type);
        v[I.filter[f].cutoff] = cutoff;
        v[I.filter[f].res] = res;
        v[I.filter[f].drive] = drive;
        v[I.filter[f].var] = var;
    }
    void filterOff (int f) { v[I.filter[f].enable] = 0.0f; }

    // ---- Envelopes (seconds, sustain in dB; -60 = off) ---------------------
    void env (int e, float a, float d, float susDb, float r, float hold = 0.0f)
    {
        v[I.env[e].attack] = a;
        v[I.env[e].hold] = hold;
        v[I.env[e].decay] = d;
        v[I.env[e].sustain] = susDb;
        v[I.env[e].release] = r;
    }
    void envCurves (int e, float a, float d, float r)
    {
        v[I.env[e].attackCurve] = a;
        v[I.env[e].decayCurve] = d;
        v[I.env[e].releaseCurve] = r;
    }

    // ---- LFOs ----------------------------------------------------------------
    void lfoShape (int l, const char* folder, const char* name)
    {
        if (auto* s = LfoShapes::find (folder, name))
        {
            p.lfo[(size_t) l].shape = s->curve;
            p.lfo[(size_t) l].shapeName = s->name;
        }
    }
    // BPM-synced LFO; mode 0 Free, 1 Retrig, 2 Envelope.
    void lfoSync (int l, const char* rate, int mode = 1)
    {
        v[I.lfo[l].bpm] = 1.0f;
        v[I.lfo[l].rateSync] = (float) syncRate (rate);
        v[I.lfo[l].mode] = (float) mode;
    }
    void lfoHz (int l, float hz, int mode = 1)
    {
        v[I.lfo[l].bpm] = 0.0f;
        v[I.lfo[l].rate] = hz;
        v[I.lfo[l].mode] = (float) mode;
    }

    // ---- Matrix --------------------------------------------------------------
    int mod (MS src, int dest, float amount, bool bipolar = false, MS aux = MS::None)
    {
        ModRow r;
        r.source = src;
        r.dest = dest;
        r.amount = amount;
        r.bipolar = bipolar;
        r.aux = aux;
        return p.matrix.add (r);
    }
    void macro (int m, int dest, float amount, bool bipolar = false) { mod (macroSource (m), dest, amount, bipolar); }
    // Mod-wheel vibrato on LFO l (sine, 5.5 Hz, +-0.3 st at full wheel).
    void vibrato (int l, float semis = 0.3f)
    {
        lfoShape (l, "Basic", "Sine");
        lfoHz (l, 5.5f, 1);
        mod (lfoSource (l), I.global.tuning, tuneSemis (semis), true, MS::ModWheel);
    }
    // Velocity -> Amp: `floor` at velocity 0, full at 127.
    void velocity (float floor)
    {
        v[I.global.amp] = floor;
        mod (MS::Velocity, I.global.amp, 1.0f - floor);
    }

    // ---- FX ----------------------------------------------------------------
    Fx fx (int rack, FxType t)
    {
        Fx f;
        f.v = &v;
        f.type = t;
        f.unit = p.fx.add (rack, t);
        if (f.unit >= 0)
        {
            const auto d = fxDefaults (t);
            for (int k = 0; k < kFxParams; ++k) v[I.fx[f.unit].p[k]] = d[(size_t) k];
            v[I.fx[f.unit].mix] = fxInfo (t).defaultMix;
        }
        return f;
    }
    // Reverb: type 0 Plate, 1 Hall, 2 Vintage, 3 Nitrous, 4 Basin.
    Fx reverb (int rack, int type, float size, float decay, float mix, float hiCut = 0.35f, float loCut = 0.1f)
    {
        auto f = fx (rack, FxType::Reverb);
        f.set (0, (float) type).set (3, size).set (8, decay).set (2, hiCut).set (1, loCut).mix (mix);
        return f;
    }
    // Delay, synced: time as a fraction of the synced range (0 = 1/256 .. 1 = 8 bars).
    Fx delay (int rack, const char* note, float feedback, float mix, bool pingPong = true, float filterHz = 2500.0f)
    {
        auto f = fx (rack, FxType::Delay);
        const auto& d = fxInfo (FxType::Delay).params[4];
        // Synced times: n = 0 is the fastest (1/256), n = 1 the slowest.
        const int idx = syncRate (note);                    // 0 = 8 bars ... 11 = 1/256
        const float n = d.syncCount > 1 ? (float) (11 - idx) / (float) (d.syncCount - 1) : 0.5f;
        f.set (0, pingPong ? 1.0f : 0.0f).norm (4, n).norm (6, n).set (8, feedback).set (9, filterHz).mix (mix);
        return f;
    }
    Fx chorus (int rack, float rate, float depth, float mix)
    {
        auto f = fx (rack, FxType::Chorus);
        f.set (0, rate).set (4, depth).mix (mix);
        return f;
    }
    Fx dist (int rack, int mode, float drive, float mix = 1.0f)
    {
        auto f = fx (rack, FxType::Distortion);
        f.set (0, (float) mode).set (5, drive).mix (mix);
        return f;
    }
    Fx comp (int rack, float threshDb, float ratio, float attackMs, float releaseMs, float makeupDb)
    {
        auto f = fx (rack, FxType::Compressor);
        f.set (1, threshDb).set (2, ratio).set (3, attackMs).set (4, releaseMs).set (5, makeupDb);
        return f;
    }
    // EQ: low shelf / high shelf gains (dB) at the given frequencies.
    Fx eq (int rack, float lowHz, float lowDb, float highHz, float highDb)
    {
        auto f = fx (rack, FxType::Equalizer);
        f.set (1, lowHz).set (3, lowDb).set (5, highHz).set (7, highDb);
        return f;
    }

    void master (float db) { v[I.global.master] = db; }
    void mono (bool legato, float porta = 0.0f, bool always = false)
    {
        v[I.global.mono] = 1.0f;
        v[I.global.legato] = legato ? 1.0f : 0.0f;
        v[I.global.porta] = porta;
        v[I.global.portaAlways] = always ? 1.0f : 0.0f;
    }
};

using Build = std::function<void (B&)>;

FactoryPreset make (const char* name, const char* category, const char* description,
                    std::vector<std::string> tags, Build fn)
{
    return { name, category, description, std::move (tags),
             [fn] (Patch& p, ParamValues& v) { B b { p, v }; fn (b); } };
}

// ===========================================================================
// Bass
// ===========================================================================
void addBass (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Round Sub", "Bass", "A triangle and a sine sub through a gentle low-pass, mono with a short glide.",
                       { "sub", "clean", "mono" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.6f, 0.2f);           // triangle
        b.sub (0.7f);
        b.filter (0, "MG Low 24", 900.0f, 0.05f);
        b.v[I.filter[0].keytrack] = 1.0f;
        b.env (0, 0.003f, 1.0f, 0.0f, 0.08f);
        b.mono (true, 0.04f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.osc[0].wtpos, 0.3f);                          // triangle -> saw
        b.macro (2, I.sub.level, -0.5f);
        b.macro (3, I.filter[0].drive, 0.6f);
        b.master (1.0f);
    }));

    l.push_back (make ("Acid Line", "Bass", "A saw into the acid ladder with a snappy filter envelope and slide on overlapping notes.",
                       { "acid", "303", "mono", "resonant" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.75f, 1.0f);
        b.filter (0, "Acid Ladder", 220.0f, 0.72f, 0.25f);
        b.env (0, 0.002f, 0.4f, -8.0f, 0.06f);
        b.env (1, 0.001f, 0.22f, -60.0f, 0.1f);
        b.mod (MS::Env2, I.filter[0].cutoff, 3.5f * kOct);
        b.mono (true, 0.07f);
        auto d = b.dist (0, fxchoices::Tube, 0.35f, 0.6f);
        b.delay (0, "1/8", 0.3f, 0.12f, true, 1800.0f);
        b.macro (0, I.filter[0].cutoff, 2.5f * kOct);
        b.macro (1, I.filter[0].res, 0.25f);
        b.macro (2, I.env[1].decay, 0.25f);
        b.macro (3, d.param (5), 0.5f);
        b.master (-4.5f);
    }));

    l.push_back (make ("Reese", "Bass", "Two detuned saw stacks beating against each other, low-passed, with slow filter drift.",
                       { "reese", "dnb", "detuned" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.55f, 1.0f);
        b.unison (0, 4, 0.18f, 0.6f, 0.6f);
        b.osc (1, "Analog", "Analog Saw", 0.55f, 0.85f);
        b.pitch (1, 0, 0, 9.0f);
        b.unison (1, 3, 0.12f, 0.5f, 0.4f);
        b.route (1, 0);
        b.sub (0.45f);
        b.filter (0, "MG Low 24", 700.0f, 0.2f, 0.2f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoSync (0, "2 bars", 0);
        b.mod (MS::Lfo1, I.filter[0].cutoff, 1.2f * kOct, true, MS::Macro2);
        b.env (0, 0.005f, 1.0f, 0.0f, 0.15f);
        b.mono (true, 0.03f);
        auto d = b.dist (0, fxchoices::SoftClip, 0.2f, 1.0f);
        b.comp (0, -20.0f, 3.0f, 10.0f, 120.0f, 0.0f);
        b.v[I.macro[1]] = 0.5f;
        b.macro (0, I.filter[0].cutoff, 2.5f * kOct);
        b.macro (2, I.osc[0].detune, 0.3f);
        b.macro (3, d.param (5), 0.5f);
        b.master (-4.0f);
    }));

    l.push_back (make ("Vowel Growl", "Bass", "A bass-vowel table frequency-modulated by OSC B, swept by an eighth-note LFO.",
                       { "growl", "dubstep", "fm", "vowel" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Vowel", "Bass Vowels", 0.7f, 0.0f);
        b.osc (1, "Analog", "Basic Shapes", 0.5f, 0.0f);            // sine modulator
        b.route (1, 3);
        b.warp (0, 0, "FM (B)", 0.25f);
        b.sub (0.5f);
        b.filter (0, "MG Low 12", 3000.0f, 0.15f, 0.3f);
        b.lfoShape (0, "Basic", "Triangle");
        b.lfoSync (0, "1/8", 1);
        b.mod (MS::Lfo1, I.osc[0].wtpos, 1.0f);
        b.mod (MS::Lfo1, I.osc[0].warp1, 0.35f, false, MS::Macro2);
        b.mono (true, 0.02f);
        auto d = b.dist (0, fxchoices::Tube, 0.4f);
        b.comp (0, -18.0f, 4.0f, 5.0f, 80.0f, 0.0f);
        b.v[I.macro[1]] = 0.6f;
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (2, I.lfo[0].rateSync, 0.18f);
        b.macro (3, d.param (5), 0.4f);
        b.master (-2.0f);
    }));

    l.push_back (make ("Wobble", "Bass", "A saw and square through a resonant low-pass swept by a synced sine LFO.",
                       { "wobble", "dubstep", "lfo" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.6f, 0.4f);            // saw
        b.osc (1, "Analog", "Basic Shapes", 0.45f, 0.6f);           // square
        b.pitch (1, -1);
        b.route (1, 0);
        b.sub (0.45f);
        b.filter (0, "MG Low 24", 180.0f, 0.45f, 0.3f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoSync (0, "1/8", 1);
        b.mod (MS::Lfo1, I.filter[0].cutoff, 4.0f * kOct);
        b.mono (true, 0.0f);
        b.dist (0, fxchoices::SoftClip, 0.3f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.lfo[0].rateSync, 0.1f);
        b.macro (2, I.filter[0].res, 0.3f);
        b.macro (3, I.osc[0].wtpos, 0.4f);
        b.master (-7.0f);
    }));

    l.push_back (make ("Hard Sync Bass", "Bass", "A synced saw whose sync ratio drops with a fast envelope, over a sine sub.",
                       { "sync", "aggressive" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.6f, 0.4f);
        b.warp (0, 0, "Sync", 0.15f);
        b.sub (0.55f);
        b.filter (0, "MG Low 12", 2500.0f, 0.1f);
        b.env (0, 0.002f, 0.6f, -6.0f, 0.08f);
        b.env (1, 0.001f, 0.25f, -60.0f, 0.1f);
        b.mod (MS::Env2, I.osc[0].warp1, 0.5f);
        b.mono (true, 0.02f);
        b.dist (0, fxchoices::Tube, 0.2f, 0.7f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.env[1].decay, 0.25f);
        b.macro (2, I.osc[0].warp1, 0.4f);
        b.macro (3, I.sub.level, -0.4f);
        b.master (-2.0f);
    }));

    l.push_back (make ("Pluck Bass", "Bass", "A square with a short filter envelope and a quick decay to a low sustain.",
                       { "pluck", "funk", "short" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.7f, 0.6f);            // square
        b.sub (0.5f);
        b.filter (0, "MG Low 24", 250.0f, 0.3f);
        b.env (0, 0.001f, 0.45f, -14.0f, 0.08f);
        b.env (1, 0.001f, 0.18f, -60.0f, 0.1f);
        b.mod (MS::Env2, I.filter[0].cutoff, 3.0f * kOct);
        b.velocity (0.4f);
        b.mod (MS::Velocity, I.filter[0].cutoff, 1.0f * kOct);
        b.comp (0, -16.0f, 3.0f, 8.0f, 100.0f, 0.0f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.env[1].decay, 0.2f);
        b.macro (2, I.env[0].sustain, 0.2f);
        b.macro (3, I.filter[0].res, 0.4f);
        b.master (4.0f);
    }));

    l.push_back (make ("Pump Bass", "Bass", "A saw-stack bass ducked on every beat by a sidechain-shaped LFO.",
                       { "sidechain", "house", "pump" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "S2 Tables", "Saw Stack", 0.6f, 0.3f);
        b.unison (0, 3, 0.1f, 0.5f, 0.5f);
        b.sub (0.6f);
        b.filter (0, "MG Low 24", 600.0f, 0.15f);
        b.lfoShape (0, "Sidechain", "Pump Classic");
        b.lfoSync (0, "1/4", 0);
        b.v[I.lfo[0].host] = 1.0f;
        b.v[I.global.amp] = 0.0f;
        b.mod (MS::Lfo1, I.global.amp, 1.0f);
        b.mono (true, 0.02f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.osc[0].wtpos, 0.6f);
        b.macro (2, I.global.amp, 0.6f);                            // less pumping
        b.macro (3, I.osc[0].detune, 0.3f);
        b.master (-1.0f);
    }));
}

// ===========================================================================
// Lead
// ===========================================================================
void addLead (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Supersaw Lead", "Lead", "Seven detuned saws, legato with glide, into a ping-pong delay and a plate.",
                       { "supersaw", "trance", "wide" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.8f, 1.0f);
        b.unison (0, 7, 0.22f, 0.7f, 1.0f, 1);
        b.osc (1, "Analog", "Analog Saw", 0.45f, 1.0f);
        b.pitch (1, 1);
        b.route (1, 0);
        b.filter (0, "MG Low 12", 6000.0f, 0.1f);
        b.env (0, 0.005f, 1.0f, 0.0f, 0.25f);
        b.mono (true, 0.06f);
        b.vibrato (2);
        b.eq (0, 200.0f, -3.0f, 6000.0f, 1.5f);
        auto d = b.delay (0, "1/8", 0.35f, 0.18f);
        auto r = b.reverb (0, 0, 0.6f, 0.5f, 0.18f);
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (1, I.osc[0].detune, 0.3f);
        b.macro (2, d.mixParam(), 0.25f);
        b.macro (2, r.mixParam(), 0.25f);
        b.macro (3, I.osc[1].level, 0.3f);                         // octave layer
        b.master (0.0f);
    }));

    l.push_back (make ("Sync Scream", "Lead", "A hard-synced saw swept by an envelope, distorted, with a mod-wheel vibrato.",
                       { "sync", "aggressive", "distorted" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.6f, 0.4f);
        b.warp (0, 0, "Sync", 0.1f);
        b.unison (0, 2, 0.08f, 0.5f, 0.6f);
        b.filter (0, "MG Low 12", 5000.0f, 0.15f);
        b.env (1, 0.001f, 0.8f, -12.0f, 0.3f);
        b.mod (MS::Env2, I.osc[0].warp1, 0.55f);
        b.mono (true, 0.05f);
        b.vibrato (1);
        auto d = b.dist (0, fxchoices::Diode1, 0.3f, 0.7f);
        b.eq (0, 150.0f, -6.0f, 5000.0f, -2.0f);
        b.delay (0, "1/8", 0.3f, 0.15f);
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (1, I.osc[0].warp1, 0.4f);
        b.macro (2, I.env[1].decay, 0.3f);
        b.macro (3, d.param (5), 0.4f);
        b.master (4.0f);
    }));

    l.push_back (make ("Pulse Lead", "Lead", "A pulse wave with LFO-driven pulse width, a short glide and a dotted-eighth delay.",
                       { "pwm", "square", "retro" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "PWM Sweep", 0.75f, 0.3f);
        b.lfoShape (0, "Basic", "Triangle");
        b.lfoHz (0, 0.8f, 1);
        b.mod (MS::Lfo1, I.osc[0].wtpos, 0.5f, false, MS::None);
        b.filter (0, "MG Low 24", 4000.0f, 0.15f);
        b.mono (true, 0.04f);
        b.vibrato (1);
        auto d = b.delay (0, "1/8", 0.4f, 0.2f);
        d.set (5, 1.5f).set (7, 1.5f);                              // dotted eighths
        b.reverb (0, 0, 0.4f, 0.4f, 0.12f);
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (1, I.lfo[0].rate, 0.3f);
        b.macro (2, d.mixParam(), 0.3f);
        b.macro (3, I.filter[0].res, 0.4f);
        b.master (6.0f);
    }));

    l.push_back (make ("Talking Lead", "Lead", "A vowel wavetable scanned by a slow LFO, with chorus and a light plate.",
                       { "vowel", "formant", "vocal" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Vowel", "A-E-I-O-U", 0.75f, 0.0f);
        b.unison (0, 2, 0.06f, 0.5f, 0.5f);
        b.lfoShape (0, "Basic", "Triangle");
        b.lfoSync (0, "1 bar", 1);
        b.mod (MS::Lfo1, I.osc[0].wtpos, 1.0f, false, MS::None);
        b.warp (0, 0, "Bend +/-", 0.5f);                           // 50 % = no bend
        b.filterOff (0);
        b.mono (true, 0.05f);
        b.vibrato (1);
        b.chorus (0, 0.4f, 0.3f, 0.35f);
        b.reverb (0, 0, 0.5f, 0.45f, 0.15f);
        b.macro (0, I.osc[0].wtpos, 0.5f);
        b.macro (1, I.lfo[0].rateSync, 0.18f);
        b.macro (2, I.osc[0].detune, 0.3f);
        b.macro (3, I.osc[0].warp1, 0.4f);
        b.master (4.0f);
    }));

    l.push_back (make ("Brass Lead", "Lead", "Two saws with a swelling filter envelope and a delayed vibrato.",
                       { "brass", "analog", "expressive" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.55f, 1.0f);
        b.osc (1, "Analog", "Analog Saw", 0.55f, 1.0f);
        b.pitch (1, 0, 0, 7.0f);
        b.route (1, 0);
        b.filter (0, "MG Low 24", 500.0f, 0.15f);
        b.env (0, 0.03f, 1.0f, -2.0f, 0.2f);
        b.env (1, 0.08f, 0.6f, -8.0f, 0.3f);
        b.mod (MS::Env2, I.filter[0].cutoff, 3.0f * kOct);
        // Vibrato that fades in after the attack.
        b.lfoShape (1, "Basic", "Sine");
        b.lfoHz (1, 5.0f, 1);
        b.v[I.lfo[1].delay] = 0.35f;
        b.v[I.lfo[1].rise] = 0.6f;
        b.mod (MS::Lfo2, I.global.tuning, tuneSemis (0.2f), true);
        b.mono (true, 0.03f);
        b.reverb (0, 1, 0.5f, 0.45f, 0.15f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.env[1].attack, 0.2f);
        b.macro (2, I.osc[1].fine, 0.1f);
        b.macro (3, I.filter[0].drive, 0.5f);
        b.master (4.0f);
    }));

    l.push_back (make ("Fold Lead", "Lead", "A sine through a sine wavefolder whose depth follows an LFO, with a phaser.",
                       { "wavefold", "west coast", "harmonic" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.7f, 0.0f);
        b.warp (0, 0, "Sine Fold", 0.25f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoHz (0, 0.5f, 1);
        b.mod (MS::Lfo1, I.osc[0].warp1, 0.3f, false, MS::None);
        b.filter (0, "MG Low 12", 7000.0f, 0.1f);
        b.mono (true, 0.04f);
        b.vibrato (2);
        b.fx (0, FxType::Phaser).mix (0.4f);
        b.delay (0, "1/4", 0.3f, 0.15f);
        b.macro (0, I.osc[0].warp1, 0.4f);
        b.macro (1, I.lfo[0].rate, 0.3f);
        b.macro (2, I.filter[0].cutoff, -2.0f * kOct);
        b.macro (3, I.filter[0].res, 0.5f);
        b.master (4.0f);
    }));
}

// ===========================================================================
// Pad
// ===========================================================================
void addPad (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Warm Strings", "Pad", "Detuned analog saws with a slow attack, chorus and a hall.",
                       { "strings", "analog", "warm" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.5f, 0.5f);
        b.unison (0, 5, 0.15f, 0.7f, 1.0f, 1);
        b.filter (0, "MG Low 12", 2200.0f, 0.1f);
        b.v[I.filter[0].keytrack] = 1.0f;
        b.env (0, 0.6f, 1.0f, 0.0f, 1.2f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoHz (0, 0.15f, 1);
        b.v[I.lfo[0].phase] = 180.0f;
        b.mod (MS::Lfo1, I.osc[0].wtpos, 0.5f, true, MS::Macro2);
        b.chorus (0, 0.3f, 0.35f, 0.4f);
        auto r = b.reverb (0, 1, 0.7f, 0.6f, 0.3f);
        b.v[I.macro[1]] = 0.5f;
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[0].attack, 0.25f);
        b.master (-1.0f);
    }));

    l.push_back (make ("Glass Pad", "Pad", "A bell-spectrum table slowly scanned by an LFO, with a long plate.",
                       { "glass", "bright", "digital" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Spectral", "Bell Spectrum", 0.55f, 0.2f);
        b.unison (0, 3, 0.08f, 0.6f, 1.0f);
        b.osc (1, "Analog", "Basic Shapes", 0.35f, 0.0f);
        b.pitch (1, 1);
        b.route (1, 0);
        b.filter (0, "MG Low 12", 9000.0f, 0.05f);
        b.env (0, 0.8f, 1.0f, 0.0f, 1.5f);
        b.lfoShape (0, "Basic", "Triangle");
        b.lfoSync (0, "2 bars", 0);
        b.mod (MS::Lfo1, I.osc[0].wtpos, 0.6f, false, MS::Macro2);
        auto r = b.reverb (0, 0, 0.8f, 0.7f, 0.35f, 0.5f);
        b.v[I.macro[1]] = 0.6f;
        b.macro (0, I.filter[0].cutoff, -3.0f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.osc[1].level, 0.3f);
        b.master (-4.0f);
    }));

    l.push_back (make ("Choir", "Pad", "Two vowel tables morphing from ah to oo, with a slow attack and a hall on a send bus.",
                       { "choir", "vocal", "formant" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Vowel", "Choir Ahh-Ooh", 0.6f, 0.0f);
        b.unison (0, 4, 0.1f, 0.7f, 1.0f);
        b.osc (1, "Vowel", "Female Vowels", 0.35f, 0.0f);
        b.route (1, 0);
        b.filter (0, "MG Low 12", 6000.0f, 0.05f);
        b.env (0, 0.7f, 1.0f, 0.0f, 1.3f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoSync (0, "4 bars", 1);
        b.mod (MS::Lfo1, I.osc[0].wtpos, 1.0f, false, MS::Macro2);
        b.mod (MS::Lfo1, I.osc[1].wtpos, 0.5f, false, MS::Macro2);
        b.chorus (0, 0.25f, 0.3f, 0.3f);
        // Hall on BUS 1, fed from Filter 1 (a send, so the dry signal stays full).
        b.reverb (kFxBus1, 1, 0.8f, 0.65f, 1.0f);
        b.v[I.filter[0].bus1] = 0.4f;
        b.v[I.macro[1]] = 0.7f;
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (2, I.filter[0].bus1, 0.4f);
        b.macro (3, I.osc[0].detune, 0.3f);
        b.master (-2.0f);
    }));

    l.push_back (make ("Drift Pad", "Pad", "A spectral comb table wandered by a Lorenz chaos LFO, through a notch filter.",
                       { "chaos", "evolving", "spectral" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Spectral", "Spectral Comb", 0.55f, 0.3f);
        b.unison (0, 3, 0.1f, 0.6f, 1.0f);
        b.filter (0, "Notch 12", 1500.0f, 0.3f);
        b.filter (1, "MG Low 12", 4500.0f, 0.1f);
        b.v[I.filter[0].route] = 0.0f;                              // F1 -> F2
        b.v[I.filter[1].route] = 1.0f;
        b.env (0, 1.0f, 1.0f, 0.0f, 1.8f);
        b.v[I.lfo[0].type] = 2.0f;                                  // Chaos: Lorenz
        b.lfoHz (0, 0.2f, 0);
        b.mod (MS::Lfo1, I.osc[0].wtpos, 0.5f, true, MS::Macro2);
        b.v[I.lfo[1].type] = 3.0f;                                  // Chaos: Rossler
        b.lfoHz (1, 0.13f, 0);
        b.mod (MS::Lfo2, I.filter[0].cutoff, 2.0f * kOct, true);
        b.fx (0, FxType::Hyper).mix (0.4f);
        auto r = b.reverb (0, 3, 0.7f, 0.6f, 0.3f);
        b.v[I.macro[1]] = 0.7f;
        b.macro (0, I.filter[1].cutoff, 1.5f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.filter[0].res, 0.4f);
        b.master (3.0f);
    }));

    l.push_back (make ("PWM Pad", "Pad", "Two pulse waves with opposing width LFOs, chorus and a hall.",
                       { "pwm", "analog", "vintage" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "PWM Sweep", 0.5f, 0.3f);
        b.osc (1, "Analog", "PWM Sweep", 0.5f, 0.3f);
        b.pitch (1, 0, 0, -8.0f);
        b.route (1, 0);
        b.lfoShape (0, "Basic", "Triangle");
        b.lfoHz (0, 0.35f, 0);
        b.mod (MS::Lfo1, I.osc[0].wtpos, 0.6f, false, MS::Macro2);
        b.mod (MS::Lfo1, I.osc[1].wtpos, -0.6f, false, MS::Macro2);
        b.v[I.osc[1].wtpos] = 0.9f;
        b.filter (0, "MG Low 24", 1800.0f, 0.12f);
        b.env (0, 0.5f, 1.0f, 0.0f, 1.0f);
        b.env (1, 0.9f, 2.0f, -6.0f, 1.0f);
        b.mod (MS::Env2, I.filter[0].cutoff, 1.0f * kOct);
        b.chorus (0, 0.5f, 0.4f, 0.5f);
        auto r = b.reverb (0, 1, 0.6f, 0.55f, 0.25f);
        b.v[I.macro[1]] = 0.6f;
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.osc[1].fine, 0.1f);
        b.master (0.0f);
    }));

    l.push_back (make ("Slow Sweep", "Pad", "A saw stack under a resonant low-pass that sweeps over four bars, with a phaser.",
                       { "sweep", "filter", "movement" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "S2 Tables", "Saw Stack", 0.5f, 0.2f);
        b.unison (0, 4, 0.14f, 0.7f, 1.0f, 1);
        b.filter (0, "MG Low 24", 400.0f, 0.4f);
        b.env (0, 0.4f, 1.0f, 0.0f, 1.2f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoSync (0, "4 bars", 1);
        b.v[I.lfo[0].phase] = 270.0f;                               // start at the bottom
        b.mod (MS::Lfo1, I.filter[0].cutoff, 4.0f * kOct, false, MS::Macro2);
        b.fx (0, FxType::Phaser).mix (0.35f);
        auto r = b.reverb (0, 1, 0.65f, 0.55f, 0.25f);
        b.v[I.macro[1]] = 0.8f;
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.filter[0].res, 0.3f);
        b.master (2.0f);
    }));
}

// ===========================================================================
// Pluck
// ===========================================================================
void addPluck (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Saw Pluck", "Pluck", "A saw with a fast filter and amp decay, into a ping-pong delay.",
                       { "pluck", "house", "delay" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.7f, 1.0f);
        b.unison (0, 3, 0.1f, 0.6f, 0.8f);
        b.filter (0, "MG Low 24", 350.0f, 0.2f);
        b.v[I.filter[0].keytrack] = 1.0f;
        b.env (0, 0.001f, 0.5f, -60.0f, 0.3f);
        b.env (1, 0.001f, 0.2f, -60.0f, 0.2f);
        b.mod (MS::Env2, I.filter[0].cutoff, 4.0f * kOct);
        b.velocity (0.3f);
        auto d = b.delay (0, "1/8", 0.35f, 0.2f);
        auto r = b.reverb (0, 0, 0.5f, 0.45f, 0.15f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.env[1].decay, 0.2f);
        b.macro (2, d.mixParam(), 0.3f);
        b.macro (2, r.mixParam(), 0.2f);
        b.macro (3, I.env[0].decay, 0.2f);
        b.master (2.0f);
    }));

    l.push_back (make ("Mallet", "Pluck", "An FM table struck with a short envelope on the FM index, with a room reverb.",
                       { "mallet", "fm", "percussive" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Digital", "FM 1-3", 0.7f, 0.0f);
        b.env (0, 0.001f, 0.7f, -60.0f, 0.4f);
        b.env (1, 0.001f, 0.12f, -60.0f, 0.1f);
        b.mod (MS::Env2, I.osc[0].wtpos, 0.6f);
        b.filterOff (0);
        b.velocity (0.3f);
        b.mod (MS::Velocity, I.osc[0].wtpos, 0.2f);
        auto r = b.reverb (0, 0, 0.35f, 0.4f, 0.2f);
        b.macro (0, I.osc[0].wtpos, 0.4f);
        b.macro (1, I.env[1].decay, 0.3f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[0].decay, 0.25f);
        b.master (-3.0f);
    }));

    l.push_back (make ("Kalimba", "Pluck", "A sine with a bright FM attack from a harmonic-ratio OSC B and a short decay.",
                       { "kalimba", "fm", "tuned percussion" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.75f, 0.0f);
        b.osc (1, "Analog", "Basic Shapes", 0.5f, 0.0f);
        b.v[I.osc[1].pitchMode] = 2.0f;                             // Ratio
        b.v[I.osc[1].ratio] = 3.5f;
        b.route (1, 3);
        b.warp (0, 0, "FM (B)", 0.0f);
        b.env (0, 0.001f, 0.6f, -60.0f, 0.35f);
        b.env (1, 0.001f, 0.08f, -60.0f, 0.05f);
        b.mod (MS::Env2, I.osc[0].warp1, 0.35f);
        b.filterOff (0);
        b.velocity (0.3f);
        auto r = b.reverb (0, 0, 0.4f, 0.4f, 0.2f);
        b.macro (0, I.osc[0].warp1, 0.2f);
        b.macro (1, I.osc[1].ratio, 0.1f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[0].decay, 0.25f);
        b.master (3.0f);
    }));

    l.push_back (make ("Chord Stab", "Pluck", "A saw stacked with octave and fifth unison copies, short and filtered.",
                       { "stab", "chord", "house" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.5f, 1.0f);
        b.unison (0, 4, 0.12f, 0.6f, 1.0f);
        b.v[I.osc[0].uniStack] = 2.0f;                              // 12+7
        b.filter (0, "MG Low 12", 900.0f, 0.15f);
        b.env (0, 0.001f, 0.35f, -60.0f, 0.2f);
        b.env (1, 0.001f, 0.25f, -60.0f, 0.2f);
        b.mod (MS::Env2, I.filter[0].cutoff, 2.5f * kOct);
        b.velocity (0.4f);
        b.comp (0, -20.0f, 3.0f, 5.0f, 80.0f, 0.0f);
        auto r = b.reverb (0, 0, 0.55f, 0.5f, 0.2f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.env[1].decay, 0.3f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[0].decay, 0.3f);
        b.master (4.0f);
    }));

    l.push_back (make ("Harp", "Pluck", "A harmonic-series table whose brightness falls with the note, with a long plate.",
                       { "harp", "soft", "acoustic" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "S2 Tables", "Harmonic Series", 0.7f, 0.0f);
        b.env (0, 0.001f, 1.0f, -60.0f, 0.8f);
        b.env (1, 0.001f, 0.4f, -60.0f, 0.3f);
        b.mod (MS::Env2, I.osc[0].wtpos, 0.4f);
        b.filter (0, "MG Low 12", 5000.0f, 0.05f);
        b.velocity (0.3f);
        auto r = b.reverb (0, 0, 0.7f, 0.6f, 0.3f);
        b.macro (0, I.osc[0].wtpos, 0.3f);
        b.macro (1, I.env[1].decay, 0.3f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[0].decay, 0.3f);
        b.master (3.0f);
    }));

    l.push_back (make ("Comb Pluck", "Pluck", "A noise burst exciting a key-tracked comb filter, over a soft triangle body.",
                       { "karplus", "string", "comb", "noise" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.3f, 0.2f);           // triangle body, straight to Main
        b.route (0, 1);
        b.noise (1, 0.0f);
        // A comb tuned to the note (cutoff = pitch at C4, keytracked) with a
        // low-pass in its feedback loop (VAR), then a low-pass for tone.
        b.filter (0, "Cmb L", 266.0f, 1.0f, 0.0f, 0.75f);   // ~30 cents sharp: the loop filter lowers the pitch
        b.v[I.filter[0].level] = 6.0f;
        b.v[I.filter[0].keytrack] = 1.0f;
        b.v[I.filter[0].route] = 0.0f;                              // F1 -> F2
        b.filter (1, "MG Low 12", 4000.0f, 0.05f);
        b.v[I.filter[1].keytrack] = 1.0f;
        b.v[I.filter[1].route] = 1.0f;
        b.env (0, 0.001f, 1.5f, -60.0f, 0.4f);
        // Noise only for the first few milliseconds.
        b.env (1, 0.0f, 0.03f, -60.0f, 0.03f);
        b.mod (MS::Env2, I.noise.level, 1.0f);
        b.velocity (0.3f);
        auto r = b.reverb (0, 0, 0.5f, 0.5f, 0.2f);
        b.macro (0, I.filter[1].cutoff, 2.0f * kOct);
        b.macro (1, I.filter[0].var, -0.3f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[1].decay, 0.15f);
        b.master (0.0f);
    }));
}

// ===========================================================================
// Keys
// ===========================================================================
void addKeys (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Electric Piano", "Keys", "A sine phase-modulated by a sine an octave up, with a velocity-dependent bark and tremolo.",
                       { "ep", "rhodes", "fm" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.75f, 0.0f);
        b.osc (1, "Analog", "Basic Shapes", 0.5f, 0.0f);
        b.pitch (1, 1);
        b.route (1, 3);
        b.warp (0, 0, "FM (B)", 0.05f);
        b.env (0, 0.002f, 2.5f, -60.0f, 0.35f);
        b.env (1, 0.001f, 0.35f, -20.0f, 0.3f);
        b.mod (MS::Env2, I.osc[0].warp1, 0.12f);
        b.mod (MS::Velocity, I.osc[0].warp1, 0.1f);
        b.filterOff (0);
        b.velocity (0.3f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoHz (0, 4.5f, 0);
        b.mod (MS::Lfo1, I.osc[0].pan, 0.25f, true, MS::Macro2);
        b.chorus (0, 0.4f, 0.25f, 0.3f);
        auto r = b.reverb (0, 0, 0.45f, 0.45f, 0.15f);
        b.v[I.macro[1]] = 0.5f;
        b.macro (0, I.osc[0].warp1, 0.12f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[0].decay, 0.2f);
        b.master (3.0f);
    }));

    l.push_back (make ("Drawbar Organ", "Keys", "Organ drawbar registrations with a rotary-style chorus and a little overdrive.",
                       { "organ", "drawbar", "sustained" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "S2 Tables", "Organ Drawbars", 0.55f, 0.0f);
        b.pitch (0, -1);
        b.filterOff (0);
        b.env (0, 0.004f, 1.0f, 0.0f, 0.05f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoHz (0, 6.0f, 0);
        b.mod (MS::Lfo1, I.global.tuning, tuneSemis (0.06f), true, MS::Macro2);
        b.mod (MS::Lfo1, I.osc[0].pan, 0.2f, true, MS::Macro2);
        b.chorus (0, 6.0f, 0.2f, 0.35f);
        auto d = b.dist (0, fxchoices::Tube, 0.15f, 0.5f);
        auto r = b.reverb (0, 0, 0.4f, 0.4f, 0.12f);
        b.v[I.macro[1]] = 0.5f;
        b.macro (0, I.osc[0].wtpos, 1.0f);                          // registrations
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, d.param (5), 0.5f);
        b.master (2.0f);
    }));

    l.push_back (make ("Clav", "Keys", "A narrow pulse through a band-pass with a fast envelope, short and funky.",
                       { "clav", "funk", "percussive" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.9f, 0.8f);
        b.v[I.filter[0].level] = 2.0f;            // pulse 25 %
        b.filter (0, "Band 12", 1200.0f, 0.3f);
        b.v[I.filter[0].keytrack] = 1.0f;
        b.env (0, 0.001f, 0.9f, -60.0f, 0.06f);
        b.env (1, 0.001f, 0.15f, -60.0f, 0.1f);
        b.mod (MS::Env2, I.filter[0].cutoff, 1.5f * kOct);
        b.velocity (0.3f);
        b.mod (MS::Velocity, I.filter[0].cutoff, 1.0f * kOct);
        b.fx (0, FxType::Phaser).mix (0.25f);
        b.comp (0, -20.0f, 3.0f, 3.0f, 60.0f, 0.0f);
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (1, I.osc[0].wtpos, 0.2f);
        b.macro (2, I.filter[0].res, 0.4f);
        b.macro (3, I.env[0].decay, 0.3f);
        b.master (6.0f);
    }));

    l.push_back (make ("Soft Keys", "Keys", "A skewed triangle with a gentle filter decay and a warm plate.",
                       { "soft", "mellow", "piano" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Tri-Saw Skew", 0.7f, 0.4f);
        b.filter (0, "MG Low 12", 900.0f, 0.05f);
        b.v[I.filter[0].keytrack] = 1.0f;
        b.env (0, 0.004f, 2.5f, -60.0f, 0.45f);
        b.env (1, 0.002f, 0.8f, -60.0f, 0.4f);
        b.mod (MS::Env2, I.filter[0].cutoff, 2.0f * kOct);
        b.velocity (0.3f);
        b.mod (MS::Velocity, I.filter[0].cutoff, 1.0f * kOct);
        b.chorus (0, 0.3f, 0.2f, 0.25f);
        auto r = b.reverb (0, 0, 0.5f, 0.5f, 0.2f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.osc[0].wtpos, 0.5f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[0].decay, 0.2f);
        b.master (0.0f);
    }));

    l.push_back (make ("Bell Keys", "Keys", "A bell spectrum whose upper partials decay faster than the fundamental, in a hall.",
                       { "bell", "glass", "tuned percussion" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Spectral", "Bell Spectrum", 0.65f, 0.0f);
        b.osc (1, "Analog", "Basic Shapes", 0.3f, 0.0f);
        b.pitch (1, 2);
        b.route (1, 0);
        b.filterOff (0);
        b.env (0, 0.001f, 3.0f, -60.0f, 1.0f);
        b.env (1, 0.001f, 1.2f, -60.0f, 0.5f);
        b.mod (MS::Env2, I.osc[0].wtpos, -1.0f);
        b.v[I.osc[0].wtpos] = 1.0f;                                 // bright at the attack, darker as it rings
        b.mod (MS::Env2, I.osc[1].level, 0.3f);
        b.v[I.osc[1].level] = 0.0f;
        b.velocity (0.3f);
        auto r = b.reverb (0, 1, 0.7f, 0.6f, 0.25f);
        b.macro (0, I.osc[0].wtpos, -0.5f);
        b.macro (1, I.env[1].decay, 0.3f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[0].decay, 0.2f);
        b.master (-4.0f);
    }));
}

// ===========================================================================
// Arp / Seq
// ===========================================================================
void arpSlot (B& b, ArpShape shape, const char* rate, int range, float gate)
{
    auto& s = b.p.arp.slots[0];
    s.shape = shape;
    s.rate = syncRate (rate);
    s.range = range;
    s.gate = gate;
    b.v[b.I.global.arpOn] = 1.0f;
}

void addArp (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Up Arp", "Arp / Seq", "Sixteenth-note arpeggio rising over two octaves, plucked saw with ping-pong delay and plate sends.",
                       { "arp", "trance", "pluck" }, [] (B& b)
    {
        const auto& I = b.I;
        arpSlot (b, ArpShape::Up, "1/16", 1, 0.6f);
        b.osc (0, "Analog", "Analog Saw", 0.65f, 1.0f);
        b.unison (0, 2, 0.08f, 0.5f, 0.6f);
        b.filter (0, "MG Low 24", 500.0f, 0.25f);
        b.env (0, 0.001f, 0.25f, -60.0f, 0.12f);
        b.env (1, 0.001f, 0.12f, -60.0f, 0.1f);
        b.mod (MS::Env2, I.filter[0].cutoff, 3.5f * kOct);
        // Sends: delay on BUS 1, plate on BUS 2.
        b.delay (kFxBus1, "1/8", 0.4f, 1.0f);
        b.reverb (kFxBus2, 0, 0.5f, 0.45f, 1.0f);
        b.v[I.filter[0].bus1] = 0.3f;
        b.v[I.filter[0].bus2] = 0.2f;
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.env[1].decay, 0.2f);
        b.macro (2, I.filter[0].bus1, 0.3f);
        b.macro (2, I.filter[0].bus2, 0.3f);
        b.macro (3, I.filter[0].res, 0.35f);
        b.master (2.0f);
    }));

    l.push_back (make ("Random Bleeps", "Arp / Seq", "Random-order square bleeps over two octaves, with sample-and-hold on the filter.",
                       { "arp", "random", "sample and hold" }, [] (B& b)
    {
        const auto& I = b.I;
        arpSlot (b, ArpShape::Random, "1/16", 1, 0.4f);
        b.osc (0, "Analog", "Basic Shapes", 0.6f, 0.6f);
        b.filter (0, "MG Low 12", 1200.0f, 0.4f);
        b.env (0, 0.001f, 0.15f, -60.0f, 0.08f);
        b.v[I.lfo[0].type] = 4.0f;                                  // S&H
        b.lfoSync (0, "1/16", 0);
        b.mod (MS::Lfo1, I.filter[0].cutoff, 3.0f * kOct, true, MS::Macro2);
        auto d = b.delay (0, "1/8", 0.4f, 0.25f);
        b.reverb (0, 0, 0.5f, 0.5f, 0.15f);
        b.v[I.macro[1]] = 0.6f;
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (2, d.mixParam(), 0.3f);
        b.macro (3, I.osc[0].wtpos, -0.2f);
        b.master (6.0f);
    }));

    l.push_back (make ("Tresillo Gate", "Arp / Seq", "Held chords chopped into a 3-3-2 rhythm by a stepped LFO on the amp.",
                       { "gate", "rhythmic", "chords" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.5f, 0.8f);
        b.unison (0, 3, 0.12f, 0.6f, 1.0f);
        b.filter (0, "MG Low 12", 2500.0f, 0.1f);
        b.lfoShape (0, "Rhythmic", "Tresillo");
        b.lfoSync (0, "1 bar", 1);
        b.v[I.lfo[0].smooth] = 0.04f;
        b.v[I.global.amp] = 0.0f;
        b.mod (MS::Lfo1, I.global.amp, 1.0f);
        b.env (0, 0.005f, 1.0f, 0.0f, 0.2f);
        b.delay (0, "1/8", 0.3f, 0.15f);
        auto r = b.reverb (0, 0, 0.5f, 0.5f, 0.2f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.lfo[0].smooth, 0.3f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.global.amp, 0.5f);                            // gate depth down
        b.master (-2.0f);
    }));

    l.push_back (make ("Trance Gate", "Arp / Seq", "A supersaw chord gated in sixteenths with an offbeat accent pattern.",
                       { "gate", "trance", "supersaw" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.45f, 1.0f);
        b.unison (0, 6, 0.2f, 0.7f, 1.0f, 1);
        b.filter (0, "MG Low 12", 3500.0f, 0.1f);
        b.lfoShape (0, "Rhythmic", "Stutter");
        b.lfoSync (0, "1 bar", 1);
        b.v[I.lfo[0].smooth] = 0.03f;
        b.v[I.global.amp] = 0.0f;
        b.mod (MS::Lfo1, I.global.amp, 1.0f);
        b.env (0, 0.003f, 1.0f, 0.0f, 0.2f);
        b.eq (0, 250.0f, -4.0f, 6000.0f, 2.0f);
        auto d = b.delay (0, "1/16", 0.3f, 0.15f);
        auto r = b.reverb (0, 0, 0.6f, 0.5f, 0.2f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.osc[0].detune, 0.3f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (2, d.mixParam(), 0.2f);
        b.macro (3, I.global.amp, 0.5f);
        b.master (0.0f);
    }));

    l.push_back (make ("Octave Bounce", "Arp / Seq", "Eighth-note up-down arpeggio jumping octaves, a sync-warped tone with a filter envelope.",
                       { "arp", "octave", "sync" }, [] (B& b)
    {
        const auto& I = b.I;
        arpSlot (b, ArpShape::UpDown, "1/8", 1, 0.5f);
        b.osc (0, "Analog", "Basic Shapes", 0.6f, 0.4f);
        b.warp (0, 0, "Sync", 0.2f);
        b.sub (0.35f, -1, 0);
        b.filter (0, "MG Low 24", 700.0f, 0.3f);
        b.env (0, 0.001f, 0.3f, -60.0f, 0.15f);
        b.env (1, 0.001f, 0.18f, -60.0f, 0.1f);
        b.mod (MS::Env2, I.filter[0].cutoff, 3.0f * kOct);
        b.mod (MS::Env2, I.osc[0].warp1, 0.3f);
        auto d = b.delay (0, "1/4", 0.3f, 0.15f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.osc[0].warp1, 0.3f);
        b.macro (2, d.mixParam(), 0.3f);
        b.macro (3, I.filter[0].res, 0.3f);
        b.master (5.0f);
    }));
}

// ===========================================================================
// FX
// ===========================================================================
void addFx (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Riser", "FX", "Noise and a saw rising in pitch and brightness over two bars, into a long hall.",
                       { "riser", "build", "noise" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.4f, 1.0f);
        b.unison (0, 5, 0.25f, 0.7f, 1.0f);
        b.noise (1, 0.4f);
        b.filter (0, "MG Low 12", 300.0f, 0.3f);
        // LFO 1 in envelope mode: one ramp over 2 bars.
        b.lfoShape (0, "Basic", "Saw Up");
        b.lfoSync (0, "2 bars", 2);
        b.mod (MS::Lfo1, I.filter[0].cutoff, 5.0f * kOct);
        b.mod (MS::Lfo1, I.osc[0].coarse, coarseSemis (12.0f));
        b.env (0, 0.3f, 1.0f, 0.0f, 1.0f);
        auto r = b.reverb (0, 1, 0.8f, 0.6f, 0.35f);
        b.fx (0, FxType::Flanger).mix (0.3f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.osc[0].coarse, coarseSemis (12.0f));
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.noise.level, 0.4f);
        b.master (3.0f);
    }));

    l.push_back (make ("Downlifter", "FX", "A detuned saw and noise falling two octaves over a bar while the filter closes.",
                       { "downlifter", "transition", "fall" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.45f, 1.0f);
        b.unison (0, 4, 0.2f, 0.7f, 1.0f);
        b.noise (2, 0.3f);
        b.filter (0, "MG Low 12", 300.0f, 0.2f);
        b.lfoShape (0, "Basic", "Saw Down");
        b.lfoSync (0, "1 bar", 2);
        b.mod (MS::Lfo1, I.filter[0].cutoff, 5.0f * kOct);
        b.mod (MS::Lfo1, I.osc[0].coarse, coarseSemis (24.0f));
        b.v[I.osc[0].coarse] = -12.0f;
        b.env (0, 0.002f, 1.0f, 0.0f, 0.8f);
        auto d = b.delay (0, "1/8", 0.35f, 0.2f);
        auto r = b.reverb (0, 1, 0.7f, 0.5f, 0.3f);
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (1, I.lfo[0].rateSync, -0.18f);                    // slower fall
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, d.mixParam(), 0.3f);
        b.master (5.0f);
    }));

    l.push_back (make ("Laser", "FX", "A fast downward pitch sweep on an FM tone, repeated by a delay.",
                       { "laser", "zap", "pitch" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Digital", "FM 2-1", 0.85f, 0.5f);
        b.filterOff (0);
        b.env (0, 0.001f, 0.35f, -60.0f, 0.1f);
        b.env (1, 0.0f, 0.25f, -60.0f, 0.1f);
        b.envCurves (1, 0.0f, -0.8f, -0.6f);
        b.mod (MS::Env2, I.osc[0].coarse, coarseSemis (40.0f));
        b.mod (MS::Env2, I.osc[0].wtpos, 0.5f);
        auto d = b.delay (0, "1/8", 0.45f, 0.3f);
        b.macro (0, I.osc[0].wtpos, 0.5f);
        b.macro (1, I.env[1].decay, 0.3f);
        b.macro (2, d.mixParam(), 0.3f);
        b.macro (3, I.osc[0].coarse, coarseSemis (-12.0f));
        b.master (6.0f);
    }));

    l.push_back (make ("Ring Clang", "FX", "A saw ring-modulated by an inharmonic sine, metallic, with a short tail.",
                       { "ring mod", "metallic", "inharmonic" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 1.0f, 0.4f);
        b.osc (1, "Analog", "Basic Shapes", 0.6f, 0.0f);
        b.v[I.osc[1].pitchMode] = 2.0f;
        b.v[I.osc[1].ratio] = 2.37f;
        b.route (1, 3);
        b.warp (0, 0, "RM (B)", 1.0f);
        b.filter (0, "MG Low 12", 6000.0f, 0.1f);
        b.v[I.filter[0].level] = 6.0f;
        b.env (0, 0.001f, 1.2f, -60.0f, 0.5f);
        b.lfoShape (0, "Basic", "Exp Down");
        b.lfoSync (0, "1/2", 2);
        b.mod (MS::Lfo1, I.osc[1].ratio, 0.05f);
        auto r = b.reverb (0, 0, 0.5f, 0.5f, 0.25f);
        b.macro (0, I.filter[0].cutoff, -2.0f * kOct);
        b.macro (1, I.osc[1].ratio, 0.2f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.osc[0].warp1, -0.5f);
        b.master (6.0f);
    }));
}

// ===========================================================================
// Atmosphere
// ===========================================================================
void addAtmos (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Rain Room", "Atmosphere", "A soft sine chord under a rain recording, filtered and in a big hall.",
                       { "rain", "ambient", "texture" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.5f, 0.15f);
        b.unison (0, 3, 0.06f, 0.6f, 1.0f);
        b.noiseSample ("Organics", "Rain", 0.35f);
        b.v[I.noise.oneShot] = 0.0f;
        b.filter (0, "MG Low 12", 3500.0f, 0.05f);
        b.env (0, 1.2f, 1.0f, 0.0f, 2.0f);
        auto r = b.reverb (0, 1, 0.85f, 0.65f, 0.4f);
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (1, I.noise.level, 0.4f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.osc[0].wtpos, 0.2f);
        b.master (-1.0f);
    }));

    l.push_back (make ("Night Wind", "Atmosphere", "Pink noise through a band-pass moved by a Rossler chaos LFO, with a wind recording.",
                       { "wind", "noise", "chaos" }, [] (B& b)
    {
        const auto& I = b.I;
        b.v[I.osc[0].enable] = 0.0f;
        b.noise (2, 1.0f);
        b.filter (0, "Band 12", 700.0f, 0.55f);
        b.v[I.lfo[0].type] = 3.0f;                                  // Rossler
        b.lfoHz (0, 0.12f, 0);
        b.mod (MS::Lfo1, I.filter[0].cutoff, 2.5f * kOct, true, MS::Macro2);
        b.env (0, 1.5f, 1.0f, 0.0f, 2.5f);
        b.filter (1, "MG Low 12", 4000.0f, 0.05f);
        b.v[I.filter[1].level] = 6.0f;
        b.v[I.filter[0].route] = 0.0f;
        b.v[I.filter[1].route] = 1.0f;
        auto r = b.reverb (0, 1, 0.8f, 0.6f, 0.4f);
        b.v[I.macro[1]] = 0.7f;
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.filter[0].res, 0.35f);
        b.master (6.0f);
    }));

    l.push_back (make ("Deep Drone", "Atmosphere", "Low detuned saws and a sub under a slowly drifting low-pass, with a long release.",
                       { "drone", "dark", "evolving" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Analog Saw", 0.45f, 0.7f);
        b.pitch (0, -1);
        b.unison (0, 4, 0.1f, 0.7f, 1.0f);
        b.osc (1, "Spectral", "Odd Sweep", 0.35f, 0.4f);
        b.route (1, 0);
        b.sub (0.3f, -1, 0, true);
        b.filter (0, "MG Low 24", 600.0f, 0.25f);
        b.env (0, 2.0f, 1.0f, 0.0f, 4.0f);
        b.v[I.lfo[0].type] = 3.0f;
        b.lfoHz (0, 0.08f, 0);
        b.mod (MS::Lfo1, I.filter[0].cutoff, 2.0f * kOct, true, MS::Macro2);
        b.lfoShape (1, "Basic", "Sine");
        b.lfoSync (1, "8 bars", 0);
        b.mod (MS::Lfo2, I.osc[1].wtpos, 0.4f, true);
        auto r = b.reverb (0, 4, 0.8f, 0.6f, 0.3f);
        b.v[I.macro[1]] = 0.7f;
        b.macro (0, I.filter[0].cutoff, 2.0f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.filter[0].drive, 0.5f);
        b.master (-4.0f);
    }));

    l.push_back (make ("Formant Haze", "Atmosphere", "A talk-box table stepped by a smoothed sample-and-hold LFO, in a Nitrous reverb.",
                       { "formant", "texture", "random" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Vowel", "Talk Box", 0.5f, 0.0f);
        b.unison (0, 4, 0.12f, 0.7f, 1.0f);
        b.v[I.lfo[0].type] = 4.0f;                                  // S&H
        b.lfoSync (0, "1/4", 1);
        b.v[I.lfo[0].smooth] = 0.5f;
        b.mod (MS::Lfo1, I.osc[0].wtpos, 1.0f, false, MS::Macro2);
        b.filter (0, "MG Low 12", 5000.0f, 0.1f);
        b.env (0, 0.9f, 1.0f, 0.0f, 2.0f);
        auto r = b.reverb (0, 3, 0.8f, 0.7f, 0.4f);
        b.v[I.macro[1]] = 0.8f;
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.lfo[0].smooth, 0.4f);
        b.master (-2.0f);
    }));

    l.push_back (make ("Tape Memory", "Atmosphere", "A detuned sine and triangle chord with tape hiss, wow, chorus and a downsampler.",
                       { "lofi", "tape", "nostalgic" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.55f, 0.1f);
        b.unison (0, 2, 0.05f, 0.5f, 0.6f);
        b.noiseSample ("Analog", "Tape Hiss", 0.3f);
        b.filter (0, "MG Low 12", 2800.0f, 0.05f);
        b.env (0, 0.4f, 1.0f, 0.0f, 1.5f);
        b.lfoShape (0, "Basic", "Sine");
        b.lfoHz (0, 0.6f, 0);
        b.mod (MS::Lfo1, I.global.tuning, tuneSemis (0.12f), true, MS::Macro2);
        b.chorus (0, 0.5f, 0.3f, 0.4f);
        auto d = b.dist (0, fxchoices::Downsample, 0.25f, 0.4f);
        auto r = b.reverb (0, 2, 0.6f, 0.55f, 0.3f);
        b.v[I.macro[1]] = 0.5f;
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, d.mixParam(), 0.5f);
        b.master (2.0f);
    }));
}

// ===========================================================================
// Drum (pitch tracking off: the same drum on every key)
// ===========================================================================
void addDrums (std::vector<FactoryPreset>& l)
{
    l.push_back (make ("Kick", "Drum", "A sine with a fast pitch drop and a click, the same on every key.",
                       { "kick", "808", "one shot" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.9f, 0.0f);
        b.v[I.osc[0].pitchTrack] = 0.0f;
        b.pitch (0, 2, 7);                                          // MIDI 31, ~49 Hz
        b.v[I.osc[0].rand] = 0.0f;
        b.v[I.osc[0].phase] = 90.0f;
        b.filterOff (0);
        b.env (0, 0.001f, 0.45f, -60.0f, 0.05f);
        b.envCurves (0, 0.0f, -0.4f, -0.6f);
        b.env (1, 0.0f, 0.07f, -60.0f, 0.05f);
        b.mod (MS::Env2, I.osc[0].coarse, coarseSemis (36.0f));
        b.noiseSample ("Attacks", "Click", 0.3f, false);
        b.v[I.noise.oneShot] = 1.0f;
        auto d = b.dist (0, fxchoices::Tube, 0.15f, 0.5f);
        b.macro (0, I.env[1].decay, 0.15f);                         // longer pitch drop
        b.macro (1, I.env[0].decay, 0.2f);
        b.macro (2, I.osc[0].wtpos, 0.25f);                        // sine -> triangle
        b.macro (3, d.param (5), 0.5f);
        b.master (2.5f);
    }));

    l.push_back (make ("Snare", "Drum", "A pitched triangle body and a band-passed noise burst.",
                       { "snare", "one shot" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.6f, 0.2f);
        b.v[I.osc[0].pitchTrack] = 0.0f;
        b.pitch (0, 4, 7);                                          // ~196 Hz
        b.route (0, 1);
        b.noise (1, 0.7f);
        b.filter (0, "Band 12", 3500.0f, 0.15f);
        b.env (0, 0.001f, 0.22f, -60.0f, 0.05f);
        b.env (1, 0.0f, 0.04f, -60.0f, 0.05f);
        b.mod (MS::Env2, I.osc[0].coarse, coarseSemis (12.0f));
        b.env (2, 0.0f, 0.09f, -60.0f, 0.05f);
        b.v[I.osc[0].level] = 0.0f;
        b.mod (MS::Env3, I.osc[0].level, 0.6f);
        b.comp (0, -18.0f, 4.0f, 2.0f, 60.0f, 3.0f);
        auto r = b.reverb (0, 0, 0.3f, 0.3f, 0.12f);
        b.macro (0, I.filter[0].cutoff, 1.5f * kOct);
        b.macro (1, I.env[0].decay, 0.2f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.osc[0].coarse, coarseSemis (7.0f));
        b.master (3.0f);
    }));

    l.push_back (make ("Closed Hat", "Drum", "White noise through a steep high-pass with a very short decay.",
                       { "hihat", "one shot" }, [] (B& b)
    {
        const auto& I = b.I;
        b.v[I.osc[0].enable] = 0.0f;
        b.noise (1, 0.9f);
        b.filter (0, "High 24", 7000.0f, 0.2f);
        b.v[I.filter[0].level] = 9.0f;
        b.env (0, 0.0f, 0.07f, -60.0f, 0.03f);
        b.envCurves (0, 0.0f, -0.7f, -0.6f);
        b.eq (0, 400.0f, -6.0f, 10000.0f, 3.0f);
        b.macro (0, I.filter[0].cutoff, -1.0f * kOct);
        b.macro (1, I.env[0].decay, 0.3f);                          // towards an open hat
        b.macro (2, I.filter[0].res, 0.3f);
        b.macro (3, I.noise.tilt, 0.3f, true);
        b.master (6.0f);
    }));

    l.push_back (make ("Clap", "Drum", "Three quick band-passed noise bursts and a short tail, shaped by an envelope-mode LFO.",
                       { "clap", "one shot" }, [] (B& b)
    {
        const auto& I = b.I;
        b.v[I.osc[0].enable] = 0.0f;
        b.noise (1, 0.9f);
        b.filter (0, "Band 12", 1400.0f, 0.25f);
        b.v[I.filter[0].level] = 9.0f;
        b.env (0, 0.0f, 0.25f, -60.0f, 0.08f);
        // The bursts: LFO 1, one 60 ms pass in envelope mode.
        Curve c;
        c.points = { { 0.0f, 1.0f, 0.0f, false }, { 0.2f, 0.15f, 0.0f, false }, { 0.33f, 1.0f, 0.0f, true },
                     { 0.53f, 0.15f, 0.0f, false }, { 0.66f, 1.0f, 0.0f, true }, { 1.0f, 0.35f, 0.0f, false } };
        b.p.lfo[0].shape = c;
        b.p.lfo[0].shapeName = "Clap Bursts";
        b.lfoHz (0, 16.0f, 2);
        b.v[I.global.amp] = 0.0f;
        b.mod (MS::Lfo1, I.global.amp, 1.0f);
        auto r = b.reverb (0, 0, 0.3f, 0.3f, 0.18f);
        b.macro (0, I.filter[0].cutoff, 1.0f * kOct);
        b.macro (1, I.env[0].decay, 0.2f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.filter[0].res, 0.3f);
        b.master (6.0f);
    }));

    l.push_back (make ("Tom", "Drum", "A sine with a medium pitch drop and a soft noise attack.",
                       { "tom", "one shot" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.85f, 0.05f);
        b.v[I.osc[0].pitchTrack] = 0.0f;
        b.pitch (0, 3, 9);                                          // ~110 Hz
        b.v[I.osc[0].rand] = 0.0f;
        b.filterOff (0);
        b.env (0, 0.001f, 0.5f, -60.0f, 0.08f);
        b.env (1, 0.0f, 0.15f, -60.0f, 0.05f);
        b.mod (MS::Env2, I.osc[0].coarse, coarseSemis (9.0f));
        b.noise (2, 0.0f, false);
        b.env (2, 0.0f, 0.03f, -60.0f, 0.03f);
        b.mod (MS::Env3, I.noise.level, 0.3f);
        auto r = b.reverb (0, 0, 0.35f, 0.35f, 0.15f);
        b.macro (0, I.osc[0].coarse, coarseSemis (12.0f));
        b.macro (1, I.env[0].decay, 0.2f);
        b.macro (2, r.mixParam(), 0.3f);
        b.macro (3, I.env[1].decay, 0.2f);
        b.master (6.0f);
    }));

    l.push_back (make ("Rim", "Drum", "A short click and a high band-passed tone, dry and tight.",
                       { "rim", "percussion", "one shot" }, [] (B& b)
    {
        const auto& I = b.I;
        b.osc (0, "Analog", "Basic Shapes", 0.7f, 0.2f);
        b.v[I.osc[0].pitchTrack] = 0.0f;
        b.pitch (0, 4, 0, 0.0f);
        b.v[I.osc[0].coarse] = 24.0f + 7.0f;                        // ~ 830 Hz
        b.noiseSample ("Attacks", "Tick", 0.6f, true);
        b.v[I.noise.oneShot] = 1.0f;
        b.filter (0, "Band 12", 1700.0f, 0.4f);
        b.v[I.filter[0].level] = 12.0f;
        b.env (0, 0.0f, 0.06f, -60.0f, 0.03f);
        b.envCurves (0, 0.0f, -0.7f, -0.6f);
        b.macro (0, I.filter[0].cutoff, 1.0f * kOct);
        b.macro (1, I.env[0].decay, 0.2f);
        b.macro (2, I.filter[0].res, 0.3f);
        b.macro (3, I.osc[0].coarse, coarseSemis (-12.0f));
        b.master (6.0f);
    }));
}
} // namespace

std::vector<FactoryPreset> buildFactorySounds()
{
    std::vector<FactoryPreset> l;
    l.push_back ({ "Init", "Init", "OSC A playing the Default Shapes table into Filter 1 (MG Low 12, fully open).", { "init" },
                   [] (Patch&, ParamValues&) {} });
    addBass (l);
    addLead (l);
    addPad (l);
    addPluck (l);
    addKeys (l);
    addArp (l);
    addFx (l);
    addAtmos (l);
    addDrums (l);
    return l;
}
} // namespace tincture
