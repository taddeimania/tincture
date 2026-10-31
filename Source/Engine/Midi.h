// Midi.h — plain MIDI events, a fixed-capacity event buffer, and the small
// timing helpers shared by the arpeggiator and the clip player (block clock,
// swing, scheduled-note queue, lock-free single-producer FIFO).
//
// Nothing here allocates after construction: everything is safe on the
// audio thread.
#pragma once

#include "Core.h"

#include <atomic>
#include <cmath>
#include <cstdint>

namespace tincture
{
// ---------------------------------------------------------------------------
// One MIDI event at a sample offset inside the current block.
// ---------------------------------------------------------------------------
struct MidiEvent
{
    enum Source : uint8_t { kInput = 0, kClip = 1, kArp = 2 };

    int offset = 0;                 // sample offset in the block
    uint8_t status = 0, data1 = 0, data2 = 0;
    uint8_t source = kInput;        // which stage produced it (MIDI OUT filtering)

    int type() const noexcept       { return status & 0xF0; }
    int channel() const noexcept    { return status & 0x0F; }
    bool isNoteOn() const noexcept  { return type() == 0x90 && data2 > 0; }
    bool isNoteOff() const noexcept { return type() == 0x80 || (type() == 0x90 && data2 == 0); }
    bool isNote() const noexcept    { return type() == 0x80 || type() == 0x90; }
    bool isCC() const noexcept      { return type() == 0xB0; }
    bool isPolyAftertouch() const noexcept { return type() == 0xA0; }
    bool isPitchBend() const noexcept { return type() == 0xE0; }
    int note() const noexcept       { return data1; }
    int velocity() const noexcept   { return data2; }
    int ccNumber() const noexcept   { return data1; }
    int ccValue() const noexcept    { return data2; }

    static MidiEvent noteOn (int offset, int note, int velocity, int channel = 0, uint8_t src = kInput) noexcept
    {
        return { offset, (uint8_t) (0x90 | (channel & 15)), (uint8_t) (note & 127), (uint8_t) std::clamp (velocity, 1, 127), src };
    }
    static MidiEvent noteOff (int offset, int note, int releaseVelocity = 64, int channel = 0, uint8_t src = kInput) noexcept
    {
        return { offset, (uint8_t) (0x80 | (channel & 15)), (uint8_t) (note & 127), (uint8_t) std::clamp (releaseVelocity, 0, 127), src };
    }
    static MidiEvent cc (int offset, int number, int value, int channel = 0, uint8_t src = kInput) noexcept
    {
        return { offset, (uint8_t) (0xB0 | (channel & 15)), (uint8_t) (number & 127), (uint8_t) (value & 127), src };
    }
};

// ---------------------------------------------------------------------------
// Fixed-capacity event list kept sorted by offset. Events with equal offsets
// keep their insertion order (so an on/off pair added in that order stays in
// that order). add() returns false when full (the event is dropped).
// ---------------------------------------------------------------------------
class MidiBuffer
{
public:
    static constexpr int kCapacity = 2048;

    void clear() noexcept { count = 0; }
    int size() const noexcept { return count; }
    bool empty() const noexcept { return count == 0; }
    bool full() const noexcept { return count >= kCapacity; }

    bool add (const MidiEvent& e) noexcept
    {
        if (count >= kCapacity) return false;
        int i = count;
        while (i > 0 && ev[i - 1].offset > e.offset)
        {
            ev[i] = ev[i - 1];
            --i;
        }
        ev[i] = e;
        ++count;
        return true;
    }
    // Adds every event of `other` (stable: on equal offsets, existing events first).
    void addAll (const MidiBuffer& other) noexcept { for (const auto& e : other) add (e); }

    const MidiEvent& operator[] (int i) const noexcept { return ev[i]; }
    const MidiEvent* data() const noexcept { return ev; }
    const MidiEvent* begin() const noexcept { return ev; }
    const MidiEvent* end() const noexcept { return ev + count; }

private:
    MidiEvent ev[kCapacity];
    int count = 0;
};

// ---------------------------------------------------------------------------
// Beat <-> sample mapping for one block. beat0 is the (host or internal)
// position in quarter notes at sample 0. An event at beat b lands on the
// first sample whose beat is >= b.
// ---------------------------------------------------------------------------
struct BlockClock
{
    double beat0 = 0.0;
    double beatsPerSample = 120.0 / 60.0 / 48000.0;
    int numSamples = 0;

    double beatAt (int sample) const noexcept { return beat0 + sample * beatsPerSample; }
    double endBeat() const noexcept { return beatAt (numSamples); }
    int offsetOf (double beat) const noexcept
    {
        const double s = std::ceil ((beat - beat0) / beatsPerSample - 1.0e-6);
        return s < 0.0 ? 0 : (s > 1.0e9 ? 1000000000 : (int) s);
    }
    bool inBlock (double beat) const noexcept { return offsetOf (beat) < numSamples; }
};

// Next grid line at or after `beat` (grid <= 0: beat itself).
inline double quantiseUp (double beat, double grid) noexcept
{
    if (grid <= 0.0) return beat;
    return std::ceil (beat / grid - 1.0e-9) * grid;
}

// ---------------------------------------------------------------------------
// Swing: a monotone time warp applied to note times. Within each pair of
// `division` beats the off-beat moves from 50% of the pair to (50 + 25*amount)%.
// amount 0..1 (the G.swing parameter); 1 = 75% (Ableton display "75%").
// ---------------------------------------------------------------------------
struct Swing
{
    double division = 0.25;         // beats (1/16)
    double amount = 0.0;            // 0..1

    double apply (double beat) const noexcept
    {
        if (amount <= 0.0 || division <= 0.0) return beat;
        const double pair = 2.0 * division;
        const double k = std::floor (beat / pair);
        const double x = beat - k * pair;
        const double mid = division * (1.0 + 0.5 * amount);
        const double y = x < division ? x * (mid / division)
                                      : mid + (x - division) * ((pair - mid) / division);
        return k * pair + y;
    }
};

// ---------------------------------------------------------------------------
// Scheduled notes in absolute beats. Players generate notes slightly ahead
// (swing can delay them into the next block) and emit what's due per block.
// A note's on and off share an id so a stop can cut both consistently.
// ---------------------------------------------------------------------------
class NoteQueue
{
public:
    static constexpr int kCapacity = 1024;

    struct Entry
    {
        double beat;
        uint32_t id;            // shared by a note's on and off
        int16_t owner;          // player-defined (clip voice index, ...)
        uint8_t note, velocity; // velocity: on velocity, or release velocity for offs
        bool on;
        bool dead;
    };

    void clear() noexcept { count = 0; }
    int size() const noexcept { return count; }
    bool hasRoomForNote() const noexcept { return count + 2 <= kCapacity; }

    // Schedules a note; `offBeat` is pushed after `onBeat` if needed so the
    // off always follows the on. Returns false when full.
    bool addNote (double onBeat, double offBeat, int note, int velocity, int releaseVelocity, int owner) noexcept
    {
        if (! hasRoomForNote()) return false;
        if (offBeat < onBeat + 1.0e-7) offBeat = onBeat + 1.0e-7;
        const uint32_t id = ++nextId;
        e[count++] = { onBeat, id, (int16_t) owner, (uint8_t) note, (uint8_t) velocity, true, false };
        e[count++] = { offBeat, id, (int16_t) owner, (uint8_t) note, (uint8_t) releaseVelocity, false, false };
        return true;
    }

    // Emits all entries due before the block end into `out`, in time order
    // (ties: offs first, then by id so a note's on precedes its off).
    void emitDue (const BlockClock& clk, MidiBuffer& out, uint8_t source, int channel = 0) noexcept
    {
        const double end = clk.endBeat();
        for (;;)
        {
            int best = -1;
            for (int i = 0; i < count; ++i)
                if (e[i].beat < end && clk.offsetOf (e[i].beat) < clk.numSamples && (best < 0 || before (e[i], e[best])))
                    best = i;
            if (best < 0) break;
            const Entry x = e[best];
            removeAt (best);
            const int off = std::min (clk.offsetOf (x.beat), clk.numSamples - 1);
            out.add (x.on ? MidiEvent::noteOn (off, x.note, x.velocity, channel, source)
                          : MidiEvent::noteOff (off, x.note, x.velocity, channel, source));
        }
    }

    // Stops an owner's notes at `beat`: unsent ons at/after it are dropped
    // with their offs; offs of notes already sounding move to `beat`.
    // owner < 0 = every owner.
    void cut (int owner, double beat) noexcept
    {
        for (int i = 0; i < count; ++i)
        {
            Entry& x = e[i];
            if ((owner < 0 || x.owner == owner) && x.on && x.beat >= beat - 1.0e-9)
                for (int j = 0; j < count; ++j)
                    if (e[j].id == x.id) e[j].dead = true;
        }
        int n = 0;
        for (int i = 0; i < count; ++i)
            if (! e[i].dead) e[n++] = e[i];
        count = n;
        for (int i = 0; i < count; ++i)
            if ((owner < 0 || e[i].owner == owner) && ! e[i].on && e[i].beat > beat)
                e[i].beat = beat;
    }

    // Queued offs for notes that are sounding (their on was already sent).
    bool isSounding (uint32_t id) const noexcept
    {
        bool hasOn = false, hasOff = false;
        for (int i = 0; i < count; ++i)
            if (e[i].id == id) { if (e[i].on) hasOn = true; else hasOff = true; }
        return hasOff && ! hasOn;
    }

    const Entry& operator[] (int i) const noexcept { return e[i]; }

private:
    static bool before (const Entry& a, const Entry& b) noexcept
    {
        if (! same (a.beat, b.beat)) return a.beat < b.beat;
        if (a.on != b.on) return ! a.on;
        return a.id < b.id;
    }
    void removeAt (int i) noexcept { e[i] = e[--count]; }

    Entry e[kCapacity];
    int count = 0;
    uint32_t nextId = 0;
};

// ---------------------------------------------------------------------------
// Lock-free single-producer / single-consumer FIFO (trivially copyable T).
// ---------------------------------------------------------------------------
template <typename T, int Capacity>
class SpscFifo
{
    static_assert ((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
public:
    bool push (const T& v) noexcept
    {
        const uint32_t w = writePos.load (std::memory_order_relaxed);
        if (w - readPos.load (std::memory_order_acquire) >= (uint32_t) Capacity) return false;
        items[w & (Capacity - 1)] = v;
        writePos.store (w + 1, std::memory_order_release);
        return true;
    }
    bool pop (T& v) noexcept
    {
        const uint32_t r = readPos.load (std::memory_order_relaxed);
        if (r == writePos.load (std::memory_order_acquire)) return false;
        v = items[r & (Capacity - 1)];
        readPos.store (r + 1, std::memory_order_release);
        return true;
    }
private:
    T items[(size_t) Capacity] {};
    std::atomic<uint32_t> writePos { 0 }, readPos { 0 };
};
} // namespace tincture
