// EngineClickTests.cpp — a new note must not click the voices already
// sounding. Each case renders a patch twice, with and without the extra
// note, and checks the difference right after the note-on: with a slow
// ENV 1 attack the new note itself is near-silent there, so anything else is
// a discontinuity in the other voices.
//
//   - FM from an oscillator rendered later (A <- B), with MIDI splitting ticks
//   - voice stealing (poly 2, a third note), with a sub oscillator whose phase restarts
//   - mono retrigger of a sounding note
//   - Limit Same Note Poly retrigger
// Returns non-zero on failure.
#include "Engine/ParamIds.h"
#include "Engine/Synth.h"
#include "Engine/Wavetable.h"
#include "Model/Patch.h"

#include <cmath>
#include <cstdio>
#include <functional>
#include <vector>

using namespace tincture;

namespace
{
int failures = 0, checks = 0;

void check (bool ok, const char* what, double value)
{
    ++checks;
    std::printf ("  %-44s %8.4f  %s\n", what, value, ok ? "ok" : "FAIL");
    if (! ok) ++failures;
}

using Setup = std::function<void (Patch&, ParamValues&)>;
struct Note { int at, note; bool on; };

std::vector<float> render (const Setup& setup, const std::vector<Note>& notes, int length)
{
    auto patch = std::make_shared<Patch>();
    ParamValues pv;
    pv.resetToDefaults();
    setup (*patch, pv);
    Synth s;
    s.prepare (48000.0, 512);
    s.setPatch (CompiledPatch::compile (patch));
    std::vector<float> out, L (512), R (512);
    MidiBuffer mo;
    HostInfo h;
    for (int pos = 0; pos < length; pos += 512)
    {
        std::vector<MidiEvent> ev;
        for (const auto& n : notes)
            if (n.at >= pos && n.at < pos + 512)
                ev.push_back (n.on ? MidiEvent::noteOn (n.at - pos, n.note, 100) : MidiEvent::noteOff (n.at - pos, n.note));
        mo.clear();
        s.process (L.data(), R.data(), 512, ev.data(), (int) ev.size(), pv, h, mo);
        out.insert (out.end(), L.begin(), L.end());
    }
    return out;
}

// Largest second difference of (with - without) in the 5 ms after `at`,
// relative to the largest second difference of the signal before it. A click
// (a step or a phase jump) gives ratios of 50..1000 here; the intended 10 ms
// S-curve fade of a replaced voice stays below 3.
double clickRatio (const Setup& setup, std::vector<Note> base, Note extra, int at)
{
    const int len = at + 4800;
    const auto a = render (setup, base, len);
    base.push_back (extra);
    const auto b = render (setup, base, len);
    double ref = 1.0e-12, spike = 0.0;
    for (int i = at - 4800; i < at; ++i)
        ref = std::max (ref, (double) std::abs (a[(size_t) i] - 2.0f * a[(size_t) i - 1] + a[(size_t) i - 2]));
    for (int i = at; i < at + 240; ++i)
    {
        const double d0 = b[(size_t) i] - a[(size_t) i], d1 = b[(size_t) i - 1] - a[(size_t) i - 1], d2 = b[(size_t) i - 2] - a[(size_t) i - 2];
        spike = std::max (spike, std::abs (d0 - 2.0 * d1 + d2));
    }
    return spike / ref;
}
} // namespace

int main()
{
    const auto& I = ids();
    const int at = 48000 + 137;          // mid-block, so the tick is split

    std::printf ("FM from a later oscillator (A <- FM (B))\n");
    const Setup fm = [&] (Patch& p, ParamValues& v)
    {
        p.wavetable[1].table = FactoryTables::defaultTable();
        v[I.osc[1].enable] = 1.0f;
        v[I.osc[1].route] = 3.0f;                       // modulator only
        v[I.osc[1].oct] = 1.0f;
        v[I.osc[0].warp1Mode] = (float) [] { const auto& w = choices::warpModes(); for (size_t i = 0; i < w.size(); ++i) if (w[i] == "FM (B)") return (int) i; return 0; }();
        v[I.osc[0].warp1] = 0.3f;
        v[I.filter[0].enable] = 0.0f;
        v[I.env[0].attack] = 2.0f;
    };
    check (clickRatio (fm, { { 0, 60, true } }, { at, 67, true }, at) < 3.0, "new note: no jump in the sounding voice", clickRatio (fm, { { 0, 60, true } }, { at, 67, true }, at));

    std::printf ("Voice stealing (poly 2) with a sub oscillator\n");
    const Setup steal = [&] (Patch&, ParamValues& v)
    {
        v[I.global.poly] = 2.0f;
        v[I.osc[0].enable] = 0.0f;
        v[I.sub.enable] = 1.0f;
        v[I.sub.phase] = 112.0f;
        v[I.sub.level] = 0.8f;
        v[I.filter[0].enable] = 0.0f;
        v[I.env[0].attack] = 2.0f;
    };
    const double st = clickRatio (steal, { { 0, 48, true }, { 4800, 55, true } }, { at, 64, true }, at);
    check (st < 3.0, "third note: stolen voice fades instead of cutting", st);

    std::printf ("Mono retrigger\n");
    const Setup mono = [&] (Patch&, ParamValues& v)
    {
        v[I.global.mono] = 1.0f;
        v[I.sub.enable] = 1.0f;
        v[I.sub.phase] = 90.0f;
        v[I.filter[0].enable] = 0.0f;
        v[I.env[0].attack] = 0.3f;
    };
    const double mr = clickRatio (mono, { { 0, 48, true }, { at - 64, 48, false } }, { at, 48, true }, at);
    check (mr < 3.0, "same note again: no phase restart in the old note", mr);

    std::printf ("Limit Same Note Poly\n");
    const Setup limit = [&] (Patch& p, ParamValues& v)
    {
        p.limitSameNote = true;
        v[I.sub.enable] = 1.0f;
        v[I.sub.phase] = 90.0f;
        v[I.filter[0].enable] = 0.0f;
        v[I.env[0].attack] = 2.0f;
        // A re-struck key releases the old note first (NoteProcessor); a long
        // release keeps that out of the way so this measures the replace fade.
        v[I.env[0].release] = 4.0f;
    };
    const double lr = clickRatio (limit, { { 0, 48, true } }, { at, 48, true }, at);
    check (lr < 3.0, "same note again: old voice fades", lr);

    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
