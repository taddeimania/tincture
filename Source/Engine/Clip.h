// Clip.h — the CLIP module: clip banks (12 clips of notes + settings, saved
// as patch data), the piano-roll edit operations on a clip, and ClipPlayer,
// the host-synced audio-thread player (launch quantise, MIDI key triggering,
// KB SPAN, recording).
//
// Also holds the sequencing pieces shared with the arpeggiator: the option
// lists both modules use and PlayCursor (the playhead walk over a looped
// region in the Normal / Random / Rand.Start / Rand.End / Static modes).
//
// Times are in beats (quarter notes). A bar is 4 beats (4/4 assumed: the
// Context has no time signature).
#pragma once

#include "Core.h"
#include "Midi.h"
#include "../Model/Tree.h"

#include <array>
#include <string>
#include <vector>

namespace tincture
{
constexpr int kNumSeqSlots = 12;        // arp slots / clips per bank

namespace seqchoices
{
    const std::vector<std::string>& launchQuants();    // Off, 1/64, 1/32, 1/16, 1/8, 1/4, 1/2, 1 bar, 2 bars, 4 bars
    const std::vector<std::string>& triggerOctaves();  // Off, C-2, C-1, C0 ... C8 (stored value = index - 1)
    const std::vector<std::string>& kbSpans();         // Off, Mono, Poly, Offset
    const std::vector<std::string>& clipModes();       // Normal, Random, Rand.No Dup, Rand.Start, Rand.End, Static
    const std::vector<std::string>& clipTimes();       // 1/64 ... 1 bar (slice length for the random modes)
    const std::vector<std::string>& clipRates();       // 1/8x ... 8x
    const std::vector<std::string>& offsetQuants();    // Off, 1/32, 1/16, 1/8, 1/4, 1/2, 1 Bar
    const std::vector<std::string>& triggerModes();    // MONO, POLY
    const std::vector<std::string>& recordModes();     // OVERDUB, EXTEND
    const std::vector<std::string>& swingDivisions();  // 1/32, 1/16, 1/8, 1/4
}
double launchQuantBeats (int index);     // 0 for Off
double clipTimeBeats (int index);
double clipRateFactor (int index);       // 0.125 .. 8
double offsetQuantBeats (int index);     // 0 for Off
double swingDivisionBeats (int index);

// ---------------------------------------------------------------------------
// PlayCursor: walks a playhead through [loopStart, loopEnd) and reports the
// covered source ranges. Shared by clips and arp patterns.
// ---------------------------------------------------------------------------
enum class PlayMode : uint8_t { Normal, Random, RandNoDup, RandStart, RandEnd, Static, OneShot };

struct PlayRegion
{
    double loopStart = 0.0, loopEnd = 4.0;
    double slice = 0.5;                 // slice length for the random modes
    PlayMode mode = PlayMode::Normal;
};

struct PlayCursor
{
    double pos = 0.0;                   // current source position
    double segEnd = 0.0;                // end of the current contiguous run
    int cycle = 0;                      // times the playhead went back to the loop start
    bool done = false;                  // One Shot finished / Static sent
    int lastSlice = -1;
    Rng rng { 0x1234567u };

    void start (double startPos, const PlayRegion& r) noexcept;

    // Advances by `amount` source beats. For each contiguous source range
    // [a, b) it calls piece (a, b, elapsed, cycle), where `elapsed` is the
    // source time from the start of this advance to a. Static mode reports
    // [pos, pos + 1e-6) once and never moves.
    template <typename Fn>
    void advance (double amount, const PlayRegion& r, Fn&& piece)
    {
        const double len = r.loopEnd - r.loopStart;
        if (done || amount <= 0.0 || len <= 1.0e-6) return;
        if (r.mode == PlayMode::Static)
        {
            piece (pos, pos + 1.0e-6, 0.0, cycle);
            done = true;
            return;
        }
        double elapsed = 0.0;
        for (int guard = 0; elapsed < amount && guard < 4096; ++guard)
        {
            if (segEnd > r.loopEnd || segEnd <= r.loopStart) segEnd = r.loopEnd;
            if (pos >= segEnd - 1.0e-12 || pos < r.loopStart)
            {
                nextSegment (r);
                if (done) return;
            }
            const double take = std::min (segEnd - pos, amount - elapsed);
            piece (pos, pos + take, elapsed, cycle);
            pos += take;
            elapsed += take;
        }
    }

private:
    void nextSegment (const PlayRegion& r) noexcept;
    int numSlices (const PlayRegion& r) const noexcept;
    int randomSlice (const PlayRegion& r, bool noRepeat) noexcept;
};

// ---------------------------------------------------------------------------
// Clip data.
// ---------------------------------------------------------------------------
struct ClipNote
{
    double start = 0.0;                 // beats from the clip start
    double length = 0.25;
    int pitch = 60;
    int velocity = 100;                 // 1..127
    int releaseVelocity = 64;
    bool muted = false;
    bool selected = false;              // editor state (not saved)
    int take = 0;                       // 0 = committed; > 0 = recorded, not yet committed (drawn red)

    double end() const noexcept { return start + length; }
};

// The CLIP SETTINGS pane: these are what EDIT ALL applies to all clips.
struct ClipSettings
{
    int kbSpan = 0;                     // seqchoices::kbSpans()
    int transpose = 0;                  // -48..48
    int mode = 0;                       // seqchoices::clipModes()
    int time = 3;                       // seqchoices::clipTimes(), default 1/8
    int rate = 3;                       // seqchoices::clipRates(), default 1x
    bool bpm = true;                    // off: free tempo (120 BPM internal)
    bool triplet = false, dotted = false;
    int launchQuant = 3;                // seqchoices::launchQuants(), default 1/16
    bool retrig = false;
    bool veloTrig = false;
    bool noteGate = false;

    void save (Node&) const;
    void load (const Node&);
    bool operator== (const ClipSettings&) const = default;
};

struct Clip
{
    std::string name;                   // empty = slot number
    std::vector<ClipNote> notes;        // kept sorted by start, then pitch
    double length = 4.0;                // end flag
    double loopStart = 0.0;             // start flag
    double startOffset = 0.0;           // start-offset marker
    int offsetQuant = 0;                // marker Quantize submenu: seqchoices::offsetQuants()
    ClipSettings settings;

    bool empty() const noexcept { return notes.empty(); }
    double loopLength() const noexcept { return length - loopStart; }
    // Source beats per host beat at the given host tempo.
    double speed (double hostBpm) const noexcept;
    PlayRegion region() const noexcept;

    void save (Node&) const;
    void load (const Node&);

    // ---- editing (message thread) -------------------------------------
    void sort();
    int addNote (const ClipNote& n);                         // returns its index after sorting
    int noteAt (double beat, int pitch) const;               // -1 if none
    bool removeNoteAt (double beat, int pitch);              // double-click on a note
    void selectAll (bool on = true);
    void selectRect (double t0, double t1, int p0, int p1, bool additive);
    int numSelected() const;
    bool selectionRange (double& t0, double& t1) const;     // false when nothing selected
    void deleteSelected();
    void moveSelected (double dBeats, int dSemis);           // arrows: grid / ±1 / ±12
    void resizeSelected (double dBeats, double minLength);   // Shift-arrows
    void quantiseSelected (double grid, bool ends = true);
    void duplicateSelected();                                // copy placed at the selection end
    void chopSelected (double grid);
    void conformSelected (int key, int scale);
    void legatoSelected();
    void toggleMuteSelected();
    void reverseSelected();
    void scaleTimeSelected (double factor);                  // Scale Time 50% / 200%
    void doubleClip();                                       // Double Entire clip
    void scaleClip (double factor);                          // Scale Entire clip 50% / 200%
    void wrapToStartOffset();                                // Wrap Clip to Begin Here

    // Clipboard: notes relative to the selection start.
    std::vector<ClipNote> copySelected() const;
    std::vector<ClipNote> cutSelected();
    void pasteNotes (const std::vector<ClipNote>& clip, double at);     // no overwrite; pasted notes selected
    // Time-range clipboard (notes crossing the edges are split).
    std::vector<ClipNote> copyRange (double t0, double t1) const;
    std::vector<ClipNote> cutRange (double t0, double t1);
    void pasteRange (const std::vector<ClipNote>& clip, double at, double rangeLength);   // overwrites [at, at+len)

    // Recording takes.
    void commitTakes();
    void discardTakes();
    bool hasUncommitted() const;

    // FOLD / zoom-to-fit helpers.
    void usedPitches (bool used[128]) const;
    bool noteBounds (double& t0, double& t1, int& lowPitch, int& highPitch) const;

private:
    void removeRange (double t0, double t1);                 // splits notes crossing the edges
};

struct ClipBank
{
    std::string name = "Init";
    std::array<Clip, kNumSeqSlots> clips;
    int triggerMode = 0;                // 0 MONO, 1 POLY
    bool editAll = false;
    int selected = 0;
    int triggerOctave = 0;              // -1 = Off, 0 = C-2 (notes 0..11) ... 10 = C8
    int previewClip = -1;
    bool lockModule = false;
    int recordMode = 0;                 // 0 OVERDUB, 1 EXTEND
    bool metronome = false;
    double grid = 0.25;                 // piano-roll grid in beats (recording quantise)
    bool snap = true;                   // Disable Snap off

    void init();                        // the Factory > Init bank
    void save (Node&) const;
    void load (const Node&);

    Clip copyClip (int slot) const { return clips[(size_t) slot]; }
    void pasteClip (int slot, const Clip& c) { clips[(size_t) slot] = c; }
    void eraseClip (int slot) { clips[(size_t) slot] = Clip(); }
    void renameClip (int slot, const std::string& n) { clips[(size_t) slot].name = n; }
    std::string clipName (int slot) const;
    void setPreviewClip (int slot) { previewClip = previewClip == slot ? -1 : slot; }

    // A CLIP SETTINGS edit: applied to every clip when EDIT ALL is on or Alt is held.
    template <typename Fn> void applySettings (int slot, bool altHeld, Fn&& fn)
    {
        if (editAll || altHeld) { for (auto& c : clips) fn (c.settings); }
        else fn (clips[(size_t) slot].settings);
    }
};

// ---------------------------------------------------------------------------
// ClipPlayer (audio thread).
// ---------------------------------------------------------------------------
struct RecordedNote
{
    int slot = 0;
    double start = 0.0, length = 0.25;  // quantised, beats in the clip
    uint8_t pitch = 60, velocity = 100, releaseVelocity = 64;
};

class ClipPlayer
{
public:
    static constexpr int kVoices = 16;

    void reset() noexcept;

    // Control, called in time order before render() for the block.
    // trigNote >= 0: the voice belongs to that MIDI key (NOTE GATE / KB SPAN).
    void launch (int slot, double beat, const ClipBank&, double hostBpm,
                 int trigNote = -1, int trigVelocity = 127, int kbTranspose = 0, double kbOffset = 0.0, bool kbPoly = false) noexcept;
    void stopSlot (int slot, double beat) noexcept;
    void stopKey (int note, double beat) noexcept;          // voices started by this MIDI key
    void stopAll (double beat) noexcept;
    void toggle (int slot, double beat, const ClipBank&, double hostBpm) noexcept;

    // Renders the block's notes into `out` (source kClip).
    void render (const BlockClock&, const ClipBank&, double hostBpm, const Swing&, MidiBuffer& out) noexcept;
    // Every sounding note ends now (offset 0), nothing pending is sent.
    void flush (const BlockClock&, MidiBuffer& out) noexcept;
    // The clock jumped (host relocation / loop / start): re-place voices.
    void resync (double newBeat, const ClipBank&, double hostBpm) noexcept;

    // Recording into one slot (the clip keeps playing after stop).
    void startRecording (int slot, int mode, double beat, const ClipBank&, double hostBpm) noexcept;
    void stopRecording (double beat, const ClipBank&, double hostBpm) noexcept;
    void recordNoteOn (int note, int velocity, double beat, const ClipBank&, double hostBpm) noexcept;
    void recordNoteOff (int note, int releaseVelocity, double beat, const ClipBank&, double hostBpm) noexcept;
    bool popRecorded (RecordedNote& n) noexcept { return recorded.pop (n); }
    bool isRecording() const noexcept { return recSlot >= 0; }
    int recordingSlot() const noexcept { return recSlot; }
    double recordPosition() const noexcept { return recPos; }

    // State for the UI and the note processor.
    bool isPlaying (int slot) const noexcept;
    bool isPending (int slot) const noexcept;
    uint32_t playingMask() const noexcept;
    double playhead (int slot) const noexcept;               // -1 when not playing
    int lastLaunched() const noexcept { return lastSlot; }

private:
    struct Voice
    {
        bool active = false;
        bool started = false;
        int slot = -1;
        double startBeat = 0.0, stopBeat = 1.0e300;
        int trigNote = -1;
        float velScale = 1.0f;
        int kbTranspose = 0;
        uint32_t order = 0;
        double cursorBeat = 0.0;        // host beat that cursor.pos corresponds to
        PlayCursor cursor;
    };
    int allocVoice() noexcept;
    double hostStartPos (const Clip&, double beat, double hostBpm) const noexcept;
    double positionAt (const Voice&, const Clip&, double beat, double hostBpm, bool wrap) const noexcept;
    PlayRegion regionFor (const Voice&, const Clip&) const noexcept;

    Voice voices[kVoices];
    NoteQueue queue;
    uint32_t launchCounter = 0;
    int lastSlot = -1;

    // Recording.
    int recSlot = -1, recMode = 0;
    double recPos = 0.0;
    struct Open { bool on = false; double pos = 0.0; uint8_t vel = 0; };
    Open open[128];
    SpscFifo<RecordedNote, 512> recorded;
    void finishRecorded (int note, int relVel, double endPos, const ClipBank&) noexcept;
};
} // namespace tincture
