#include "ParamIds.h"

namespace tincture
{
namespace choices
{
#define TINCTURE_LIST(name, ...) const std::vector<std::string>& name() { static const std::vector<std::string> l { __VA_ARGS__ }; return l; }
TINCTURE_LIST (oscTypes, "Wavetable", "Multisample", "Sample", "Granular", "Spectral")
TINCTURE_LIST (routes, "Filter", "Main", "Direct", "None")
TINCTURE_LIST (unisonModes, "Linear", "Super", "Exp", "Inv", "Random")
TINCTURE_LIST (unisonStacks, "Off", "12 (1-3x)", "12+7 (1-3x)", "Center-12", "Center-24")
TINCTURE_LIST (phaseMemory, "All Voices", "Contiguous", "Per Voice")
TINCTURE_LIST (pitchModes, "Semitones", "Harmonics", "Ratio")
TINCTURE_LIST (subShapes, "Sine", "Rounded Rect", "Triangle", "Saw", "Square", "Pulse")
TINCTURE_LIST (noiseColours, "Sample", "White", "Pink", "Brown", "Geiger")
TINCTURE_LIST (loopModes, "One-shot", "Fwd Loop", "Rev Loop", "Fwd/Rev Loop", "Tailed")
TINCTURE_LIST (filterRoutes1, "Filter 2", "Main", "Direct", "None")
TINCTURE_LIST (filterRoutes2, "Filter 1", "Main", "Direct", "None")
TINCTURE_LIST (busRoutes1, "Main", "Direct", "Bus 2")
TINCTURE_LIST (busRoutes2, "Main", "Direct", "Bus 1")
TINCTURE_LIST (lfoTypes, "Normal", "Path", "Chaos: Lorenz", "Chaos: Rossler", "S&H")
TINCTURE_LIST (lfoModes, "Free", "Retrig", "Envelope")
TINCTURE_LIST (lfoDirections, "Forward", "Reverse", "Ping Pong")
TINCTURE_LIST (syncRates, "8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32", "1/64", "1/128", "1/256")
TINCTURE_LIST (keys, "-", "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B")
TINCTURE_LIST (winShapes, "Hann", "Welch", "Gaussian", "Blackman-Harris", "Sinc", "Tukey", "Triangle", "Trapezoid", "ExpDec", "Exp Dec Rev")
TINCTURE_LIST (voiceSteal, "Newest", "Oldest", "Highest", "Lowest", "Velocity")
TINCTURE_LIST (scales,
    "Major", "Dorian", "Phrygian", "Lydian", "Mixolydian", "Minor", "Locrian", "Minor (Harmonic)", "Romanian Minor",
    "Gypsy", "Algerian", "Hindu", "Nat Bhairava", "Acoustic", "Arabian", "Flamenco", "Klezmer", "Neopolitan", "Persian",
    "Purvi Theta", "Todi Theta", "Ultra Phrygian", "Iwato",
    "Insen", "Japanese", "Balinese", "Hirajoshi", "Akebono", "Asian", "Egyptian", "Major Pentatonic", "Minor Pentatonic",
    "Major Blues", "Minor Blues", "Piongio", "Hexatonic", "Ancient Chinese", "Augmented", "Whole Tone", "Whole Dim.",
    "Half Dim.", "Minor (Melodic)", "Major+b7", "Eastern", "Bebop Minor", "Bebop Major",
    "Polymode", "Adonai Malakh", "Nine Tone", "Major/Minor", "Chromatic")
TINCTURE_LIST (warpModes,
    "Off", "Sync",
    "Bend +", "Bend -", "Bend +/-", "PWM", "Asym +", "Asym -", "Asym +/-", "Flip", "Mirror",
    "Remap 1", "Remap 2", "Remap 3", "Remap 4", "Quantize", "Odd/Even",
    "LPF", "HPF",
    "Tube", "Soft Clip", "Hard Clip", "Diode 1", "Diode 2", "Linear Fold", "Sine Fold", "Zero-Square", "Asym",
    "Rectify", "Sine Shaper", "Stomp Box", "Tape Sat.", "Soft Sat.",
    "FM (A)", "FM (B)", "FM (C)", "FM (Noise)", "FM (Sub)", "FM (Filter 1)", "FM (Filter 2)",
    "PD (A)", "PD (B)", "PD (C)", "PD (Noise)", "PD (Sub)", "PD (Filter 1)", "PD (Filter 2)", "PD (Self)",
    "AM (A)", "AM (B)", "AM (C)", "AM (Noise)", "AM (Sub)", "AM (Filter 1)", "AM (Filter 2)",
    "RM (A)", "RM (B)", "RM (C)", "RM (Noise)", "RM (Sub)", "RM (Filter 1)", "RM (Filter 2)")
TINCTURE_LIST (fmTypes, "Phase", "Thru-Zero", "Exp", "Linear")
#undef TINCTURE_LIST

}

double syncRateBeats (int i)
{
    static const double b[] { 32.0, 16.0, 8.0, 4.0, 2.0, 1.0, 0.5, 0.25, 0.125, 0.0625, 0.03125, 0.015625 };
    return b[i < 0 ? 0 : (i > 11 ? 11 : i)];
}

namespace
{
ParamIdTable table;
constexpr uint32_t V  = kModulatable | kPerVoice;      // per-voice, modulatable
constexpr uint32_t G  = kModulatable;                  // global, modulatable
constexpr uint32_t NA = kNoAutomate;                   // patch setting, not a host parameter
constexpr uint32_t H  = 0;                             // host-automatable, not modulatable

void routing (ParamRegistry& r, const std::string& p, const std::string& n, int defRoute, int& route, int& bal, int& b1, int& b2, int& env)
{
    route = r.choice (p + ".route", n + " Route", "ROUTE", choices::routes(), defRoute, H);
    bal   = r.percent (p + ".fbal", n + " Filter Balance", "BAL", 0.0f, V);
    b1    = r.percent (p + ".bus1", n + " Bus 1", "1", 0.0f, V);
    b2    = r.percent (p + ".bus2", n + " Bus 2", "2", 0.0f, V);
    env   = r.toggle (p + ".env1", n + " Env 1 Gate", "ENV", true, NA);
}
}

const ParamIdTable& ids() { return table; }

void registerAllParams (ParamRegistry& r)
{
    auto& t = table;
    const char* letters[] { "A", "B", "C" };

    // ---- Global ----------------------------------------------------------
    r.setGroup ("GLOBAL");
    auto& g = t.global;
    g.master      = r.linear ("G.master", "Master Volume", "MAIN", -60.0f, 12.0f, 0.0f, Unit::Db, G);
    g.amp         = r.percent ("G.amp", "Amp", "AMP", 1.0f, V);
    g.mainLevel   = r.linear ("G.main", "Main Level", "MAIN", -60.0f, 6.0f, 0.0f, Unit::Db, G);
    g.directLevel = r.linear ("G.direct", "Direct Level", "DIRECT", -60.0f, 12.0f, 0.0f, Unit::Db, G);
    g.bus1Level   = r.linear ("Bus1.level", "Bus 1 Level", "LEVEL", -60.0f, 12.0f, 0.0f, Unit::Db, G);
    g.bus1Route   = r.choice ("Bus1.route", "Bus 1 Route", "ROUTE", choices::busRoutes1(), 0, H);
    g.bus2Level   = r.linear ("Bus2.level", "Bus 2 Level", "LEVEL", -60.0f, 12.0f, 0.0f, Unit::Db, G);
    g.bus2Route   = r.choice ("Bus2.route", "Bus 2 Route", "ROUTE", choices::busRoutes2(), 0, H);
    g.poly        = r.integer ("G.poly", "Poly", "POLY", 1, kMaxVoices, 8, Unit::None, H);
    g.mono        = r.toggle ("G.mono", "Mono", "MONO", false, H);
    g.legato      = r.toggle ("G.legato", "Legato", "LEGATO", false, H);
    g.porta       = r.time ("G.porta", "Portamento", "PORTA", 10.0f, 0.0f, V);
    g.portaCurve  = r.bipolar ("G.portaCurve", "Porta Curve", "CURVE", 0.0f, V);
    g.portaAlways = r.toggle ("G.portaAlways", "Porta Always", "ALWAYS", false, H);
    g.portaScaled = r.toggle ("G.portaScaled", "Porta Scaled", "SCALED", false, H);
    g.voiceSteal  = r.choice ("G.steal", "Voice Steal Priority", "STEAL", choices::voiceSteal(), 0, NA);
    g.bendUp      = r.integer ("G.bendUp", "Pitch Bend Up", "BEND UP", 0, 48, 2, Unit::Semitones, NA);
    g.bendDown    = r.integer ("G.bendDown", "Pitch Bend Down", "BEND DN", -48, 0, -2, Unit::Semitones, NA);
    g.transpose   = r.integer ("G.transpose", "Transpose", "TRANSPOSE", -48, 48, 0, Unit::Semitones, H);
    g.key         = r.choice ("G.key", "Key", "KEY", choices::keys(), 0, H);
    g.scale       = r.choice ("G.scale", "Scale", "SCALE", choices::scales(), 0, H);
    g.swing       = r.percent ("G.swing", "Swing", "SWING", 0.0f, H);
    g.tuning      = r.linear ("G.tuning", "Main Tuning", "TUNING", -24.0f, 24.0f, 0.0f, Unit::Semitones, V);
    g.concertA    = r.linear ("G.concertA", "Concert Pitch", "A4", 400.0f, 480.0f, 440.0f, Unit::Hz, NA);
    g.velo = g.note = -1;
    g.arpOn       = r.toggle ("G.arp", "Arp On", "ARP", false, H);
    g.clipOn      = r.toggle ("G.clip", "Clip On", "CLIP", false, H);
    g.midiOut     = r.choice ("G.midiOut", "MIDI Out", "MIDI OUT", { "Off", "Clip Player", "On" }, 0, NA);

    // ---- Oscillators A, B, C ----------------------------------------------
    for (int o = 0; o < kNumOscs; ++o)
    {
        const std::string p = letters[o], n = p;
        r.setGroup (std::string ("OSC ") + letters[o]);
        auto& s = t.osc[o];
        s.enable    = r.toggle (p + ".on", n + " Enable", "OSC " + p, o == 0, H);
        s.type      = r.choice (p + ".type", n + " Type", "TYPE", choices::oscTypes(), 0, NA);
        routing (r, p, n, o == 0 ? 0 : 1, s.route, s.filterBal, s.bus1, s.bus2, s.env1Gate);
        s.oct       = r.integer (p + ".oct", n + " Octave", "OCT", -4, 4, 0, Unit::None, V);
        s.sem       = r.integer (p + ".sem", n + " Semitone", "SEM", -12, 12, 0, Unit::Semitones, V);
        s.fine      = r.linear (p + ".fine", n + " Fine", "FIN", -100.0f, 100.0f, 0.0f, Unit::Cents, V);
        s.coarse    = r.linear (p + ".crs", n + " Coarse Pitch", "CRS", -48.0f, 48.0f, 0.0f, Unit::Semitones, V);
        s.pitchMode = r.choice (p + ".pitchMode", n + " Pitch Mode", "MODE", choices::pitchModes(), 0, NA);
        s.ratio     = r.linear (p + ".ratio", n + " Ratio", "RATIO", 0.125f, 16.0f, 1.0f, Unit::Ratio, V);
        s.pitchTrack= r.toggle (p + ".track", n + " Pitch Tracking", "TRACK", true, NA);
        s.bendTrack = r.toggle (p + ".bendTrack", n + " Pitch Bend Tracking", "BEND", true, NA);
        s.level     = r.percent (p + ".level", n + " Level", "LEVEL", 0.75f, V);
        s.pan       = r.bipolar (p + ".pan", n + " Pan", "PAN", 0.0f, V);
        s.wtpos     = r.percent (p + ".wtpos", n + " WT Pos", "WT POS", 0.0f, V);
        s.wtSmooth  = r.toggle (p + ".wtSmooth", n + " WT Smooth Interpolation", "SMOOTH", true, NA);
        s.phase     = r.linear (p + ".phase", n + " Phase", "PHASE", 0.0f, 360.0f, 180.0f, Unit::Degrees, V);
        s.rand      = r.percent (p + ".rand", n + " Random Phase", "RAND", 1.0f, V);
        s.phaseMem  = r.choice (p + ".phaseMem", n + " Phase Memory", "PHASE MEM", choices::phaseMemory(), 0, NA);
        s.phaseLegato = r.toggle (p + ".phaseLegato", n + " Phase Legato", "PH LEG", false, NA);
        s.unison    = r.integer (p + ".unison", n + " Unison", "UNISON", 1, kMaxUnison, 1, Unit::None, H);
        s.detune    = r.percent (p + ".detune", n + " Uni Detune", "DETUNE", 0.25f, V);
        s.blend     = r.percent (p + ".blend", n + " Uni Blend", "BLEND", 0.75f, V);
        s.uniMode   = r.choice (p + ".uniMode", n + " Uni Mode", "MODE", choices::unisonModes(), 0, NA);
        s.uniStack  = r.choice (p + ".uniStack", n + " Uni Stack", "STACK", choices::unisonStacks(), 0, NA);
        s.uniWidth  = r.percent (p + ".uniWidth", n + " Uni Width", "WIDTH", 1.0f, V);
        s.uniRange  = r.linear (p + ".uniRange", n + " Uni Range", "RANGE", 0.0f, 48.0f, 2.0f, Unit::Semitones, V);
        s.uniWtpos  = r.bipolar (p + ".uniWtpos", n + " Uni WT Pos", "WT POS", 0.0f, V);
        s.uniWarp1  = r.bipolar (p + ".uniWarp1", n + " Uni Warp 1", "WARP A", 0.0f, V);
        s.uniWarp2  = r.bipolar (p + ".uniWarp2", n + " Uni Warp 2", "WARP B", 0.0f, V);
        s.uniStart  = r.percent (p + ".uniStart", n + " Uni Start", "START", 0.0f, V);
        s.uniSpan   = r.percent (p + ".uniSpan", n + " Uni Span", "SPAN", 0.0f, V);
        s.warp1Mode = r.choice (p + ".warp1Mode", n + " Warp 1 Mode", "WARP 1", choices::warpModes(), 0, NA);
        s.warp1     = r.percent (p + ".warp1", n + " Warp 1", "WARP 1", 0.0f, V);
        s.warp2Mode = r.choice (p + ".warp2Mode", n + " Warp 2 Mode", "WARP 2", choices::warpModes(), 0, NA);
        s.warp2     = r.percent (p + ".warp2", n + " Warp 2", "WARP 2", 0.0f, V);
        s.warpVar   = r.percent (p + ".warpVar", n + " Warp Var", "VAR", 0.0f, V);
        s.fmType    = r.choice (p + ".fmType", n + " FM Type", "FM", choices::fmTypes(), 0, NA);
        // Sample-based modes.
        s.start     = r.percent (p + ".start", n + " Start", "START", 0.0f, V);
        s.end       = r.percent (p + ".end", n + " End", "END", 1.0f, V);
        s.loopStart = r.percent (p + ".loopStart", n + " Loop Start", "LS", 0.0f, V);
        s.loopEnd   = r.percent (p + ".loopEnd", n + " Loop End", "LE", 1.0f, V);
        s.loopMode  = r.choice (p + ".loopMode", n + " Loop Mode", "LOOP", choices::loopModes(), 0, NA);
        s.xfade     = r.percent (p + ".xfade", n + " Loop Crossfade", "XFADE", 0.0f, V);
        s.scan      = r.linear (p + ".scan", n + " Scan", "SCAN", -2.0f, 2.0f, 1.0f, Unit::Percent, V);
        r.edit (s.scan).displayScale = 100.0f;
        s.timbre    = r.bipolar (p + ".timbre", n + " Timbre", "TIMBRE", 0.0f, V);
        s.density   = r.linear (p + ".density", n + " Grain Density", "DENS", 0.5f, 400.0f, 20.0f, Unit::Hz, V);
        r.edit (s.density).scale = ParamScale::Log;
        s.grainLen  = r.time (p + ".grainLen", n + " Grain Length", "LENGTH", 2.0f, 0.1f, V);
        s.grOffset  = r.percent (p + ".grOffset", n + " Grain Rand Offset", "OFFSET", 0.0f, V);
        s.grDir     = r.percent (p + ".grDir", n + " Grain Rand Dir", "DIR", 0.0f, V);
        s.grPitch   = r.percent (p + ".grPitch", n + " Grain Rand Pitch", "PITCH", 0.0f, V);
        s.grLenRand = r.percent (p + ".grLenRand", n + " Grain Rand Length", "RAND", 0.0f, V);
        s.grPanRand = r.percent (p + ".grPanRand", n + " Grain Rand Pan", "RAND", 0.0f, V);
        s.grGainRand= r.percent (p + ".grGainRand", n + " Grain Rand Gain", "RAND", 0.0f, V);
        s.winAmt    = r.percent (p + ".winAmt", n + " Window Amount", "AMOUNT", 1.0f, V);
        s.winAmtRand= r.percent (p + ".winAmtRand", n + " Window Amount Rand", "RAND", 0.0f, V);
        s.winSkew   = r.bipolar (p + ".winSkew", n + " Window Skew", "SKEW", 0.0f, V);
        s.winSkewRand = r.percent (p + ".winSkewRand", n + " Window Skew Rand", "RAND", 0.0f, V);
        s.winShape  = r.choice (p + ".winShape", n + " Window Shape", "SHAPE", choices::winShapes(), 0, NA);
        s.specLo    = r.logHz (p + ".specLo", n + " Spectral Low", "LO", 20.0f, 20000.0f, 20.0f, V);
        s.specHi    = r.logHz (p + ".specHi", n + " Spectral High", "HI", 20.0f, 20000.0f, 20000.0f, V);
        s.specCut   = r.percent (p + ".specCut", n + " Spectral Cut", "CUT", 0.0f, V);
        s.specMix   = r.percent (p + ".specMix", n + " Spectral Mix", "MIX", 1.0f, V);
    }

    // ---- Sub ---------------------------------------------------------------
    r.setGroup ("SUB");
    {
        auto& s = t.sub;
        s.enable = r.toggle ("Sub.on", "Sub Enable", "SUB", false, H);
        routing (r, "Sub", "Sub", 1, s.route, s.filterBal, s.bus1, s.bus2, s.env1Gate);
        s.oct    = r.integer ("Sub.oct", "Sub Octave", "OCT", -4, 4, 0, Unit::None, V);
        s.coarse = r.linear ("Sub.crs", "Sub Coarse Pitch", "CRS", -48.0f, 48.0f, 0.0f, Unit::Semitones, V);
        s.shape  = r.choice ("Sub.shape", "Sub Shape", "SHAPE", choices::subShapes(), 0, NA);
        s.phase  = r.linear ("Sub.phase", "Sub Phase", "PHASE", 0.0f, 360.0f, 0.0f, Unit::Degrees, V);
        s.contiguous = r.toggle ("Sub.contig", "Sub Contiguous Phase", "CONTIG", false, NA);
        s.pan    = r.bipolar ("Sub.pan", "Sub Pan", "PAN", 0.0f, V);
        s.level  = r.percent ("Sub.level", "Sub Level", "LEVEL", 0.75f, V);
        s.pitchTrack = r.toggle ("Sub.track", "Sub Pitch Tracking", "TRACK", true, NA);
    }

    // ---- Noise -------------------------------------------------------------
    r.setGroup ("NOISE");
    {
        auto& s = t.noise;
        s.enable  = r.toggle ("N.on", "Noise Enable", "NOISE", false, H);
        routing (r, "N", "Noise", 1, s.route, s.filterBal, s.bus1, s.bus2, s.env1Gate);
        s.colour  = r.choice ("N.colour", "Noise Colour", "COLOUR", choices::noiseColours(), 1, NA);
        s.oneShot = r.toggle ("N.oneShot", "Noise One-shot", "ONE SHOT", false, NA);
        s.start   = r.percent ("N.start", "Noise Start", "START", 0.0f, V);
        s.rand    = r.percent ("N.rand", "Noise Random Start", "RAND", 0.0f, V);
        s.pitch   = r.percent ("N.pitch", "Noise Pitch", "PITCH", 0.5f, V);
        s.fine    = r.linear ("N.fine", "Noise Fine", "FINE", -100.0f, 100.0f, 0.0f, Unit::Cents, V);
        s.pan     = r.bipolar ("N.pan", "Noise Pan", "PAN", 0.0f, V);
        s.level   = r.percent ("N.level", "Noise Level", "LEVEL", 0.25f, V);
        s.stereo  = r.percent ("N.stereo", "Noise Stereo", "STEREO", 1.0f, V);
        s.tilt    = r.bipolar ("N.tilt", "Noise Filter", "FILTER", 0.0f, V);
        s.pitchTrack = r.toggle ("N.track", "Noise Pitch Tracking", "TRACK", false, NA);
    }

    // ---- Filters -----------------------------------------------------------
    for (int f = 0; f < kNumFilters; ++f)
    {
        const std::string p = "F" + std::to_string (f + 1), n = "Filter " + std::to_string (f + 1);
        r.setGroup (f == 0 ? "FILTER 1" : "FILTER 2");
        auto& s = t.filter[f];
        s.enable   = r.toggle (p + ".on", n + " Enable", "FILTER " + std::to_string (f + 1), f == 0, H);
        s.type     = r.choice (p + ".type", n + " Type", "TYPE", choices::filterTypes(), 1, NA);   // MG Low 12
        s.cutoff   = r.logHz (p + ".cutoff", n + " Cutoff", "CUTOFF", 8.0f, 22000.0f, 22000.0f, V);
        s.res      = r.percent (p + ".res", n + " Resonance", "RES", 0.1f, V);
        s.drive    = r.percent (p + ".drive", n + " Drive", "DRIVE", 0.0f, V);
        s.var      = r.percent (p + ".var", n + " Var", "FAT", 0.0f, V);
        s.pan      = r.bipolar (p + ".pan", n + " Pan", "PAN", 0.0f, V);
        s.mix      = r.percent (p + ".mix", n + " Mix", "MIX", 1.0f, V);
        s.level    = r.linear (p + ".level", n + " Level", "LEVEL", -60.0f, 12.0f, 0.0f, Unit::Db, V);
        s.keytrack = r.toggle (p + ".keytrack", n + " Keytrack", "KEY", false, NA);
        s.cleanDrive = r.toggle (p + ".clean", n + " Drive Clean Mode", "CLEAN", false, NA);
        s.route    = r.choice (p + ".route", n + " Route", "ROUTE", f == 0 ? choices::filterRoutes1() : choices::filterRoutes2(), 1, H);
        s.bus1     = r.percent (p + ".bus1", n + " Bus 1", "1", 0.0f, V);
        s.bus2     = r.percent (p + ".bus2", n + " Bus 2", "2", 0.0f, V);
        s.chanPan  = r.bipolar (p + ".chanPan", n + " Channel Pan", "PAN", 0.0f, V);
        s.env1Gate = r.toggle (p + ".env1", n + " Env 1 Gate", "ENV", true, NA);
    }

    // ---- Envelopes ---------------------------------------------------------
    for (int e = 0; e < kNumEnvs; ++e)
    {
        const std::string p = "E" + std::to_string (e + 1), n = "Env " + std::to_string (e + 1);
        r.setGroup ("ENV " + std::to_string (e + 1));
        auto& s = t.env[e];
        s.attack  = r.time (p + ".atk", n + " Attack", "ATK", 32.0f, 0.001f, V);
        s.hold    = r.time (p + ".hold", n + " Hold", "HOLD", 32.0f, 0.0f, V);
        s.decay   = r.time (p + ".dec", n + " Decay", "DEC", 32.0f, 1.0f, V);
        s.sustain = r.linear (p + ".sus", n + " Sustain", "SUS", -60.0f, 0.0f, 0.0f, Unit::Db, V);
        s.release = r.time (p + ".rel", n + " Release", "REL", 32.0f, 0.015f, V);
        s.attackCurve  = r.bipolar (p + ".atkCurve", n + " Attack Curve", "A CRV", 0.2f, NA);
        s.decayCurve   = r.bipolar (p + ".decCurve", n + " Decay Curve", "D CRV", -0.6f, NA);
        s.releaseCurve = r.bipolar (p + ".relCurve", n + " Release Curve", "R CRV", -0.6f, NA);
        s.bpm     = r.toggle (p + ".bpm", n + " BPM", "BPM", false, NA);
        s.legatoInvert = r.toggle (p + ".legInv", n + " Legato Inverted", "L", false, NA);
    }

    // ---- LFOs --------------------------------------------------------------
    for (int l = 0; l < kNumLfos; ++l)
    {
        const std::string p = "L" + std::to_string (l + 1), n = "LFO " + std::to_string (l + 1);
        r.setGroup (n);
        auto& s = t.lfo[l];
        s.type      = r.choice (p + ".type", n + " Type", "TYPE", choices::lfoTypes(), 0, NA);
        s.mode      = r.choice (p + ".mode", n + " Mode", "MODE", choices::lfoModes(), 1, NA);
        s.mono      = r.toggle (p + ".mono", n + " Mono", "MONO", false, NA);
        s.direction = r.choice (p + ".dir", n + " Direction", "DIR", choices::lfoDirections(), 0, NA);
        s.gridX     = r.integer (p + ".gridX", n + " Grid X", "GRID X", 1, 32, 8, Unit::None, NA);
        s.gridY     = r.integer (p + ".gridY", n + " Grid Y", "GRID Y", 1, 32, 8, Unit::None, NA);
        s.host      = r.toggle (p + ".host", n + " Host Anchor", "HOST", false, NA);
        s.bpm       = r.toggle (p + ".bpm", n + " BPM", "BPM", true, NA);
        s.rate      = r.linear (p + ".rate", n + " Rate", "RATE", 0.01f, 20.0f, 2.0f, Unit::Hz, V);
        r.edit (s.rate).scale = ParamScale::Log;
        s.rateSync  = r.integer (p + ".sync", n + " Rate (Sync)", "RATE", 0, (int) choices::syncRates().size() - 1, 5, Unit::None, V);
        r.edit (s.rateSync).scale = ParamScale::Choice;
        r.edit (s.rateSync).choices = choices::syncRates();
        s.trip      = r.toggle (p + ".trip", n + " Triplet", "TRIP", false, NA);
        s.dot       = r.toggle (p + ".dot", n + " Dotted", "DOT", false, NA);
        s.swing     = r.toggle (p + ".swing", n + " Swing", "SWING", false, NA);
        s.x10       = r.toggle (p + ".x10", n + " 10x", "10x", false, NA);
        s.rise      = r.time (p + ".rise", n + " Rise", "RISE", 8.0f, 0.0f, V);
        s.delay     = r.time (p + ".delay", n + " Delay", "DELAY", 8.0f, 0.0f, V);
        s.smooth    = r.percent (p + ".smooth", n + " Smooth", "SMOOTH", 0.0f, V);
        s.phase     = r.linear (p + ".phase", n + " Phase", "PHASE", 0.0f, 360.0f, 0.0f, Unit::Degrees, V);
        s.phaseSnap = r.toggle (p + ".phaseSnap", n + " Phase Snap to Grid", "SNAP", false, NA);
    }

    // ---- Macros ------------------------------------------------------------
    r.setGroup ("MACROS");
    for (int m = 0; m < kNumMacros; ++m)
        t.macro[m] = r.percent ("M" + std::to_string (m + 1), "Macro " + std::to_string (m + 1), std::to_string (m + 1), 0.0f, G);

    // ---- FX units ------------------------------------------------------------
    // Racks are built from these units (Fx/FxRack.h decides what each means).
    r.setGroup ("FX");
    for (int u = 0; u < kNumFxUnits; ++u)
    {
        const std::string p = "FX" + std::to_string (u + 1), n = "FX " + std::to_string (u + 1);
        t.fx[u].mix = r.percent (p + ".mix", n + " Mix", "MIX", 1.0f, G);
        for (int k = 0; k < kFxParams; ++k)
            t.fx[u].p[k] = r.percent (p + ".p" + std::to_string (k + 1), n + " P" + std::to_string (k + 1), "P" + std::to_string (k + 1), 0.5f, G);
    }
}
} // namespace tincture
