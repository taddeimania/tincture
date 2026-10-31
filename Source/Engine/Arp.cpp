// Arp.cpp — arp bank data, save/load, shape orders and the arp player.
#include "Arp.h"
#include "ParamIds.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace tincture
{
namespace arpchoices
{
#define TINCTURE_LIST(name, ...) const std::vector<std::string>& name() { static const std::vector<std::string> l { __VA_ARGS__ }; return l; }
TINCTURE_LIST (shapes, "Up", "Down", "Up/Down", "Down/Up", "Up+Down", "Down+Up", "Thumb Up", "Thumb UD", "Pinky Up", "Pinky UD",
               "Converge", "Diverge", "Con+Diverge", "As Played", "Chord", "Random", "Rand No Dup", "Rand Drift", "Rand Once", "Pattern")
TINCTURE_LIST (rangeShapes, "Up", "Down", "Up/Down", "Down/Up", "Up+Down", "Down+Up", "Thumb Up", "Thumb UD", "Pinky Up", "Pinky UD",
               "Converge", "Diverge", "Con+Diverge", "Chord", "Random", "Rand No Dup", "Rand Drift", "Rand Once")
TINCTURE_LIST (patternModes, "Normal", "Reverse", "Pendulum", "Random", "Rand Start", "Rand End", "One Shot", "Static")
TINCTURE_LIST (patternTimes, "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars", "4 bars")
TINCTURE_LIST (stepModes, "Normal", "New Only", "Chord", "Chord (new)")
TINCTURE_LIST (wrapModes, "Normal", "Clip", "Fold", "Skip")
#undef TINCTURE_LIST
}

double patternTimeBeats (int i)
{
    static const double t[] { 0.25, 0.5, 1.0, 2.0, 4.0, 8.0, 16.0 };
    return t[std::clamp (i, 0, 6)];
}

ArpShape rangeShapeAsShape (int rs) noexcept
{
    rs = std::clamp (rs, 0, kNumRangeShapes - 1);
    return (ArpShape) (rs <= 12 ? rs : rs + 1);     // skip As Played
}

namespace
{
inline int floorDiv (int a, int b) noexcept { return a >= 0 ? a / b : -((-a + b - 1) / b); }
inline int modPos (int a, int b) noexcept { const int r = a % b; return r < 0 ? r + b : r; }
inline bool isRandomPick (ArpShape s) noexcept { return s == ArpShape::Random || s == ArpShape::RandNoDup || s == ArpShape::RandDrift; }
}

int buildArpOrder (ArpShape shape, int n, int8_t* out, int cap) noexcept
{
    int len = 0;
    auto push = [&] (int v) { if (len < cap) out[len++] = (int8_t) v; };
    if (n <= 0) return 0;
    switch (shape)
    {
        case ArpShape::Down:        for (int i = n - 1; i >= 0; --i) push (i); break;
        case ArpShape::UpDown:      for (int i = 0; i < n; ++i) push (i); for (int i = n - 2; i >= 1; --i) push (i); break;
        case ArpShape::DownUp:      for (int i = n - 1; i >= 0; --i) push (i); for (int i = 1; i <= n - 2; ++i) push (i); break;
        case ArpShape::UpPlusDown:  for (int i = 0; i < n; ++i) push (i); for (int i = n - 1; i >= 0; --i) push (i); break;
        case ArpShape::DownPlusUp:  for (int i = n - 1; i >= 0; --i) push (i); for (int i = 0; i < n; ++i) push (i); break;
        case ArpShape::ThumbUp:
            if (n == 1) { push (0); break; }
            for (int i = 1; i < n; ++i) { push (0); push (i); }
            break;
        case ArpShape::ThumbUD:
            if (n == 1) { push (0); break; }
            for (int i = 1; i < n; ++i) { push (0); push (i); }
            for (int i = n - 2; i >= 2; --i) { push (0); push (i); }
            break;
        case ArpShape::PinkyUp:
            if (n == 1) { push (0); break; }
            for (int i = 0; i <= n - 2; ++i) { push (n - 1); push (i); }
            break;
        case ArpShape::PinkyUD:
            if (n == 1) { push (0); break; }
            for (int i = 0; i <= n - 2; ++i) { push (n - 1); push (i); }
            for (int i = n - 3; i >= 1; --i) { push (n - 1); push (i); }
            break;
        case ArpShape::Converge:
        case ArpShape::Diverge:
        case ArpShape::ConDiverge:
        {
            for (int i = 0, j = n - 1; i <= j; ++i, --j) { push (i); if (j != i) push (j); }
            const int c = len;
            if (shape == ArpShape::Diverge) std::reverse (out, out + len);
            else if (shape == ArpShape::ConDiverge)
            {
                for (int k = c - 2; k >= 0; --k) push (out[k]);
                if (len > 1 && out[len - 1] == out[0]) --len;
            }
            break;
        }
        case ArpShape::Chord:       push (-1); break;
        default:                    for (int i = 0; i < n; ++i) push (i); break;
    }
    return len;
}

// ---------------------------------------------------------------------------
// ArpPattern
// ---------------------------------------------------------------------------
ArpPattern ArpPattern::makeInit()
{
    ArpPattern p;
    for (int i = 0; i < 16; ++i)
    {
        ArpPatternStep s;
        s.start = i * 0.25;
        s.length = 0.25;
        s.row = (i % 4) + 1;
        p.steps.push_back (s);
    }
    return p;
}

void ArpPattern::sortSteps()
{
    std::stable_sort (steps.begin(), steps.end(), [] (const ArpPatternStep& a, const ArpPatternStep& b)
                      { return a.start != b.start ? a.start < b.start : a.row < b.row; });
}

float ArpPattern::velocityAt (double beat) const noexcept
{
    if (velocityLane.empty()) return 1.0f;
    if (beat <= velocityLane.front().beat) return velocityLane.front().value;
    for (size_t i = 1; i < velocityLane.size(); ++i)
        if (beat < velocityLane[i].beat)
        {
            const auto& a = velocityLane[i - 1];
            const auto& b = velocityLane[i];
            const double t = (beat - a.beat) / std::max (1.0e-9, b.beat - a.beat);
            return (float) (a.value + (b.value - a.value) * t);
        }
    return velocityLane.back().value;
}

double ArpPattern::strumAt (double beat) const noexcept
{
    for (const auto& s : strums)
        if (beat >= s.start - 1.0e-9 && beat < s.start + s.length) return s.spread;
    return 0.0;
}

int ArpPattern::resolveRow (int row, int n, int& transpose) const noexcept
{
    transpose = 0;
    if (n <= 0) return -1;
    const int i = row - 1;
    switch (wrap)
    {
        case 1: return std::clamp (i, 0, n - 1);
        case 2:
        {
            if (n == 1) return 0;
            const int per = 2 * (n - 1);
            const int m = modPos (i, per);
            return m < n ? m : per - m;
        }
        case 3: return i < 0 || i >= n ? -1 : i;
        default:
        {
            int q = floorDiv (i, n);
            const int idx = i - q * n;
            if (wrapRange > 0) q = std::clamp (q, -wrapRange, wrapRange);
            transpose = q * wrapPitch;
            return idx;
        }
    }
}

void ArpPattern::save (Node& n) const
{
    n.set ("name", name).set ("length", length).set ("mode", mode).set ("time", time).set ("stepMode", stepMode)
     .set ("wrap", wrap).set ("wrapPitch", wrapPitch).set ("wrapRange", wrapRange);
    for (const auto& s : steps)
    {
        Node& c = n.add ("step");
        c.set ("s", s.start).set ("l", s.length).set ("row", s.row).set ("oct", s.octave).set ("v", (double) s.velocity);
        if (s.accent) c.set ("acc", 1.0);
        if (s.tie) c.set ("tie", 1.0);
        if (s.muted) c.set ("mute", 1.0);
    }
    for (const auto& s : strums)
        n.add ("strum").set ("s", s.start).set ("l", s.length).set ("spread", s.spread);
    for (const auto& p : velocityLane)
        n.add ("vel").set ("b", p.beat).set ("v", (double) p.value);
}

void ArpPattern::load (const Node& n)
{
    *this = ArpPattern();
    name = n.str ("name", "Init");
    length = std::clamp (n.num ("length", 4.0), 1.0 / 16.0, 256.0);
    mode = std::clamp (n.integer ("mode", 0), 0, 7);
    time = std::clamp (n.integer ("time", 2), 0, 6);
    stepMode = std::clamp (n.integer ("stepMode", 0), 0, 3);
    wrap = std::clamp (n.integer ("wrap", 0), 0, 3);
    wrapPitch = std::clamp (n.integer ("wrapPitch", 12), 0, 24);
    wrapRange = std::clamp (n.integer ("wrapRange", 0), 0, 8);
    for (auto* c : n.children ("step"))
    {
        ArpPatternStep s;
        s.start = std::max (0.0, c->num ("s", 0.0));
        s.length = std::max (1.0e-4, c->num ("l", 0.25));
        s.row = std::clamp (c->integer ("row", 1), -32, 64);
        s.octave = std::clamp (c->integer ("oct", 0), -8, 8);
        s.velocity = (float) std::clamp (c->num ("v", 1.0), 0.0, 2.0);
        s.accent = c->flag ("acc");
        s.tie = c->flag ("tie");
        s.muted = c->flag ("mute");
        steps.push_back (s);
    }
    for (auto* c : n.children ("strum"))
        strums.push_back ({ c->num ("s", 0.0), std::max (0.0, c->num ("l", 1.0)), std::clamp (c->num ("spread", 1.0 / 48.0), 0.0, 1.0) });
    for (auto* c : n.children ("vel"))
        velocityLane.push_back ({ c->num ("b", 0.0), (float) std::clamp (c->num ("v", 1.0), 0.0, 2.0) });
    sortSteps();
}

// ---------------------------------------------------------------------------
// ArpSlot / ArpBank
// ---------------------------------------------------------------------------
std::string ArpSlot::rateLabel() const
{
    char buf[32];
    if (shape == ArpShape::Pattern) { std::snprintf (buf, sizeof (buf), "%gx", (double) patternRate); return buf; }
    if (hzMode) { std::snprintf (buf, sizeof (buf), "%.2f Hz", (double) rateHz); return buf; }
    const auto& r = choices::syncRates();
    std::string s = r[(size_t) std::clamp (rate, 0, (int) r.size() - 1)];
    if (triplet) s += "T";
    if (dotted) s += "D";
    return s;
}

std::string ArpSlot::displayName() const
{
    if (! name.empty()) return name;
    return arpchoices::shapes()[(size_t) shape] + " (" + rateLabel() + ")";
}

double ArpSlot::stepBeats (double bpm) const noexcept
{
    double b = hzMode ? (std::max (1.0, bpm) / 60.0) / std::max (0.1, (double) rateHz) : syncRateBeats (rate);
    if (! hzMode)
    {
        if (triplet) b *= 2.0 / 3.0;
        if (dotted) b *= 1.5;
    }
    return std::max (1.0 / 256.0, b);
}

void ArpSlot::save (Node& n) const
{
    n.set ("name", name).set ("shape", (int) shape).set ("hz", hzMode ? 1.0 : 0.0).set ("rate", rate).set ("rateHz", (double) rateHz)
     .set ("trip", triplet ? 1.0 : 0.0).set ("dot", dotted ? 1.0 : 0.0).set ("patternRate", (double) patternRate)
     .set ("rangeShape", rangeShape).set ("shift", shift).set ("range", range)
     .set ("latch", latch ? 1.0 : 0.0).set ("thru", thru ? 1.0 : 0.0).set ("offset", offset).set ("repeats", repeats)
     .set ("gate", (double) gate).set ("chance", (double) chance).set ("chancePre", chancePre ? 1.0 : 0.0)
     .set ("rtLaunch", retrigLaunch ? 1.0 : 0.0).set ("rtRate", retrigRate ? 1.0 : 0.0).set ("rtRateIndex", retrigRateIndex)
     .set ("rtNote", retrigNote ? 1.0 : 0.0).set ("rtFirst", retrigFirst ? 1.0 : 0.0)
     .set ("velOn", velocityOn ? 1.0 : 0.0).set ("velRetrig", velocityRetrig ? 1.0 : 0.0).set ("decay", (double) decay).set ("target", target);
    pattern.save (n.add ("pattern"));
}

void ArpSlot::load (const Node& n)
{
    *this = ArpSlot();
    const ArpSlot d;
    name = n.str ("name");
    shape = (ArpShape) std::clamp (n.integer ("shape", 0), 0, kNumArpShapes - 1);
    hzMode = n.flag ("hz");
    rate = std::clamp (n.integer ("rate", d.rate), 0, (int) choices::syncRates().size() - 1);
    rateHz = (float) std::clamp (n.num ("rateHz", d.rateHz), 0.1, 50.0);
    triplet = n.flag ("trip");
    dotted = n.flag ("dot");
    patternRate = (float) std::clamp (n.num ("patternRate", 1.0), 0.25, 4.0);
    rangeShape = std::clamp (n.integer ("rangeShape", 0), 0, kNumRangeShapes - 1);
    shift = std::clamp (n.integer ("shift", 0), -24, 24);
    range = std::clamp (n.integer ("range", 0), 0, 8);
    latch = n.flag ("latch");
    thru = n.flag ("thru");
    offset = std::clamp (n.integer ("offset", 0), -16, 16);
    repeats = std::clamp (n.integer ("repeats", 0), 0, 64);
    gate = (float) std::clamp (n.num ("gate", 1.0), 0.01, 2.0);
    chance = (float) std::clamp (n.num ("chance", 1.0), 0.0, 1.0);
    chancePre = n.flag ("chancePre");
    retrigLaunch = n.flag ("rtLaunch", true);
    retrigRate = n.flag ("rtRate");
    retrigRateIndex = std::clamp (n.integer ("rtRateIndex", d.retrigRateIndex), 0, (int) choices::syncRates().size() - 1);
    retrigNote = n.flag ("rtNote");
    retrigFirst = n.flag ("rtFirst");
    velocityOn = n.flag ("velOn");
    velocityRetrig = n.flag ("velRetrig");
    decay = (float) std::clamp (n.num ("decay", d.decay), 0.0, 1.0);
    target = std::clamp (n.integer ("target", 0), 0, 127);
    if (auto* p = n.child ("pattern")) pattern.load (*p);
}

void ArpBank::init() { *this = ArpBank(); }

void ArpBank::save (Node& n) const
{
    n.set ("name", name).set ("launchQuant", launchQuant).set ("editAll", editAll ? 1.0 : 0.0).set ("selected", selected)
     .set ("triggerOctave", triggerOctave).set ("showNames", showNames ? 1.0 : 0.0).set ("lock", lockModule ? 1.0 : 0.0);
    for (int i = 0; i < kNumSeqSlots; ++i)
    {
        Node& c = n.add ("slot");
        c.set ("index", i);
        slots[(size_t) i].save (c);
    }
}

void ArpBank::load (const Node& n)
{
    init();
    name = n.str ("name", "Init");
    launchQuant = std::clamp (n.integer ("launchQuant", 3), 0, 9);
    editAll = n.flag ("editAll");
    selected = std::clamp (n.integer ("selected", 0), 0, kNumSeqSlots - 1);
    triggerOctave = std::clamp (n.integer ("triggerOctave", 0), -1, 10);
    showNames = n.flag ("showNames", true);
    lockModule = n.flag ("lock");
    for (auto* c : n.children ("slot"))
    {
        const int i = c->integer ("index", -1);
        if (i >= 0 && i < kNumSeqSlots) slots[(size_t) i].load (*c);
    }
}

// ---------------------------------------------------------------------------
// ArpPlayer
// ---------------------------------------------------------------------------
void ArpPlayer::reset() noexcept
{
    count = 0;
    physCount = 0;
    for (auto& p : phys) p = false;
    sustain = false;
    current = -1;
    running = finished = false;
    queue.clear();
    dirty = true;
    stepCounter = 0;
}

void ArpPlayer::addHeld (int note, int velocity) noexcept
{
    for (int i = 0; i < count; ++i)
        if (held[i].note == note) { held[i].velocity = (uint8_t) velocity; return; }
    if (count >= kMaxHeld) return;
    held[count++] = { (uint8_t) note, (uint8_t) velocity, ++arrival };
    dirty = true;
}

void ArpPlayer::removeHeld (int note) noexcept
{
    for (int i = 0; i < count; ++i)
        if (held[i].note == note)
        {
            for (int j = i + 1; j < count; ++j) held[j - 1] = held[j];
            --count;
            dirty = true;
            return;
        }
}

void ArpPlayer::pruneToPhysical() noexcept
{
    for (int i = count; --i >= 0;)
        if (! phys[held[i].note]) removeHeld (held[i].note);
}

void ArpPlayer::rebuildOrders (const ArpSlot& s) noexcept
{
    dirty = false;
    for (int i = 0; i < count; ++i) sorted[i] = (uint8_t) i;
    std::sort (sorted, sorted + count, [this] (uint8_t a, uint8_t b) { return held[a].note < held[b].note; });

    int8_t items[256];
    int len = buildArpOrder (s.shape, count, items, 256);
    if (s.shape == ArpShape::RandOnce)
        for (int i = len - 1; i > 0; --i) std::swap (items[i], items[rng.next() % (uint32_t) (i + 1)]);
    for (int i = 0; i < len; ++i)
        order[i] = items[i] < 0 ? (int8_t) -1 : (s.shape == ArpShape::AsPlayed ? items[i] : (int8_t) sorted[items[i]]);
    orderLen = len;

    const ArpShape rs = rangeShapeAsShape (s.rangeShape);
    rangeLen = std::max (1, buildArpOrder (rs, s.range + 1, rangeOrder, 64));
    if (rs == ArpShape::RandOnce)
        for (int i = rangeLen - 1; i > 0; --i) std::swap (rangeOrder[i], rangeOrder[rng.next() % (uint32_t) (i + 1)]);
}

int ArpPlayer::pickRandom (ArpShape shape, int n, int& last) noexcept
{
    if (n <= 1) { last = 0; return 0; }
    int i = 0;
    if (shape == ArpShape::RandNoDup && last >= 0)
        i = (last + 1 + (int) (rng.next() % (uint32_t) (n - 1))) % n;
    else if (shape == ArpShape::RandDrift && last >= 0)
    {
        i = last + ((rng.next() & 1u) ? 1 : -1);
        if (i < 0) i = 1;
        if (i >= n) i = n - 2;
    }
    else i = (int) (rng.next() % (uint32_t) n);
    last = i;
    return i;
}

int ArpPlayer::currentRangeK (const ArpSlot& s) noexcept
{
    if (s.range <= 0) return 0;
    const ArpShape rs = rangeShapeAsShape (s.rangeShape);
    if (rs == ArpShape::Chord) return -1;
    if (isRandomPick (rs))
    {
        if (! kChosen) { curK = pickRandom (rs, s.range + 1, lastK); kChosen = true; }
        return curK;
    }
    return std::clamp ((int) rangeOrder[rangePos % rangeLen], 0, s.range);
}

void ArpPlayer::advanceRangeCycle (const ArpSlot& s) noexcept
{
    kChosen = false;
    const int lr = isRandomPick (rangeShapeAsShape (s.rangeShape)) ? s.range + 1 : (s.range > 0 ? rangeLen : 1);
    if (++rangePos >= lr)
    {
        rangePos = 0;
        ++cycles;
        if (s.repeats > 0 && cycles >= s.repeats) finished = true;
    }
}

void ArpPlayer::resetPosition (bool fresh, const ArpSlot& s) noexcept
{
    notePos = rangePos = cycles = 0;
    finished = false;
    lastIdx = lastK = -1;
    kChosen = false;
    if (fresh || s.velocityRetrig) velDecay = 0.0f;
    if (s.shape == ArpShape::RandOnce || rangeShapeAsShape (s.rangeShape) == ArpShape::RandOnce) dirty = true;
}

void ArpPlayer::startRun (double beat, const ArpBank& b) noexcept
{
    const ArpSlot& s = slot (b);
    running = true;
    startBeat = quantiseUp (beat, launchQuantBeats (b.launchQuant));
    nextStep = startBeat;
    lastStep = -1.0e300;
    patBeat = startBeat;
    patCycle = 0;
    PlayRegion r;
    r.loopEnd = s.pattern.length;
    patCursor.start (0.0, r);
    wasPattern = s.shape == ArpShape::Pattern;
    resetPosition (true, s);
    stepCounter = 0;
}

void ArpPlayer::noteOn (int note, int velocity, double beat, const ArpBank& b) noexcept
{
    const ArpSlot& s = slot (b);
    const bool wasEmpty = count == 0;
    const int physBefore = physCount;
    if (latchOn (b) && physCount == 0 && count > 0) { count = 0; dirty = true; }   // a new chord replaces the latched one
    if (! phys[note & 127]) { phys[note & 127] = true; ++physCount; }
    addHeld (note & 127, velocity);
    lastNoteOnBeat = beat;
    if (! running || wasEmpty) startRun (beat, b);
    else if (s.retrigNote && (! s.retrigFirst || physBefore == 0))
    {
        resetPosition (false, s);
        if (s.shape == ArpShape::Pattern)
        {
            PlayRegion r;
            r.loopEnd = s.pattern.length;
            patCursor.start (0.0, r);
            patCycle = 0;
            patBeat = std::max (patBeat, beat);
        }
    }
}

void ArpPlayer::noteOff (int note, double, const ArpBank& b) noexcept
{
    if (phys[note & 127]) { phys[note & 127] = false; --physCount; }
    if (! latchOn (b)) removeHeld (note & 127);
}

void ArpPlayer::setSustain (bool down, const ArpBank& b) noexcept
{
    const bool before = latchOn (b);
    sustain = down;
    if (before && ! latchOn (b)) pruneToPhysical();
}

void ArpPlayer::launch (int slotIdx, double beat, const ArpBank& b) noexcept
{
    const bool before = latchOn (b);
    current = std::clamp (slotIdx, 0, kNumSeqSlots - 1);
    if (before && ! latchOn (b)) pruneToPhysical();
    dirty = true;
    if (count > 0 && (! running || slot (b).retrigLaunch)) startRun (beat, b);
}

void ArpPlayer::stop() noexcept
{
    count = 0;
    running = false;
    dirty = true;
}

int ArpPlayer::outVelocity (int in, const ArpSlot& s) const noexcept
{
    if (! s.velocityOn) return in;
    return std::clamp ((int) std::lround ((float) in + (float) (s.target - in) * velDecay), 1, 127);
}

void ArpPlayer::playNote (double t, double lengthBeats, int pitch, int velocity, const Swing& swing) noexcept
{
    if (pitch < 0 || pitch > 127) return;
    queue.addNote (swing.apply (t), swing.apply (t + std::max (1.0e-6, lengthBeats)), pitch, std::clamp (velocity, 1, 127), 64, 0);
}

void ArpPlayer::regularStep (double t, const ArpSlot& s, double bpm, const Swing& swing) noexcept
{
    if (s.retrigRate && lastStep > -1.0e299)
    {
        const double R = syncRateBeats (s.retrigRateIndex);
        if (std::floor (t / R + 1.0e-9) != std::floor (lastStep / R + 1.0e-9)) resetPosition (false, s);
    }
    lastStep = t;
    const double len = s.stepBeats (bpm);
    nextStep = t + len;
    if (count == 0 || finished) { running = false; return; }
    if (dirty) rebuildOrders (s);

    const bool play = s.chance >= 1.0f || rng.uni() < s.chance;
    if (! play && s.chancePre) return;              // skipped notes don't advance the shape

    const bool rnd = isRandomPick (s.shape);
    const int L = rnd ? count : std::max (1, orderLen);
    const int h = rnd ? (int) sorted[pickRandom (s.shape, count, lastIdx)] : (int) order[modPos (notePos + s.offset, L)];
    const int k = currentRangeK (s);
    if (play)
    {
        const int k0 = k < 0 ? 0 : k, k1 = k < 0 ? s.range : k;
        const int h0 = h < 0 ? 0 : h, h1 = h < 0 ? count - 1 : h;
        for (int hi = h0; hi <= h1; ++hi)
            for (int kk = k0; kk <= k1; ++kk)
                playNote (t, len * s.gate, held[hi].note + kk * s.shift, outVelocity (held[hi].velocity, s), swing);
    }
    if (s.velocityOn) velDecay += (1.0f - velDecay) * s.decay;
    ++stepCounter;
    if (++notePos >= L)
    {
        notePos = 0;
        advanceRangeCycle (s);
    }
}

void ArpPlayer::patternStep (double t, int stepIdx, const ArpSlot& s, double mul, const BlockClock& clk, const Swing& swing) noexcept
{
    const ArpPattern& p = s.pattern;
    const ArpPatternStep& st = p.steps[(size_t) stepIdx];
    if (st.muted || count == 0 || finished) return;
    if (dirty) rebuildOrders (s);
    const bool newOnly = p.stepMode == 1 || p.stepMode == 3;
    const bool chord = p.stepMode >= 2;
    if (newOnly && std::abs (t - lastNoteOnBeat) > clk.beatsPerSample * 1.01) return;
    if (s.chance < 1.0f && rng.uni() >= s.chance) return;

    double lenSrc = st.length * s.gate;
    if (st.tie)
    {
        double next = 1.0e300, first = 1.0e300;
        for (const auto& o : p.steps)
        {
            first = std::min (first, o.start);
            if (o.start > st.start + 1.0e-9) next = std::min (next, o.start);
        }
        lenSrc = (next < 1.0e299 ? next : p.length + first) - st.start;
    }
    const double lenBeats = lenSrc / mul;
    const double factor = (double) st.velocity * p.velocityAt (st.start) * (st.accent ? 1.3 : 1.0);
    const double spread = p.strumAt (st.start) / mul;

    int pitches[kMaxHeld];
    int vels[kMaxHeld];
    int np = 0;
    if (chord)
    {
        const int inv = st.row - 1 + s.offset;
        for (int j = 0; j < count; ++j)
        {
            const int q = j + inv;
            const int h = sorted[modPos (q, count)];
            pitches[np] = held[h].note + 12 * floorDiv (q, count) + 12 * st.octave;
            vels[np++] = held[h].velocity;
        }
    }
    else
    {
        int tr = 0;
        const int idx = p.resolveRow (st.row + s.offset, count, tr);
        if (idx < 0) return;
        const int h = sorted[idx];
        pitches[np] = held[h].note + tr + 12 * st.octave;
        vels[np++] = held[h].velocity;
    }

    const int k = currentRangeK (s);
    const int k0 = k < 0 ? 0 : k, k1 = k < 0 ? s.range : k;
    int rank = 0;
    for (int kk = k0; kk <= k1; ++kk)
        for (int j = 0; j < np; ++j)
        {
            const int v = (int) std::lround (outVelocity (vels[j], s) * factor);
            playNote (t + spread * rank, lenBeats, pitches[j] + kk * s.shift, v, swing);
            ++rank;
        }
    if (s.velocityOn) velDecay += (1.0f - velDecay) * s.decay;
    ++stepCounter;
}

void ArpPlayer::patternAdvance (double limitBeat, const BlockClock& clk, const ArpSlot& s, const Swing& swing) noexcept
{
    const ArpPattern& p = s.pattern;
    static const PlayMode modes[] { PlayMode::Normal, PlayMode::Normal, PlayMode::Normal, PlayMode::Random,
                                    PlayMode::RandStart, PlayMode::RandEnd, PlayMode::OneShot, PlayMode::Static };
    PlayRegion r;
    r.loopStart = 0.0;
    r.loopEnd = p.length;
    r.slice = patternTimeBeats (p.time);
    r.mode = modes[std::clamp (p.mode, 0, 7)];
    const double mul = std::clamp ((double) s.patternRate, 0.25, 4.0);

    for (int guard = 0; guard < 64 && limitBeat > patBeat && running; ++guard)
    {
        double segEnd = limitBeat;
        bool retrig = false;
        if (s.retrigRate)
        {
            const double nb = quantiseUp (patBeat + 1.0e-9, syncRateBeats (s.retrigRateIndex));
            if (nb < limitBeat) { segEnd = nb; retrig = true; }
        }
        const double from = patBeat;
        patCursor.advance ((segEnd - from) * mul, r, [&] (double a, double b, double elapsed, int cyc)
        {
            if (cyc != patCycle) { patCycle = cyc; advanceRangeCycle (s); }
            const bool rev = p.mode == 1 || (p.mode == 2 && (cyc & 1));
            for (size_t i = 0; i < p.steps.size(); ++i)
            {
                const auto& st = p.steps[i];
                if (st.start >= p.length - 1.0e-9) continue;      // past LENGTH
                const double start = rev ? std::max (0.0, p.length - st.start - st.length) : st.start;
                if (start >= a - 1.0e-9 && start < b - 1.0e-9)
                    patternStep (from + (elapsed + start - a) / mul, (int) i, s, mul, clk, swing);
            }
        });
        patBeat = segEnd;
        if (patCursor.done || finished || count == 0) running = false;
        if (retrig && running)
        {
            resetPosition (false, s);
            patCursor.start (0.0, r);
            patCycle = 0;
        }
    }
}

void ArpPlayer::advance (const BlockClock& clk, int limit, const ArpBank& b, double bpm, const Swing& swing) noexcept
{
    if (! running) return;
    const ArpSlot& s = slot (b);
    const double limitBeat = clk.beat0 + (limit - 1 + 1.0e-6) * clk.beatsPerSample;
    const bool isPattern = s.shape == ArpShape::Pattern;
    if (isPattern != wasPattern)
    {
        // SHAPE switched between Pattern and a regular shape while running.
        wasPattern = isPattern;
        const double at = std::max (clk.beat0, isPattern ? nextStep : patBeat);
        patBeat = nextStep = at;
        PlayRegion r;
        r.loopEnd = s.pattern.length;
        patCursor.start (0.0, r);
        patCycle = 0;
    }
    if (isPattern) { patternAdvance (limitBeat, clk, s, swing); return; }

    // Far behind (tempo or rate jumped): realign to the step grid.
    if (nextStep < clk.beat0 - 1.0) nextStep = quantiseUp (clk.beat0, s.stepBeats (bpm));
    for (int guard = 0; running && nextStep <= limitBeat && guard < 4096; ++guard)
        regularStep (nextStep, s, bpm, swing);
}

void ArpPlayer::emit (const BlockClock& clk, MidiBuffer& out) noexcept { queue.emitDue (clk, out, MidiEvent::kArp); }

void ArpPlayer::flush (const BlockClock& clk, MidiBuffer& out) noexcept
{
    queue.cut (-1, -1.0e300);
    queue.emitDue (clk, out, MidiEvent::kArp);
}

void ArpPlayer::resync (double newBeat, const ArpBank& b) noexcept
{
    queue.cut (-1, -1.0e300);
    if (! running) return;
    startBeat = nextStep = patBeat = quantiseUp (newBeat, launchQuantBeats (b.launchQuant));
    lastStep = -1.0e300;
}
} // namespace tincture
