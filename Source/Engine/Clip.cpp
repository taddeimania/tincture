// Clip.cpp — clip data, piano-roll edits, save/load and the clip player.
#include "Clip.h"
#include "Scales.h"

#include <algorithm>
#include <cmath>

namespace tincture
{
// ---------------------------------------------------------------------------
// Option lists
// ---------------------------------------------------------------------------
namespace seqchoices
{
#define TINCTURE_LIST(name, ...) const std::vector<std::string>& name() { static const std::vector<std::string> l { __VA_ARGS__ }; return l; }
TINCTURE_LIST (launchQuants, "Off", "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars")
TINCTURE_LIST (triggerOctaves, "Off", "C-2", "C-1", "C0", "C1", "C2", "C3", "C4", "C5", "C6", "C7", "C8")
TINCTURE_LIST (kbSpans, "Off", "Mono", "Poly", "Offset")
TINCTURE_LIST (clipModes, "Normal", "Random", "Rand.No Dup", "Rand.Start", "Rand.End", "Static")
TINCTURE_LIST (clipTimes, "1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar")
TINCTURE_LIST (clipRates, "1/8x", "1/4x", "1/2x", "1x", "2x", "4x", "8x")
TINCTURE_LIST (offsetQuants, "Off", "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar")
TINCTURE_LIST (triggerModes, "MONO", "POLY")
TINCTURE_LIST (recordModes, "OVERDUB", "EXTEND")
TINCTURE_LIST (swingDivisions, "1/32", "1/16", "1/8", "1/4")
#undef TINCTURE_LIST
}

namespace
{
template <size_t N> double pick (const double (&t)[N], int i) noexcept { return t[i < 0 ? 0 : (i >= (int) N ? (int) N - 1 : i)]; }
inline double fmodPos (double x, double m) noexcept { const double r = std::fmod (x, m); return r < 0.0 ? r + m : r; }
inline bool noteLess (const ClipNote& a, const ClipNote& b) noexcept { return a.start != b.start ? a.start < b.start : a.pitch < b.pitch; }
}

double launchQuantBeats (int i)  { static const double t[] { 0.0, 0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 }; return pick (t, i); }
double clipTimeBeats (int i)     { static const double t[] { 0.0625, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 }; return pick (t, i); }
double clipRateFactor (int i)    { static const double t[] { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0, 8.0 }; return pick (t, i); }
double offsetQuantBeats (int i)  { static const double t[] { 0.0, 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 }; return pick (t, i); }
double swingDivisionBeats (int i){ static const double t[] { 0.125, 0.25, 0.5, 1.0 }; return pick (t, i); }

// ---------------------------------------------------------------------------
// PlayCursor
// ---------------------------------------------------------------------------
int PlayCursor::numSlices (const PlayRegion& r) const noexcept
{
    const double s = r.slice > 1.0e-6 ? r.slice : 0.25;
    return std::max (1, (int) std::ceil ((r.loopEnd - r.loopStart) / s - 1.0e-9));
}

int PlayCursor::randomSlice (const PlayRegion& r, bool noRepeat) noexcept
{
    const int n = numSlices (r);
    int s = (int) (rng.next() % (uint32_t) n);
    if (noRepeat && n > 1 && s == lastSlice)
        s = (s + 1 + (int) (rng.next() % (uint32_t) (n - 1))) % n;
    lastSlice = s;
    return s;
}

void PlayCursor::start (double startPos, const PlayRegion& r) noexcept
{
    done = false;
    cycle = 0;
    lastSlice = -1;
    const double len = r.loopEnd - r.loopStart;
    if (len <= 1.0e-6) { pos = r.loopStart; segEnd = r.loopEnd; return; }
    pos = startPos;
    if (pos < r.loopStart || pos >= r.loopEnd) pos = r.loopStart + fmodPos (pos - r.loopStart, len);
    const double S = r.slice > 1.0e-6 ? r.slice : 0.25;
    const int n = numSlices (r);
    const int cur = std::min (n - 1, (int) std::floor ((pos - r.loopStart) / S + 1.0e-9));
    switch (r.mode)
    {
        case PlayMode::Random:
        case PlayMode::RandNoDup:
            lastSlice = cur;
            segEnd = std::min (r.loopStart + (cur + 1) * S, r.loopEnd);
            break;
        case PlayMode::RandEnd:
        {
            const int k = cur + 1 + (int) (rng.next() % (uint32_t) (n - cur));
            segEnd = std::min (r.loopStart + k * S, r.loopEnd);
            break;
        }
        default:
            segEnd = r.loopEnd;
            break;
    }
}

void PlayCursor::nextSegment (const PlayRegion& r) noexcept
{
    const double S = r.slice > 1.0e-6 ? r.slice : 0.25;
    switch (r.mode)
    {
        case PlayMode::OneShot:
        case PlayMode::Static:
            done = true;
            return;
        case PlayMode::Random:
        case PlayMode::RandNoDup:
        {
            const int s = randomSlice (r, r.mode == PlayMode::RandNoDup);
            pos = r.loopStart + s * S;
            segEnd = std::min (pos + S, r.loopEnd);
            ++cycle;
            return;
        }
        case PlayMode::RandStart:
        {
            const int s = randomSlice (r, false);
            pos = r.loopStart + s * S;
            segEnd = r.loopEnd;
            ++cycle;
            return;
        }
        case PlayMode::RandEnd:
        {
            const int k = 1 + (int) (rng.next() % (uint32_t) numSlices (r));
            pos = r.loopStart;
            segEnd = std::min (r.loopStart + k * S, r.loopEnd);
            ++cycle;
            return;
        }
        case PlayMode::Normal:
        default:
            pos = r.loopStart;
            segEnd = r.loopEnd;
            ++cycle;
            return;
    }
}

// ---------------------------------------------------------------------------
// ClipSettings / Clip save + load
// ---------------------------------------------------------------------------
void ClipSettings::save (Node& n) const
{
    n.set ("kbSpan", kbSpan).set ("trans", transpose).set ("mode", mode).set ("time", time).set ("rate", rate)
     .set ("bpm", bpm ? 1.0 : 0.0).set ("trip", triplet ? 1.0 : 0.0).set ("dot", dotted ? 1.0 : 0.0)
     .set ("launchQuant", launchQuant).set ("retrig", retrig ? 1.0 : 0.0).set ("veloTrig", veloTrig ? 1.0 : 0.0)
     .set ("noteGate", noteGate ? 1.0 : 0.0);
}

void ClipSettings::load (const Node& n)
{
    const ClipSettings d;
    kbSpan = std::clamp (n.integer ("kbSpan", d.kbSpan), 0, 3);
    transpose = std::clamp (n.integer ("trans", d.transpose), -48, 48);
    mode = std::clamp (n.integer ("mode", d.mode), 0, 5);
    time = std::clamp (n.integer ("time", d.time), 0, 6);
    rate = std::clamp (n.integer ("rate", d.rate), 0, 6);
    bpm = n.flag ("bpm", d.bpm);
    triplet = n.flag ("trip", d.triplet);
    dotted = n.flag ("dot", d.dotted);
    launchQuant = std::clamp (n.integer ("launchQuant", d.launchQuant), 0, 9);
    retrig = n.flag ("retrig", d.retrig);
    veloTrig = n.flag ("veloTrig", d.veloTrig);
    noteGate = n.flag ("noteGate", d.noteGate);
}

double Clip::speed (double hostBpm) const noexcept
{
    double f = clipRateFactor (settings.rate);
    if (settings.triplet) f *= 1.5;         // note lengths x2/3
    if (settings.dotted) f /= 1.5;          // note lengths x1.5
    if (! settings.bpm) f *= 120.0 / std::max (1.0, hostBpm);
    return f;
}

PlayRegion Clip::region() const noexcept
{
    static const PlayMode modes[] { PlayMode::Normal, PlayMode::Random, PlayMode::RandNoDup, PlayMode::RandStart, PlayMode::RandEnd, PlayMode::Static };
    PlayRegion r;
    r.loopStart = loopStart;
    r.loopEnd = length;
    r.slice = clipTimeBeats (settings.time);
    r.mode = modes[std::clamp (settings.mode, 0, 5)];
    return r;
}

void Clip::save (Node& n) const
{
    n.set ("name", name).set ("length", length).set ("loopStart", loopStart).set ("offset", startOffset).set ("offsetQuant", offsetQuant);
    settings.save (n.add ("settings"));
    for (const auto& x : notes)
    {
        Node& c = n.add ("note");
        c.set ("s", x.start).set ("l", x.length).set ("p", x.pitch).set ("v", x.velocity).set ("r", x.releaseVelocity);
        if (x.muted) c.set ("m", 1.0);
        if (x.take) c.set ("t", x.take);
    }
}

void Clip::load (const Node& n)
{
    *this = Clip();
    name = n.str ("name");
    length = std::max (1.0 / 64.0, n.num ("length", 4.0));
    loopStart = std::clamp (n.num ("loopStart", 0.0), 0.0, length - 1.0 / 64.0);
    startOffset = std::clamp (n.num ("offset", loopStart), loopStart, length);
    offsetQuant = std::clamp (n.integer ("offsetQuant", 0), 0, 6);
    if (auto* s = n.child ("settings")) settings.load (*s);
    for (auto* c : n.children ("note"))
    {
        ClipNote x;
        x.start = std::max (0.0, c->num ("s", 0.0));
        x.length = std::max (1.0e-4, c->num ("l", 0.25));
        x.pitch = std::clamp (c->integer ("p", 60), 0, 127);
        x.velocity = std::clamp (c->integer ("v", 100), 1, 127);
        x.releaseVelocity = std::clamp (c->integer ("r", 64), 0, 127);
        x.muted = c->flag ("m");
        x.take = c->integer ("t", 0);
        notes.push_back (x);
    }
    sort();
}

// ---------------------------------------------------------------------------
// Clip editing
// ---------------------------------------------------------------------------
void Clip::sort() { std::stable_sort (notes.begin(), notes.end(), noteLess); }

int Clip::addNote (const ClipNote& n)
{
    ClipNote x = n;
    x.start = std::max (0.0, x.start);
    x.length = std::max (1.0e-4, x.length);
    x.pitch = std::clamp (x.pitch, 0, 127);
    x.velocity = std::clamp (x.velocity, 1, 127);
    auto it = std::upper_bound (notes.begin(), notes.end(), x, noteLess);
    return (int) (notes.insert (it, x) - notes.begin());
}

int Clip::noteAt (double beat, int pitch) const
{
    for (int i = (int) notes.size(); --i >= 0;)
        if (notes[(size_t) i].pitch == pitch && beat >= notes[(size_t) i].start && beat < notes[(size_t) i].end()) return i;
    return -1;
}

bool Clip::removeNoteAt (double beat, int pitch)
{
    const int i = noteAt (beat, pitch);
    if (i < 0) return false;
    notes.erase (notes.begin() + i);
    return true;
}

void Clip::selectAll (bool on) { for (auto& n : notes) n.selected = on; }

void Clip::selectRect (double t0, double t1, int p0, int p1, bool additive)
{
    if (t1 < t0) std::swap (t0, t1);
    if (p1 < p0) std::swap (p0, p1);
    for (auto& n : notes)
    {
        const bool hit = n.start < t1 && n.end() > t0 && n.pitch >= p0 && n.pitch <= p1;
        n.selected = additive ? (n.selected || hit) : hit;
    }
}

int Clip::numSelected() const { return (int) std::count_if (notes.begin(), notes.end(), [] (const ClipNote& n) { return n.selected; }); }

bool Clip::selectionRange (double& t0, double& t1) const
{
    bool any = false;
    for (const auto& n : notes)
        if (n.selected)
        {
            t0 = any ? std::min (t0, n.start) : n.start;
            t1 = any ? std::max (t1, n.end()) : n.end();
            any = true;
        }
    return any;
}

void Clip::deleteSelected() { notes.erase (std::remove_if (notes.begin(), notes.end(), [] (const ClipNote& n) { return n.selected; }), notes.end()); }

void Clip::moveSelected (double dBeats, int dSemis)
{
    // Keep the selection's shape: limit the move by the notes nearest the edges.
    double minStart = 1.0e300;
    int lo = 127, hi = 0;
    for (const auto& n : notes)
        if (n.selected) { minStart = std::min (minStart, n.start); lo = std::min (lo, n.pitch); hi = std::max (hi, n.pitch); }
    if (minStart > 1.0e299) return;
    dBeats = std::max (dBeats, -minStart);
    dSemis = std::clamp (dSemis, -lo, 127 - hi);
    for (auto& n : notes)
        if (n.selected) { n.start += dBeats; n.pitch += dSemis; }
    sort();
}

void Clip::resizeSelected (double dBeats, double minLength)
{
    for (auto& n : notes)
        if (n.selected) n.length = std::max (std::max (minLength, 1.0e-4), n.length + dBeats);
}

void Clip::quantiseSelected (double grid, bool ends)
{
    if (grid <= 0.0) return;
    for (auto& n : notes)
    {
        if (! n.selected) continue;
        const double e = n.end();
        n.start = std::round (n.start / grid) * grid;
        if (ends)
        {
            const double qe = std::round (e / grid) * grid;
            n.length = qe > n.start + 1.0e-9 ? qe - n.start : grid;
        }
    }
    sort();
}

void Clip::duplicateSelected()
{
    double t0, t1;
    if (! selectionRange (t0, t1)) return;
    const double shift = t1 - t0;
    std::vector<ClipNote> copies;
    for (auto& n : notes)
        if (n.selected) { ClipNote c = n; c.start += shift; c.take = 0; copies.push_back (c); n.selected = false; }
    notes.insert (notes.end(), copies.begin(), copies.end());
    sort();
}

void Clip::chopSelected (double grid)
{
    if (grid <= 1.0e-6) return;
    std::vector<ClipNote> out;
    for (const auto& n : notes)
    {
        if (! n.selected) { out.push_back (n); continue; }
        for (double s = n.start; s < n.end() - 1.0e-9; s += grid)
        {
            ClipNote p = n;
            p.start = s;
            p.length = std::min (grid, n.end() - s);
            out.push_back (p);
        }
    }
    notes.swap (out);
    sort();
}

void Clip::conformSelected (int key, int scale)
{
    for (auto& n : notes)
        if (n.selected) n.pitch = scales::quantise (n.pitch, key, scale);
    sort();
}

void Clip::legatoSelected()
{
    std::vector<ClipNote*> sel;
    for (auto& n : notes) if (n.selected) sel.push_back (&n);
    for (size_t i = 0; i < sel.size(); ++i)
        for (size_t j = i + 1; j < sel.size(); ++j)
            if (sel[j]->start > sel[i]->start + 1.0e-9) { sel[i]->length = sel[j]->start - sel[i]->start; break; }
}

void Clip::toggleMuteSelected()
{
    bool allMuted = true, any = false;
    for (const auto& n : notes) if (n.selected) { any = true; allMuted = allMuted && n.muted; }
    if (! any) return;
    for (auto& n : notes) if (n.selected) n.muted = ! allMuted;
}

void Clip::reverseSelected()
{
    double t0, t1;
    if (! selectionRange (t0, t1)) return;
    for (auto& n : notes)
        if (n.selected) n.start = t0 + t1 - n.end();
    sort();
}

void Clip::scaleTimeSelected (double f)
{
    double t0, t1;
    if (f <= 0.0 || ! selectionRange (t0, t1)) return;
    for (auto& n : notes)
        if (n.selected) { n.start = t0 + (n.start - t0) * f; n.length *= f; }
    sort();
}

void Clip::doubleClip()
{
    const size_t count = notes.size();
    for (size_t i = 0; i < count; ++i)
    {
        ClipNote c = notes[i];
        c.start += length;
        c.selected = false;
        notes.push_back (c);
    }
    length *= 2.0;
    sort();
}

void Clip::scaleClip (double f)
{
    if (f <= 0.0) return;
    length *= f;
    loopStart *= f;
    startOffset *= f;
    for (auto& n : notes) { n.start *= f; n.length *= f; }
}

void Clip::wrapToStartOffset()
{
    const double L = loopLength();
    if (L <= 1.0e-9) return;
    for (auto& n : notes)
        if (n.start >= loopStart && n.start < length)
            n.start = loopStart + fmodPos (n.start - startOffset, L);
    startOffset = loopStart;
    sort();
}

std::vector<ClipNote> Clip::copySelected() const
{
    std::vector<ClipNote> out;
    double t0, t1;
    if (! selectionRange (t0, t1)) return out;
    for (const auto& n : notes)
        if (n.selected) { ClipNote c = n; c.start -= t0; c.selected = false; c.take = 0; out.push_back (c); }
    return out;
}

std::vector<ClipNote> Clip::cutSelected()
{
    auto out = copySelected();
    deleteSelected();
    return out;
}

void Clip::pasteNotes (const std::vector<ClipNote>& clip, double at)
{
    selectAll (false);
    for (const auto& n : clip)
    {
        ClipNote c = n;
        c.start += at;
        c.selected = true;
        notes.push_back (c);
    }
    sort();
}

std::vector<ClipNote> Clip::copyRange (double t0, double t1) const
{
    std::vector<ClipNote> out;
    for (const auto& n : notes)
    {
        const double s = std::max (n.start, t0), e = std::min (n.end(), t1);
        if (e <= s + 1.0e-9) continue;
        ClipNote c = n;
        c.start = s - t0;
        c.length = e - s;
        c.selected = false;
        c.take = 0;
        out.push_back (c);
    }
    return out;
}

void Clip::removeRange (double t0, double t1)
{
    std::vector<ClipNote> out;
    for (const auto& n : notes)
    {
        if (n.end() <= t0 + 1.0e-9 || n.start >= t1 - 1.0e-9) { out.push_back (n); continue; }
        if (n.start < t0) { ClipNote a = n; a.length = t0 - n.start; out.push_back (a); }
        if (n.end() > t1) { ClipNote b = n; b.start = t1; b.length = n.end() - t1; out.push_back (b); }
    }
    notes.swap (out);
    sort();
}

std::vector<ClipNote> Clip::cutRange (double t0, double t1)
{
    auto out = copyRange (t0, t1);
    removeRange (t0, t1);
    return out;
}

void Clip::pasteRange (const std::vector<ClipNote>& clip, double at, double rangeLength)
{
    removeRange (at, at + rangeLength);
    pasteNotes (clip, at);
}

void Clip::commitTakes() { for (auto& n : notes) n.take = 0; }
void Clip::discardTakes() { notes.erase (std::remove_if (notes.begin(), notes.end(), [] (const ClipNote& n) { return n.take != 0; }), notes.end()); }
bool Clip::hasUncommitted() const { return std::any_of (notes.begin(), notes.end(), [] (const ClipNote& n) { return n.take != 0; }); }

void Clip::usedPitches (bool used[128]) const
{
    for (int i = 0; i < 128; ++i) used[i] = false;
    for (const auto& n : notes) used[n.pitch] = true;
}

bool Clip::noteBounds (double& t0, double& t1, int& lo, int& hi) const
{
    if (notes.empty()) return false;
    t0 = 1.0e300; t1 = -1.0e300; lo = 127; hi = 0;
    for (const auto& n : notes)
    {
        t0 = std::min (t0, n.start); t1 = std::max (t1, n.end());
        lo = std::min (lo, n.pitch); hi = std::max (hi, n.pitch);
    }
    return true;
}

// ---------------------------------------------------------------------------
// ClipBank
// ---------------------------------------------------------------------------
void ClipBank::init() { *this = ClipBank(); }

std::string ClipBank::clipName (int slot) const
{
    const auto& n = clips[(size_t) slot].name;
    return n.empty() ? std::to_string (slot + 1) : n;
}

void ClipBank::save (Node& n) const
{
    n.set ("name", name).set ("triggerMode", triggerMode).set ("triggerOctave", triggerOctave).set ("preview", previewClip)
     .set ("lock", lockModule ? 1.0 : 0.0).set ("recordMode", recordMode).set ("metronome", metronome ? 1.0 : 0.0)
     .set ("grid", grid).set ("snap", snap ? 1.0 : 0.0).set ("selected", selected).set ("editAll", editAll ? 1.0 : 0.0);
    for (int i = 0; i < kNumSeqSlots; ++i)
    {
        Node& c = n.add ("clip");
        c.set ("slot", i);
        clips[(size_t) i].save (c);
    }
}

void ClipBank::load (const Node& n)
{
    init();
    name = n.str ("name", "Init");
    triggerMode = std::clamp (n.integer ("triggerMode", 0), 0, 1);
    triggerOctave = std::clamp (n.integer ("triggerOctave", 0), -1, 10);
    previewClip = std::clamp (n.integer ("preview", -1), -1, kNumSeqSlots - 1);
    lockModule = n.flag ("lock");
    recordMode = std::clamp (n.integer ("recordMode", 0), 0, 1);
    metronome = n.flag ("metronome");
    grid = std::clamp (n.num ("grid", 0.25), 1.0 / 512.0, 16.0);
    snap = n.flag ("snap", true);
    selected = std::clamp (n.integer ("selected", 0), 0, kNumSeqSlots - 1);
    editAll = n.flag ("editAll");
    for (auto* c : n.children ("clip"))
    {
        const int i = c->integer ("slot", -1);
        if (i >= 0 && i < kNumSeqSlots) clips[(size_t) i].load (*c);
    }
}

// ---------------------------------------------------------------------------
// ClipPlayer
// ---------------------------------------------------------------------------
void ClipPlayer::reset() noexcept
{
    for (auto& v : voices) v = Voice();
    queue.clear();
    lastSlot = -1;
    recSlot = -1;
    for (auto& o : open) o = Open();
    RecordedNote dummy;
    while (recorded.pop (dummy)) {}
}

int ClipPlayer::allocVoice() noexcept
{
    int best = 0;
    for (int i = 0; i < kVoices; ++i)
    {
        if (! voices[i].active) return i;
        if (voices[i].order < voices[best].order) best = i;
    }
    queue.cut (best, -1.0e300);         // steal the oldest: its notes end now
    return best;
}

double ClipPlayer::hostStartPos (const Clip& c, double beat, double hostBpm) const noexcept
{
    return c.startOffset + beat * c.speed (hostBpm);
}

PlayRegion ClipPlayer::regionFor (const Voice& v, const Clip& c) const noexcept
{
    PlayRegion r = c.region();
    if (v.slot == recSlot && recMode == 1)
    {
        r.loopEnd = 1.0e9;              // EXTEND: no wrap while recording
        r.mode = PlayMode::Normal;
    }
    return r;
}

void ClipPlayer::launch (int slot, double beat, const ClipBank& bank, double hostBpm,
                         int trigNote, int trigVelocity, int kbTranspose, double kbOffset, bool kbPoly) noexcept
{
    if (slot < 0 || slot >= kNumSeqSlots) return;
    const Clip& c = bank.clips[(size_t) slot];
    const double q = quantiseUp (beat, launchQuantBeats (c.settings.launchQuant));

    for (int i = 0; i < kVoices; ++i)
    {
        Voice& v = voices[i];
        if (! v.active) continue;
        const bool sameSlot = v.slot == slot;
        if ((sameSlot && ! kbPoly) || (! sameSlot && bank.triggerMode == 0))
        {
            if (! v.started && q <= v.startBeat) v.active = false;
            else v.stopBeat = std::min (v.stopBeat, q);
        }
    }

    const int i = allocVoice();
    Voice& v = voices[i];
    v = Voice();
    v.active = true;
    v.slot = slot;
    v.startBeat = q;
    v.trigNote = trigNote;
    v.velScale = c.settings.veloTrig && trigNote >= 0 ? (float) trigVelocity / 127.0f : 1.0f;
    v.kbTranspose = kbTranspose;
    v.order = ++launchCounter;
    v.cursor.rng = Rng (0x9E3779B9u * (launchCounter + 1));
    const double base = c.startOffset + kbOffset;
    const double pos = c.settings.retrig ? base : base + q * c.speed (hostBpm);
    v.cursor.start (pos, regionFor (v, c));
    v.cursorBeat = q;
    lastSlot = slot;
}

void ClipPlayer::stopSlot (int slot, double beat) noexcept
{
    for (auto& v : voices)
        if (v.active && v.slot == slot)
        {
            if (! v.started && beat <= v.startBeat) v.active = false;
            else v.stopBeat = std::min (v.stopBeat, beat);
        }
}

void ClipPlayer::stopKey (int note, double beat) noexcept
{
    for (auto& v : voices)
        if (v.active && v.trigNote == note)
        {
            if (! v.started && beat <= v.startBeat) v.active = false;
            else v.stopBeat = std::min (v.stopBeat, beat);
        }
}

void ClipPlayer::stopAll (double beat) noexcept
{
    for (int s = 0; s < kNumSeqSlots; ++s) stopSlot (s, beat);
}

void ClipPlayer::toggle (int slot, double beat, const ClipBank& bank, double hostBpm) noexcept
{
    if (isPlaying (slot) || isPending (slot)) stopSlot (slot, beat);
    else launch (slot, beat, bank, hostBpm);
}

void ClipPlayer::render (const BlockClock& clk, const ClipBank& bank, double hostBpm, const Swing& swing, MidiBuffer& out) noexcept
{
    const double b0 = clk.beat0, b1 = clk.endBeat();
    for (int i = 0; i < kVoices; ++i)
    {
        Voice& v = voices[i];
        if (! v.active) continue;
        const Clip& c = bank.clips[(size_t) v.slot];
        const double a = std::max (b0, v.startBeat), e = std::min (b1, v.stopBeat);
        if (a < e)
        {
            v.started = true;
            const double speed = c.speed (hostBpm);
            const PlayRegion region = regionFor (v, c);
            const int transpose = c.settings.transpose + v.kbTranspose;
            const auto& notes = c.notes;
            v.cursor.advance ((e - a) * speed, region, [&] (double sa, double sb, double elapsed, int)
            {
                auto it = std::lower_bound (notes.begin(), notes.end(), sa - 1.0e-9,
                                            [] (const ClipNote& n, double t) { return n.start < t; });
                for (; it != notes.end() && it->start < sb - 1.0e-9; ++it)
                {
                    if (it->muted) continue;
                    const int pitch = it->pitch + transpose;
                    if (pitch < 0 || pitch > 127) continue;
                    const double on = a + (elapsed + std::max (0.0, it->start - sa)) / speed;
                    const double lenSrc = std::max (1.0e-6, std::min (it->length, region.loopEnd - it->start));
                    const double off = on + lenSrc / speed;
                    const int vel = std::clamp ((int) std::lround ((float) it->velocity * v.velScale), 1, 127);
                    queue.addNote (swing.apply (on), swing.apply (off), pitch, vel, it->releaseVelocity, i);
                }
            });
            v.cursorBeat = e;
            if (v.slot == recSlot) recPos = v.cursor.pos;
        }
        if (v.stopBeat <= b1)
        {
            queue.cut (i, v.stopBeat);
            v.active = false;
        }
    }
    queue.emitDue (clk, out, MidiEvent::kClip);
}

void ClipPlayer::flush (const BlockClock& clk, MidiBuffer& out) noexcept
{
    queue.cut (-1, -1.0e300);
    queue.emitDue (clk, out, MidiEvent::kClip);
}

void ClipPlayer::resync (double newBeat, const ClipBank& bank, double hostBpm) noexcept
{
    queue.cut (-1, -1.0e300);           // sounding notes end at the next emit (offset 0)
    for (auto& v : voices)
    {
        if (! v.active) continue;
        const Clip& c = bank.clips[(size_t) v.slot];
        if (v.stopBeat < 1.0e299) { v.stopBeat = newBeat; continue; }
        if (! v.started) v.startBeat = quantiseUp (newBeat, launchQuantBeats (c.settings.launchQuant));
        else v.startBeat = newBeat;
        if (! c.settings.retrig || ! v.started)
            v.cursor.start (c.settings.retrig ? c.startOffset : hostStartPos (c, v.startBeat, hostBpm), regionFor (v, c));
        v.cursorBeat = v.startBeat;
    }
}

bool ClipPlayer::isPlaying (int slot) const noexcept
{
    for (const auto& v : voices) if (v.active && v.started && v.slot == slot) return true;
    return false;
}

bool ClipPlayer::isPending (int slot) const noexcept
{
    for (const auto& v : voices) if (v.active && ! v.started && v.slot == slot) return true;
    return false;
}

uint32_t ClipPlayer::playingMask() const noexcept
{
    uint32_t m = 0;
    for (const auto& v : voices) if (v.active && v.started) m |= 1u << v.slot;
    return m;
}

double ClipPlayer::playhead (int slot) const noexcept
{
    const Voice* best = nullptr;
    for (const auto& v : voices)
        if (v.active && v.started && v.slot == slot && (! best || v.order > best->order)) best = &v;
    return best ? best->cursor.pos : -1.0;
}

// ---- recording -------------------------------------------------------------
double ClipPlayer::positionAt (const Voice& v, const Clip& c, double beat, double hostBpm, bool wrap) const noexcept
{
    double p = v.cursor.pos + std::max (0.0, beat - v.cursorBeat) * c.speed (hostBpm);
    const double L = c.loopLength();
    if (wrap && L > 1.0e-9 && p >= c.length) p = c.loopStart + fmodPos (p - c.loopStart, L);
    return p;
}

void ClipPlayer::startRecording (int slot, int mode, double beat, const ClipBank& bank, double hostBpm) noexcept
{
    if (slot < 0 || slot >= kNumSeqSlots) return;
    recSlot = slot;
    recMode = mode;
    for (auto& o : open) o = Open();
    if (! isPlaying (slot) && ! isPending (slot)) launch (slot, beat, bank, hostBpm);
}

void ClipPlayer::stopRecording (double beat, const ClipBank& bank, double hostBpm) noexcept
{
    if (recSlot < 0) return;
    for (int n = 0; n < 128; ++n)
        if (open[n].on) recordNoteOff (n, 64, beat, bank, hostBpm);
    recSlot = -1;
}

void ClipPlayer::recordNoteOn (int note, int velocity, double beat, const ClipBank& bank, double hostBpm) noexcept
{
    if (recSlot < 0) return;
    const Clip& c = bank.clips[(size_t) recSlot];
    for (const auto& v : voices)
        if (v.active && v.slot == recSlot)
        {
            open[note & 127] = { true, positionAt (v, c, beat, hostBpm, recMode == 0), (uint8_t) velocity };
            return;
        }
}

void ClipPlayer::recordNoteOff (int note, int releaseVelocity, double beat, const ClipBank& bank, double hostBpm) noexcept
{
    if (recSlot < 0 || ! open[note & 127].on) return;
    const Clip& c = bank.clips[(size_t) recSlot];
    for (const auto& v : voices)
        if (v.active && v.slot == recSlot)
        {
            finishRecorded (note & 127, releaseVelocity, positionAt (v, c, beat, hostBpm, recMode == 0), bank);
            return;
        }
    open[note & 127].on = false;
}

void ClipPlayer::finishRecorded (int note, int relVel, double endPos, const ClipBank& bank) noexcept
{
    Open& o = open[note];
    o.on = false;
    const Clip& c = bank.clips[(size_t) recSlot];
    const double L = c.loopLength();
    double start = o.pos, len = endPos - start;
    if (len <= 0.0 && recMode == 0 && L > 0.0) len += L;    // the note crossed the loop end
    const double grid = bank.snap ? bank.grid : 0.0;
    if (grid > 0.0)
    {
        start = std::round (start / grid) * grid;
        len = std::max (grid, std::round (len / grid) * grid);
        if (recMode == 0 && start >= c.length - 1.0e-9 && L > 0.0) start -= L;
    }
    RecordedNote r;
    r.slot = recSlot;
    r.start = std::max (0.0, start);
    r.length = std::max (1.0e-4, len);
    r.pitch = (uint8_t) note;
    r.velocity = o.vel;
    r.releaseVelocity = (uint8_t) relVel;
    recorded.push (r);
}
} // namespace tincture
