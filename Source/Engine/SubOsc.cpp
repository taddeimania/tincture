// SubOsc.cpp — SUB oscillator: band-limited single-frame mip tables read
// with linear interpolation (the same reader as the wavetable oscillator).
#include "SubOsc.h"
#include "Wavetable.h"

#include <array>

namespace tincture
{
namespace
{
// One single-frame table per shape (Sine, Rounded Rect, Triangle, Saw, Square, 25% Pulse).
const std::array<std::shared_ptr<const Wavetable>, 6>& shapeTables()
{
    static const std::array<std::shared_ptr<const Wavetable>, 6> t = []
    {
        const double pi = 3.14159265358979323846;
        auto frame = [] (auto f) { std::vector<float> v (Wavetable::kFrameSize); for (int i = 0; i < Wavetable::kFrameSize; ++i) v[(size_t) i] = (float) f ((double) i / Wavetable::kFrameSize); return v; };
        std::array<std::shared_ptr<const Wavetable>, 6> a;
        a[0] = Wavetable::create ("Sine", "", { frame ([&] (double x) { return std::sin (2.0 * pi * x); }) });
        a[1] = Wavetable::create ("Rounded Rect", "", { frame ([&] (double x) { return std::tanh (3.0 * std::sin (2.0 * pi * x)); }) });
        a[2] = Wavetable::create ("Triangle", "", { frame ([] (double x) { return x < 0.25 ? 4.0 * x : (x < 0.75 ? 2.0 - 4.0 * x : 4.0 * x - 4.0); }) });
        a[3] = Wavetable::create ("Saw", "", { frame ([] (double x) { return x < 0.5 ? 2.0 * x : 2.0 * x - 2.0; }) });
        a[4] = Wavetable::create ("Square", "", { frame ([] (double x) { return x < 0.5 ? 1.0 : -1.0; }) });
        a[5] = Wavetable::create ("Pulse", "", { frame ([] (double x) { return x < 0.25 ? 1.0 : -1.0; }) });
        return a;
    }();
    return t;
}
} // namespace

float SubOsc::sample (int shape, float t) noexcept
{
    const Wavetable& w = *shapeTables()[(size_t) std::clamp (shape, 0, 5)];
    const float x = clamp01 (t) * (float) Wavetable::kFrameSize;
    const int j = std::min ((int) x, Wavetable::kFrameSize - 1);
    const float* f = w.mipFrame (0, 0);
    return f[j] + (f[j + 1] - f[j]) * (x - (float) j);
}

void SubOsc::prepare (const Context& ctx)
{
    sampleRate = (float) ctx.sampleRate;
    (void) shapeTables();   // build the tables off the audio thread
    reset();
}

void SubOsc::reset() noexcept
{
    acc = 0;
    gainsValid = false;
}

void SubOsc::noteOn (const NoteInfo& note, const OscTick& tick, const SubIds& ids, Shared& sh)
{
    const ParamView& p = *tick.p;
    stamp = ++sh.counter;
    sh.newest = stamp;
    if (note.legato) return;   // legato re-pitch: keep running
    if (p.on (ids.contiguous) && sh.valid) acc = sh.phase;
    else
    {
        const double x = (double) p (ids.phase) / 360.0;
        acc = (uint32_t) (int64_t) ((x - std::floor (x)) * 4294967296.0);
    }
    gainsValid = false;
}

void SubOsc::render (OscTick& tick, const SubIds& ids, Shared& sh)
{
    const int n = std::clamp (tick.n, 0, kTick);
    if (n == 0 || tick.p == nullptr) { tick.activeUnison = 0; return; }
    const ParamView& p = *tick.p;
    const float sr = tick.ctx != nullptr ? (float) tick.ctx->sampleRate : sampleRate;

    const bool track = p.on (ids.pitchTrack);
    const float offs = 12.0f * (float) p.integer (ids.oct) + p (ids.coarse);
    const float b0 = (track ? tick.pitchStart : tick.bendSemis) + offs;
    const float b1 = (track ? tick.pitchEnd : tick.bendSemis) + offs;
    const float inc0 = std::min (noteToHz (b0) / sr, 0.49f);
    const float inc1 = std::min (noteToHz (b1) / sr, 0.49f);

    // Mip level for this tick's rate (harmonics may fold only into the inaudible top band).
    const Wavetable& table = *shapeTables()[(size_t) std::clamp (p.integer (ids.shape), 0, 5)];
    const int mip = Wavetable::mipFor (std::max (inc0, inc1), std::max (0.5f, 1.0f - 20000.0f / sr));
    const float* f = table.mipFrame (mip, 0);
    const float L = (float) Wavetable::mipLength (mip);
    const uint32_t i0 = (uint32_t) (inc0 * 4294967296.0f);
    const int32_t di = (int32_t) (((int64_t) (uint32_t) (inc1 * 4294967296.0f) - (int64_t) i0) / n);
    const float p0 = (float) (acc >> 8) * (1.0f / 16777216.0f);
    const float incF = (float) i0 * (1.0f / 4294967296.0f), fd = (float) di * (1.0f / 4294967296.0f);
    for (int i = 0; i < n; ++i)
    {
        const float fl = (float) i;
        float x = p0 + fl * incF + (0.5f * fl * (fl - 1.0f)) * fd;
        x = (x - (float) (int) x) * L;
        const int j = (int) x;
        buf[i] = f[j] + (f[j + 1] - f[j]) * (x - (float) j);
    }
    acc += (uint32_t) ((uint64_t) i0 * (uint64_t) n + (uint64_t) ((int64_t) di * (int64_t) (n * (n - 1) / 2)));

    float tl, tr;
    panGains (p (ids.pan), tl, tr);
    const float level = std::max (0.0f, p (ids.level));
    tl *= level; tr *= level;
    const float l0 = gainsValid ? gL : tl, r0 = gainsValid ? gR : tr;
    const float dl = (tl - l0) / (float) n, dr = (tr - r0) / (float) n;
    for (int i = 0; i < n; ++i)
    {
        const float fi = (float) (i + 1);
        tick.outL[i] = buf[i] * (l0 + dl * fi);
        tick.outR[i] = buf[i] * (r0 + dr * fi);
    }
    if (tick.outMono != nullptr)
        for (int i = 0; i < n; ++i) tick.outMono[i] = 0.5f * (tick.outL[i] + tick.outR[i]);
    gL = tl; gR = tr; gainsValid = true;
    tick.activeUnison = 1;

    if (stamp == sh.newest) { sh.phase = acc; sh.valid = true; }
}
} // namespace tincture
