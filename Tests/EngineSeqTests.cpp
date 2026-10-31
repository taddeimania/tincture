// EngineSeqTests.cpp — tests for the note-generation side: scales, the
// arpeggiator, the clip player/recorder, the NoteProcessor pipeline
// (transpose / key-scale mapping, MIDI OUT) and state save/load.
// Prints a summary and a micro-benchmark; returns non-zero on failure.
#include "Engine/Arp.h"
#include "Engine/Clip.h"
#include "Engine/NoteProcessor.h"
#include "Engine/Scales.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <cstdio>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <vector>

using namespace tincture;

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { ++checks; const auto va = (a); const auto vb = (b); if (! (va == vb)) { ++failures; std::printf ("  FAIL %s:%d  %s == %s  (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, (long long) va, (long long) vb); } } while (0)

std::string seqStr (const std::vector<int>& v)
{
    std::string s;
    for (int x : v) s += std::to_string (x) + " ";
    return s;
}
#define CHECK_SEQ(got, ...) do { ++checks; const std::vector<int> want { __VA_ARGS__ }; if (got != want) { ++failures; std::printf ("  FAIL %s:%d  got [%s] want [%s]\n", __FILE__, __LINE__, seqStr (got).c_str(), seqStr (want).c_str()); } } while (0)

// Absolute-time event, as the rig records it.
struct Ev { long long t; int status, note, vel, src; bool on() const { return (status & 0xF0) == 0x90 && vel > 0; } bool off() const { return (status & 0xF0) == 0x80 || ((status & 0xF0) == 0x90 && vel == 0); } };

// Drives a NoteProcessor block by block, like the synth would.
struct Rig
{
    std::unique_ptr<NoteProcessor> np = std::make_unique<NoteProcessor>();
    std::unique_ptr<MidiBuffer> outV = std::make_unique<MidiBuffer>(), outH = std::make_unique<MidiBuffer>();
    ArpBank arp;
    ClipBank clips;
    ParamValues pv;
    Context ctx;
    long long pos = 0;
    std::vector<std::pair<long long, MidiEvent>> pending;     // absolute sample, event
    std::vector<Ev> voices, host;

    Rig()
    {
        pv.resetToDefaults();
        ctx.sampleRate = 48000.0;
        ctx.bpm = 120.0;
        ctx.playing = true;
        np->prepare (ctx);
    }
    void set (int id, float v) { pv[id] = v; }
    void noteOn (long long t, int n, int vel = 100) { pending.push_back ({ t, MidiEvent::noteOn (0, n, vel) }); }
    void noteOff (long long t, int n) { pending.push_back ({ t, MidiEvent::noteOff (0, n) }); }
    void cc (long long t, int num, int val) { pending.push_back ({ t, MidiEvent::cc (0, num, val) }); }

    void run (long long samples, int block = 512)
    {
        std::stable_sort (pending.begin(), pending.end(), [] (auto& a, auto& b) { return a.first < b.first; });
        const long long end = pos + samples;
        std::vector<MidiEvent> in;
        while (pos < end)
        {
            const int n = (int) std::min<long long> (block, end - pos);
            in.clear();
            for (auto it = pending.begin(); it != pending.end();)
            {
                if (it->first < pos + n) { MidiEvent e = it->second; e.offset = (int) std::max (0LL, it->first - pos); in.push_back (e); it = pending.erase (it); }
                else ++it;
            }
            ctx.ppq = pos * ctx.bpm / 60.0 / ctx.sampleRate;
            np->process (in.data(), (int) in.size(), n, ctx, pv, arp, clips, *outV, *outH);
            for (const auto& e : *outV) voices.push_back ({ pos + e.offset, e.status, e.data1, e.data2, e.source });
            for (const auto& e : *outH) host.push_back ({ pos + e.offset, e.status, e.data1, e.data2, e.source });
            pos += n;
        }
    }
    std::vector<int> onNotes (const std::vector<Ev>& v) const { std::vector<int> r; for (auto& e : v) if (e.on()) r.push_back (e.note); return r; }
    std::vector<long long> onTimes (const std::vector<Ev>& v) const { std::vector<long long> r; for (auto& e : v) if (e.on()) r.push_back (e.t); return r; }
};

const auto& G() { return ids().global; }

// Per pitch: never two ons without an off between; returns false and prints if malformed.
bool wellFormed (const std::vector<Ev>& evs, bool requireAllOff)
{
    int sounding[128] {};
    for (const auto& e : evs)
    {
        if (e.on()) { if (sounding[e.note]) { std::printf ("    double note-on %d at %lld\n", e.note, e.t); return false; } sounding[e.note] = 1; }
        else if (e.off()) { if (! sounding[e.note]) { std::printf ("    stray note-off %d at %lld\n", e.note, e.t); return false; } sounding[e.note] = 0; }
    }
    if (requireAllOff)
        for (int p = 0; p < 128; ++p)
            if (sounding[p])
            {
                std::printf ("    hanging note %d; its last events:", p);
                int shown = 0;
                for (auto it = evs.rbegin(); it != evs.rend() && shown < 6; ++it)
                    if (it->note == p) { std::printf (" [%s t=%lld src=%d]", it->on() ? "on" : "off", it->t, it->src); ++shown; }
                std::printf ("\n");
                return false;
            }
    return true;
}

// ---------------------------------------------------------------------------
void testScales()
{
    std::printf ("scales\n");
    static const int expected[scales::kNumScales] = {
        7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 8, 7, 7, 7, 7, 7, 7, 5,
        5, 5, 5, 5, 5, 7, 5, 5, 5, 6, 6, 6, 6, 5, 6, 6, 8, 8, 7, 8, 7, 8, 8,
        9, 8, 9, 10, 12 };
    CHECK_EQ ((int) choices::scales().size(), scales::kNumScales);
    std::set<uint16_t> masks;
    for (int s = 0; s < scales::kNumScales; ++s)
    {
        CHECK_EQ (scales::size (s), expected[s]);
        CHECK (scales::mask (s) & 1u);                   // root present
        masks.insert (scales::mask (s));
        for (int key = -1; key < 12; ++key)
            for (int n = 0; n < 128; ++n)
            {
                const int q = scales::quantise (n, key, s);
                if (q < 0 || q > 127 || ! scales::inScale (q, key, s) || scales::quantise (q, key, s) != q || std::abs (q - n) > 6)
                {
                    ++failures;
                    std::printf ("  FAIL quantise scale %d key %d note %d -> %d\n", s, key, n, q);
                    return;
                }
            }
    }
    ++checks;
    CHECK_EQ ((int) masks.size(), scales::kNumScales);   // all 51 interval sets distinct
    CHECK_EQ (scales::quantise (61, 0, 0), 62);          // C# in C major: tie between C and D goes up
    CHECK_EQ (scales::quantise (66, 0, 0), 67);          // F# -> G (tie)
    CHECK_EQ (scales::quantise (63, 2, 0), 64);          // D# in D major -> E (tie, D# is between D and E)
    CHECK_EQ (scales::quantise (70, 9, 5), 69 + 2);      // A minor, A# -> B (tie up)
    CHECK_EQ (scales::quantise (127, 1, 30), 125);       // C# maj pent: G9 would go up to G#9 (128) -> F9 instead
    CHECK (scales::isRoot (62, 2) && ! scales::isRoot (60, 2));
    CHECK (scales::inScale (61, -1, 0));
    CHECK_EQ (scales::keyFromParam (0.0f), -1);
    CHECK_EQ (scales::keyFromParam (1.0f), 0);
    CHECK_EQ (scales::keyFromParam (12.0f), 11);
}

// Holds `notes` from sample 0 and returns the first `count` arp note-ons.
std::vector<int> arpSeq (ArpShape shape, std::vector<int> notes, int count, std::function<void (ArpSlot&)> tweak = {})
{
    Rig r;
    r.set (G().arpOn, 1.0f);
    r.arp.launchQuant = 0;
    r.arp.slots[0].shape = shape;
    r.arp.slots[0].gate = 0.5f;
    if (tweak) tweak (r.arp.slots[0]);
    for (size_t i = 0; i < notes.size(); ++i) r.noteOn (0, notes[i]);
    r.run (6000LL * count - 10);
    auto v = r.onNotes (r.voices);
    if ((int) v.size() > count) v.resize ((size_t) count);
    return v;
}

void testArpShapes()
{
    std::printf ("arp shapes\n");
    const std::vector<int> ceg { 60, 64, 67 };
    CHECK_SEQ (arpSeq (ArpShape::Up, ceg, 6), 60, 64, 67, 60, 64, 67);
    CHECK_SEQ (arpSeq (ArpShape::Down, ceg, 6), 67, 64, 60, 67, 64, 60);
    CHECK_SEQ (arpSeq (ArpShape::UpDown, ceg, 6), 60, 64, 67, 64, 60, 64);
    CHECK_SEQ (arpSeq (ArpShape::DownUp, ceg, 6), 67, 64, 60, 64, 67, 64);
    CHECK_SEQ (arpSeq (ArpShape::UpPlusDown, ceg, 7), 60, 64, 67, 67, 64, 60, 60);
    CHECK_SEQ (arpSeq (ArpShape::DownPlusUp, ceg, 6), 67, 64, 60, 60, 64, 67);
    CHECK_SEQ (arpSeq (ArpShape::ThumbUp, ceg, 6), 60, 64, 60, 67, 60, 64);
    CHECK_SEQ (arpSeq (ArpShape::PinkyUp, ceg, 6), 67, 60, 67, 64, 67, 60);
    const std::vector<int> cegb { 60, 64, 67, 71 };
    CHECK_SEQ (arpSeq (ArpShape::Converge, cegb, 5), 60, 71, 64, 67, 60);
    CHECK_SEQ (arpSeq (ArpShape::Diverge, cegb, 5), 67, 64, 71, 60, 67);
    CHECK_SEQ (arpSeq (ArpShape::ConDiverge, cegb, 7), 60, 71, 64, 67, 64, 71, 60);
    CHECK_SEQ (arpSeq (ArpShape::ThumbUD, cegb, 10), 60, 64, 60, 67, 60, 71, 60, 67, 60, 64);
    CHECK_SEQ (arpSeq (ArpShape::Up, ceg, 6, [] (ArpSlot& s) { s.offset = 1; }), 64, 67, 60, 64, 67, 60);
    CHECK_SEQ (arpSeq (ArpShape::AsPlayed, { 67, 60, 64 }, 4), 67, 60, 64, 67);
    CHECK_SEQ (arpSeq (ArpShape::Chord, ceg, 6), 60, 64, 67, 60, 64, 67);   // two steps of three

    auto rnd = arpSeq (ArpShape::RandNoDup, cegb, 64);
    bool okNoDup = rnd.size() == 64;
    for (size_t i = 1; i < rnd.size(); ++i) okNoDup = okNoDup && rnd[i] != rnd[i - 1];
    CHECK (okNoDup);
    auto once = arpSeq (ArpShape::RandOnce, cegb, 12);
    CHECK (once.size() == 12 && std::equal (once.begin(), once.begin() + 4, once.begin() + 4) && std::set<int> (once.begin(), once.begin() + 4).size() == 4);
    auto drift = arpSeq (ArpShape::RandDrift, cegb, 64);
    bool okDrift = drift.size() == 64;
    for (size_t i = 1; i < drift.size(); ++i)
    {
        const auto a = std::find (cegb.begin(), cegb.end(), drift[i - 1]) - cegb.begin();
        const auto b = std::find (cegb.begin(), cegb.end(), drift[i]) - cegb.begin();
        okDrift = okDrift && std::abs ((int) (a - b)) == 1;
    }
    CHECK (okDrift);
}

void testArpRange()
{
    std::printf ("arp transpose range (SHIFT=3 RANGE=4)\n");
    auto rs = [] (int shape, int count)
    {
        return arpSeq (ArpShape::Up, { 60 }, count, [shape] (ArpSlot& s) { s.shift = 3; s.range = 4; s.rangeShape = shape; });
    };
    CHECK_SEQ (rs (0, 6), 60, 63, 66, 69, 72, 60);                       // Up
    CHECK_SEQ (rs (1, 6), 72, 69, 66, 63, 60, 72);                       // Down
    CHECK_SEQ (rs (2, 9), 60, 63, 66, 69, 72, 69, 66, 63, 60);           // Up/Down
    CHECK_SEQ (rs (3, 9), 72, 69, 66, 63, 60, 63, 66, 69, 72);           // Down/Up
    CHECK_SEQ (rs (4, 10), 60, 63, 66, 69, 72, 72, 69, 66, 63, 60);      // Up+Down
    CHECK_SEQ (rs (6, 9), 60, 63, 60, 66, 60, 69, 60, 72, 60);           // Thumb Up
    CHECK_SEQ (rs (8, 9), 72, 60, 72, 63, 72, 66, 72, 69, 72);           // Pinky Up
    CHECK_SEQ (rs (10, 6), 60, 72, 63, 69, 66, 60);                      // Converge
    CHECK_SEQ (rs (11, 6), 66, 69, 63, 72, 60, 66);                      // Diverge
    CHECK_SEQ (rs (13, 10), 60, 63, 66, 69, 72, 60, 63, 66, 69, 72);     // Chord: all five per step
    // Chord range: five notes on the same sample per step.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.arp.slots[0].shift = 3; r.arp.slots[0].range = 4; r.arp.slots[0].rangeShape = 13;
        r.noteOn (0, 60);
        r.run (5000);
        auto t = r.onTimes (r.voices);
        CHECK (t.size() == 5 && t.front() == 0 && t.back() == 0);
    }
    auto rnd = rs (15, 50);   // Rand No Dup
    bool ok = rnd.size() == 50;
    for (size_t i = 0; i < rnd.size(); ++i) ok = ok && (rnd[i] - 60) % 3 == 0 && rnd[i] >= 60 && rnd[i] <= 72 && (i == 0 || rnd[i] != rnd[i - 1]);
    CHECK (ok);
    // Two notes, range 1, shift 12: P then P+12.
    CHECK_SEQ (arpSeq (ArpShape::Up, { 60, 64 }, 6, [] (ArpSlot& s) { s.shift = 12; s.range = 1; }), 60, 64, 72, 76, 60, 64);
    // Repeats = 2 (one cycle = 2 notes x 2 transpositions) then stops.
    auto rep = arpSeq (ArpShape::Up, { 60, 64 }, 20, [] (ArpSlot& s) { s.shift = 12; s.range = 1; s.repeats = 2; });
    CHECK_SEQ (rep, 60, 64, 72, 76, 60, 64, 72, 76);
}

void testArpTiming()
{
    std::printf ("arp timing\n");
    // 1/16 at 120 BPM / 48 kHz = 6000 samples; gate 50% = 3000. Odd block size.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.arp.slots[0].gate = 0.5f;
        r.noteOn (0, 60); r.noteOn (0, 64);
        r.run (48000, 333);
        bool ok = true;
        int k = 0;
        for (const auto& e : r.voices)
        {
            if (e.on()) { ok = ok && e.t == 6000LL * k; ++k; }
            else if (e.off()) ok = ok && e.t == 6000LL * (k - 1) + 3000;
        }
        CHECK (ok && k == 8);
        CHECK (wellFormed (r.voices, false));
    }
    // Gate 100% and repeated pitch: off and on land on the same sample, off first.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.noteOn (0, 60);
        r.run (24000, 256);
        CHECK (wellFormed (r.voices, false));
        bool ok = r.onTimes (r.voices).size() == 4;
        for (size_t i = 1; i < r.voices.size(); ++i)
            if (r.voices[i].on() && r.voices[i].t > 0) ok = ok && r.voices[i - 1].off() && r.voices[i - 1].t == r.voices[i].t;
        CHECK (ok);
    }
    // Gate 150%: overlapping same pitch stays well-formed.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.arp.slots[0].gate = 1.5f;
        r.noteOn (0, 60);
        r.noteOff (30000, 60);
        r.run (60000, 500);
        CHECK (wellFormed (r.voices, true));
    }
    // Launch quantise: key at sample 1000.
    for (int q : { 3, 5, 7 })   // 1/16, 1/4, 1 bar
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = q;
        r.noteOn (1000, 60);
        r.run (200000, 480);
        auto t = r.onTimes (r.voices);
        const long long grid = (long long) std::llround (launchQuantBeats (q) * 24000.0);
        CHECK (! t.empty() && t[0] == grid);
        CHECK (t.size() > 1 && t[1] == grid + 6000);
    }
    // Hz mode: 4 Hz = 12000 samples regardless of tempo.
    {
        Rig r;
        r.ctx.bpm = 97.0;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.arp.slots[0].hzMode = true;
        r.arp.slots[0].rateHz = 4.0f;
        r.noteOn (0, 60);
        r.run (48000);
        auto t = r.onTimes (r.voices);
        CHECK (t.size() == 4 && t[1] == 12000 && t[3] == 36000);
    }
    // Swing 50% on 1/16: off-beats move by 1/4 of a 16th (1500 samples).
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.set (G().swing, 0.5f);
        r.arp.launchQuant = 0;
        r.arp.slots[0].gate = 0.25f;
        r.noteOn (0, 60);
        r.run (24000, 512);
        auto t = r.onTimes (r.voices);
        CHECK (t.size() == 4 && t[0] == 0 && t[1] == 7500 && t[2] == 12000 && t[3] == 19500);
        CHECK (wellFormed (r.voices, false));
    }
    // Host stopped: free-running internal clock keeps the same timing.
    {
        Rig r;
        r.ctx.playing = false;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.noteOn (100, 60);
        r.run (24000, 512);
        auto t = r.onTimes (r.voices);
        CHECK (t.size() == 4 && t[0] == 100 && t[1] == 6100 && t[3] == 18100);
    }
}

void testArpPlayback()
{
    std::printf ("arp chance / latch / retrigger / velocity / trigger keys / pattern\n");
    {
        auto none = arpSeq (ArpShape::Up, { 60, 64, 67 }, 16, [] (ArpSlot& s) { s.chance = 0.0f; });
        CHECK (none.empty());
        auto all = arpSeq (ArpShape::Up, { 60, 64, 67 }, 16, [] (ArpSlot& s) { s.chance = 1.0f; });
        CHECK_EQ ((int) all.size(), 16);
        // Pre: a skipped note doesn't advance, so the played notes are still in order.
        auto pre = arpSeq (ArpShape::Up, { 60, 64, 67 }, 200, [] (ArpSlot& s) { s.chance = 0.5f; s.chancePre = true; });
        bool ok = pre.size() > 40 && pre.size() < 160 && pre[0] == 60;
        for (size_t i = 1; i < pre.size(); ++i)
            ok = ok && pre[i] == (pre[i - 1] == 60 ? 64 : pre[i - 1] == 64 ? 67 : 60);
        CHECK (ok);
    }
    // Latch: release keeps it playing; a new chord after release replaces it.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.arp.slots[0].latch = true;
        r.arp.slots[0].gate = 0.5f;
        r.noteOn (0, 60); r.noteOn (0, 64);
        r.noteOff (1000, 60); r.noteOff (1000, 64);
        r.run (36000);
        CHECK_SEQ (r.onNotes (r.voices), 60, 64, 60, 64, 60, 64);
        r.voices.clear();
        r.noteOn (36000, 72);
        r.noteOff (37000, 72);
        r.run (18000);
        CHECK_SEQ (r.onNotes (r.voices), 72, 72, 72);
    }
    // No latch: arp stops after release; sustain pedal latches while the arp is on.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.noteOn (0, 60);
        r.noteOff (7000, 60);
        r.run (48000);
        CHECK_EQ ((int) r.onNotes (r.voices).size(), 2);
        CHECK (wellFormed (r.voices, true));
        r.voices.clear();
        r.cc (48000, 64, 127);
        r.noteOn (48000, 62);
        r.noteOff (49000, 62);
        r.run (24000);
        CHECK_EQ ((int) r.onNotes (r.voices).size(), 4);
        r.cc (72000, 64, 0);
        r.run (24000);
        CHECK (r.onNotes (r.voices).size() <= 5);
        CHECK (wellFormed (r.voices, true));
    }
    // Retrigger NOTE restarts the pattern on a new key.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.arp.slots[0].retrigNote = true;
        r.noteOn (0, 60); r.noteOn (0, 64); r.noteOn (0, 67);
        r.noteOn (12000, 72);     // after 60 64; restart -> 60 64 67 72
        r.run (6 * 6000 - 10);
        CHECK_SEQ (r.onNotes (r.voices), 60, 64, 60, 64, 67, 72);
    }
    // Retrigger RATE every 1/8: Up over 4 notes plays 60 64 60 64 ...
    CHECK_SEQ (arpSeq (ArpShape::Up, { 60, 64, 67, 71 }, 6, [] (ArpSlot& s) { s.retrigRate = true; s.retrigRateIndex = 6; }), 60, 64, 60, 64, 60, 64);
    // Velocity decay toward 0.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.arp.slots[0].velocityOn = true;
        r.arp.slots[0].decay = 0.5f;
        r.arp.slots[0].target = 0;
        r.noteOn (0, 60, 120);
        r.run (24000);
        std::vector<int> v;
        for (auto& e : r.voices) if (e.on()) v.push_back (e.vel);
        CHECK_SEQ (v, 120, 60, 30, 15);
    }
    // THRU: input note also plays.
    {
        auto v = arpSeq (ArpShape::Up, { 60 }, 3, [] (ArpSlot& s) { s.thru = true; });
        CHECK (v.size() == 3);
    }
    // Trigger octave launches slot 3 (D in the lowest octave) which plays Down.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.arp.slots[2].shape = ArpShape::Down;
        r.noteOn (0, 60); r.noteOn (0, 64);
        r.noteOn (12000, 2);
        r.run (24000 - 10);
        CHECK_SEQ (r.onNotes (r.voices), 60, 64, 64, 60);
        CHECK (r.np->telemetry().arpSlot.load() == 2);
    }
    // Pattern: init pattern rows 1 2 3 4 over a 3-note chord (row 4 wraps +12).
    CHECK_SEQ (arpSeq (ArpShape::Pattern, { 60, 64, 67 }, 8), 60, 64, 67, 72, 60, 64, 67, 72);
    CHECK_SEQ (arpSeq (ArpShape::Pattern, { 60, 64, 67 }, 5, [] (ArpSlot& s) { s.pattern.wrap = 3; }), 60, 64, 67, 60);   // Skip (row 4 silent)
    CHECK_SEQ (arpSeq (ArpShape::Pattern, { 60, 64, 67 }, 4, [] (ArpSlot& s) { s.pattern.wrap = 2; }), 60, 64, 67, 64);   // Fold
    CHECK_SEQ (arpSeq (ArpShape::Pattern, { 60, 64, 67 }, 8, [] (ArpSlot& s) { s.patternRate = 2.0f; s.pattern.length = 1.0; }), 60, 64, 67, 72, 60, 64, 67, 72);
    CHECK_SEQ (arpSeq (ArpShape::Pattern, { 60, 64, 67 }, 8, [] (ArpSlot& s) { s.pattern.length = 1.0; s.pattern.mode = 1; }), 72, 67, 64, 60, 72, 67, 64, 60);
    CHECK_SEQ (arpSeq (ArpShape::Pattern, { 60, 64, 67 }, 12, [] (ArpSlot& s) { s.pattern.length = 1.0; s.pattern.mode = 2; }), 60, 64, 67, 72, 72, 67, 64, 60, 60, 64, 67, 72);
    CHECK_SEQ (arpSeq (ArpShape::Pattern, { 60, 64 }, 8, [] (ArpSlot& s) { s.pattern.length = 1.0; s.shift = 12; s.range = 1; }), 60, 64, 72, 76, 72, 76, 84, 88);
    CHECK_SEQ (arpSeq (ArpShape::Pattern, { 60, 64, 67 }, 6, [] (ArpSlot& s) { s.pattern.length = 1.0; s.pattern.mode = 6; }), 60, 64, 67, 72);  // One Shot
    {
        auto v = arpSeq (ArpShape::Pattern, { 60, 64, 67 }, 6, [] (ArpSlot& s) { s.pattern.stepMode = 2; s.pattern.steps.resize (1); s.pattern.steps[0].row = 2; });
        CHECK_SEQ (v, 64, 67, 72);  // Chord, 1st inversion
    }
}

// ---------------------------------------------------------------------------
Clip makeClip()
{
    Clip c;
    c.length = 2.0;
    c.settings.launchQuant = 0;
    c.settings.retrig = true;
    ClipNote a; a.start = 0.0; a.length = 0.5; a.pitch = 60; a.velocity = 90;
    ClipNote b; b.start = 1.0; b.length = 0.5; b.pitch = 64; b.velocity = 100;
    c.addNote (a);
    c.addNote (b);
    return c;
}

void testClips()
{
    std::printf ("clip player\n");
    {
        Rig r;
        r.set (G().clipOn, 1.0f);
        r.clips.clips[0] = makeClip();
        r.np->post ({ SeqCommand::ClipLaunch, 0 });
        r.run (48000 * 3, 333);
        std::vector<long long> want;
        for (int loop = 0; loop < 3; ++loop) { want.push_back (48000LL * loop); want.push_back (48000LL * loop + 24000); }
        CHECK (r.onTimes (r.voices) == want);
        CHECK_SEQ (r.onNotes (r.voices), 60, 64, 60, 64, 60, 64);
        bool offs = true;
        for (auto& e : r.voices) if (e.off()) offs = offs && (e.t % 24000) == 12000;
        CHECK (offs);
        CHECK (wellFormed (r.voices, false));
        CHECK ((r.np->telemetry().clipPlaying.load() & 1u) != 0);
    }
    // Launch quantise to 1 bar from a MIDI trigger key; rate 2x; transpose; stop by NOTE GATE.
    {
        Rig r;
        r.set (G().clipOn, 1.0f);
        r.clips.clips[1] = makeClip();
        r.clips.clips[1].settings.launchQuant = 7;
        r.clips.clips[1].settings.rate = 4;          // 2x
        r.clips.clips[1].settings.transpose = 2;
        r.clips.clips[1].settings.noteGate = true;
        r.noteOn (5000, 1);                          // C#-2 -> slot 2
        r.noteOff (96000 + 30000, 1);
        r.run (200000, 512);
        CHECK_SEQ (r.onNotes (r.voices), 62, 66, 62);
        auto t = r.onTimes (r.voices);
        CHECK (t.size() == 3 && t[0] == 96000 && t[1] == 108000 && t[2] == 120000);
        CHECK (wellFormed (r.voices, true));
        CHECK (r.voices.back().t == 126000);         // NOTE GATE cut the last note
    }
    // Non-retrig clip launched mid-bar joins at the host position.
    {
        Rig r;
        r.set (G().clipOn, 1.0f);
        r.clips.clips[0] = makeClip();
        r.clips.clips[0].settings.retrig = false;
        r.run (30000);
        r.np->post ({ SeqCommand::ClipLaunch, 0 });
        r.run (48000);
        auto t = r.onTimes (r.voices);
        CHECK (! t.empty() && t[0] == 48000);       // note at clip beat 0 = host beat 2
    }
    // KB SPAN Mono: keys play the clip transposed by (note - 60).
    {
        Rig r;
        r.set (G().clipOn, 1.0f);
        r.clips.clips[0] = makeClip();
        r.clips.clips[0].settings.kbSpan = 1;
        r.clips.clips[0].settings.noteGate = true;
        r.noteOn (0, 65);
        r.noteOff (47000, 65);
        r.run (60000);
        CHECK_SEQ (r.onNotes (r.voices), 65, 69);
        CHECK (wellFormed (r.voices, true));
    }
    // MONO trigger mode: launching slot 2 stops slot 1; POLY keeps both.
    for (int mode : { 0, 1 })
    {
        Rig r;
        r.set (G().clipOn, 1.0f);
        r.clips.triggerMode = mode;
        r.clips.clips[0] = makeClip();
        r.clips.clips[1] = makeClip();
        r.clips.clips[1].settings.transpose = 12;
        r.np->post ({ SeqCommand::ClipLaunch, 0 });
        r.run (1000);
        r.np->post ({ SeqCommand::ClipLaunch, 1 });
        r.run (96000);
        const uint32_t m = r.np->telemetry().clipPlaying.load();
        CHECK (mode == 0 ? m == 2u : m == 3u);
        CHECK (wellFormed (r.voices, false));
    }
    // Static mode: only the start events, once.
    {
        Rig r;
        r.set (G().clipOn, 1.0f);
        r.clips.clips[0] = makeClip();
        r.clips.clips[0].settings.mode = 5;
        r.np->post ({ SeqCommand::ClipLaunch, 0 });
        r.run (48000 * 3);
        CHECK_SEQ (r.onNotes (r.voices), 60);
    }
    // Random slices: every note-on is a clip note and lands on a slice boundary.
    {
        Rig r;
        r.set (G().clipOn, 1.0f);
        Clip c;
        c.length = 4.0;
        c.settings.launchQuant = 0;
        c.settings.retrig = true;
        c.settings.mode = 2;            // Rand.No Dup
        c.settings.time = 4;            // 1/4 slices
        for (int i = 0; i < 4; ++i) { ClipNote n; n.start = i; n.length = 0.5; n.pitch = 60 + i; c.addNote (n); }
        r.clips.clips[0] = c;
        r.np->post ({ SeqCommand::ClipLaunch, 0 });
        r.run (48000 * 8);
        auto v = r.onNotes (r.voices);
        auto t = r.onTimes (r.voices);
        bool ok = v.size() == 16;
        for (size_t i = 0; i < v.size(); ++i) ok = ok && t[i] == 24000LL * (long long) i && v[i] >= 60 && v[i] <= 63 && (i == 0 || v[i] != v[i - 1]);
        CHECK (ok);
    }
}

void testRecording()
{
    std::printf ("clip recording\n");
    Rig r;
    r.set (G().clipOn, 1.0f);
    r.clips.clips[0].settings.launchQuant = 0;
    r.clips.clips[0].settings.retrig = true;
    r.clips.selected = 0;
    r.np->post ({ SeqCommand::RecordStart, 0 });
    r.noteOn (24000 + 700, 67, 90);         // beat 1.029 -> 1.0
    r.noteOff (24000 + 700 + 11000, 67);    // ~0.458 beats -> 0.5
    r.noteOn (90000, 69);                   // beat 3.75, held across the loop end
    r.noteOff (90000 + 12000, 69);          // ends at beat 4.25 -> length 0.5
    r.run (110000);
    CHECK_EQ (r.np->drainRecorded (r.clips), 2);
    const Clip& c = r.clips.clips[0];
    CHECK (c.notes.size() == 2);
    if (c.notes.size() == 2)
    {
        CHECK (c.notes[0].start == 1.0 && c.notes[0].length == 0.5 && c.notes[0].pitch == 67 && c.notes[0].velocity == 90 && c.notes[0].take == 1);
        CHECK (c.notes[1].start == 3.75 && c.notes[1].length == 0.5 && c.notes[1].pitch == 69);
    }
    CHECK (c.hasUncommitted());
    r.clips.clips[0].commitTakes();
    CHECK (! r.clips.clips[0].hasUncommitted());
    // The recorded note plays back on the next pass (beat 5 = sample 120000).
    r.np->post ({ SeqCommand::RecordStop, 0 });
    r.voices.clear();
    r.run (40000);
    auto t = r.onTimes (r.voices);
    CHECK (std::find (t.begin(), t.end(), 120000) != t.end());

    // EXTEND grows the clip.
    Rig x;
    x.set (G().clipOn, 1.0f);
    x.clips.recordMode = 1;
    x.clips.clips[3].settings.launchQuant = 0;
    x.clips.clips[3].settings.retrig = true;
    x.np->post ({ SeqCommand::RecordStart, 3 });
    x.noteOn (24000 * 6, 60);
    x.noteOff (24000 * 6 + 6000, 60);
    x.run (24000 * 7);
    x.np->drainRecorded (x.clips);
    CHECK (x.clips.clips[3].notes.size() == 1 && x.clips.clips[3].notes[0].start == 6.0);
    CHECK (x.clips.clips[3].length == 8.0);
}

void testClipEdits()
{
    std::printf ("clip edit operations\n");
    Clip c;
    auto add = [&] (double s, double l, int p) { ClipNote n; n.start = s; n.length = l; n.pitch = p; c.addNote (n); };
    add (0.1, 0.4, 60); add (1.05, 0.3, 62); add (2.0, 0.5, 64);
    c.selectAll();
    c.quantiseSelected (0.25);
    CHECK (c.notes[0].start == 0.0 && c.notes[0].length == 0.5 && c.notes[1].start == 1.0 && c.notes[1].length == 0.25);
    c.legatoSelected();
    CHECK (c.notes[0].length == 1.0 && c.notes[1].length == 1.0 && c.notes[2].length == 0.5);
    c.reverseSelected();        // range 0..2.5
    CHECK (c.notes[0].pitch == 64 && c.notes[0].start == 0.0 && c.notes[2].pitch == 60 && c.notes[2].start == 1.5);
    c.moveSelected (0.25, 200);   // pitch clamped as a group
    CHECK (c.notes[0].pitch == 127 && c.notes[0].start == 0.25);
    c.moveSelected (0.0, -200);
    CHECK (c.notes[2].pitch == 0);
    c.duplicateSelected();
    CHECK (c.notes.size() == 6 && c.numSelected() == 3 && c.notes[3].selected);
    c.selectAll();
    c.deleteSelected();
    CHECK (c.empty());

    add (0.0, 1.0, 60);
    c.selectAll();
    c.chopSelected (0.25);
    CHECK (c.notes.size() == 4 && c.notes[3].start == 0.75);
    c.selectAll (false);
    c.selectRect (0.5, 0.6, 0, 127, false);
    CHECK (c.numSelected() == 1);
    c.toggleMuteSelected();
    CHECK (c.notes[2].muted);
    c.selectAll (false);
    c.notes.clear();
    add (0.0, 1.0, 61); add (1.0, 1.0, 66);
    c.selectAll();
    c.conformSelected (0, 0);
    CHECK (c.notes[0].pitch == 62 && c.notes[1].pitch == 67);
    c.scaleTimeSelected (0.5);
    CHECK (c.notes[1].start == 0.5 && c.notes[1].length == 0.5);
    auto clip = c.copySelected();
    c.pasteNotes (clip, 2.0);
    CHECK (c.notes.size() == 4 && c.numSelected() == 2 && c.notes[2].start == 2.0);

    // Time-range cut splits notes crossing the edges; range paste overwrites.
    Clip d;
    ClipNote n; n.start = 0.0; n.length = 4.0; n.pitch = 60; d.addNote (n);
    auto piece = d.cutRange (1.0, 2.0);
    CHECK (piece.size() == 1 && piece[0].start == 0.0 && piece[0].length == 1.0);
    CHECK (d.notes.size() == 2 && d.notes[0].length == 1.0 && d.notes[1].start == 2.0 && d.notes[1].length == 2.0);
    d.pasteRange (piece, 2.5, 1.0);
    CHECK (d.notes.size() == 4);

    // Wrap Clip to Begin Here.
    Clip w;
    w.length = 4.0;
    w.startOffset = 1.0;
    n.start = 0.0; n.length = 0.5; w.addNote (n);
    n.start = 1.0; n.pitch = 62; w.addNote (n);
    w.wrapToStartOffset();
    CHECK (w.startOffset == 0.0 && w.notes[0].pitch == 62 && w.notes[0].start == 0.0 && w.notes[1].start == 3.0);
    w.doubleClip();
    CHECK (w.length == 8.0 && w.notes.size() == 4);
    w.scaleClip (0.5);
    CHECK (w.length == 4.0 && w.notes[1].start == 1.5);
}

// ---------------------------------------------------------------------------
void testPipeline()
{
    std::printf ("transpose / key / scale / MIDI OUT\n");
    // A held note keeps its mapping when TRANSPOSE changes.
    {
        Rig r;
        r.noteOn (0, 60);
        r.run (1000);
        r.set (G().transpose, 5.0f);
        r.noteOn (2000, 62);
        r.noteOff (3000, 60);
        r.noteOff (3000, 62);
        r.run (5000);
        CHECK (r.voices.size() == 4 && r.voices[0].note == 60 && r.voices[1].note == 67);
        CHECK (r.voices[2].off() && r.voices[2].note == 60 && r.voices[3].off() && r.voices[3].note == 67);
    }
    // KEY D, SCALE Major quantises; two inputs mapping to the same pitch stay well formed.
    {
        Rig r;
        r.set (G().key, 3.0f);      // D
        r.set (G().scale, 0.0f);
        r.noteOn (0, 63);           // D# -> E
        r.noteOn (10, 64);          // E
        r.noteOff (20, 63);
        r.noteOff (30, 64);
        r.run (100);
        CHECK (wellFormed (r.voices, true));
        CHECK (r.voices.size() == 4 && r.voices[0].note == 64 && r.voices.back().t == 30);
    }
    // MIDI OUT: Off / Clip Player (no key quantise) / On (quantised, arp included, no direct input).
    for (int mode : { 0, 1, 2 })
    {
        Rig r;
        r.set (G().clipOn, 1.0f);
        r.set (G().midiOut, (float) mode);
        r.set (G().key, 1.0f);      // C
        r.set (G().scale, 30.0f);   // major pentatonic
        Clip c;
        c.settings.launchQuant = 0;
        c.settings.retrig = true;
        ClipNote n; n.start = 0.0; n.length = 0.5; n.pitch = 66; c.addNote (n);    // F# -> G when quantised (tie goes up)
        r.clips.clips[0] = c;
        r.np->post ({ SeqCommand::ClipLaunch, 0 });
        r.noteOn (100, 72);
        r.noteOff (200, 72);
        r.run (20000);
        CHECK_SEQ (r.onNotes (r.voices), 67, 72);
        if (mode == 0) CHECK (r.host.empty());
        if (mode == 1) CHECK_SEQ (r.onNotes (r.host), 66);
        if (mode == 2) CHECK_SEQ (r.onNotes (r.host), 67);
        CHECK (wellFormed (r.host, true));
    }
    // Arp OSC MAPPING range: notes outside the ARP range bypass the arp.
    {
        Rig r;
        KeyboardSettings k;
        k.mapping[KeyboardSettings::Arp].keyLo = 60;
        r.np->setKeyboardSettings (k);
        r.set (G().arpOn, 1.0f);
        r.arp.launchQuant = 0;
        r.noteOn (0, 48);
        r.noteOn (0, 60);
        r.noteOff (20000, 48);
        r.noteOff (20000, 60);
        r.run (30000);
        auto v = r.onNotes (r.voices);
        CHECK (std::count (v.begin(), v.end(), 48) == 1 && std::count (v.begin(), v.end(), 60) == 4);
        CHECK (wellFormed (r.voices, true));
    }
}

// Random note on/off with random transpose / key / scale / arp / clip / midi-out changes.
void testFuzz()
{
    std::printf ("fuzz: every note-on gets a note-off\n");
    std::mt19937 rng (1234);
    for (int run = 0; run < 40; ++run)
    {
        Rig r;
        r.clips.clips[0] = makeClip();
        r.clips.clips[0].settings.launchQuant = (int) (rng() % 4);
        r.clips.clips[1] = makeClip();
        r.clips.clips[1].settings.kbSpan = 2;
        r.clips.triggerOctave = 1;              // C-1 octave = notes 12..23
        r.arp.slots[0].gate = 0.2f + (float) (rng() % 200) / 100.0f;
        r.arp.slots[0].range = (int) (rng() % 3);
        r.arp.slots[0].shift = 7;
        r.arp.slots[0].shape = (ArpShape) (rng() % kNumArpShapes);
        r.arp.slots[0].rangeShape = (int) (rng() % kNumRangeShapes);
        r.arp.slots[0].thru = rng() % 2;
        r.arp.slots[0].latch = rng() % 4 == 0;
        r.set (G().swing, (float) (rng() % 3) * 0.4f);
        bool held[128] {};
        int blocks = 300;
        for (int b = 0; b < blocks; ++b)
        {
            const long long t0 = r.pos;
            const int bs = 64 + (int) (rng() % 900);
            std::vector<long long> times;
            for (int k = (int) (rng() % 4); k > 0; --k) times.push_back (t0 + (long long) (rng() % (unsigned) bs));
            std::sort (times.begin(), times.end());     // input must be time-ordered per key
            for (long long t : times)
            {
                const int note = 10 + (int) (rng() % 80);
                if (held[note]) { r.noteOff (t, note); held[note] = false; }
                else { r.noteOn (t, note, 1 + (int) (rng() % 127)); held[note] = true; }
            }
            if (rng() % 10 == 0) r.set (G().transpose, (float) ((int) (rng() % 25) - 12));
            if (rng() % 10 == 0) r.set (G().key, (float) (rng() % 13));
            if (rng() % 10 == 0) r.set (G().scale, (float) (rng() % 51));
            if (rng() % 25 == 0) r.set (G().arpOn, r.pv[G().arpOn] > 0.5f ? 0.0f : 1.0f);
            if (rng() % 25 == 0) r.set (G().clipOn, r.pv[G().clipOn] > 0.5f ? 0.0f : 1.0f);
            if (rng() % 30 == 0) r.set (G().midiOut, (float) (rng() % 3));
            if (rng() % 40 == 0) r.np->post ({ SeqCommand::ClipToggle, 0 });
            if (rng() % 80 == 0) r.pos += 24000;        // host jumps forward
            if (rng() % 120 == 0) r.ctx.playing = ! r.ctx.playing;
            if (rng() % 50 == 0) r.np->post ({ SeqCommand::ArpLaunch, (int8_t) (rng() % 12) });
            r.run (bs, bs);
        }
        // Release everything, stop clips, turn latch-capable things off, let tails finish.
        for (int n = 0; n < 128; ++n) if (held[n]) r.noteOff (r.pos, n);
        r.np->post ({ SeqCommand::ClipStopAll, 0 });
        r.np->post ({ SeqCommand::ArpStop, 0 });
        r.run (200000, 512);
        r.set (G().midiOut, 0.0f);
        r.run (512);
        const bool okV = wellFormed (r.voices, true), okH = wellFormed (r.host, true);
        if (! okV || ! okH) std::printf ("    (run %d)\n", run);
        CHECK (okV);
        CHECK (okH);
    }
    // Host transport jumps (loop back) while the arp and a clip play.
    {
        Rig r;
        r.set (G().arpOn, 1.0f);
        r.set (G().clipOn, 1.0f);
        r.clips.clips[0] = makeClip();
        r.np->post ({ SeqCommand::ClipLaunch, 0 });
        r.noteOn (0, 60); r.noteOn (0, 63);
        r.run (50000);
        r.pos = 0;                                  // host loops back to 0
        r.run (30000);
        r.ctx.playing = false;                      // host stops: internal clock continues
        r.run (30000);
        r.noteOff (r.pos, 60); r.noteOff (r.pos, 63);
        r.np->post ({ SeqCommand::ClipStopAll, 0 });
        r.run (48000);
        CHECK (wellFormed (r.voices, true));
    }
}

void testSaveLoad()
{
    std::printf ("save / load\n");
    ArpBank a;
    a.name = "Mine";
    a.launchQuant = 6;
    a.triggerOctave = 4;
    a.slots[3].shape = ArpShape::Pattern;
    a.slots[3].shift = -5;
    a.slots[3].range = 3;
    a.slots[3].rangeShape = 12;
    a.slots[3].gate = 1.37f;
    a.slots[3].chancePre = true;
    a.slots[3].retrigRate = true;
    a.slots[3].hzMode = true;
    a.slots[3].rateHz = 3.5f;
    a.slots[3].name = "Lead";
    a.slots[3].pattern.steps[2].accent = true;
    a.slots[3].pattern.steps[5].tie = true;
    a.slots[3].pattern.steps[7].octave = -1;
    a.slots[3].pattern.strums.push_back ({ 1.0, 2.0, 0.05 });
    a.slots[3].pattern.velocityLane.push_back ({ 0.5, 1.5f });
    a.slots[3].pattern.wrap = 2;
    a.applyEdit (0, true, [] (ArpSlot& s) { s.repeats = 7; });
    Node na ("arpBank");
    a.save (na);
    const auto bin = na.toBinary();
    Node back;
    CHECK (Node::fromBinary (bin.data(), bin.size(), back));
    ArpBank b;
    b.load (back);
    Node nb ("arpBank");
    b.save (nb);
    CHECK (nb.toBinary() == bin);
    CHECK (b.slots[3].shape == ArpShape::Pattern && b.slots[3].shift == -5 && b.slots[3].pattern.steps[2].accent && b.slots[11].repeats == 7);
    CHECK (b.slots[3].displayName() == "Lead" && b.slots[0].displayName() == "Up (1/16)");

    ClipBank c;
    c.name = "Bank";
    c.triggerMode = 1;
    c.previewClip = 4;
    c.clips[4] = makeClip();
    c.clips[4].name = "Riff";
    c.clips[4].notes[1].muted = true;
    c.clips[4].notes[0].take = 1;
    c.clips[4].startOffset = 0.5;
    c.applySettings (2, true, [] (ClipSettings& s) { s.rate = 5; s.kbSpan = 3; });
    Node nc ("clipBank");
    c.save (nc);
    const auto cbin = nc.toBinary();
    Node cback;
    CHECK (Node::fromBinary (cbin.data(), cbin.size(), cback));
    ClipBank d;
    d.load (cback);
    Node nd ("clipBank");
    d.save (nd);
    CHECK (nd.toBinary() == cbin);
    CHECK (d.clips[4].notes.size() == 2 && d.clips[4].notes[1].muted && d.clips[4].startOffset == 0.5 && d.clips[9].settings.rate == 5);
    CHECK (d.clipName (4) == "Riff" && d.clipName (0) == "1");

    // Missing keys = defaults.
    ArpBank e;
    e.load (Node ("arpBank"));
    CHECK (e.slots[5].shape == ArpShape::Up && e.launchQuant == 3);
    ClipBank f;
    f.load (Node ("clipBank"));
    CHECK (f.clips[0].length == 4.0 && f.clips[0].settings.launchQuant == 3);

    KeyboardSettings k;
    k.swingDivision = 2;
    k.mapping[KeyboardSettings::OscB] = { 36, 72, 10, 100, true, true };
    Node nk ("keyboard");
    k.save (nk);
    KeyboardSettings k2;
    k2.load (nk);
    CHECK (k2.swingDivision == 2 && k2.mapping == k.mapping);
    CHECK (k.mapping[KeyboardSettings::OscB].map (30, 64) == 42 && k.mapping[KeyboardSettings::OscB].map (60, 5) == -1);
}

void benchmark()
{
    std::printf ("benchmark\n");
    Rig r;
    r.set (G().arpOn, 1.0f);
    r.set (G().clipOn, 1.0f);
    r.set (G().key, 1.0f);
    r.set (G().scale, 5.0f);
    r.set (G().swing, 0.3f);
    r.clips.triggerMode = 1;
    for (int s = 0; s < 4; ++s)
    {
        Clip c;
        c.settings.launchQuant = 0;
        for (int i = 0; i < 64; ++i) { ClipNote n; n.start = i * 0.0625; n.length = 0.05; n.pitch = 40 + (i * 7 + s * 5) % 40; c.addNote (n); }
        r.clips.clips[(size_t) s] = c;
        r.np->post ({ SeqCommand::ClipLaunch, (int8_t) s });
    }
    r.arp.slots[0].rate = 9;        // 1/64
    r.arp.slots[0].range = 2;
    r.arp.slots[0].shift = 12;
    r.arp.slots[0].thru = true;
    r.noteOn (0, 60); r.noteOn (0, 64); r.noteOn (0, 67);
    const int blocks = 20000, bs = 512;
    auto t0 = std::chrono::steady_clock::now();
    r.run ((long long) blocks * bs, bs);
    auto t1 = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro> (t1 - t0).count() / blocks;
    std::printf ("  arp (1/64, range 2, thru) + 4 clips x 64 notes/bar + key/scale + swing: %.2f us per 512-sample block (%.4f%% of real time at 48 kHz); %zu events out\n",
                 us, 100.0 * us / (bs / 48000.0 * 1.0e6), r.voices.size());
}
} // namespace

int main()
{
    testScales();
    testArpShapes();
    testArpRange();
    testArpTiming();
    testArpPlayback();
    testClips();
    testRecording();
    testClipEdits();
    testPipeline();
    testFuzz();
    testSaveLoad();
    benchmark();
    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
