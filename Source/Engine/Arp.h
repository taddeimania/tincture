// Arp.h — the ARP module: arp banks (12 slots + bank settings, saved as patch
// data), the custom Pattern data model (Arp Pattern Editor), and ArpPlayer,
// the sample-accurate, host-synced arpeggiator that runs on the audio thread.
//
// Timing: everything is in beats. The player schedules notes into a
// NoteQueue at absolute (swung) beats and emits what falls in each block.
#pragma once

#include "Clip.h"           // PlayCursor, shared option lists, BlockClock, NoteQueue

#include <array>
#include <string>
#include <vector>

namespace tincture
{
// SHAPE menu (proposed list from spec E 1.4) + Pattern.
enum class ArpShape : uint8_t
{
    Up, Down, UpDown, DownUp, UpPlusDown, DownPlusUp, ThumbUp, ThumbUD, PinkyUp, PinkyUD,
    Converge, Diverge, ConDiverge, AsPlayed, Chord, Random, RandNoDup, RandDrift, RandOnce, Pattern
};
constexpr int kNumArpShapes = 20;
constexpr int kNumRangeShapes = 18;     // the Shape submenu of RANGE (no As Played / Pattern)

namespace arpchoices
{
    const std::vector<std::string>& shapes();         // SHAPE menu, ArpShape order
    const std::vector<std::string>& rangeShapes();    // RANGE > Shape menu, verbatim order
    const std::vector<std::string>& patternModes();   // Normal, Reverse, Pendulum, Random, Rand Start, Rand End, One Shot, Static
    const std::vector<std::string>& patternTimes();   // 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars
    const std::vector<std::string>& stepModes();      // Normal, New Only, Chord, Chord (new)
    const std::vector<std::string>& wrapModes();      // Normal, Clip, Fold, Skip
}
double patternTimeBeats (int index);
ArpShape rangeShapeAsShape (int rangeShape) noexcept;

// One cycle of a deterministic shape over n items (0 = lowest): writes item
// indices to `out` (-1 = all items at once) and returns the cycle length.
// Random / Rand No Dup / Rand Drift return n (the player picks per step);
// Rand Once and As Played return 0..n-1 (the player supplies the order).
int buildArpOrder (ArpShape shape, int n, int8_t* out, int capacity) noexcept;

// ---------------------------------------------------------------------------
// Pattern data (SHAPE = Pattern). Rows are step numbers relative to the held
// notes: 1 = lowest held note, 2 = the next, 0 / -1 below (resolved by WRAP).
// ---------------------------------------------------------------------------
struct ArpPatternStep
{
    double start = 0.0;                 // beats from the pattern start
    double length = 0.25;
    int row = 1;
    int octave = 0;                     // extra octaves
    float velocity = 1.0f;              // 0..2 (100% = 1)
    bool accent = false;                // Accent lane
    bool tie = false;                   // hold until the next step starts (legato)
    bool muted = false;
};

struct ArpStrum                         // Strum lane region
{
    double start = 0.0, length = 1.0;
    double spread = 1.0 / 48.0;         // beats between successive notes of a chord
};

struct ArpLanePoint { double beat = 0.0; float value = 1.0f; };    // Velocity lane, 0..2

struct ArpPattern
{
    std::string name = "Init";
    double length = 4.0;                // LENGTH (1.0.0 = one bar)
    int mode = 0;                       // arpchoices::patternModes()
    int time = 2;                       // TIME for the random modes (patternTimes)
    int stepMode = 0;                   // arpchoices::stepModes()
    int wrap = 0;                       // arpchoices::wrapModes()
    int wrapPitch = 12;                 // 0..24 semitones per wrap
    int wrapRange = 0;                  // max wrap transpositions, 0 = unlimited
    std::vector<ArpPatternStep> steps;  // sorted by start, then row
    std::vector<ArpStrum> strums;
    std::vector<ArpLanePoint> velocityLane;   // empty = 100% everywhere

    static ArpPattern makeInit();       // 16 x 1/16 steps, rows 1 2 3 4 ...
    void sortSteps();
    float velocityAt (double beat) const noexcept;      // lane value (linear between points)
    double strumAt (double beat) const noexcept;        // spread at a step start, 0 if none

    // Resolves a row against n held notes (sorted low to high): returns the
    // note index (or -1 to skip) and the wrap transposition in semitones.
    int resolveRow (int row, int n, int& transpose) const noexcept;

    void save (Node&) const;
    void load (const Node&);
};

// ---------------------------------------------------------------------------
// One arp slot: PATTERN, TRANSPOSE, PLAYBACK, RETRIGGER and VELOCITY panes.
// ---------------------------------------------------------------------------
struct ArpSlot
{
    std::string name;                   // empty = "<Shape> (<rate>)"
    // PATTERN
    ArpShape shape = ArpShape::Up;
    bool hzMode = false;                // BPM / HZ toggle
    int rate = 7;                       // index into choices::syncRates(), default 1/16
    float rateHz = 8.0f;                // 0.1..50 Hz
    bool triplet = false, dotted = false;
    float patternRate = 1.0f;           // SHAPE = Pattern: x0.25..x4
    ArpPattern pattern = ArpPattern::makeInit();
    // TRANSPOSE
    int rangeShape = 0;                 // arpchoices::rangeShapes()
    int shift = 0;                      // -24..24 semitones
    int range = 0;                      // 0..8 transpositions
    // PLAYBACK
    bool latch = false, thru = false;
    int offset = 0;                     // -16..16
    int repeats = 0;                    // 0 = inf, else 1..64
    float gate = 1.0f;                  // 0.01..2 (1..200 %)
    float chance = 1.0f;                // 0..1
    bool chancePre = false;
    // RETRIGGER
    bool retrigLaunch = true;
    bool retrigRate = false;
    int retrigRateIndex = 3;            // choices::syncRates(), default 1 bar
    bool retrigNote = false, retrigFirst = false;
    // VELOCITY
    bool velocityOn = false, velocityRetrig = false;
    float decay = 0.25f;                // 0..1 per step
    int target = 0;                     // 0..127

    std::string rateLabel() const;
    std::string displayName() const;
    double stepBeats (double bpm) const noexcept;

    void save (Node&) const;
    void load (const Node&);
};

struct ArpBank
{
    std::string name = "Init";
    std::array<ArpSlot, kNumSeqSlots> slots;
    int launchQuant = 3;                // seqchoices::launchQuants(), default 1/16
    bool editAll = false;
    int selected = 0;
    int triggerOctave = 0;              // -1 = Off, 0 = C-2 (notes 0..11) ... 10 = C8
    bool showNames = true;
    bool lockModule = false;

    void init();                        // Factory > Init
    void save (Node&) const;
    void load (const Node&);

    // Slot menu: Rename / Copy / Paste / Erase Pattern.
    void renameSlot (int i, const std::string& n) { slots[(size_t) i].name = n; }
    ArpSlot copySlot (int i) const { return slots[(size_t) i]; }
    void pasteSlot (int i, const ArpSlot& s) { slots[(size_t) i] = s; }
    void eraseSlot (int i) { slots[(size_t) i] = ArpSlot(); }

    // An arp-parameter edit: applied to all slots when EDIT ALL is on or Alt is held.
    template <typename Fn> void applyEdit (int slot, bool altHeld, Fn&& fn)
    {
        if (editAll || altHeld) { for (auto& s : slots) fn (s); }
        else fn (slots[(size_t) slot]);
    }
};

// ---------------------------------------------------------------------------
// ArpPlayer (audio thread).
//
// Per block the note processor calls, in time order:
//     advance (clk, sampleLimit, ...)   steps due before that sample
//     noteOn / noteOff / setSustain / launch at that sample's beat
// then advance (clk, numSamples, ...) and emit (clk, out).
// ---------------------------------------------------------------------------
class ArpPlayer
{
public:
    static constexpr int kMaxHeld = 32;

    void reset() noexcept;

    void noteOn (int note, int velocity, double beat, const ArpBank&) noexcept;
    void noteOff (int note, double beat, const ArpBank&) noexcept;
    void setSustain (bool down, const ArpBank&) noexcept;          // CC64 -> latch while the arp is on
    void launch (int slot, double beat, const ArpBank&) noexcept;  // ARP keyboard / trigger octave / slot play button
    void stop() noexcept;                                          // drop held + latched notes

    // Runs every step due before sample `limit` of the block.
    void advance (const BlockClock&, int limit, const ArpBank&, double bpm, const Swing&) noexcept;
    void emit (const BlockClock&, MidiBuffer& out) noexcept;
    void flush (const BlockClock&, MidiBuffer& out) noexcept;      // sounding notes end now
    void resync (double newBeat, const ArpBank&) noexcept;         // the clock jumped

    // Telemetry.
    int playingSlot (const ArpBank& b) const noexcept { return running ? slotIndex (b) : -1; }
    int slotIndex (const ArpBank& b) const noexcept { return current >= 0 ? current : b.selected; }
    int stepIndex() const noexcept { return stepCounter; }
    bool isRunning() const noexcept { return running; }
    int heldCount() const noexcept { return count; }

private:
    struct Held { uint8_t note, velocity; uint32_t order; };

    const ArpSlot& slot (const ArpBank& b) const noexcept { return b.slots[(size_t) slotIndex (b)]; }
    bool latchOn (const ArpBank& b) const noexcept { return slot (b).latch || sustain; }
    void addHeld (int note, int velocity) noexcept;
    void removeHeld (int note) noexcept;
    void pruneToPhysical() noexcept;
    void startRun (double beat, const ArpBank&) noexcept;
    void resetPosition (bool fresh, const ArpSlot&) noexcept;
    void rebuildOrders (const ArpSlot&) noexcept;
    int pickRandom (ArpShape shape, int n, int& last) noexcept;
    int currentRangeK (const ArpSlot&) noexcept;
    void advanceRangeCycle (const ArpSlot&) noexcept;
    void regularStep (double t, const ArpSlot&, double bpm, const Swing&) noexcept;
    void patternAdvance (double limitBeat, const BlockClock&, const ArpSlot&, const Swing&) noexcept;
    void patternStep (double t, int stepIdx, const ArpSlot&, double mul, const BlockClock&, const Swing&) noexcept;
    void playNote (double t, double lengthBeats, int pitch, int velocity, const Swing&) noexcept;
    int outVelocity (int inVelocity, const ArpSlot&) const noexcept;

    // Held notes: `sorted` low to high; `held` keeps arrival order.
    Held held[kMaxHeld];
    int count = 0;
    uint8_t sorted[kMaxHeld];
    bool phys[128] {};
    int physCount = 0;
    bool sustain = false;
    uint32_t arrival = 0;
    double lastNoteOnBeat = -1.0e300;

    int current = -1;                   // launched slot, -1 = follow the bank's selected slot
    bool running = false, finished = false;
    double startBeat = 0.0, nextStep = 0.0, lastStep = -1.0e300;

    int8_t order[256];
    int orderLen = 0;
    int8_t rangeOrder[64];
    int rangeLen = 1;
    bool dirty = true;
    int notePos = 0, rangePos = 0, cycles = 0;
    int lastIdx = -1, lastK = -1, curK = 0;
    bool kChosen = false;
    float velDecay = 0.0f;
    int stepCounter = 0;

    PlayCursor patCursor;
    double patBeat = 0.0;
    int patCycle = 0;
    bool wasPattern = false;

    Rng rng { 0xA5A5F00Du };
    NoteQueue queue;
};
} // namespace tincture
