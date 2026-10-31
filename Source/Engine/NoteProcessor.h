// NoteProcessor.h — the per-block MIDI pipeline the synth runs before the
// voices:
//
//   MIDI in ─┬─ clip trigger keys / KB SPAN keys ─> ClipPlayer ─┐
//            ├─ arp trigger keys (launch slots) ─────────────────┤
//            └─ playable notes ──────────────────────────────────┴─> ArpPlayer (if ARP on)
//                                   ─> TRANSPOSE ─> KEY/SCALE quantise ─> voices
//                                                                      └─> MIDI OUT (host)
//
// Every note-on that reaches the voices is remembered with the pitch it was
// mapped to, so its note-off goes to the same pitch even if TRANSPOSE, KEY
// or SCALE change while it is held. Output streams are well formed per
// pitch: a pitch already sounding gets a note-off before a new note-on, and a
// pitch's note-off is sent only when its last holder ends.
//
// Threads: process() runs on the audio thread. post() (commands from the UI)
// and drainRecorded() run on the message thread; telemetry() is read by the UI.
#pragma once

#include "Arp.h"
#include "Clip.h"
#include "Midi.h"
#include "ParamIds.h"
#include "Params.h"

#include <array>
#include <atomic>

namespace tincture
{
// ---------------------------------------------------------------------------
// Keyboard-bar patch data that isn't a parameter: the swing division and the
// OSC MAPPING note/velocity ranges (rows SUB, OSC A, OSC B, OSC C, NOISE, ARP).
// ---------------------------------------------------------------------------
struct NoteRange
{
    int keyLo = 0, keyHi = 127, velLo = 1, velHi = 127;
    bool fold = false;                  // out-of-range notes are octave-folded into the range
    bool warp = false;                  // oscillators only: keep running as a warp source

    // The note to use for this row, or -1 when the row ignores it.
    int map (int note, int velocity) const noexcept;
    bool operator== (const NoteRange&) const = default;
};

struct KeyboardSettings
{
    enum Row { Sub = 0, OscA, OscB, OscC, Noise, Arp, kNumRows };
    int swingDivision = 1;              // seqchoices::swingDivisions(), default 1/16
    std::array<NoteRange, kNumRows> mapping {};

    void save (Node&) const;
    void load (const Node&);
};

// UI -> audio thread commands.
struct SeqCommand
{
    enum Type : uint8_t { ArpLaunch, ArpStop, ClipLaunch, ClipStop, ClipToggle, ClipStopAll, RecordStart, RecordStop, AllNotesOff };
    Type type = AllNotesOff;
    int8_t slot = 0;
};

// Audio -> UI state (relaxed atomics; read whenever).
struct SeqTelemetry
{
    std::atomic<int> arpSlot { -1 };            // playing arp slot, -1 = none
    std::atomic<int> arpStep { 0 };
    std::atomic<uint32_t> clipPlaying { 0 };    // bit n = clip n playing
    std::atomic<uint32_t> clipPending { 0 };    // bit n = launched, waiting for its quantise point
    std::atomic<float> clipPlayhead[kNumSeqSlots];
    std::atomic<int> recordingSlot { -1 };
    std::atomic<float> recordPosition { 0.0f };
    std::atomic<double> beat { 0.0 };           // sequencer clock at the end of the last block
    // Recent notes sent to the voices (MIDI OUT dot display): pitch | velocity << 8 | source << 16.
    static constexpr int kHistory = 32;
    std::atomic<uint32_t> noteCount { 0 };
    std::atomic<uint32_t> notes[kHistory];

    SeqTelemetry();
};

class NoteProcessor
{
public:
    NoteProcessor();

    void prepare (const Context&);
    void reset() noexcept;

    // Runs the pipeline for one block. `in` must be sorted by offset. Clears
    // and fills `toVoices` (all events for the synth, incl. CC/bend) and
    // `toHost` (MIDI OUT; note events only).
    void process (const MidiEvent* in, int numIn, int numSamples, const Context& ctx, const ParamValues& pv,
                  const ArpBank& arpBank, const ClipBank& clipBank, MidiBuffer& toVoices, MidiBuffer& toHost) noexcept;

    // Note-offs for everything sounding (audio thread; e.g. on bypass / transport panic).
    void allNotesOff (int offset, MidiBuffer& toVoices, MidiBuffer& toHost) noexcept;

    // Message thread.
    bool post (const SeqCommand& c) noexcept { return commands.push (c); }
    void setKeyboardSettings (const KeyboardSettings&) noexcept;
    // Moves notes recorded on the audio thread into their clip (as an
    // uncommitted take; EXTEND grows the clip). Returns the number added.
    int drainRecorded (ClipBank& bank);

    const SeqTelemetry& telemetry() const noexcept { return telem; }

private:
    enum Consumed : uint8_t { kNone = 0, kPlay, kClipTrigger, kArpTrigger, kClipKeys };
    static constexpr uint8_t kArpLaunchStatus = 0xF4;   // internal event in the arp input stream

    struct Mapping { int16_t target = -1; uint8_t count = 0; };

    void mapEvent (const MidiEvent& e, int transpose, int key, int scale, bool hostOn, MidiBuffer& toVoices, MidiBuffer& toHost) noexcept;
    void trackOn (uint8_t* counts, int pitch, int vel, int offset, int channel, uint8_t src, MidiBuffer& out) noexcept;
    void trackOff (uint8_t* counts, int pitch, int vel, int offset, int channel, uint8_t src, MidiBuffer& out) noexcept;
    void releaseHost (uint8_t* counts, int offset, MidiBuffer& toHost) noexcept;
    int kbSlot (const ClipBank&) const noexcept;
    void publish (const ArpBank&, const MidiBuffer& toVoices) noexcept;

    ArpPlayer arp;
    ClipPlayer clip;
    MidiBuffer playable, clipOut, arpIn, arpOut, direct, finalStream;

    Consumed consumed[128] {};
    int16_t arpRoute[3][128];           // pitch fed to the arp per (source, note); -1 = bypassed the arp
    Mapping map[3][128];
    uint8_t voiceCount[128] {}, hostCount[128] {}, clipHostCount[128] {};

    double sampleRate = 48000.0;
    double expectedBeat = 0.0;
    bool wasPlaying = false, first = true;
    bool prevArpOn = false, prevClipOn = false;
    int prevMidiOut = 0;
    int chan = 0;

    std::atomic<int> swingDivision { 1 };
    std::atomic<int> arpKeyLo { 0 }, arpKeyHi { 127 }, arpVelLo { 1 }, arpVelHi { 127 };
    std::atomic<bool> arpFold { false };

    SpscFifo<SeqCommand, 64> commands;
    SeqTelemetry telem;
};
} // namespace tincture
