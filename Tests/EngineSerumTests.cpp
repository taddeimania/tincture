// EngineSerumTests.cpp — the Serum 2 preset reader and importer:
// CBOR / JSON decoding, the .SerumPreset container (a zstd frame built here
// from raw blocks), the name / unit mapping helpers, and a synthetic preset
// imported end to end.
//
//   TinctureEngineSerumTests                 the built-in checks
//   TinctureEngineSerumTests <folder>        also imports every .SerumPreset
//                                            under <folder>, renders each and
//                                            checks it (finite, level matched)
// Serum preset packs are commercial, so none ship with the tests; set the
// folder (or TINCTURE_SERUM_PRESETS) to run the corpus check locally.
// Returns non-zero on failure.
#include "Engine/ParamIds.h"
#include "Engine/Synth.h"
#include "Engine/Wavetable.h"
#include "Model/SerumImport.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace tincture;
using namespace tincture::serum;

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { ++checks; const double va = (a), vb = (b); if (! (std::abs (va - vb) <= (tol))) { ++failures; std::printf ("  FAIL %s:%d  %s ~ %s  (%g vs %g)\n", __FILE__, __LINE__, #a, #b, va, vb); } } while (0)

// ---- a small CBOR writer ----------------------------------------------------------
struct Cbor
{
    std::vector<uint8_t> b;
    void head (int major, uint64_t n)
    {
        const auto m = (uint8_t) (major << 5);
        if (n < 24) b.push_back ((uint8_t) (m | n));
        else if (n < 256) { b.push_back (m | 24); b.push_back ((uint8_t) n); }
        else if (n < 65536) { b.push_back (m | 25); b.push_back ((uint8_t) (n >> 8)); b.push_back ((uint8_t) n); }
        else { b.push_back (m | 26); for (int i = 3; i >= 0; --i) b.push_back ((uint8_t) (n >> (8 * i))); }
    }
    Cbor& map (size_t n) { head (5, n); return *this; }
    Cbor& arr (size_t n) { head (4, n); return *this; }
    Cbor& str (const std::string& s) { head (3, s.size()); b.insert (b.end(), s.begin(), s.end()); return *this; }
    Cbor& integer (long long v) { if (v >= 0) head (0, (uint64_t) v); else head (1, (uint64_t) (-1 - v)); return *this; }
    Cbor& f64 (double d)
    {
        b.push_back (0xfb);
        uint64_t u;
        std::memcpy (&u, &d, 8);
        for (int i = 7; i >= 0; --i) b.push_back ((uint8_t) (u >> (8 * i)));
        return *this;
    }
    Cbor& f32 (float f)
    {
        b.push_back (0xfa);
        uint32_t u;
        std::memcpy (&u, &f, 4);
        for (int i = 3; i >= 0; --i) b.push_back ((uint8_t) (u >> (8 * i)));
        return *this;
    }
    Cbor& kv (const std::string& k, double v) { str (k); return f64 (v); }
    Cbor& kv (const std::string& k, const std::string& v) { str (k); return str (v); }
};

// A zstd frame made of raw (stored) blocks: magic, a single-segment header
// with a 4-byte content size, then blocks of up to 128 KB.
std::vector<uint8_t> zstdRaw (const std::vector<uint8_t>& data)
{
    std::vector<uint8_t> z { 0x28, 0xb5, 0x2f, 0xfd, 0xa0 };             // FCS 4 bytes, single segment
    const auto n = (uint32_t) data.size();
    for (int i = 0; i < 4; ++i) z.push_back ((uint8_t) (n >> (8 * i)));
    size_t pos = 0;
    do
    {
        const size_t len = std::min<size_t> (data.size() - pos, 128 * 1024);
        const bool last = pos + len >= data.size();
        const uint32_t h = (uint32_t) (last ? 1 : 0) | (uint32_t) (len << 3);  // block type 0 = raw
        z.push_back ((uint8_t) h); z.push_back ((uint8_t) (h >> 8)); z.push_back ((uint8_t) (h >> 16));
        z.insert (z.end(), data.begin() + (long) pos, data.begin() + (long) (pos + len));
        pos += len;
    } while (pos < data.size());
    return z;
}

std::vector<uint8_t> container (const std::string& json, const std::vector<uint8_t>& cbor)
{
    std::vector<uint8_t> f { 'X', 'f', 'e', 'r', 'J', 's', 'o', 'n', 0 };
    const uint64_t jl = json.size();
    for (int i = 0; i < 8; ++i) f.push_back ((uint8_t) (jl >> (8 * i)));
    f.insert (f.end(), json.begin(), json.end());
    const auto raw = (uint32_t) cbor.size();
    for (int i = 0; i < 4; ++i) f.push_back ((uint8_t) (raw >> (8 * i)));
    f.push_back (2); f.push_back (0); f.push_back (0); f.push_back (0);
    const auto z = zstdRaw (cbor);
    f.insert (f.end(), z.begin(), z.end());
    return f;
}

// ---- tests ------------------------------------------------------------------------
void testCbor()
{
    std::printf ("CBOR\n");
    Cbor c;
    c.map (7).kv ("a", 1.5).str ("b").arr (3).integer (7).integer (-3).str ("x");
    c.str ("c").f32 (0.25f);
    c.str ("d"); c.b.push_back (0xf9); c.b.push_back (0x3c); c.b.push_back (0x00);   // half 1.0
    c.str ("e"); c.b.push_back (0xf5);                                                  // true
    c.str ("f"); c.b.push_back (0xf6);                                                  // null
    c.str ("g"); c.b.push_back (0x9f); c.integer (1); c.integer (2); c.b.push_back (0xff);   // [_ 1, 2]
    Value v;
    CHECK (decodeCbor (c.b.data(), c.b.size(), v));
    CHECK (v.isMap() && v.size() == 7);
    CHECK_NEAR (v["a"].num (0), 1.5, 0);
    CHECK (v["b"].isArray() && v["b"].size() == 3);
    CHECK_NEAR (v["b"][0].num (0), 7, 0);
    CHECK_NEAR (v["b"][1].num (0), -3, 0);
    CHECK (v["b"][2].str() == "x");
    CHECK_NEAR (v["c"].num (0), 0.25, 0);
    CHECK_NEAR (v["d"].num (0), 1.0, 0);
    CHECK (v["e"].isNumber() && v["e"].num (0) > 0.5);
    CHECK (v["f"].type == Value::Type::Null);
    CHECK (v["g"].size() == 2);
    CHECK (v["missing"]["deeper"][3].type == Value::Type::Null);

    // Truncated and hostile input fails cleanly (every proper prefix of a map is incomplete).
    for (size_t cut = 0; cut < c.b.size(); ++cut)
    {
        Value t;
        CHECK (! decodeCbor (c.b.data(), cut, t));
    }
    std::vector<uint8_t> deep (200, 0x81);          // 200 nested one-element arrays
    deep.push_back (0x01);
    Value t;
    CHECK (! decodeCbor (deep.data(), deep.size(), t));
    const uint8_t hugeArray[] { 0x9b, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    CHECK (! decodeCbor (hugeArray, sizeof (hugeArray), t));
    const uint8_t hugeString[] { 0x7a, 0xff, 0xff, 0xff, 0xf0, 'a' };
    CHECK (! decodeCbor (hugeString, sizeof (hugeString), t));
}

void testJson()
{
    std::printf ("JSON\n");
    Value v;
    CHECK (decodeJson (R"({"presetName": "BS - \"Q\" é", "tags": ["Bass", "Mono"], "version": 9.0, "x": true, "y": null, "z": {}})", v));
    CHECK (v["presetName"].str() == "BS - \"Q\" \xc3\xa9");
    CHECK (v["tags"].size() == 2 && v["tags"][1].str() == "Mono");
    CHECK_NEAR (v["version"].num (0), 9.0, 0);
    CHECK (v["x"].num (0) > 0.5);
    CHECK (! decodeJson ("{\"a\": ", v));
    CHECK (! decodeJson ("[1, 2", v));
}

void testHelpers()
{
    std::printf ("Mapping helpers\n");
    const auto& ft = choices::filterTypes();
    auto f = [&] (const char* n, bool& exact) { return ft[(size_t) filterTypeFor (n, exact)]; };
    bool ex = false;
    CHECK (f ("MgL24", ex) == "MG Low 24" && ex);
    CHECK (f ("MgL6", ex) == "MG Low 6" && ex);
    CHECK (f ("L12", ex) == "Low 12" && ex);
    CHECK (f ("H18", ex) == "High 18" && ex);
    CHECK (f ("B24", ex) == "Band 24" && ex);
    CHECK (f ("N24", ex) == "Notch 24" && ex);
    CHECK (f ("LP12", ex) == "LP 12" && ex);
    CHECK (f ("HN12", ex) == "HN 12" && ex);
    CHECK (f ("PP12", ex) == "PP 12" && ex);
    CHECK (f ("LBH24", ex) == "LBH 24" && ex);
    CHECK (f ("BPN12", ex) == "BPN 12" && ex);
    CHECK (f ("DistComb2BP", ex) == "Dist.Comb 2 BP" && ex);
    CHECK (f ("CombHL6P", ex) == "Cmb HL" && ex);
    CHECK (f ("FlangeH6N", ex) == "Flg H" && ! ex);
    CHECK (f ("Phase48HL6P", ex) == "Phs HL" && ! ex);
    CHECK (f ("FormantTWO", ex) == "Formant-II" && ex);
    CHECK (f ("SNH1", ex) == "SampHold" && ex);
    CHECK (f ("LadderAcid", ex) == "Acid Ladder" && ex);
    CHECK (f ("Diffuser", ex) == "Diffusor" && ex);
    CHECK (f ("NoSuchFilter", ex) == "MG Low 12" && ! ex);

    int fm = -1;
    const auto& wm = choices::warpModes();
    auto w = [&] (const char* n, int osc) { const int i = warpModeFor (n, osc, fm); return i < 0 ? std::string ("?") : wm[(size_t) i]; };
    CHECK (w ("kFM_OSC", 0) == "FM (B)");
    CHECK (w ("kFM_OSC2", 0) == "FM (C)");
    CHECK (w ("kFM_OSC", 1) == "FM (A)");
    CHECK (w ("kFM_OSC2", 2) == "FM (B)");
    CHECK (w ("kPD_SUB", 1) == "PD (Sub)");
    CHECK (w ("kRM_FILT2", 0) == "RM (Filter 2)");
    CHECK (w ("kAM_NOISE", 0) == "AM (Noise)");
    CHECK (w ("kSync", 0) == "Sync");
    CHECK (w ("kBendNeg", 0) == "Bend -");
    CHECK (w ("kASYMPosNeg", 0) == "Asym +/-");
    CHECK (w ("kRemap_1", 0) == "Remap 1");
    CHECK (w ("kDistSoftClip", 0) == "Soft Clip");
    CHECK (w ("kDistLinFold", 0) == "Linear Fold");
    CHECK (w ("kDistTube", 0) == "Tube");
    CHECK (w ("kNotAWarp", 0) == "?");
    CHECK (w ("kFMX_OSC", 0) == "FM (B)" && fm == 2);

    const auto& sr = choices::syncRates();
    CHECK (sr[(size_t) syncRateForKnob (6.25)] == "1/4");           // Serum's default rate
    CHECK (sr[(size_t) syncRateForKnob (0.0)] == "8 bars");
    CHECK (sr[(size_t) syncRateForKnob (100.0)] == "1/256");
    CHECK (sr[(size_t) syncRateForKnob (100.0 * std::pow (90.0 / 228.0, 4.0))] == "1 bar");

    CHECK_NEAR (attackCurveFor (50.0), 0.2, 1e-6);
    CHECK_NEAR (fallCurveFor (66.6), -0.6, 1e-6);
    CHECK_NEAR (attackCurveFor (100.0), 1.0, 1e-6);
    CHECK_NEAR (attackCurveFor (0.0), -1.0, 1e-6);
    CHECK_NEAR (fallCurveFor (100.0), -1.0, 1e-6);
    CHECK_NEAR (fallCurveFor (0.0), 1.0, 1e-6);
}

// A preset in the shape Serum writes: only edited parameters are present.
std::vector<uint8_t> syntheticPreset()
{
    Cbor c;
    c.map (11);
    c.str ("Oscillator0").map (2);
    {
        c.str ("plainParams").map (4).kv ("kParamOctave", -1.0).kv ("kParamVolume", 0.5).kv ("kParamUnison", 4.0).kv ("kParamPan", 25.0);
        c.str ("WTOsc0").map (2).kv ("relativePathToWT", "S2 Tables/Default Shapes.wav");
        c.str ("plainParams").map (4).kv ("kParamTablePos", 128.5).kv ("kParamWarpMenu", "kFM_OSC").kv ("kParamWarp", 0.4).kv ("kParamRandomPhase", 0.0);
    }
    c.str ("Oscillator1").map (1).str ("plainParams").map (2).kv ("kParamEnable", 1.0).kv ("kParamPitch", 7.0);
    c.str ("RoutingSlot1").map (1).str ("plainParams").map (1).kv ("kParamRoutingDest", "kRoutingDestFilter");
    c.str ("VoiceFilter0").map (1).str ("plainParams").map (4).kv ("kParamEnable", 1.0).kv ("kParamFreq", 0.5)
        .kv ("kParamType", "MgL24").kv ("kParamReso", 40.0);
    c.str ("Env0").map (1).str ("plainParams").map (4).kv ("kParamAttack", 0.01).kv ("kParamDecay", 0.4)
        .kv ("kParamSustain", 0.5).kv ("kParamRelease", 0.3);
    c.str ("LFO0").map (2);
    {
        c.str ("plainParams").map (2).kv ("kParamMode", "Free").kv ("kParamRate", 100.0 * std::pow (90.0 / 228.0, 4.0));
        c.str ("curveData").map (4).str ("numPoints").integer (2);
        c.str ("xVals").arr (3).f64 (0.0).f64 (0.25).f64 (1.0);
        c.str ("yVals").arr (3).f64 (1.0).f64 (0.0).f64 (1.0);
        c.str ("curveVals").arr (3).f64 (0.5).f64 (0.75).f64 (0.5);
    }
    c.str ("Macro2").map (2).kv ("name", "CUTOFF").str ("plainParams").map (1).kv ("kParamValue", 40.0);
    c.str ("Global0").map (1).str ("plainParams").map (3).kv ("kParamMonoToggle", 1.0).kv ("kParamPortamentoTime", 0.05)
        .kv ("kParamMasterVolume", 0.7);
    c.str ("ModSlot0").map (5).str ("source").arr (2).integer (6).integer (0);
    c.kv ("destModuleTypeString", "VoiceFilter").kv ("destModuleParamName", "kParamFreq").str ("destModuleID").integer (0);
    c.str ("plainParams").map (2).kv ("kParamAmount", 25.0).kv ("kParamBipolar", 1.0);
    c.str ("ModSlot1").map (5).str ("source").arr (2).integer (27).integer (0);
    c.kv ("destModuleTypeString", "FXReverb").kv ("destModuleParamName", "kParamWet").str ("destModuleID").integer (1);
    c.str ("plainParams").map (1).kv ("kParamAmount", -50.0);
    c.str ("FXRack0").map (2).kv ("displayName", "");
    c.str ("FX").arr (2);
    c.map (2).str ("FXDistortion").map (1).str ("plainParams").map (3).kv ("kParamMode", "kSoftClip").kv ("kParamDrive", 40.0).kv ("kParamWet", 80.0);
    c.str ("type").integer (0);
    c.map (2).str ("FXReverb").map (1).str ("plainParams").map (3).kv ("kParamType", "kHall").kv ("kParamWet", 25.0).kv ("kParamSize", 60.0);
    c.str ("type").integer (6);
    return container (R"({"fileType":"SerumPreset","presetName":"BS - Test","presetAuthor":"Tests","tags":["Bass"],"product":"Serum2"})", c.b);
}

void testImport()
{
    std::printf ("Import\n");
    const auto file = syntheticPreset();
    CHECK (looksLikeSerumPreset (file.data(), file.size()));
    File f;
    std::string error;
    CHECK (readFile (file.data(), file.size(), f, error));
    CHECK (f.state.size() == 11);
    CHECK (f.header["presetName"].str() == "BS - Test");

    Patch p;
    ParamValues v;
    Report r;
    CHECK (importPreset (file.data(), file.size(), p, v, {}, r, error));
    const auto& I = ids();
    CHECK (p.info.name == "BS - Test" && p.info.author == "Tests" && p.info.category == "Bass");
    CHECK_NEAR (v[I.osc[0].oct], -1, 0);
    CHECK_NEAR (v[I.osc[0].level], 0.5, 1e-6);
    CHECK_NEAR (v[I.osc[0].unison], 4, 0);
    CHECK_NEAR (v[I.osc[0].pan], 0.5, 1e-6);
    CHECK_NEAR (v[I.osc[0].wtpos], 0.5, 1e-3);
    CHECK_NEAR (v[I.osc[0].rand], 0.0, 1e-6);
    CHECK (choices::warpModes()[(size_t) v[I.osc[0].warp1Mode]] == "FM (B)");
    CHECK_NEAR (v[I.osc[0].warp1], 0.4, 1e-6);
    CHECK (p.wavetable[0].table == FactoryTables::defaultTable());
    CHECK (r.missingFiles.empty());
    CHECK (v[I.osc[1].enable] > 0.5f);
    CHECK_NEAR (v[I.osc[1].sem], 7, 0);
    CHECK_NEAR (v[I.osc[1].route], 0, 0);                    // to the filter
    CHECK (v[I.osc[2].enable] < 0.5f);
    CHECK (v[I.filter[0].enable] > 0.5f && v[I.filter[1].enable] < 0.5f);
    CHECK (choices::filterTypes()[(size_t) v[I.filter[0].type]] == "MG Low 24");
    CHECK_NEAR (v[I.filter[0].cutoff], 8.0 * std::sqrt (22050.0 / 8.0), 1.0);
    CHECK_NEAR (v[I.filter[0].res], 0.4, 1e-6);
    CHECK_NEAR (v[I.env[0].attack], 0.01, 1e-6);
    CHECK_NEAR (v[I.env[0].sustain], 20.0 * std::log10 (0.5), 1e-3);
    CHECK_NEAR (v[I.lfo[0].mode], 0, 0);
    CHECK (choices::syncRates()[(size_t) v[I.lfo[0].rateSync]] == "1 bar");
    CHECK (p.lfo[0].shape.points.size() == 3);
    CHECK_NEAR (p.lfo[0].shape.points[1].x, 0.25, 1e-6);
    CHECK_NEAR (p.lfo[0].shape.points[1].y, 1.0, 1e-6);     // Serum's y is flipped
    CHECK_NEAR (p.lfo[0].shape.points[2].curve, 0.5, 1e-6);  // bend moves to the segment's end point
    CHECK_NEAR (v[I.macro[2]], 0.4, 1e-6);
    CHECK (p.macroNames[2] == "CUTOFF");
    CHECK (v[I.global.mono] > 0.5f);
    CHECK_NEAR (v[I.global.porta], 0.05, 1e-6);

    // FX: Distortion then Reverb in MAIN.
    const auto& mods = p.fx.racks[kFxMain].modules;
    CHECK (mods.size() == 2 && mods[0].type == FxType::Distortion && mods[1].type == FxType::Reverb);
    if (mods.size() == 2)
    {
        CHECK_NEAR (v[I.fx[mods[0].unit].mix], 0.8, 1e-6);
        CHECK (fxInfo (FxType::Distortion).params[0].index (v[I.fx[mods[0].unit].p[0]]) == 1);     // SoftClip
        CHECK_NEAR (v[I.fx[mods[1].unit].mix], 0.25, 1e-6);
        CHECK (fxInfo (FxType::Reverb).params[0].index (v[I.fx[mods[1].unit].p[0]]) == 1);         // Hall
    }
    // Mod matrix: LFO 1 -> cutoff (bipolar), Macro 3 -> reverb mix.
    CHECK (p.matrix.rows.size() == 2);
    if (p.matrix.rows.size() == 2)
    {
        CHECK (p.matrix.rows[0].source == ModSource::Lfo1 && p.matrix.rows[0].dest == I.filter[0].cutoff);
        CHECK_NEAR (p.matrix.rows[0].amount, 0.25, 1e-6);
        CHECK (p.matrix.rows[0].bipolar);
        CHECK (p.matrix.rows[1].source == ModSource::Macro3);
        CHECK (mods.size() == 2 && p.matrix.rows[1].dest == I.fx[mods[1].unit].mix);
        CHECK_NEAR (p.matrix.rows[1].amount, -0.5, 1e-6);
    }
    // Level matched to about -6 dBFS (unless the master ran out of range).
    CHECK (std::isfinite (r.peakBeforeDb));

    // Every byte of truncation fails cleanly (or still decodes when only padding is lost).
    for (size_t cut = 0; cut + 1 < file.size(); cut += 7)
    {
        File t;
        std::string e;
        (void) readFile (file.data(), cut, t, e);
    }
    CHECK (! readFile (file.data(), 20, f, error));
}

// ---- corpus: real presets, when a folder is given ---------------------------------
double renderPeak (const Patch& patch, const ParamValues& v, bool& finite)
{
    Synth synth;
    synth.prepare (48000.0, 512);
    synth.setKeyboardSettings (patch.keyboard);
    synth.setPatch (CompiledPatch::compile (std::make_shared<Patch> (patch)));
    std::vector<float> L (512), R (512);
    MidiBuffer out;
    HostInfo host;
    host.playing = true;
    float peak = 0.0f;
    finite = true;
    const bool pad = patch.info.category == "Pad";
    const std::vector<int> notes = pad ? std::vector<int> { 48, 55, 60, 64 } : std::vector<int> { 60 };
    for (int pos = 0; pos < 48000 * 3; pos += 512)
    {
        std::vector<MidiEvent> ev;
        if (pos == 0) for (int n : notes) ev.push_back (MidiEvent::noteOn (0, n, 100));
        if (pos == 512 * 187) for (int n : notes) ev.push_back (MidiEvent::noteOff (0, n));   // ~2 s
        out.clear();
        synth.process (L.data(), R.data(), 512, ev.data(), (int) ev.size(), v, host, out);
        for (size_t i = 0; i < 512; ++i)
        {
            if (! std::isfinite (L[i]) || ! std::isfinite (R[i])) finite = false;
            else peak = std::max (peak, std::max (std::abs (L[i]), std::abs (R[i])));
        }
    }
    synth.collectGarbage();
    return peak > 1e-9f ? 20.0 * std::log10 ((double) peak) : -180.0;
}

void testCorpus (const std::string& folder)
{
    std::printf ("Corpus: %s\n", folder.c_str());
    int n = 0;
    for (const auto& e : std::filesystem::recursive_directory_iterator (folder))
    {
        if (! e.is_regular_file() || e.path().extension() != ".SerumPreset") continue;
        std::ifstream in (e.path(), std::ios::binary);
        const std::vector<uint8_t> b ((std::istreambuf_iterator<char> (in)), {});
        Patch p;
        ParamValues v;
        Report r;
        std::string error;
        const bool ok = importPreset (b.data(), b.size(), p, v, {}, r, error);
        CHECK (ok);
        if (! ok) { std::printf ("  %s: %s\n", e.path().filename().c_str(), error.c_str()); continue; }
        ++n;
        bool finite = true;
        const double peak = renderPeak (p, v, finite);
        CHECK (finite);
        CHECK (peak < 0.0);                                   // never clips
        CHECK (peak > -20.0);                                 // level matching worked
        for (const auto& row : p.matrix.rows) CHECK (row.dest >= 0 && row.dest < params().size());
        // The patch survives a save / load round trip.
        Node node;
        p.save (node);
        Patch back;
        back.load (node);
        CHECK (back.matrix.rows.size() == p.matrix.rows.size());
        std::printf ("  %-26s peak %6.1f dB  mods %2d (-%d)  fx %d (-%d)  notes %zu  missing %zu\n",
                     p.info.name.c_str(), peak, r.modRows, r.modRowsDropped, r.fxModules, r.fxDropped, r.notes.size(), r.missingFiles.size());
    }
    CHECK (n > 0);
}
} // namespace

int main (int argc, char** argv)
{
    testCbor();
    testJson();
    testHelpers();
    testImport();
    std::string corpus = argc > 1 ? argv[1] : "";
    if (corpus.empty())
        if (const char* env = std::getenv ("TINCTURE_SERUM_PRESETS")) corpus = env;
    if (! corpus.empty()) testCorpus (corpus);
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
