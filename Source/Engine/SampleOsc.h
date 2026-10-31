// SampleOsc.h — the sample-based oscillator modes: Multisample, Sample,
// Granular and Spectral (oscillator types 1..4 in choices::oscTypes()).
//
// Split of work:
//   - SampleOscData is the oscillator's patch data (everything that isn't a
//     parameter) plus the derived, immutable data the audio thread reads
//     (prepared/mip-mapped audio, slices, spectral analysis frames). The
//     derived data is built by rebuild() on the message thread; the voice
//     code receives a const SampleOscData& and never changes it.
//   - SampleOsc is one voice's oscillator: preallocated in prepare(), no
//     allocation or locks in noteOn()/render().
//
// Sample-rate handling: positions are in frames of the (processed) sample;
// the sample's own rate is converted to the engine rate in the increment.
#pragma once

#include "Module.h"
#include "SampleData.h"
#include "Warp.h"
#include "../Model/Curve.h"

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tincture
{
// ---------------------------------------------------------------------------
// Small radix-2 complex FFT (split re/im arrays, in place, unnormalised).
// Used by the spectral mode, the slice/pitch analysis and the tests.
// ---------------------------------------------------------------------------
class SampleFft
{
public:
    void init (int size);                         // power of two; allocates
    int size() const noexcept { return n; }
    void forward (float* re, float* im) const noexcept;   // X[k] = sum x[n] e^{-i2πkn/N}
    void inverse (float* re, float* im) const noexcept;   // x[n] = sum X[k] e^{+i2πkn/N} (no 1/N)

private:
    void transform (float* re, float* im, float sign) const noexcept;
    int n = 0;
    std::vector<float> cosT, sinT;
    std::vector<int> bitrev;
};

// ---------------------------------------------------------------------------
// Audio-thread-ready copy of a sample: zero padded on both sides so the
// interpolators never bounds-check, plus 2x/4x/8x band-limited decimated
// copies (mip levels) used when the playback rate is high, to keep
// aliasing down when pitching up.
// ---------------------------------------------------------------------------
struct PreparedSample
{
    static constexpr int kPad = 8;
    static constexpr int kLevels = 4;

    double sampleRate = 48000.0;
    int frames = 0;                     // level 0 length
    int channels = 1;
    int rootNote = 60;
    int loopStart = -1, loopEnd = -1;   // file metadata (frames), -1 = none
    std::vector<int> slices;            // slice start frames, ascending, first is 0 when slicing
    std::vector<float> data[kLevels][2];
    int levelFrames[kLevels] {};

    const float* channel (int level, int c) const noexcept
    {
        return data[level][c < channels ? c : 0].data() + kPad;
    }

    static std::shared_ptr<const PreparedSample> build (const SampleData& s);
};

// ---------------------------------------------------------------------------
// STFT analysis of a sample for the spectral mode. Frame k covers source
// frames [(k - kPreFrames)*hop, ... + fftSize). Mono (the channels are summed).
// ---------------------------------------------------------------------------
struct SpectralData
{
    static constexpr int kFftSize = 2048;
    static constexpr int kHop = 512;
    static constexpr int kBins = kFftSize / 2 + 1;
    static constexpr int kPreFrames = kFftSize / kHop - 1;   // frames before source frame 0 (pre-roll)

    double sampleRate = 48000.0;
    int frames = 0;                     // number of analysis frames
    int sampleFrames = 0;
    std::vector<float> mag;             // frames * kBins
    std::vector<float> freq;            // true frequency in bins (phase-vocoder estimate)
    std::vector<float> phase;           // analysis phase, -pi..pi
    std::vector<uint8_t> transient;     // per frame: 1 = transient onset

    const float* magAt (int f) const noexcept { return mag.data() + (size_t) f * kBins; }
    const float* freqAt (int f) const noexcept { return freq.data() + (size_t) f * kBins; }
    const float* phaseAt (int f) const noexcept { return phase.data() + (size_t) f * kBins; }
};

// DAHDSR settings for the Multisample envelope pane (times in seconds,
// sustain 0..1). Patch data: these aren't parameters in ParamIds.
struct SampleEnvSettings
{
    float delay = 0.0f, attack = 0.0f, hold = 0.0f, decay = 1.0f, sustain = 1.0f, release = 0.015f;
    bool delaySync = false;             // DELAY in beats (delay value = beats) when on
    void save (Node& n) const;
    void load (const Node& n);
   #if defined (__clang__) || defined (__GNUC__)
    #pragma GCC diagnostic push
    #pragma GCC diagnostic ignored "-Wfloat-equal"
   #endif
    bool operator== (const SampleEnvSettings&) const = default;   // exact: change detection
   #if defined (__clang__) || defined (__GNUC__)
    #pragma GCC diagnostic pop
   #endif
};

// ---------------------------------------------------------------------------
// A zone-mapped instrument (from an SFZ or a folder of files).
// ---------------------------------------------------------------------------
struct MultiSample
{
    struct Zone
    {
        std::shared_ptr<const SampleData> sample;
        int loKey = 0, hiKey = 127, loVel = 1, hiVel = 127, root = 60;
        // Optional SFZ extras (the plugin layer's SFZ parser fills these).
        float tuneCents = 0.0f, gainDb = 0.0f;
        bool loop = false;              // loop_mode=loop_continuous / loop_sustain (uses the sample's loop points)
        bool hasEnv = false;            // SFZ ampeg_* given for this region
        SampleEnvSettings env;
    };
    std::string name;
    std::vector<Zone> zones;

    // Best zone for a key/velocity (velocity 1..127), or -1.
    int findZone (int key, int velocity) const noexcept;

    // Build a key/velocity map from (file name, sample) pairs. Roots come from
    // rootNoteFromName() (else the sample's rootNote). Key ranges split half
    // way between neighbouring roots; files sharing a root become velocity
    // layers, ordered by a "v<n>"/"vel<n>" token or a dynamic word (pp..ff)
    // in the name, else by peak level.
    static MultiSample fromFiles (const std::string& name,
                                  const std::vector<std::pair<std::string, std::shared_ptr<const SampleData>>>& files);

    void save (Node& n) const;
    static std::shared_ptr<const MultiSample> load (const Node& n);
};

// A non-destructive edit on the loaded sample (Normalize / Reverse / Trim).
struct SampleEdit
{
    enum Kind : int { Normalize = 0, Reverse = 1, Trim = 2 };
    int kind = Normalize;
    float a = 0.0f, b = 1.0f;           // Trim: kept range as a fraction of the sample before this edit
    bool operator== (const SampleEdit&) const = default;
};

// ---------------------------------------------------------------------------
// The oscillator's patch data for the sample modes.
// ---------------------------------------------------------------------------
struct SampleOscData
{
    enum Slicing : int { SlicingOff = 0, SliceAuto, SliceManual };
    enum Snap : int { SnapOff = 0, SnapZero, SnapBeats, SnapLoop };
    enum DensMode : int { DensFree = 0, DensSync, DensGrains };
    enum LenMode : int { LenFree = 0, LenSync, LenPercent };
    enum Spawn : int { SpawnTogether = 0, SpawnEven, SpawnExp, SpawnRandom };
    // X|Y "Y Axis" targets (index into granularYTargets() / spectralYTargets()).
    static const std::vector<std::string>& granularYTargets();
    static const std::vector<std::string>& spectralYTargets();
    static const std::vector<std::string>& fadeEdgeChoices();      // None, 1 ms .. 128 ms

    int type = 2;                                   // 1 Multisample, 2 Sample, 3 Granular, 4 Spectral
    std::shared_ptr<const SampleData> sample;       // shared by Sample / Granular / Spectral
    std::shared_ptr<const MultiSample> multi;       // Multisample only
    int rootNote = -1;                              // -1 = the sample's own root

    // Non-destructive edits (applied in order), then Fade Edges.
    std::vector<SampleEdit> edits;
    int fadeEdges = 0;                              // index into fadeEdgeChoices()

    // Display / editing state (UI only, saved with the patch).
    bool showMarkerAnimation = false, zoomToStartEnd = false, showWaveform = false;
    int snap = SnapOff;
    int lastPlayedZone = -1;                        // for "Switch to Single Sample" (set by the plugin from telemetry)

    // Slicing.
    int slicing = SlicingOff;
    float sliceThreshold = 0.25f;                   // auto: level threshold, 0..1 of the sample's peak
    std::vector<float> manualSlices;                // manual: slice starts as fractions 0..1 of the processed sample
    bool playSliceToEnd = false, playSingleSlice = false, autoSyncToClip = false;
    int singleSlice = 0;                            // slice index for Play Single Slice (no modulatable param yet)
    int sliceRoot = 60;                             // note that plays slice 1 (C-1..C8 menu)

    // Loop menu toggles. (The loop mode itself is the loopMode parameter;
    // "Manual" isn't in choices::loopModes(), so it's this flag.)
    bool manualLoop = false, relativeLoop = false, linkLoopLength = false, exitLoopOnRelease = false, loopGrains = false;

    // SCAN context menu.
    int scanRange = 0;                              // 0: +/-200 %, 1: +/-400 %, 2: +/-800 %
    bool scanReverse = false, scanKeyTrack = false, scanTempoLock = false, lengthToBpm = false;
    float lengthBeats = 4.0f;                       // Sample Length to BPM: start..end lasts this many beats at SCAN 100 %

    // Granular.
    int densityMode = DensFree;
    bool densTriplet = false, densDotted = false, jumpStart = false;
    int maxGrains = 128;                            // per oscillator voice incl. unison, 1..SampleOsc::kMaxGrains
    int lengthMode = LenFree;
    bool lenTriplet = false, lenDotted = false;
    bool reverseGrains = false;
    int spawnPattern = SpawnTogether;
    float randWarp1 = 0.0f, randWarp2 = 0.0f;       // per-grain warp randomisation (X|Y only)

    // X|Y control.
    bool showXY = false;
    int yTargetGranular = 0, yTargetSpectral = 0;   // 0 = None
    float yValue = 0.5f;                            // the dot's Y, 0..1 of the target's travel

    // Spectral.
    bool phaseLock = false, transients = false, specSmooth = false, specPostWarp = false;
    Curve specFilter = Curve::line (0.5f, 0.5f);    // x = log freq 20 Hz..20 kHz, y = gain (0.5 = unity, 1 = +6 dB)
    std::string specFilterTable;                    // "" = Curve Filter; else a wavetable name (resolved by the plugin)
    std::vector<float> specFilterMask;              // optional: magnitude mask over x (filled by the plugin for wavetable filters)

    // Multisample.
    SampleEnvSettings env;
    bool envOverride = false;
    bool velTrack = false;
    float velTrackAmount = 1.0f;                    // 0..1
    float phaseRand = 0.0f;                         // φ RAND 0..1: random start offset (fraction of the sample)

    // ---- Derived data (not saved; rebuild() fills it) -------------------
    std::shared_ptr<const SampleData> processed;                 // sample after edits + fades
    std::shared_ptr<const PreparedSample> prepared;
    std::shared_ptr<const SpectralData> spectral;
    std::shared_ptr<const std::vector<std::shared_ptr<const PreparedSample>>> zones;   // one per multi zone
    std::shared_ptr<const BakedCurve> specLut;

    // Message thread: apply edits, build mips, slices, the spectral filter
    // LUT and (if `withSpectral` or type is Spectral) the STFT frames.
    void rebuild (bool withSpectral = false);

    static std::shared_ptr<const SampleData> applyEdits (const SampleData& s, const std::vector<SampleEdit>& edits, int fadeEdges);
    static std::shared_ptr<const SpectralData> buildSpectral (const SampleData& s);
    // Transient detection for Slice Auto. Returns slice start frames (first = 0).
    static std::vector<int> detectSlices (const SampleData& s, float threshold);

    // "Switch to Wavetable" import. Returns frames of kWtFrameSize samples
    // (at most 256), peak-normalised.
    enum WavetableImport : int { NormalZeroSnap = 0, NormalPitchFollow, ConstantFramesize, FrequencyEstimation, Fft256, Fft512, Fft1024, Fft2048 };
    static const std::vector<std::string>& wavetableImportChoices();
    static constexpr int kWtFrameSize = 2048;
    static std::vector<std::vector<float>> toWavetable (const SampleData& s, int importMode, int maxFrames = 256);
    // Average fundamental of a sample in Hz (0 if none found). Used by the import.
    static float estimatePitch (const SampleData& s);

    void save (Node& n) const;
    void load (const Node& n);          // then call rebuild()
};

// ---------------------------------------------------------------------------
// One voice's sample-mode oscillator.
// ---------------------------------------------------------------------------
class SampleOsc
{
public:
    static constexpr int kMaxGrains = 256;          // hard cap on concurrent grains per voice (incl. unison)
    static constexpr int kMaxTelemetryGrains = 64;

    void prepare (const Context& ctx);
    void reset();

    void noteOn (const NoteInfo& note, const OscTick& tick, const OscIds& ids, const SampleOscData& data);
    void noteOff();                                 // Exit Loop on Release; Multisample envelope release
    void render (OscTick& tick, const OscIds& ids, const SampleOscData& data);

    // True once a one-shot has played out (and its grains/frames ended):
    // the voice may stop early.
    bool finished() const noexcept { return isFinished; }

    // UI telemetry, written once per tick with relaxed stores.
    struct Telemetry
    {
        std::atomic<float> playhead[kMaxUnison];    // 0..1 of the sample, < 0 = inactive
        std::atomic<int> numPlayheads { 0 };
        std::atomic<float> grainPos[kMaxTelemetryGrains];
        std::atomic<int> numGrains { 0 };           // all active grains (may exceed the telemetry slots)
        std::atomic<int> zone { -1 };               // Multisample: the zone playing
        std::atomic<int> slice { -1 };              // the slice playing
        Telemetry();
    };
    const Telemetry& telemetry() const noexcept { return tele; }

    int activeGrains() const noexcept { return numGrains; }

    // Exposed for tests: window shape (choices::winShapes index) at t 0..1.
    static float windowShape (int shape, float t) noexcept;

private:
    struct Stream
    {
        double pos = 0.0;               // frames (level 0)
        int dir = 1;                    // +1 / -1 (ping-pong and reverse loop state)
        bool inLoop = false, done = false, exiting = false;
        float gl = 0.0f, gr = 0.0f;     // last tick's gains (ramps)
        float detune = 0.0f;            // semitones
        float rnd = 0.0f;               // per-note random value (Random unison mode)
        float pan = 0.0f, gain = 1.0f, warpSpread = 0.0f;
        warp::PostState post[2][2];     // [slot][channel]
    };
    struct Region
    {
        double start = 0.0, end = 0.0, ls = 0.0, le = 0.0;
        double xlF = 0.0, xlB = 0.0;    // crossfade lengths at LE (moving forward) / LS (moving back)
        double frames = 0.0;
        int mode = 0;                   // loop mode (0 one-shot .. 4 tailed)
        bool looping = false;
    };
    struct Grain
    {
        double pos = 0.0, inc = 0.0;
        int delay = 0, len = 1, age = 0, level = 0;
        int stream = 0;
        float invLen = 1.0f, gl = 1.0f, gr = 1.0f;
        float winAmt = 1.0f, peak = 0.5f, slopeA = 1.0f, slopeB = 1.0f;
        float warp1 = 0.0f, warp2 = 0.0f;
        int dir = 1;
        warp::PostState post[2][2];
    };
    struct TickParams;

    void startStreams (const OscTick& tick, const OscIds& ids, const SampleOscData& data);
    void computeRegion (const ParamView& P, const OscIds& ids, const SampleOscData& data, const PreparedSample& ps, Region& r) const;
    void computeUnison (const ParamView& P, const OscIds& ids, int n);
    void renderSample (OscTick& tick, const OscIds& ids, const SampleOscData& data, const PreparedSample& ps, TickParams& tp);
    void renderGranular (OscTick& tick, const OscIds& ids, const SampleOscData& data, const PreparedSample& ps, TickParams& tp);
    void renderSpectral (OscTick& tick, const OscIds& ids, const SampleOscData& data, TickParams& tp);
    void synthHop (const SampleOscData& data, const SpectralData& sd, TickParams& tp, bool prefill);
    void spawnGrains (int offset, int age0, const SampleOscData& data, const PreparedSample& ps, TickParams& tp);
    int boundedStream (Stream& s, const Region& r, const PreparedSample& ps, int level, double inc0, double incStep,
                       const float* fm, int n, float* L, float* R, bool stereo) noexcept;
    void advancePlayhead (Stream& s, double delta, const Region& r) const noexcept;
    static void boundary (Stream& s, const Region& r, double lo, double hi) noexcept;
    static void bounds (const Stream& s, const Region& r, double& lo, double& hi) noexcept;
    static void applyPostWarps (float* buf, int n, int offset, warp::PostState& s1, warp::PostState& s2,
                                float amt1, float amt2, const TickParams& tp) noexcept;
    float multiEnvelope (int n) noexcept;

    Context ctx;
    const ParamRegistry* reg = nullptr;
    const float* winTables = nullptr;               // 10 x (kWinSize + 1)
    Rng rng;
    NoteInfo noteInfo;
    int type = 0, numStreams = 1;
    bool isFinished = true, active = false, exitOnRelease = false, envActive = false;
    int sliceIndex = -1, zoneIndex = -1;
    float velGain = 1.0f;

    Stream streams[kMaxUnison];
    float uniX[kMaxUnison] {};                      // -1..1 spread position per unison voice

    // Granular.
    Grain grains[kMaxGrains];
    int numGrains = 0;
    double spawnCountdown = 0.0;
    float gOffset = 0.0f, gDir = 0.0f, gPitch = 0.0f, gLenRand = 0.0f, gPanRand = 0.0f, gGainRand = 0.0f;
    float gWinAmt = 1.0f, gWinAmtRand = 0.0f, gWinSkew = 0.0f, gWinSkewRand = 0.0f;
    bool firstTick = true;
    float lvlPrev = 0.0f;

    // Spectral.
    SampleFft fft;
    std::vector<float> specPhase;                   // kMaxUnison * kBins (synthesis phases)
    std::vector<int> specLastFrame;                 // kMaxUnison
    std::vector<float> accL, accR;                  // overlap-add, kFftSize
    std::vector<float> zr, zi, cLr, cLi, cRr, cRi;  // frame spectra
    std::vector<float> srcMag, bestMag, newPhase;   // per bin scratch for one stream
    std::vector<int> peaks;
    std::vector<float> mask, binX, anaWin, synWin;
    float maskKey[6] {};
    std::vector<float> hopL, hopR;                  // the current output hop
    int hopRead = SpectralData::kHop;
    bool specStarted = false;
    int specTailHops = 0;

    // Multisample envelope.
    int envStage = 0;
    float envLevel = 0.0f, envTime = 0.0f, envRelStart = 0.0f;
    SampleEnvSettings envSet;

    // Scratch (one tick).
    float tmpL[kTick] {}, tmpR[kTick] {}, fmBuf[kTick] {};

    Telemetry tele;
};
} // namespace tincture
