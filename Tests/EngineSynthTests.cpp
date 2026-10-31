// EngineSynthTests.cpp — the voice/synth core: notes, voicing, glide,
// routing, modulation, patch swaps, preset encoding.
#include "Engine/Synth.h"
#include "Engine/Fx/FxTypes.h"
#include "Model/FactoryPresets.h"

#include <cmath>
#include <cstdio>

using namespace tincture;

namespace
{
int checks = 0, failures = 0;
void check (bool ok, const char* what)
{
    ++checks;
    if (! ok) { ++failures; std::printf ("  FAIL %s\n", what); }
}

struct Rig
{
    Synth synth;
    ParamValues pv;
    std::shared_ptr<Patch> patch = std::make_shared<Patch>();
    HostInfo host;
    MidiBuffer out;
    std::vector<float> L, R;
    static constexpr double sr = 48000.0;

    Rig() { synth.prepare (sr, 512); pv.resetToDefaults(); }
    void apply() { synth.setPatch (CompiledPatch::compile (patch)); synth.collectGarbage(); }
    void run (int samples, std::vector<MidiEvent> ev = {})
    {
        L.assign ((size_t) samples, 0.0f);
        R.assign ((size_t) samples, 0.0f);
        for (int pos = 0; pos < samples; pos += 512)
        {
            const int n = std::min (512, samples - pos);
            std::vector<MidiEvent> here;
            for (auto e : ev) if (e.offset >= pos && e.offset < pos + n) { e.offset -= pos; here.push_back (e); }
            out.clear();
            synth.process (L.data() + pos, R.data() + pos, n, here.data(), (int) here.size(), pv, host, out);
        }
        synth.collectGarbage();
    }
    static MidiEvent on (int note, int vel = 100, int at = 0) { MidiEvent e; e.offset = at; e.status = 0x90; e.data1 = (uint8_t) note; e.data2 = (uint8_t) vel; return e; }
    static MidiEvent off (int note, int at = 0) { MidiEvent e; e.offset = at; e.status = 0x80; e.data1 = (uint8_t) note; e.data2 = 64; return e; }
    static MidiEvent cc (int c, int v, int at = 0) { MidiEvent e; e.offset = at; e.status = 0xB0; e.data1 = (uint8_t) c; e.data2 = (uint8_t) v; return e; }
    float peak (int from = 0, int to = -1) const
    {
        if (to < 0) to = (int) L.size();
        float p = 0.0f;
        for (int i = from; i < to; ++i) p = std::max (p, std::abs (L[(size_t) i]));
        return p;
    }
    // Frequency by counting rising zero crossings (of a smoothed copy).
    double freq (int from, int to) const
    {
        int first = -1, last = -1, count = 0;
        for (int i = from + 1; i < to; ++i)
            if (L[(size_t) i - 1] <= 0.0f && L[(size_t) i] > 0.0f)
            {
                if (first < 0) first = i; else ++count;
                last = i;
            }
        return count > 0 ? sr * count / (double) (last - first) : 0.0;
    }
};

void sineInit (Rig& r)
{
    // OSC A plays frame 0 of "Default Shapes" (a sine) straight to Main.
    const auto& I = ids();
    r.pv[I.osc[0].route] = 1;
    r.pv[I.osc[0].wtpos] = 0.0f;
    r.pv[I.osc[0].rand] = 0.0f;
}
} // namespace

int main()
{
    const auto& I = ids();

    std::printf ("note pitch and release\n");
    {
        Rig r;
        sineInit (r);
        r.apply();
        r.run (48000, { Rig::on (69), Rig::off (69, 24000) });
        const double f = r.freq (4800, 23000);
        check (std::abs (f - 440.0) < 0.5, "A4 = 440 Hz");
        check (r.peak (4800, 23000) > 0.1f, "audible");
        check (r.peak (24000 + 4800) < 1e-4f, "silent after the 15 ms release");
        r.run (512);
        check (r.synth.telemetry.activeVoices.load() == 0, "voice freed");
    }

    std::printf ("polyphony limit and stealing\n");
    {
        Rig r;
        sineInit (r);
        r.pv[I.global.poly] = 3;
        r.apply();
        r.run (4800, { Rig::on (60), Rig::on (64, 100, 100), Rig::on (67, 100, 200), Rig::on (71, 100, 300) });
        check (r.synth.telemetry.activeVoices.load() == 3, "3 voices max");
        check (r.synth.telemetry.lastNote.load() == 71, "newest note sounds");
    }

    std::printf ("mono legato glide\n");
    {
        Rig r;
        sineInit (r);
        r.pv[I.global.mono] = 1;
        r.pv[I.global.legato] = 1;
        r.pv[I.global.porta] = 0.1f;
        r.apply();
        r.run (48000, { Rig::on (57), Rig::on (69, 100, 12000) });
        check (r.synth.telemetry.activeVoices.load() == 1, "one voice in mono");
        const double fStart = r.freq (2000, 11000), fEnd = r.freq (24000, 47000);
        check (std::abs (fStart - 220.0) < 0.5, "first note 220 Hz");
        check (std::abs (fEnd - 440.0) < 0.5, "glided to 440 Hz");
        const double fMid = r.freq (12000 + 1500, 12000 + 2700);
        check (fMid > 225.0 && fMid < 435.0, "mid-glide pitch between");
        // Releasing the top note returns to the held one.
        r.run (24000, { Rig::off (69) });
        check (std::abs (r.freq (12000, 23000) - 220.0) < 1.0, "back to the held note");
    }

    std::printf ("sustain pedal\n");
    {
        Rig r;
        sineInit (r);
        r.apply();
        r.run (9600, { Rig::cc (64, 127), Rig::on (60), Rig::off (60, 2000) });
        check (r.peak (8000) > 0.05f, "held by the pedal");
        r.run (9600, { Rig::cc (64, 0) });
        check (r.peak (4800) < 1e-4f, "released with the pedal");
    }

    std::printf ("routing: filter, direct, buses\n");
    {
        Rig r;
        r.pv[I.osc[0].route] = 0;            // Filter 1
        r.pv[I.filter[0].enable] = 1;
        r.pv[I.filter[0].cutoff] = 200.0f;   // dark
        r.pv[I.osc[0].wtpos] = 0.5f;         // a brighter frame
        r.apply();
        r.run (24000, { Rig::on (60) });
        float filtered = 0; for (int i = 12000; i < 24000; ++i) filtered += std::abs (r.L[(size_t) i] - r.L[(size_t) i - 1]);
        Rig d;
        d.pv[I.osc[0].route] = 2;            // Direct, bypasses the filter
        d.pv[I.osc[0].wtpos] = 0.5f;
        d.apply();
        d.run (24000, { Rig::on (60) });
        float direct = 0; for (int i = 12000; i < 24000; ++i) direct += std::abs (d.L[(size_t) i] - d.L[(size_t) i - 1]);
        check (filtered < direct * 0.5f, "filter darkens; direct bypasses it");

        // Bus send with a delay on BUS 1: an echo after the note.
        Rig b;
        sineInit (b);
        b.pv[I.osc[0].bus1] = 1.0f;
        b.pv[I.osc[0].level] = 0.75f;
        const int u = b.patch->fx.add (1, FxType::Delay);
        check (u >= 0, "delay added to BUS 1");
        auto def = fxDefaults (FxType::Delay);
        for (int k = 0; k < kFxParams; ++k) b.pv[I.fx[u].p[k]] = def[(size_t) k];
        b.pv[I.fx[u].mix] = 1.0f;
        b.apply();
        b.run (48000 * 2, { Rig::on (60), Rig::off (60, 2400) });
        check (b.peak (48000, 96000) > 1e-3f, "bus delay echoes after the note");
    }

    std::printf ("modulation: LFO to level, macro to cutoff\n");
    {
        Rig r;
        sineInit (r);
        r.patch->matrix.assign (ModSource::Lfo1, I.osc[0].level, -1.0f);
        r.pv[I.lfo[0].bpm] = 0;
        r.pv[I.lfo[0].rate] = 4.0f;
        r.apply();
        r.run (48000, { Rig::on (60) });
        float lo = 1e9f, hi = 0;
        for (int w = 0; w < 8; ++w)
        {
            const float p = r.peak (12000 + w * 4000, 12000 + w * 4000 + 1500);
            lo = std::min (lo, p); hi = std::max (hi, p);
        }
        check (hi > lo * 2.0f, "LFO moves the level");
    }

    std::printf ("patch swaps while playing\n");
    {
        Rig r;
        sineInit (r);
        r.apply();
        bool finite = true;
        for (int i = 0; i < 40; ++i)
        {
            auto p = std::make_shared<Patch> (*r.patch);
            if (i % 2) p->matrix.assign (ModSource::Lfo2, I.osc[0].wtpos, 0.5f); else p->matrix.initModulations();
            if (i % 3 == 0) p->fx.add (0, FxType::Reverb);
            r.patch = p;
            r.apply();
            r.run (1024, i == 0 ? std::vector<MidiEvent> { Rig::on (60), Rig::on (64) } : std::vector<MidiEvent> {});
            for (float x : r.L) finite &= std::isfinite (x);
        }
        check (finite, "no NaN/Inf across 40 swaps");
    }

    std::printf ("presets encode/decode\n");
    {
        Patch p;
        ParamValues v;
        v.resetToDefaults();
        v[I.osc[1].level] = 0.33f;
        p.matrix.assign (ModSource::Env2, I.filter[0].cutoff, 0.4f);
        p.info.name = "Round Trip";
        const auto bin = encodePreset (p, v).toBinary();
        Node n;
        check (Node::fromBinary (bin.data(), bin.size(), n), "parse");
        Patch q;
        ParamValues w;
        check (decodePreset (n, q, w), "decode");
        check (std::abs (w[I.osc[1].level] - 0.33f) < 1e-6f, "param survives");
        check (q.matrix.rows.size() == 1 && q.info.name == "Round Trip", "patch survives");
        check (encodePreset (q, w).toBinary() == bin, "re-encode identical");
    }

    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
