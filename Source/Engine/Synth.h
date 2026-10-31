// Synth.h — the sound engine: voices, routing, the mixer and the FX racks,
// driven by MIDI and the parameter values each block.
//
// Threads: prepare() and setPatch() are called from the message thread;
// process() from the audio thread. A patch is swapped in without locks: the
// message thread hands over a CompiledPatch, the audio thread takes it at the
// start of a block and hands the old one back for the message thread to free
// (collectGarbage()).
#pragma once

#include "Core.h"
#include "Filters.h"
#include "Fx/FxEngine.h"
#include "Midi.h"
#include "ModMatrix.h"
#include "Module.h"
#include "NoiseOsc.h"
#include "NoteProcessor.h"
#include "SampleOsc.h"
#include "SubOsc.h"
#include "Telemetry.h"
#include "WavetableOsc.h"
#include "../Model/Patch.h"

#include <atomic>
#include <memory>

namespace tincture
{
// Everything the audio thread needs from a patch, immutable.
struct CompiledPatch
{
    std::shared_ptr<const Patch> patch;
    std::shared_ptr<const CompiledMatrix> matrix;
    std::shared_ptr<const LfoShapeSet> shapes;
    std::shared_ptr<const FxLayout> fx;
    std::shared_ptr<const ArpBank> arp;
    std::shared_ptr<const ClipBank> clips;

    // Message thread: build from a patch.
    static std::unique_ptr<CompiledPatch> compile (std::shared_ptr<const Patch>);
};

struct HostInfo
{
    double bpm = 120.0, ppq = 0.0;
    bool playing = false;
};

class Synth
{
public:
    Synth();
    ~Synth();

    // Message thread (audio stopped).
    void prepare (double sampleRate, int maxBlock);
    void setOversample (int factor);          // QUALITY: 1, 2 or 4 for the oscillators

    // Message thread, any time.
    void setPatch (std::unique_ptr<CompiledPatch>);
    void collectGarbage();                    // free patches the audio thread has finished with
    void post (const SeqCommand& c)          { notes->post (c); }
    void setKeyboardSettings (const KeyboardSettings& k) { notes->setKeyboardSettings (k); }
    NoteProcessor& noteProcessor() noexcept { return *notes; }

    // Audio thread. `midiIn` events have sample offsets within the block;
    // `midiOut` receives MIDI OUT notes.
    void process (float* outL, float* outR, int numSamples, const MidiEvent* midiIn, int numMidi,
                  const ParamValues& pv, const HostInfo& host, MidiBuffer& midiOut);
    void allNotesOff();                       // audio thread (panic on transport jumps etc.)

    Telemetry telemetry;
    MatrixTelemetry matrixTelemetry;
    LfoTelemetry lfoTelemetry[kNumLfos];
    const SeqTelemetry& seqTelemetry() const { return notes->telemetry(); }
    int latencySamples() const noexcept { return fxLatency; }
    FxEngine& fxEngine() noexcept { return fx; }
    // Live playheads / grains of a voice's sample-mode oscillator (message thread
    // may read: the fields are atomics). Use telemetry.newestVoice for the voice.
    const SampleOsc::Telemetry& sampleTelemetry (int voice, int osc) const;

private:
    struct Voice;
    void startNote (int note, float velocity, int offset, const ParamValues& pv);
    void stopNote (int note, float releaseVelocity, const ParamValues& pv);
    void releaseVoice (Voice&, float releaseVelocity);
    int pickVoice (const ParamValues& pv);
    int freeSlot() const;                     // an inactive voice slot, or -1
    void killVoice (Voice&);                  // start the steal fade
    static constexpr double kStealFadeSeconds = 0.010;
    void renderVoice (Voice&, int offset, int n, const ParamValues& pv);
    void handleEvent (const MidiEvent& e, const ParamValues& pv);
    void swapInPatch();

    Context ctx;
    int maxBlock = 512;
    std::vector<std::unique_ptr<Voice>> voices;
    std::unique_ptr<NoteProcessor> notes;
    std::unique_ptr<ModGlobal> modGlobal;
    FxEngine fx;
    int fxLatency = 0;

    // Patch handover.
    std::unique_ptr<CompiledPatch> current;
    std::atomic<CompiledPatch*> pending { nullptr };
    SpscFifo<CompiledPatch*, 64> retired;

    // Shared oscillator state (phase memory, scratch).
    WavetableOsc::Shared wtShared[kNumOscs];
    SubOsc::Shared subShared;

    // Mix buses for the block: main, direct, bus 1, bus 2 (L, R).
    enum { kMain = 0, kDirect, kBus1, kBus2, kNumDest };
    std::vector<float> bus[kNumDest][2];
    MidiBuffer toVoices, chunkIn, hostOutChunk;

    // Performance state.
    float modWheel = 0.0f, aftertouch = 0.0f, pitchBend = 0.0f;
    bool sustainDown = false;
    bool heldNotes[128] {};
    int noteStack[128] {};                    // mono: held notes, most recent last
    int noteStackSize = 0;
    float lastPitch = -1.0f;                  // portamento source
    uint32_t noteCounter = 0, seed = 0x1234567u;
    bool altFlipFlop = false;
    int monoReleaseCounter = 0;
};
} // namespace tincture
