// ModMatrix.h — the modulation matrix: sources, rows (patch data), the
// compiled form used on the audio thread, the VELO / NOTE graphs, and the
// per-voice / global modulation runtime (ModVoice / ModGlobal) that ties the
// envelopes, LFOs and matrix together for the voice/synth core.
//
// Signal math per row (spec §5.2, normalised space; a "+50 %" row moves the
// knob half its travel whatever the units):
//     s  = CRV_src (source)          uni sources 0..1, bi sources -1..1 (curve applied to (s+1)/2)
//     s  = slew (s, RISE, FALL)      custom-curve slew limiter
//     if POL = bi and the source is unipolar: s = 2s - 1
//     a  = aux == none ? 1 : CRV_aux (aux01)       bipolar aux -> (a+1)/2
//     if INV: a = 1 - a
//     m  = s * AMOUNT * a * OUTPUT
//     dest = fromNorm (clamp01 (toNorm (knob) + sum of m over the rows on dest))
//
// Evaluation order, per tick, per voice (ModVoice::tick does this):
//   1. Global rows (dest without kPerVoice: macros, master, FX, buses, ...)
//      once per BLOCK from the newest voice's sources: macro dests first, then
//      the rest, which see the modulated macros (ModGlobal::beginBlock).
//   2. "Modulator" rows: dests that are ENV / LFO parameters. Env/LFO sources
//      hold their PREVIOUS tick's values here, so LFO 1 -> LFO 2 RATE (and any
//      cycle such as LFO 2 -> LFO 1 RATE as well) works with one tick (kTick
//      samples) of latency and no ordering problem.
//   3. Envelopes and LFOs tick with those modulated parameters.
//   4. All other per-voice rows, with this tick's env / LFO values.
#pragma once

#include "Envelope.h"
#include "Lfo.h"

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tincture
{
// ---------------------------------------------------------------------------
// Sources (49 + None). Order = the standard source tree order. Saved by id.
// ---------------------------------------------------------------------------
enum class ModSource : int8_t
{
    None = -1,
    Env1 = 0, Env2, Env3, Env4,
    Lfo1, Lfo2, Lfo3, Lfo4, Lfo5, Lfo6, Lfo7, Lfo8, Lfo9, Lfo10,
    Velocity, ReleaseVelocity, Note, NoteOnRand1, NoteOnRand2, NoteOnRandDiscrete, NoteOnAlt1, NoteOnAlt2,
    ActiveVoices, VoiceIndex, VoiceMod1, VoiceMod2, Fixed,
    OscA, OscB, OscC, Noise, Sub,
    Macro1, Macro2, Macro3, Macro4, Macro5, Macro6, Macro7, Macro8,
    ExprX, ExprY, ExprZ,
    Filter1, Filter2,
    ModWheel, Aftertouch, PolyAftertouch, PitchBend,
    Count
};
constexpr int kNumModSources = (int) ModSource::Count;
static_assert (kNumModSources == 49, "the spec has 49 sources");

inline ModSource envSource (int i) noexcept   { return (ModSource) ((int) ModSource::Env1 + i); }
inline ModSource lfoSource (int i) noexcept   { return (ModSource) ((int) ModSource::Lfo1 + i); }
inline ModSource macroSource (int i) noexcept { return (ModSource) ((int) ModSource::Macro1 + i); }

struct ModSourceInfo
{
    const char* id;         // saved in presets, e.g. "lfo3"
    const char* shortName;  // matrix cell, e.g. "LFO 3"
    const char* menuName;   // source menu item, e.g. "LFO 3", "Poly Aftertch"
    bool bipolar;           // native range -1..1 (else 0..1)
    bool perVoice;          // differs per voice (else one value for the synth)
};
const ModSourceInfo& modSourceInfo (ModSource s) noexcept;     // None -> { "none", "---", "---" }
ModSource modSourceFromId (const std::string& id) noexcept;    // None if unknown

// The standard source tree (§5.2): 7 sub-menus then 4 single items. A
// single item is a category with one source and `leaf` = true.
struct ModSourceCategory
{
    const char* name;
    std::vector<ModSource> sources;
    bool leaf;
};
const std::vector<ModSourceCategory>& modSourceTree();

// Current value of every source for one voice. Filled by ModVoice (envs,
// LFOs, note sources, globals); the voice writes the audio/expression
// sources itself (see ModVoice).
struct SourceValues
{
    float v[kNumModSources] {};
    float& operator[] (ModSource s) noexcept { return v[(int) s]; }
    float operator[] (ModSource s) const noexcept { return v[(int) s]; }
    // Copies the sources that are the same for every voice (!perVoice).
    void copyGlobalsFrom (const SourceValues& g) noexcept;
};

// ---------------------------------------------------------------------------
// Rows (patch data)
// ---------------------------------------------------------------------------
constexpr int kMaxModRows = 64;

// The CRV cell's simple bend: 0.5 = linear, > 0.5 bows up (x^(1/k)), < 0.5 down.
float modSimpleBend (float x, float bend) noexcept;

struct ModRow
{
    ModSource source = ModSource::None;
    float curve = 0.5f;             // simple bend, 0.5 = linear (CRV)
    bool customCurve = false;       // "Editable Curve": use srcCurve instead
    Curve srcCurve;
    float rise = 0.0f, fall = 0.0f; // slew limiter, seconds for a full-scale move (custom curve editor)
    float amount = 0.0f;            // -1..1
    bool bipolar = false;           // POL
    int dest = -1;                  // parameter index
    ModSource aux = ModSource::None;
    bool auxInvert = false;
    float auxCurve = 0.5f;
    bool auxCustom = false;
    Curve auxCurveData;
    float output = 1.0f;            // 0..1
    bool bypass = false;

    bool active() const noexcept { return source != ModSource::None && dest >= 0 && ! bypass; }
    void save (Node& n) const;
    void load (const Node& n);      // dest is looked up by the saved parameter id
    bool operator== (const ModRow& o) const;
};

struct ModMatrixData
{
    std::vector<ModRow> rows;       // at most kMaxModRows
    bool locked = false;            // "Lock Matrix (Keep Assignments on Preset Change)"

    // Editing. Destinations must be kModulatable parameters (else -1 / false).
    int add (const ModRow& r);                                   // index or -1 (full / bad dest)
    // Drag-create / "Mod Source" menu: Amount 100 %, POL by the dest's default.
    // Returns the existing row if this source already drives this dest.
    int assign (ModSource s, int dest, float amount = 1.0f);
    bool remove (int index);
    bool move (int from, int to);
    void sortBySource();                                         // enum order, empty rows last
    void sortByDestination();                                    // parameter order, empty rows last
    std::vector<int> findRows (int dest) const;
    std::vector<int> rowsForSource (ModSource s) const;          // also the tile's (n) badge
    int find (ModSource s, int dest) const;                      // -1 if none
    void removeSource (ModSource s);                             // "Remove All Destinations"
    void removeDest (int dest);                                  // "Remove All Modulators"
    void setSourceBypass (ModSource s, bool bypass);             // "Bypass All Destinations"
    void removeRowsWhere (const std::function<bool (int dest)>& pred);
    void removeRowsForGroup (const std::string& group);          // a module's params (ParamInfo::group)
    static bool defaultBipolar (int dest);                       // §3: centred controls get bi

    // Matrix menu.
    // "Create Vibrato (Unused LFO->Pitch via Wheel)": first LFO without
    // destinations -> Main Tuning, aux Mod Wheel, +-0.5 st, bi. If `pv` /
    // `lfos` (kNumLfos entries) are given and the LFO is untouched it is set
    // to a 5 Hz sine. Returns the LFO index or -1 (none free / matrix full).
    int createVibrato (ParamValues* pv = nullptr, LfoData* lfos = nullptr);
    // "Create Velo->Amp Assignment": Velocity -> Amp, 100 %, uni. The Amp
    // knob is set to 0 so the velocity spans the whole range. Returns the row.
    int createVeloAmp (ParamValues& pv);
    // "Apply and Delete Macros": bakes each macro's current offset into its
    // destinations' base values, then deletes every macro-source row.
    void applyAndDeleteMacros (ParamValues& pv);
    void initModulations() { rows.clear(); }

    void save (Node& n) const;
    void load (const Node& n);
    // Preset load: does nothing (returns false) while the matrix is locked;
    // the lock itself is a user setting and is kept.
    bool loadPreset (const Node& n);
};

// VELO / NOTE graph sources (§7).
struct ModGraphs
{
    Curve velo, note;               // default: the diagonal
    bool veloLegato = false, noteLegato = false;    // "Legato (Portamento Time)" (glide: core)

    void save (Node& n) const;
    void load (const Node& n);
};

// ---------------------------------------------------------------------------
// Runtime state (fixed size, no allocation).
// ---------------------------------------------------------------------------
// Per block: shared by all voices.
struct MatrixBlock
{
    float baseNorm[kMaxModRows] {};     // per slot: normalised base value
    float globalModded[kMaxModRows] {}; // per slot (global slots): plain value after global rows
    float macro[kNumMacros] {};         // macro source values (after macro modulation)
    void applyMacros (SourceValues& s) const noexcept
    {
        for (int m = 0; m < kNumMacros; ++m) s[macroSource (m)] = macro[m];
    }
};

// Per voice (and one for the global rows).
struct MatrixVoiceState
{
    float acc[kMaxModRows] {};          // per slot accumulator (always left at 0)
    float slew[kMaxModRows] {};         // per row
    float discrete[kMaxModRows] {};     // per row: Note-On Rand (Discrete)
    float rowOut[kMaxModRows] {};       // per row: last output (telemetry)
    uint64_t slewReady = 0;
    void noteOn (uint32_t seed) noexcept;
};

// UI telemetry (written once per block by the audio thread).
struct MatrixTelemetry
{
    std::atomic<float> rowOut[kMaxModRows] {};
};

// ---------------------------------------------------------------------------
// Compiled matrix: built on the message thread, immutable, shared with the
// audio thread through std::shared_ptr<const CompiledMatrix>.
// ---------------------------------------------------------------------------
class CompiledMatrix
{
public:
    static std::shared_ptr<const CompiledMatrix> build (const ModMatrixData& data, const ModGraphs& graphs);

    // ParamView::slot: per parameter, the slot or -1.
    const int16_t* slots() const noexcept { return slotOf.data(); }
    int numSlots() const noexcept { return nSlots; }
    int slotParam (int slot) const noexcept { return slotInfo[(size_t) slot].param; }
    bool sourceUsed (ModSource s) const noexcept { return (usedMask >> (int) s) & 1u; }

    // VELO / NOTE graphs.
    float velocity (float v01) const noexcept { return veloLut (v01); }
    float note (float midiNote) const noexcept { return noteLut (midiNote * (1.0f / 127.0f)); }

    // Note-on: VELO / NOTE / Note-On Rand / Alt sources, and the voice's
    // discrete randoms and slew restart. `alt` = the synth's Note-On Alt
    // flip-flop for this note.
    void noteOnSources (const NoteInfo& n, uint32_t seed, bool alt, SourceValues& s, MatrixVoiceState& st) const noexcept;

    // Once per block: normalised base values and macro defaults.
    void beginBlock (const float* base, MatrixBlock& blk) const noexcept;
    // Once per block after beginBlock: the global rows, from `src` (the
    // newest voice's sources with the globals copied in).
    void evalGlobal (const SourceValues& src, MatrixVoiceState& st, MatrixBlock& blk, float seconds) const noexcept;
    // Per voice per tick: step 2 and step 4 of the order above. `modded` has
    // kMaxModRows floats; evalModulators also copies the global slots in.
    void evalModulators (const SourceValues& src, MatrixVoiceState& st, const MatrixBlock& blk, float* modded, float seconds) const noexcept;
    void evalMain (const SourceValues& src, MatrixVoiceState& st, const MatrixBlock& blk, float* modded, float seconds) const noexcept;

    // Row outputs for the UI: per-voice rows from `newest` (may be null), global rows from `global`.
    void publish (const MatrixVoiceState* newest, const MatrixVoiceState& global, MatrixTelemetry& t) const noexcept;

    enum Phase { kModulators = 0, kMain, kGlobalMacros, kGlobal, kNumPhases };
    int phaseOfSlot (int slot) const noexcept;

private:
    struct SmallLut
    {
        static constexpr int kSize = 256;
        float t[kSize + 1];
        float operator() (float x) const noexcept
        {
            x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
            const float f = x * (float) kSize;
            const int i = std::min ((int) f, kSize - 1);
            return t[i] + (t[i + 1] - t[i]) * (f - (float) i);
        }
    };
    enum RowFlags : uint8_t { kSrcBip = 1, kToBip = 2, kAuxBip = 4, kAuxInv = 8, kSlew = 16, kDiscrete = 32, kAuxDiscrete = 64 };
    struct Row
    {
        int16_t slot, index, lut, auxLut;
        int8_t src, aux;
        uint8_t flags;
        float k;                         // AMOUNT x OUTPUT
        float riseRate, fallRate;        // full-scale per second
    };
    enum Kind : uint8_t { kLinear = 0, kPow4, kPow, kLog, kDiscreteKind, kNumKinds };
    struct Slot
    {
        int param;
        uint8_t kind;
        float min, range, skew, log2Ratio;
    };

    void runRows (int phase, const float* src, MatrixVoiceState& st, float seconds) const noexcept;
    void finish (int phase, const float* baseNorm, float* acc, float* out) const noexcept;

    std::vector<int16_t> slotOf;
    std::vector<Slot> slotInfo;
    int nSlots = 0;
    int slotBegin[kNumPhases + 1] {};
    int kindBegin[kNumPhases][kNumKinds + 1] {};
    std::vector<Row> rows;               // grouped by phase
    int rowBegin[kNumPhases + 1] {};
    std::vector<SmallLut> luts;
    int macroSlot[kNumMacros] {};
    int macroParam[kNumMacros] {};
    int numDataRows = 0;
    int8_t rowPhase[kMaxModRows] {};     // per data row: phase, or -1 inactive
    uint64_t usedMask = 0;
    BakedCurve veloLut, noteLut;
};

// ---------------------------------------------------------------------------
// The modulation runtime used by the core.
// ---------------------------------------------------------------------------
class ModVoice;

// Synth-wide part: global (block-rate) rows, the shared MONO / FREE LFO
// instances, and the global sources.
class ModGlobal
{
public:
    // Global source values: the core writes ModWheel, Aftertouch, PitchBend
    // (-1..1) and ActiveVoices (count / POLY) here. Fixed is set to 1.
    SourceValues globals;

    void reset (uint32_t seed) noexcept;

    // Once per block, before the voices. `newest` = the newest sounding
    // voice (null when none).
    void beginBlock (const CompiledMatrix& m, const ParamValues& base, const ModVoice* newest, float blockSeconds) noexcept;

    // Once per tick, before the voices: ticks the shared LFOs. `pv` = the
    // newest voice's view (or view() when no voice sounds).
    void tick (const CompiledMatrix& m, const LfoShapeSet& shapes, const ParamView& pv, const Context& ctx,
               int offset, int n) noexcept;

    // Every note-on / when the last held note is released (MONO LFOs).
    void noteOn (const LfoShapeSet& shapes, const ParamView& pv, const Context& ctx, int offset, bool legato) noexcept;
    void allNotesReleased() noexcept;

    // Parameter view for global modules (FX, master, buses).
    ParamView view (const CompiledMatrix& m, const ParamValues& base) const noexcept
    {
        return { base.v.data(), m.slots(), block.globalModded, block.globalModded };
    }

    const MatrixBlock& matrixBlock() const noexcept { return block; }
    const Lfo& sharedLfo (int i) const noexcept { return lfo[i]; }
    bool isShared (int i) const noexcept { return shared[i]; }
    const LfoSettings& sharedSettings (int i) const noexcept { return settings[i]; }

    // UI: matrix row outputs and LFO phase/value (shared LFOs, else the newest voice's).
    void publish (const CompiledMatrix& m, const ModVoice* newest, MatrixTelemetry& mt, LfoTelemetry* lfoT /* kNumLfos */) const noexcept;

private:
    MatrixBlock block;
    MatrixVoiceState state;
    SourceValues scratch;
    Lfo lfo[kNumLfos];
    LfoSettings settings[kNumLfos];
    bool shared[kNumLfos] {};
};

// One voice's modulation: ENV 1-4, LFO 1-10, its matrix state and modulated
// parameters.
class ModVoice
{
public:
    // Sources the voice writes itself before tick(): OscA..Sub, Filter1/2
    // (last sample of the previous tick, -1..1), ExprX (-1..1) / ExprY / ExprZ,
    // PolyAftertouch, VoiceIndex, VoiceMod1/2. The rest are filled here.
    SourceValues src;

    void reset (uint32_t seed) noexcept;

    // Note-on. `fresh` = a new (or stolen) voice; false = a re-pitch of this
    // sounding voice (mono / legato), where n.legato says "don't retrigger".
    void noteOn (const CompiledMatrix& m, const LfoShapeSet& shapes, const ModGlobal& g, const NoteInfo& n,
                 const ParamValues& base, const Context& ctx, int offset, uint32_t seed, bool alt, bool fresh) noexcept;
    void noteOff (float releaseVelocity) noexcept;

    // One tick of n samples starting `offset` samples into the block.
    void tick (const CompiledMatrix& m, const LfoShapeSet& shapes, const ModGlobal& g, const ParamValues& base,
               const Context& ctx, int offset, int n) noexcept;

    // The voice's parameters after this tick's modulation (and the previous tick's, for ramps).
    ParamView view (const CompiledMatrix& m, const ParamValues& base) const noexcept
    {
        return { base.v.data(), m.slots(), modded[cur], modded[cur ^ 1] };
    }

    // Call after swapping in a new CompiledMatrix (slot numbers changed).
    void matrixChanged() noexcept { slotsChanged = true; }

    const EnvTick& envOut (int i) const noexcept { return envTick[i]; }   // ENV 1 = amp
    float lfoStart (int i) const noexcept { return lfoPrev[i]; }
    float lfoEnd (int i) const noexcept { return src.v[(int) ModSource::Lfo1 + i]; }
    bool finished() const noexcept { return env[0].finished(); }
    const Envelope& envelope (int i) const noexcept { return env[i]; }
    const Lfo& lfoInstance (int i) const noexcept { return lfo[i]; }
    const MatrixVoiceState& matrixState() const noexcept { return mst; }

private:
    friend class ModGlobal;
    Envelope env[kNumEnvs];
    Lfo lfo[kNumLfos];
    EnvTick envTick[kNumEnvs];
    float lfoPrev[kNumLfos] {};
    MatrixVoiceState mst;
    float modded[2][kMaxModRows] {};
    int cur = 0;
    bool slotsChanged = true;
};
} // namespace tincture
