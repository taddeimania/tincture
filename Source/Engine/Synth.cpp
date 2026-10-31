#include "Synth.h"

#include <cstring>
#include "Warp.h"

#include <algorithm>
#include <cmath>

namespace tincture
{
namespace
{
inline void addScaled (float* dst, const float* src, float g, int n) noexcept
{
    for (int i = 0; i < n; ++i) dst[i] += src[i] * g;
}
inline void clear (float* d, int n) noexcept { std::fill (d, d + n, 0.0f); }

// Oscillator rows in render order: Sub and Noise first (they read no other
// oscillator), then A, B, C.
enum Row { RSub = 0, RNoise, RA, RB, RC, kRows };
constexpr int kSrcOfRow[kRows] { kSrcSub, kSrcNoise, kSrcA, kSrcB, kSrcC };
constexpr int kMapRowOfRow[kRows] { KeyboardSettings::Sub, KeyboardSettings::Noise, KeyboardSettings::OscA, KeyboardSettings::OscB, KeyboardSettings::OscC };
constexpr ModSource kModSrcOfBuffer[kNumSrcBuffers] { ModSource::OscA, ModSource::OscB, ModSource::OscC, ModSource::Noise, ModSource::Sub, ModSource::Filter1, ModSource::Filter2 };
} // namespace

// ===========================================================================
// CompiledPatch
// ===========================================================================
std::unique_ptr<CompiledPatch> CompiledPatch::compile (std::shared_ptr<const Patch> p)
{
    auto c = std::make_unique<CompiledPatch>();
    c->matrix = CompiledMatrix::build (p->matrix, p->graphs);
    c->shapes = LfoShapeSet::build (p->lfo.data());
    c->fx = std::make_shared<FxLayout> (p->fx);
    c->arp = std::make_shared<ArpBank> (p->arp);
    c->clips = std::make_shared<ClipBank> (p->clips);
    c->patch = std::move (p);
    return c;
}

// ===========================================================================
// Voice
// ===========================================================================
struct Synth::Voice
{
    int index = 0;
    bool active = false, released = false, sustained = false;
    // A voice that was stolen or retriggered fades out over killLen samples
    // (instead of being cut, which clicks) while the new note starts in
    // another slot. killLeft > 0 = fading; it then goes inactive.
    int killLeft = 0, killLen = 1;
    bool killing() const noexcept { return killLeft > 0; }
    int note = 60;
    float velocity = 0.8f, polyPressure = 0.0f;
    uint32_t age = 0;

    // Portamento.
    float pitch = 60.0f, glideFrom = 60.0f, glideTo = 60.0f, glideCurve = 0.0f;
    float glideLen = 0.0f, glidePos = 0.0f;   // samples

    int mapOffset[kRows] {};                  // OSC MAPPING: semitones to add, or -1000 = row doesn't play
    bool ungatedUsed = false;
    int unison = 0;

    ModVoice mod;
    WavetableOsc wt[kNumOscs];
    SampleOsc smp[kNumOscs];
    SubOsc sub;
    NoiseOsc noise;
    FilterVoice filter[kNumFilters];
    int lastOscType[kNumOscs] { -1, -1, -1 };
    float dcX[kNumOscs][2] {}, dcY[kNumOscs][2] {};   // DC blockers for phase warps

    alignas (32) float srcBuf[kNumSrcBuffers][kTick] {};
    // The last kTick samples of each source, ending just before the current
    // tick. A module that reads a source rendered after it (A <- FM from B,
    // any osc <- a filter) reads this, so the delay is always exactly kTick
    // samples, even when MIDI events split ticks into odd lengths. (Reading
    // the previous tick's buffer directly mixed in stale samples after a
    // short tick: the click on note-on with FM/PD/AM/RM warps.)
    alignas (32) float srcHist[kNumSrcBuffers][kTick] {};
    bool srcLive[kNumSrcBuffers] {};

    void clearSources()
    {
        for (auto& h : srcHist) std::fill (std::begin (h), std::end (h), 0.0f);
        for (auto& b : srcBuf) std::fill (std::begin (b), std::end (b), 0.0f);
        std::fill (std::begin (srcLive), std::end (srcLive), false);
    }

    // After a tick of n samples: slide each source's history along.
    void advanceSources (int n)
    {
        for (int b = 0; b < kNumSrcBuffers; ++b)
        {
            float* h = srcHist[b];
            if (n < kTick) std::memmove (h, h + n, sizeof (float) * (size_t) (kTick - n));
            float* tail = h + (kTick - n);
            if (srcLive[b]) std::memcpy (tail, srcBuf[b], sizeof (float) * (size_t) n);
            else std::fill (tail, tail + n, 0.0f);
        }
    }
    alignas (32) float oscL[kRows][kTick] {}, oscR[kRows][kTick] {};
    alignas (32) float fL[kNumFilters][kTick] {}, fR[kNumFilters][kTick] {};
    // Per destination (main, direct, bus 1, bus 2) x (gated by ENV 1, not) x L/R.
    alignas (32) float acc[4][2][2][kTick] {};
    float filterCutPrev[kNumFilters] { 1000.0f, 1000.0f };
    int filterIdle[kNumFilters] {};           // ticks since the filter last had input

    void prepare (const Context& c)
    {
        for (auto& o : wt) o.prepare (c);
        for (auto& o : smp) o.prepare (c);
        sub.prepare (c);
        noise.prepare (c);
        for (auto& f : filter) f.prepare (c);
    }

    float advanceGlide (int n) noexcept
    {
        if (glidePos >= glideLen) { pitch = glideTo; return pitch; }
        glidePos = std::min (glideLen, glidePos + (float) n);
        const float t = glideLen > 0.0f ? glidePos / glideLen : 1.0f;
        pitch = glideFrom + (glideTo - glideFrom) * Curve::bend (t, glideCurve);
        return pitch;
    }
};

// ===========================================================================
// Synth
// ===========================================================================
Synth::Synth()
    : notes (std::make_unique<NoteProcessor>()), modGlobal (std::make_unique<ModGlobal>())
{
    for (int i = 0; i < kMaxVoices; ++i)
    {
        voices.push_back (std::make_unique<Voice>());
        voices.back()->index = i;
        voices.back()->mod.reset (0x51u + (uint32_t) i * 7919u);
    }
    modGlobal->reset (0xC0FFEEu);
    auto init = std::make_shared<const Patch>();
    current = CompiledPatch::compile (init);
}

Synth::~Synth()
{
    collectGarbage();
    delete pending.exchange (nullptr);
}

void Synth::prepare (double sampleRate, int block)
{
    ctx.sampleRate = sampleRate;
    ctx.invSampleRate = (float) (1.0 / sampleRate);
    maxBlock = std::max (block, kTick);
    for (auto& v : voices) { v->prepare (ctx); v->active = false; v->killLeft = 0; }
    for (auto& d : bus)
        for (auto& ch : d) ch.assign ((size_t) maxBlock, 0.0f);
    notes->prepare (ctx);
    fx.prepare (ctx, maxBlock);
    fx.setLayout (current->fx);
    fxLatency = fx.latency();
}

void Synth::setOversample (int factor)
{
    ctx.oversample = factor == 4 ? 4 : (factor == 2 ? 2 : 1);
}

void Synth::setPatch (std::unique_ptr<CompiledPatch> p)
{
    if (p == nullptr) return;
    fx.setLayout (p->fx);
    fxLatency = fx.latency();
    delete pending.exchange (p.release());   // a patch the audio thread never took
    collectGarbage();
}

void Synth::collectGarbage()
{
    fx.idle();
    CompiledPatch* old = nullptr;
    while (retired.pop (old)) delete old;
}

void Synth::swapInPatch()
{
    if (auto* p = pending.exchange (nullptr))
    {
        if (! retired.push (current.get()))
        {
            // The message thread isn't collecting; keep the old one rather than free it here.
            pending.store (p);
            return;
        }
        current.release();
        current.reset (p);
        for (auto& v : voices) v->mod.matrixChanged();
    }
}

const SampleOsc::Telemetry& Synth::sampleTelemetry (int voice, int osc) const
{
    voice = std::clamp (voice, 0, kMaxVoices - 1);
    osc = std::clamp (osc, 0, kNumOscs - 1);
    return voices[(size_t) voice]->smp[osc].telemetry();
}

void Synth::allNotesOff()
{
    for (auto& v : voices) { v->active = false; v->killLeft = 0; }
    std::fill (std::begin (heldNotes), std::end (heldNotes), false);
    noteStackSize = 0;
    sustainDown = false;
    modGlobal->allNotesReleased();
}

// ---------------------------------------------------------------------------
// Notes
// ---------------------------------------------------------------------------
int Synth::freeSlot() const
{
    for (int i = 0; i < kMaxVoices; ++i)
        if (! voices[(size_t) i]->active) return i;
    return -1;
}

void Synth::killVoice (Voice& v)
{
    if (! v.active || v.killing()) return;
    v.killLen = std::max (1, (int) (kStealFadeSeconds * ctx.sampleRate));
    v.killLeft = v.killLen;
}

int Synth::pickVoice (const ParamValues& pv)
{
    const int poly = std::clamp ((int) pv[ids().global.poly], 1, kMaxVoices);
    int activeCount = 0;
    for (int i = 0; i < kMaxVoices; ++i)
        activeCount += voices[(size_t) i]->active && ! voices[(size_t) i]->killing() ? 1 : 0;

    if (activeCount < poly)
        if (const int f = freeSlot(); f >= 0) return f;

    // Steal: released voices first (the oldest), then by priority. The victim
    // fades out in its slot and the new note gets a free one; only when every
    // slot is busy is a voice reused directly.
    auto steal = [this] (int victim)
    {
        const int f = freeSlot();
        if (f < 0) { voices[(size_t) victim]->killLeft = 0; return victim; }
        killVoice (*voices[(size_t) victim]);
        return f;
    };
    int best = -1;
    uint32_t bestAge = UINT32_MAX;
    for (int i = 0; i < kMaxVoices; ++i)
    {
        auto& v = *voices[(size_t) i];
        if (v.active && ! v.killing() && v.released && v.age < bestAge) { best = i; bestAge = v.age; }
    }
    if (best >= 0) return steal (best);

    const int prio = (int) pv[ids().global.voiceSteal];
    float bestScore = 1.0e9f;
    for (int i = 0; i < kMaxVoices; ++i)
    {
        auto& v = *voices[(size_t) i];
        if (! v.active || v.killing()) continue;
        float score = 0.0f;
        switch (prio)
        {
            case 0:  score = (float) v.age; break;            // Newest kept: steal the oldest
            case 1:  score = -(float) v.age; break;           // Oldest kept: steal the newest
            case 2:  score = (float) v.note; break;           // Highest kept: steal the lowest
            case 3:  score = -(float) v.note; break;          // Lowest kept: steal the highest
            default: score = v.velocity; break;               // steal the quietest
        }
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best >= 0) return steal (best);
    // Every slot is fading: take the one closest to silence.
    int quietest = 0;
    for (int i = 1; i < kMaxVoices; ++i)
        if (voices[(size_t) i]->killLeft < voices[(size_t) quietest]->killLeft) quietest = i;
    voices[(size_t) quietest]->killLeft = 0;
    return quietest;
}

void Synth::startNote (int note, float velocity, int offset, const ParamValues& pv)
{
    const auto& g = ids().global;
    const auto& cp = *current;
    const bool mono = pv[g.mono] >= 0.5f;
    const bool legatoOn = pv[g.legato] >= 0.5f;
    const float portaTime = pv[g.porta];
    const bool always = pv[g.portaAlways] >= 0.5f;
    const bool scaled = pv[g.portaScaled] >= 0.5f;

    bool otherHeld = false;
    for (int i = 0; i < noteStackSize; ++i) otherHeld |= noteStack[i] != note;
    heldNotes[note] = true;
    // Note stack (mono): move this note to the top.
    int w = 0;
    for (int i = 0; i < noteStackSize; ++i) if (noteStack[i] != note) noteStack[w++] = noteStack[i];
    noteStackSize = w;
    if (noteStackSize < 128) noteStack[noteStackSize++] = note;

    Voice* v = nullptr;
    bool fresh = true, legato = false, stolen = false;
    float fromPitch = lastPitch;                     // portamento start
    if (mono)
    {
        // The sounding mono voice: the newest one that isn't fading out.
        Voice* cur = nullptr;
        for (auto& vp : voices)
            if (vp->active && ! vp->killing() && (cur == nullptr || vp->age > cur->age)) cur = vp.get();
        // Anything else still sounding (e.g. just switched from poly) fades out.
        for (auto& vp : voices)
            if (vp.get() != cur) killVoice (*vp);
        if (cur != nullptr) fromPitch = cur->pitch;
        if (cur != nullptr && ! cur->released && legatoOn && otherHeld)
        {
            v = cur;                                 // legato: the same voice glides on
            fresh = false;
            legato = true;
        }
        else if (cur != nullptr)
        {
            // Retrigger: the old note fades out while the new one starts in a
            // free slot, so restarted oscillators and samples don't click.
            const int f = freeSlot();
            if (f >= 0) { killVoice (*cur); v = voices[(size_t) f].get(); }
            else { v = cur; fresh = false; stolen = true; }
        }
        else
        {
            const int f = freeSlot();
            v = voices[(size_t) (f >= 0 ? f : 0)].get();
            if (v->active) { v->killLeft = 0; stolen = true; }
        }
    }
    else
    {
        if (cp.patch->limitSameNote)
            for (auto& vp : voices)
                if (vp->active && ! vp->killing() && vp->note == note) killVoice (*vp);
        const int idx = pickVoice (pv);
        v = voices[(size_t) idx].get();
        stolen = v->active;
    }
    v->killLeft = 0;

    // Portamento: glide from the last pitch when held notes overlap (or ALWAYS).
    const float target = (float) note;
    const bool glide = portaTime > 0.0005f && lastPitch >= 0.0f && (always || otherHeld);
    v->glideTo = target;
    v->glideCurve = pv[g.portaCurve];
    if (glide)
    {
        v->glideFrom = mono ? fromPitch : lastPitch;
        float t = portaTime;
        if (scaled) t *= std::abs (target - v->glideFrom) / 12.0f;
        v->glideLen = t * (float) ctx.sampleRate;
        v->glidePos = 0.0f;
        v->pitch = v->glideFrom;
    }
    else
    {
        v->glideFrom = v->pitch = target;
        v->glideLen = v->glidePos = 0.0f;
    }
    lastPitch = target;

    v->note = note;
    v->velocity = velocity;
    v->age = ++noteCounter;
    v->released = v->sustained = false;
    altFlipFlop = ! altFlipFlop;
    seed = seed * 1664525u + 1013904223u;

    // OSC MAPPING.
    const auto& kb = cp.patch->keyboard;
    for (int r = 0; r < kRows; ++r)
    {
        const int m = kb.mapping[(size_t) kMapRowOfRow[r]].map (note, (int) std::lround (velocity * 127.0f));
        v->mapOffset[r] = m < 0 ? -1000 : m - note;
    }

    NoteInfo info;
    info.note = note;
    info.velocity = velocity;
    info.voiceIndex = v->index;
    info.unisonSeed = (int) (seed >> 8);
    info.legato = legato;
    info.stolen = stolen;

    v->mod.noteOn (*cp.matrix, *cp.shapes, *modGlobal, info, pv, ctx, offset, seed, altFlipFlop, fresh);
    modGlobal->noteOn (*cp.shapes, modGlobal->view (*cp.matrix, pv), ctx, offset, legato);

    // Oscillators read their start parameters through the voice's view.
    const ParamView view = v->mod.view (*cp.matrix, pv);
    OscTick t;
    t.p = &view;
    t.ctx = &ctx;
    t.pitchStart = t.pitchEnd = v->pitch;
    const auto& I = ids();
    for (int o = 0; o < kNumOscs; ++o)
    {
        const int type = (int) pv[I.osc[o].type];
        if (type == 0) v->wt[o].noteOn (info, t, I.osc[o], cp.patch->wavetable[(size_t) o], wtShared[o]);
        else           v->smp[o].noteOn (info, t, I.osc[o], cp.patch->sample[(size_t) o]);
        v->lastOscType[o] = type;
    }
    v->sub.noteOn (info, t, I.sub, subShared);
    v->noise.noteOn (info, t, I.noise, cp.patch->noise);
    if (fresh && ! stolen)
    {
        for (auto& f : v->filter) f.reset();
        v->clearSources();                      // no FM from the slot's previous note
    }
    v->active = true;
    fx.noteOn (note);
}

void Synth::releaseVoice (Voice& v, float relVel)
{
    v.released = true;
    v.mod.noteOff (relVel);
    for (auto& s : v.smp) s.noteOff();
}

void Synth::stopNote (int note, float relVel, const ParamValues& pv)
{
    heldNotes[note] = false;
    int w = 0;
    for (int i = 0; i < noteStackSize; ++i) if (noteStack[i] != note) noteStack[w++] = noteStack[i];
    noteStackSize = w;

    const bool mono = pv[ids().global.mono] >= 0.5f;
    if (mono)
    {
        Voice* cur = nullptr;
        for (auto& vp : voices)
            if (vp->active && ! vp->killing() && (cur == nullptr || vp->age > cur->age)) cur = vp.get();
        if (cur == nullptr || cur->note != note) return;
        auto& v = *cur;
        if (noteStackSize > 0)
        {
            // Fall back to the previous held note.
            const int back = noteStack[noteStackSize - 1];
            noteStackSize--;   // startNote pushes it again
            startNote (back, v.velocity, 0, pv);
            return;
        }
        if (sustainDown) { v.sustained = true; return; }
        releaseVoice (v, relVel);
    }
    else
    {
        for (auto& vp : voices)
        {
            auto& v = *vp;
            if (! v.active || v.killing() || v.released || v.note != note) continue;
            if (sustainDown) v.sustained = true;
            else releaseVoice (v, relVel);
        }
    }
    bool any = false;
    for (bool h : heldNotes) any |= h;
    if (! any && ! sustainDown) modGlobal->allNotesReleased();
}

void Synth::handleEvent (const MidiEvent& e, const ParamValues& pv)
{
    const int type = e.status & 0xF0;
    if (type == 0x90 && e.data2 > 0) startNote (e.data1 & 127, (float) e.data2 / 127.0f, e.offset, pv);
    else if (type == 0x80 || (type == 0x90 && e.data2 == 0)) stopNote (e.data1 & 127, type == 0x80 ? (float) e.data2 / 127.0f : 0.5f, pv);
    else if (type == 0xB0)
    {
        if (e.data1 == 1) modWheel = (float) e.data2 / 127.0f;
        else if (e.data1 == 64)
        {
            sustainDown = e.data2 >= 64;
            if (! sustainDown)
                for (auto& v : voices)
                    if (v->active && v->sustained && ! heldNotes[v->note]) { v->sustained = false; releaseVoice (*v, 0.5f); }
        }
        else if (e.data1 == 120 || e.data1 == 123) allNotesOff();
    }
    else if (type == 0xE0) pitchBend = ((float) ((e.data2 << 7) | e.data1) - 8192.0f) / 8192.0f;
    else if (type == 0xD0) aftertouch = (float) e.data1 / 127.0f;
    else if (type == 0xA0)
        for (auto& v : voices)
            if (v->active && v->note == (e.data1 & 127)) v->polyPressure = (float) e.data2 / 127.0f;
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------
void Synth::renderVoice (Voice& v, int offset, int n, const ParamValues& pv)
{
    const auto& I = ids();
    const auto& cp = *current;
    const auto& patch = *cp.patch;

    // Sources the voice provides: last sample of each module's previous tick.
    for (int b = 0; b < kNumSrcBuffers; ++b)
        v.mod.src[kModSrcOfBuffer[b]] = v.srcLive[b] ? clampf (v.srcHist[b][kTick - 1], -1.0f, 1.0f) : 0.0f;
    v.mod.src[ModSource::PolyAftertouch] = v.polyPressure;
    v.mod.src[ModSource::VoiceIndex] = 0.0f;
    v.mod.tick (*cp.matrix, *cp.shapes, *modGlobal, pv, ctx, offset, n);
    const ParamView p = v.mod.view (*cp.matrix, pv);

    // Pitch.
    const auto& g = I.global;
    const float bend = pitchBend >= 0.0f ? pitchBend * pv[g.bendUp] : -pitchBend * pv[g.bendDown];
    const float p0 = v.pitch;
    const float p1 = v.advanceGlide (n);
    const float tune = p (g.tuning);

    OscTick t;
    t.n = n;
    t.p = &p;
    t.ctx = &ctx;
    t.bendSemis = bend;
    // Sources not rendered yet this tick: their history (exactly kTick samples
    // back). Each row switches its own entry to the current output once rendered.
    for (int b = 0; b < kNumSrcBuffers; ++b) t.src[b] = v.srcLive[b] ? v.srcHist[b] : nullptr;

    bool used[kNumDest][2] {};
    float* fInL[kNumFilters] { v.fL[0], v.fL[1] };
    float* fInR[kNumFilters] { v.fR[0], v.fR[1] };
    bool fHasInput[kNumFilters] {};
    for (int f = 0; f < kNumFilters; ++f) { clear (fInL[f], n); clear (fInR[f], n); }
    v.unison = 0;
    v.ungatedUsed = false;

    auto send = [&] (int dest, bool gated, const float* L, const float* R, float gain)
    {
        if (gain <= 0.0f) return;
        if (! gated) v.ungatedUsed = true;
        const int k = gated ? 0 : 1;
        float* aL = v.acc[dest][k][0];
        float* aR = v.acc[dest][k][1];
        if (! used[dest][k])
        {
            used[dest][k] = true;
            for (int i = 0; i < n; ++i) { aL[i] = L[i] * gain; aR[i] = R[i] * gain; }
            return;
        }
        addScaled (aL, L, gain, n);
        addScaled (aR, R, gain, n);
    };

    // ---- Oscillators --------------------------------------------------------
    for (int r = 0; r < kRows; ++r)
    {
        const int sb = kSrcOfRow[r];
        int enableId, routeId, balId, b1Id, b2Id, envId;
        switch (r)
        {
            case RSub:   enableId = I.sub.enable;   routeId = I.sub.route;   balId = I.sub.filterBal;   b1Id = I.sub.bus1;   b2Id = I.sub.bus2;   envId = I.sub.env1Gate; break;
            case RNoise: enableId = I.noise.enable; routeId = I.noise.route; balId = I.noise.filterBal; b1Id = I.noise.bus1; b2Id = I.noise.bus2; envId = I.noise.env1Gate; break;
            default:
            {
                const auto& o = I.osc[r - RA];
                enableId = o.enable; routeId = o.route; balId = o.filterBal; b1Id = o.bus1; b2Id = o.bus2; envId = o.env1Gate;
            }
        }
        if (! p.on (enableId) || v.mapOffset[r] <= -1000)
        {
            v.srcLive[sb] = false;
            t.src[sb] = nullptr;
            continue;
        }
        t.pitchStart = p0 + bend + tune + (float) v.mapOffset[r];
        t.pitchEnd = p1 + bend + tune + (float) v.mapOffset[r];
        t.outL = v.oscL[r];
        t.outR = v.oscR[r];
        t.outMono = v.srcBuf[sb];
        t.activeUnison = 0;
        switch (r)
        {
            case RSub:   v.sub.render (t, I.sub, subShared); break;
            case RNoise: v.noise.render (t, I.noise, patch.noise); break;
            default:
            {
                const int o = r - RA;
                const int type = (int) pv[I.osc[o].type];
                if (type != v.lastOscType[o])
                {
                    // Type changed while the note plays: start the new engine.
                    NoteInfo info;
                    info.note = v.note; info.velocity = v.velocity; info.voiceIndex = v.index;
                    if (type == 0) v.wt[o].noteOn (info, t, I.osc[o], patch.wavetable[(size_t) o], wtShared[o]);
                    else           v.smp[o].noteOn (info, t, I.osc[o], patch.sample[(size_t) o]);
                    v.lastOscType[o] = type;
                }
                if (type == 0)
                {
                    v.wt[o].render (t, I.osc[o], patch.wavetable[(size_t) o], wtShared[o]);
                    // Phase warps (Flip, PWM, Asym, ...) can leave a DC offset: block it (~6 Hz).
                    auto phaseWarp = [] (float m) { const auto k = warp::kindOf ((int) m); return k == warp::Kind::Phase || k == warp::Kind::Sync || k == warp::Kind::Pd; };
                    if (phaseWarp (pv[I.osc[o].warp1Mode]) || phaseWarp (pv[I.osc[o].warp2Mode]))
                    {
                        const float pole = 1.0f - 40.0f * ctx.invSampleRate;
                        float* ch[2] { t.outL, t.outR };
                        for (int c = 0; c < 2; ++c)
                        {
                            float x1 = v.dcX[o][c], y1 = v.dcY[o][c];
                            for (int i = 0; i < n; ++i) { const float x = ch[c][i]; y1 = x - x1 + pole * y1; x1 = x; ch[c][i] = y1; }
                            v.dcX[o][c] = x1; v.dcY[o][c] = y1;
                        }
                        if (t.outMono != nullptr) for (int i = 0; i < n; ++i) t.outMono[i] = 0.5f * (t.outL[i] + t.outR[i]);
                    }
                }
                else v.smp[o].render (t, I.osc[o], patch.sample[(size_t) o]);
            }
        }
        v.srcLive[sb] = true;
        t.src[sb] = v.srcBuf[sb];               // later rows read this tick's output
        v.unison += std::max (1, t.activeUnison);

        const bool gated = pv[envId] >= 0.5f;
        const int route = p.integer (routeId);
        if (route == 0)
        {
            const float bal = p (balId);
            const float g1 = 1.0f - bal, g2 = bal;
            if (g1 > 0.0f) { addScaled (fInL[0], v.oscL[r], g1, n); addScaled (fInR[0], v.oscR[r], g1, n); fHasInput[0] = true; }
            if (g2 > 0.0f) { addScaled (fInL[1], v.oscL[r], g2, n); addScaled (fInR[1], v.oscR[r], g2, n); fHasInput[1] = true; }
        }
        else if (route == 1) send (kMain, gated, v.oscL[r], v.oscR[r], 1.0f);
        else if (route == 2) send (kDirect, gated, v.oscL[r], v.oscR[r], 1.0f);
        if (route != 3)
        {
            send (kBus1, gated, v.oscL[r], v.oscR[r], p (b1Id));
            send (kBus2, gated, v.oscL[r], v.oscR[r], p (b2Id));
        }
    }

    // ---- Filters ------------------------------------------------------------
    int fr[kNumFilters] { p.integer (I.filter[0].route), p.integer (I.filter[1].route) };
    if (fr[0] == 0 && fr[1] == 0) fr[1] = 1;              // no loop: F2 -> Main
    const int order[2] { fr[1] == 0 ? 1 : 0, fr[1] == 0 ? 0 : 1 };
    for (int k : order)
    {
        const auto& F = I.filter[k];
        const int sb = kSrcF1 + k;
        if (fHasInput[k]) v.filterIdle[k] = 0;
        else if (v.filterIdle[k] < 1 << 20) ++v.filterIdle[k];
        // Keep a filter running for ~2 s after its input stops, for resonant tails.
        const bool running = fHasInput[k] || (float) v.filterIdle[k] * kTick < (float) ctx.sampleRate * 2.0f;
        if (! running) { v.srcLive[sb] = false; continue; }

        if (p.on (F.enable))
        {
            FilterTick ft;
            ft.n = n;
            ft.type = p.integer (F.type);
            float cut = p (F.cutoff);
            if (pv[F.keytrack] >= 0.5f) cut *= std::exp2 ((p1 - 60.0f) / 12.0f);
            cut = clampf (cut, 8.0f, std::min (22000.0f, (float) ctx.sampleRate * 0.47f));
            ft.cutoffHz = cut;
            ft.cutoffHzPrev = v.filterCutPrev[k] > 0.0f ? v.filterCutPrev[k] : cut;
            v.filterCutPrev[k] = cut;
            ft.res = p (F.res);
            ft.drive = p (F.drive);
            ft.var = p (F.var);
            ft.pan = p (F.pan);
            ft.mix = p (F.mix);
            ft.cleanDrive = pv[F.cleanDrive] >= 0.5f;
            ft.ctx = &ctx;
            ft.L = fInL[k];
            ft.R = fInR[k];
            v.filter[k].process (ft);
        }
        // Level and channel pan.
        float gl, gr;
        panGains (p (F.chanPan), gl, gr);
        const float lvl = dbToGain (p (F.level));
        for (int i = 0; i < n; ++i)
        {
            fInL[k][i] *= lvl * gl;
            fInR[k][i] *= lvl * gr;
            v.srcBuf[sb][i] = 0.5f * (fInL[k][i] + fInR[k][i]);
        }
        v.srcLive[sb] = true;

        const bool gated = pv[F.env1Gate] >= 0.5f;
        const int route = fr[k];
        if (route == 0)
        {
            const int other = 1 - k;
            addScaled (fInL[other], fInL[k], 1.0f, n);
            addScaled (fInR[other], fInR[k], 1.0f, n);
            fHasInput[other] = true;
        }
        else if (route == 1) send (kMain, gated, fInL[k], fInR[k], 1.0f);
        else if (route == 2) send (kDirect, gated, fInL[k], fInR[k], 1.0f);
        if (route != 3)
        {
            send (kBus1, gated, fInL[k], fInR[k], p (F.bus1));
            send (kBus2, gated, fInL[k], fInR[k], p (F.bus2));
        }
    }

    v.advanceSources (n);

    // ---- Amp (ENV 1) and sum into the block buses ---------------------------
    const auto& env = v.mod.envOut (0);
    constexpr float kHeadroom = 0.5f;   // -6 dB per voice, so chords don't clip at default levels
    const float a0 = env.start * p.previous (g.amp) * kHeadroom, a1 = env.end * p (g.amp) * kHeadroom;
    // Steal fade: an S-curve (smoothstep, no slope jump at either end) over killLen samples.
    float fade[kTick];
    if (v.killing())
    {
        const float inv = 1.0f / (float) v.killLen;
        for (int i = 0; i < n; ++i)
        {
            const float x = clamp01 ((float) (v.killLeft - i) * inv);   // 1 -> 0
            fade[i] = x * x * (3.0f - 2.0f * x);
        }
    }
    const float da = (a1 - a0) / (float) n;
    for (int d = 0; d < kNumDest; ++d)
        for (int c = 0; c < 2; ++c)
        {
            float* out = bus[d][c].data() + offset;
            if (used[d][0])
            {
                const float* gsrc = v.acc[d][0][c];
                if (v.killing())
                    for (int i = 0; i < n; ++i) out[i] += gsrc[i] * (a0 + da * (float) i) * fade[i];
                else
                    for (int i = 0; i < n; ++i) out[i] += gsrc[i] * (a0 + da * (float) i);
            }
            if (used[d][1])
            {
                const float* usrc = v.acc[d][1][c];
                if (v.killing())
                    for (int i = 0; i < n; ++i) out[i] += usrc[i] * kHeadroom * fade[i];
                else
                    for (int i = 0; i < n; ++i) out[i] += usrc[i] * kHeadroom;
            }
        }

    // ---- Voice end ----------------------------------------------------------
    if (v.killing())
    {
        v.killLeft -= n;
        if (v.killLeft <= 0) { v.killLeft = 0; v.active = false; }
        return;
    }
    if (v.released)
    {
        bool done = v.mod.finished();
        if (done && v.ungatedUsed)
            for (int e = 1; e < kNumEnvs; ++e) done &= v.mod.envelope (e).finished();
        if (done) v.active = false;
    }
}

void Synth::process (float* outL, float* outR, int numSamples, const MidiEvent* midiIn, int numMidi,
                     const ParamValues& pv, const HostInfo& host, MidiBuffer& midiOut)
{
    ScopedNoDenormals noDenormals;
    swapInPatch();
    const auto& cp = *current;
    const auto& g = ids().global;

    ctx.bpm = host.bpm > 0.0 ? host.bpm : 120.0;
    ctx.ppq = host.ppq;
    ctx.playing = host.playing;
    telemetry.bpm.store (ctx.bpm, std::memory_order_relaxed);
    telemetry.playing.store (host.playing, std::memory_order_relaxed);

    // Long host blocks are processed in chunks of at most maxBlock.
    int done = 0;
    while (done < numSamples)
    {
        const int len = std::min (maxBlock, numSamples - done);

        // MIDI for this chunk.
        toVoices.clear();
        chunkIn.clear();
        for (int i = 0; i < numMidi; ++i)
            if (midiIn[i].offset >= done && midiIn[i].offset < done + len)
            {
                MidiEvent e = midiIn[i];
                e.offset -= done;
                chunkIn.add (e);
            }
        notes->process (chunkIn.data(), chunkIn.size(), len, ctx, pv, *cp.arp, *cp.clips, toVoices, hostOutChunk);
        for (const auto& e : hostOutChunk) { MidiEvent x = e; x.offset += done; midiOut.add (x); }

        for (auto& d : bus) for (auto& ch : d) clear (ch.data(), len);
        fx.beginBlock (ctx);

        // Block-rate modulation.
        const Voice* newest = nullptr;
        int activeCount = 0;
        for (auto& v : voices)
            if (v->active && ! v->killing()) { ++activeCount; if (newest == nullptr || v->age > newest->age) newest = v.get(); }
        const int poly = std::max (1, (int) pv[g.poly]);
        modGlobal->globals[ModSource::ModWheel] = modWheel;
        modGlobal->globals[ModSource::Aftertouch] = aftertouch;
        modGlobal->globals[ModSource::PitchBend] = pitchBend;
        modGlobal->globals[ModSource::ActiveVoices] = std::min (1.0f, (float) activeCount / (float) poly);
        modGlobal->beginBlock (*cp.matrix, pv, newest != nullptr ? &newest->mod : nullptr, (float) len * ctx.invSampleRate);

        // Ticks, split at MIDI events.
        int pos = 0, ev = 0;
        while (pos < len)
        {
            while (ev < toVoices.size() && toVoices[ev].offset <= pos)
                handleEvent (toVoices[ev++], pv);
            int n = std::min (kTick, len - pos);
            if (ev < toVoices.size()) n = std::min (n, std::max (1, toVoices[ev].offset - pos));

            {
                // Shared (FREE / MONO) LFOs read their own settings through the
                // newest voice, so per-voice rows (macros, envelopes...) reach them.
                const Voice* nv = nullptr;
                for (auto& v : voices)
                    if (v->active && (nv == nullptr || v->age > nv->age)) nv = v.get();
                const ParamView lfoView = nv != nullptr ? nv->mod.view (*cp.matrix, pv) : modGlobal->view (*cp.matrix, pv);
                modGlobal->tick (*cp.matrix, *cp.shapes, lfoView, ctx, pos, n);
            }
            for (auto& v : voices)
                if (v->active) renderVoice (*v, pos, n, pv);
            pos += n;
            ctx.ppq += (double) n * ctx.invSampleRate * ctx.bpm / 60.0;
        }
        while (ev < toVoices.size()) handleEvent (toVoices[ev++], pv);

        // ---- Buses, FX racks, output --------------------------------------
        const ParamView gview = modGlobal->view (*cp.matrix, pv);
        int r1 = (int) pv[g.bus1Route], r2 = (int) pv[g.bus2Route];
        if (r1 == 2 && r2 == 2) r2 = 0;                   // no loop: Bus 2 -> Main
        auto runBus = [&] (int which)
        {
            const int b = which == 0 ? kBus1 : kBus2;
            fx.process (which == 0 ? 1 : 2, bus[b][0].data(), bus[b][1].data(), len, gview);
            const float lvl = dbToGain (gview (which == 0 ? g.bus1Level : g.bus2Level));
            const int route = which == 0 ? r1 : r2;
            const int dest = route == 0 ? kMain : route == 1 ? kDirect : (which == 0 ? kBus2 : kBus1);
            for (int c = 0; c < 2; ++c)
                addScaled (bus[dest][c].data(), bus[b][c].data(), lvl, len);
        };
        if (r2 == 2) { runBus (1); runBus (0); }          // Bus 2 feeds Bus 1
        else         { runBus (0); runBus (1); }

        fx.process (0, bus[kMain][0].data(), bus[kMain][1].data(), len, gview);
        const float mainG = dbToGain (gview (g.mainLevel)) * dbToGain (gview (g.master));
        const float directG = dbToGain (gview (g.directLevel)) * dbToGain (gview (g.master));
        float pkL = 0.0f, pkR = 0.0f;
        for (int i = 0; i < len; ++i)
        {
            const float l = bus[kMain][0][(size_t) i] * mainG + bus[kDirect][0][(size_t) i] * directG;
            const float r = bus[kMain][1][(size_t) i] * mainG + bus[kDirect][1][(size_t) i] * directG;
            outL[done + i] = std::isfinite (l) ? l : 0.0f;
            outR[done + i] = std::isfinite (r) ? r : 0.0f;
            pkL = std::max (pkL, std::abs (l));
            pkR = std::max (pkR, std::abs (r));
        }
        telemetry.pushPeak (pkL, pkR);
        done += len;
    }

    // ---- Telemetry --------------------------------------------------------
    const Voice* newest = nullptr;
    int activeCount = 0, unison = 0;
    uint64_t lo = 0, hi = 0;
    for (auto& v : voices)
        if (v->active)
        {
            ++activeCount;
            unison += v->unison;
            if (newest == nullptr || v->age > newest->age) newest = v.get();
        }
    for (int nn = 0; nn < 128; ++nn)
        if (heldNotes[nn]) (nn < 64 ? lo : hi) |= 1ull << (nn & 63);
    telemetry.keysLo.store (lo, std::memory_order_relaxed);
    telemetry.keysHi.store (hi, std::memory_order_relaxed);
    telemetry.activeVoices.store (activeCount, std::memory_order_relaxed);
    telemetry.activeUnison.store (unison, std::memory_order_relaxed);
    modGlobal->publish (*cp.matrix, newest != nullptr ? &newest->mod : nullptr, matrixTelemetry, lfoTelemetry);
    if (newest != nullptr)
    {
        telemetry.lastNote.store (newest->note, std::memory_order_relaxed);
        telemetry.lastVelocity.store (newest->velocity, std::memory_order_relaxed);
        for (int e = 0; e < kNumEnvs; ++e)
        {
            telemetry.env[e].store (newest->mod.envOut (e).end, std::memory_order_relaxed);
            telemetry.envStage[e].store ((int) newest->mod.envelope (e).currentStage(), std::memory_order_relaxed);
            telemetry.envProgress[e].store (newest->mod.envelope (e).progress(), std::memory_order_relaxed);
        }
        const ParamView p = newest->mod.view (*cp.matrix, pv);
        const auto& reg = params();
        for (int h = 0; h < Telemetry::kHalo; ++h)
        {
            const int id = telemetry.haloParam[h].load (std::memory_order_relaxed);
            if (id >= 0 && id < reg.size())
                telemetry.haloNorm[h].store (reg[id].toNorm (reg[id].perVoice() ? p (id) : modGlobal->view (*cp.matrix, pv) (id)), std::memory_order_relaxed);
        }
        for (int o = 0; o < kNumOscs; ++o)
        {
            telemetry.wtPos[o].store (p (ids().osc[o].wtpos), std::memory_order_relaxed);
            const auto& st = newest->smp[o].telemetry();
            const bool sampleMode = (int) pv[ids().osc[o].type] != 0 && st.numPlayheads.load (std::memory_order_relaxed) > 0;
            telemetry.oscPlayhead[o].store (sampleMode ? st.playhead[0].load (std::memory_order_relaxed) : -1.0f, std::memory_order_relaxed);
        }
        telemetry.newestVoice.store (newest->index, std::memory_order_relaxed);
    }
    else
    {
        for (int e = 0; e < kNumEnvs; ++e) telemetry.envStage[e].store (0, std::memory_order_relaxed);
        for (int o = 0; o < kNumOscs; ++o) telemetry.oscPlayhead[o].store (-1.0f, std::memory_order_relaxed);
        telemetry.newestVoice.store (-1, std::memory_order_relaxed);
    }
    for (int l = 0; l < kNumLfos; ++l)
    {
        telemetry.lfo[l].store (lfoTelemetry[l].value.load (std::memory_order_relaxed), std::memory_order_relaxed);
        telemetry.lfoPhase[l].store (lfoTelemetry[l].phase.load (std::memory_order_relaxed), std::memory_order_relaxed);
    }
}
} // namespace tincture
