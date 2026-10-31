// Bench.cpp — CPU cost of the whole engine for typical and heavy patches.
//   TinctureBench            prints % of one core at 48 kHz, 512-sample blocks
#include "Engine/Synth.h"
#include "Engine/Fx/FxTypes.h"
#include "Engine/Wavetable.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>

using namespace tincture;

namespace
{
struct Case
{
    const char* name;
    int voices;
    std::function<void (Patch&, ParamValues&)> setup;
};

double run (const Case& c, double seconds = 4.0)
{
    Synth synth;
    const double sr = 48000.0;
    const int block = 512;
    synth.prepare (sr, block);
    auto patch = std::make_shared<Patch>();
    ParamValues pv;
    pv.resetToDefaults();
    c.setup (*patch, pv);
    synth.setPatch (CompiledPatch::compile (patch));

    std::vector<float> L (block), R (block);
    MidiBuffer out;
    HostInfo host;
    host.playing = true;
    std::vector<MidiEvent> on;
    for (int v = 0; v < c.voices; ++v)
    {
        MidiEvent e;
        e.offset = 0;
        e.status = 0x90;
        e.data1 = (uint8_t) (36 + v * 5 % 40);
        e.data2 = 100;
        on.push_back (e);
    }
    // Warm up, then time.
    synth.process (L.data(), R.data(), block, on.data(), (int) on.size(), pv, host, out);
    for (int i = 0; i < 40; ++i) { out.clear(); synth.process (L.data(), R.data(), block, nullptr, 0, pv, host, out); }
    const int blocks = (int) (seconds * sr / block);
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < blocks; ++i)
    {
        out.clear();
        synth.process (L.data(), R.data(), block, nullptr, 0, pv, host, out);
    }
    const double el = std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
    synth.collectGarbage();
    return 100.0 * el / ((double) blocks * block / sr);
}
} // namespace

int main (int argc, char** argv)
{
    const int only = argc > 1 ? std::atoi (argv[1]) : -1;
    const double secs = argc > 2 ? std::atof (argv[2]) : 4.0;
    const auto& I = ids();
    std::vector<Case> cases {
        { "Init, 1 note", 1, [] (Patch&, ParamValues&) {} },
        { "Init, 8 notes", 8, [] (Patch&, ParamValues&) {} },
        { "3 osc x 7 unison, 2 filters, 8 notes", 8, [&] (Patch&, ParamValues& v)
          {
              for (int o = 0; o < 3; ++o) { v[I.osc[o].enable] = 1; v[I.osc[o].unison] = 7; v[I.osc[o].route] = 0; v[I.osc[o].filterBal] = (float) o * 0.5f; }
              v[I.filter[1].enable] = 1; v[I.filter[0].cutoff] = 2000; v[I.filter[1].cutoff] = 4000;
          } },
        { "3 osc x 7 unison + warps, 2 filters, FX rack, 8 notes", 8, [&] (Patch& p, ParamValues& v)
          {
              for (int o = 0; o < 3; ++o)
              {
                  v[I.osc[o].enable] = 1; v[I.osc[o].unison] = 7; v[I.osc[o].route] = 0;
                  v[I.osc[o].warp1Mode] = 2; v[I.osc[o].warp1] = 0.5f; v[I.osc[o].warp2Mode] = 20; v[I.osc[o].warp2] = 0.4f;
              }
              v[I.filter[1].enable] = 1;
              v[I.sub.enable] = 1; v[I.noise.enable] = 1;
              for (auto t : { FxType::Distortion, FxType::Chorus, FxType::Delay, FxType::Reverb, FxType::Compressor, FxType::Equalizer })
              {
                  const int u = p.fx.add (0, t);
                  if (u >= 0) { auto d = fxDefaults (t); for (int k = 0; k < kFxParams; ++k) v[I.fx[u].p[k]] = d[(size_t) k]; v[I.fx[u].mix] = fxInfo (t).defaultMix; }
              }
              // A few LFO and envelope modulations.
              p.matrix.assign (ModSource::Lfo1, I.osc[0].wtpos, 0.5f);
              p.matrix.assign (ModSource::Env2, I.filter[0].cutoff, 0.6f);
              p.matrix.assign (ModSource::Lfo2, I.osc[1].warp1, 0.3f);
          } },
        { "16 notes, 3 osc x 16 unison (worst case)", 16, [&] (Patch&, ParamValues& v)
          {
              for (int o = 0; o < 3; ++o) { v[I.osc[o].enable] = 1; v[I.osc[o].unison] = 16; }
              v[I.global.poly] = 16;
          } },
    };
    for (size_t i = 0; i < cases.size(); ++i)
        if (only < 0 || only == (int) i)
            std::printf ("%-60s %6.1f %% of one core\n", cases[i].name, run (cases[i], secs));
    return 0;
}
