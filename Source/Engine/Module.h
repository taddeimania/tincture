// Module.h — the contracts between the voice/synth core and the modules.
//
// Each module (oscillators, filters, envelopes, LFOs, FX, arp, clips) is
// implemented in its own files against these types. The core (Voice.cpp,
// Synth.cpp) owns the order of evaluation, the parameter values, the
// modulation and the mixing.
//
// Timing: the core runs in ticks of at most kTick samples. Once per tick it
// evaluates the modulators, works out each module's parameters for this
// voice, and calls the module to render the tick's audio. Modules should
// compute their coefficients once per tick and ramp anything audible
// (levels, cutoff) linearly across the tick to avoid zipper noise.
#pragma once

#include "Core.h"
#include "ParamIds.h"
#include "Params.h"

#include <memory>

namespace tincture
{
// ---------------------------------------------------------------------------
// Parameter access for one voice for one tick: base values, with the
// voice's modulation applied to the parameters the matrix touches.
// ---------------------------------------------------------------------------
struct ParamView
{
    const float* base = nullptr;        // plain values (ParamValues)
    const int16_t* slot = nullptr;      // per param: modulated slot or -1
    const float* modded = nullptr;      // per slot: plain value after modulation (this voice)
    const float* moddedPrev = nullptr;  // per slot: same, previous tick (for ramps); may equal modded

    float operator() (int id) const noexcept
    {
        const int s = slot[id];
        return s < 0 ? base[id] : modded[s];
    }
    float previous (int id) const noexcept
    {
        const int s = slot[id];
        return s < 0 ? base[id] : moddedPrev[s];
    }
    int integer (int id) const noexcept { return (int) std::lround ((*this) (id)); }
    bool on (int id) const noexcept { return (*this) (id) >= 0.5f; }
};

// What a voice knows about its note.
struct NoteInfo
{
    int note = 60;                      // MIDI note after transpose/scale
    float velocity = 0.8f;              // 0..1
    float releaseVelocity = 0.5f;
    int voiceIndex = 0;                 // which voice (0..kMaxVoices-1)
    int unisonSeed = 0;                 // changes every note-on (random phase etc.)
    bool legato = false;                // a legato re-pitch: don't retrigger
    bool stolen = false;                // this voice was stolen for the note
};

// ---------------------------------------------------------------------------
// Oscillator render call (OSC A/B/C in any mode, Sub, Noise).
// ---------------------------------------------------------------------------
enum SourceBuffer { kSrcA = 0, kSrcB, kSrcC, kSrcNoise, kSrcSub, kSrcF1, kSrcF2, kNumSrcBuffers };

struct OscTick
{
    int n = kTick;                      // samples this tick (1..kTick)
    // Played pitch as a fractional MIDI note, at the start and end of the
    // tick (glide, pitch bend and global tuning included; the oscillator's
    // own OCT/SEM/FIN/CRS are NOT included — the oscillator adds those).
    float pitchStart = 60.0f, pitchEnd = 60.0f;
    float bendSemis = 0.0f;             // the bend part of the pitch (for "pitch tracking off")
    const ParamView* p = nullptr;
    // This tick's audio from other modules (mono), for FM / PD / AM / RM
    // warps. Entries are null when that module is off. Modules that render
    // later in the tick than the reader hold the previous tick's audio.
    const float* src[kNumSrcBuffers] {};
    float* outL = nullptr;              // the module writes (not adds) n samples
    float* outR = nullptr;
    float* outMono = nullptr;           // optional: (L+R)/2, for other modules' warps
    const Context* ctx = nullptr;
    int activeUnison = 0;               // out: unison voices used (voice counter)
};

// ---------------------------------------------------------------------------
// Filter render call.
// ---------------------------------------------------------------------------
struct FilterTick
{
    int n = kTick;
    int type = 1;                       // index into choices::filterTypes()
    float cutoffHz = 1000.0f;           // after keytrack and modulation
    float cutoffHzPrev = 1000.0f;       // previous tick (ramp from this)
    float res = 0.0f, drive = 0.0f, var = 0.0f;   // 0..1
    float pan = 0.0f;                   // -1..1: stereo cutoff offset
    float mix = 1.0f;                   // dry/wet 0..1
    bool cleanDrive = false;
    const Context* ctx = nullptr;
    float* L = nullptr;                 // in place
    float* R = nullptr;
};
} // namespace tincture
