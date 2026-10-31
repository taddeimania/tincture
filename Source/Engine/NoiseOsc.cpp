// NoiseOsc.cpp — NOISE oscillator (colour noises and sample playback) and
// the generated factory noise samples.
#include "NoiseOsc.h"

#include <cmath>
#include <functional>
#include <mutex>

namespace tincture
{
// ---------------------------------------------------------------------------
// Colour noise generators (one sample of L and R, independent streams)
// ---------------------------------------------------------------------------
namespace
{
inline float pinkStep (float (&b)[7], float w) noexcept
{
    // Paul Kellet's refined pink filter (-3 dB/oct within +-0.05 dB above ~10 Hz).
    b[0] = 0.99886f * b[0] + w * 0.0555179f;
    b[1] = 0.99332f * b[1] + w * 0.0750759f;
    b[2] = 0.96900f * b[2] + w * 0.1538520f;
    b[3] = 0.86650f * b[3] + w * 0.3104856f;
    b[4] = 0.55000f * b[4] + w * 0.5329522f;
    b[5] = -0.7616f * b[5] - w * 0.0168980f;
    const float y = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
    b[6] = w * 0.115926f;
    return y * 0.11f;
}

inline float hermite (float xm1, float x0, float x1, float x2, float f) noexcept
{
    const float c1 = 0.5f * (x1 - xm1);
    const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
    const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}
} // namespace

// ---------------------------------------------------------------------------
// NoiseOscData
// ---------------------------------------------------------------------------
void NoiseOscData::save (Node& n) const
{
    if (! sample) return;
    Node c ("sample");
    if (FactoryNoises::isFactory (*sample)) c.set ("factory", 1.0).set ("category", sample->category).set ("name", sample->name);
    else sample->save (c);
    n.add (std::move (c));
}

void NoiseOscData::load (const Node& n)
{
    sample.reset();
    if (const Node* c = n.child ("sample"))
    {
        if (c->flag ("factory")) sample = FactoryNoises::get (c->str ("category"), c->str ("name"));
        else sample = SampleData::load (*c);
    }
    if (! sample) sample = FactoryNoises::defaultNoise();
}

// ---------------------------------------------------------------------------
// NoiseOsc
// ---------------------------------------------------------------------------
void NoiseOsc::prepare (const Context& ctx)
{
    sampleRate = (float) ctx.sampleRate;
    reset();
}

void NoiseOsc::reset() noexcept
{
    for (auto& c : pink) for (auto& v : c) v = 0.0f;
    brown[0] = brown[1] = click[0] = click[1] = tiltZ[0] = tiltZ[1] = 0.0f;
    pos = 0.0; finished = false; gainsValid = false; lastRate = lastStart = -1.0f;
}

void NoiseOsc::noteOn (const NoteInfo& note, const OscTick& tick, const NoiseIds& ids, const NoiseOscData&)
{
    const ParamView& p = *tick.p;
    const uint32_t seed = (uint32_t) note.unisonSeed * 2654435761u + (uint32_t) note.voiceIndex * 97u + 1u;
    rngL = Rng (seed ^ 0x9E3779B9u);
    rngR = Rng (seed ^ 0x7F4A7C15u);
    if (note.legato) return;
    pos = 0.0;
    randStart = clamp01 (p (ids.rand)) * rngL.uni();
    finished = false;
    lastRate = lastStart = -1.0f;
    gainsValid = false;
}

void NoiseOsc::render (OscTick& tick, const NoiseIds& ids, const NoiseOscData& data)
{
    const int n = std::clamp (tick.n, 0, kTick);
    if (n == 0 || tick.p == nullptr) { tick.activeUnison = 0; return; }
    const ParamView& p = *tick.p;
    const float sr = tick.ctx != nullptr ? (float) tick.ctx->sampleRate : sampleRate;
    const int colour = p.integer (ids.colour);
    float* L = bufL;
    float* R = bufR;

    if (colour == SampleMode)
    {
        const SampleData* s = data.sample.get();
        const int F = s != nullptr ? s->frames() : 0;
        if (F < 4 || finished)
        {
            for (int i = 0; i < n; ++i) L[i] = R[i] = 0.0f;
        }
        else
        {
            // Rate: file rate, PITCH (+-48 st around 50%), FINE, optional key tracking.
            float semis = (clamp01 (p (ids.pitch)) - 0.5f) * 96.0f + 0.01f * p (ids.fine);
            if (p.on (ids.pitchTrack)) semis += tick.pitchEnd - (float) s->rootNote;
            const float rate = (float) (s->sampleRate / (double) sr) * std::exp2 (semis * (1.0f / 12.0f));
            const float start = clamp01 (p (ids.start)) + randStart;
            const float r0 = lastRate < 0.0f ? rate : lastRate;
            const float st0 = lastStart < 0.0f ? start : lastStart;
            lastRate = rate; lastStart = start;

            const bool oneShot = p.on (ids.oneShot);
            const bool hasLoop = s->loopStart >= 0 && s->loopEnd > s->loopStart + 4 && s->loopEnd <= F;
            const double ls = hasLoop ? s->loopStart : 0.0, le = hasLoop ? s->loopEnd : (double) F;
            const double len = le - ls;
            const float* cl = s->channel (0);
            const float* cr = s->channel (1);
            const bool stereo = s->stereo();
            const double dr = (double) (rate - r0) / n;
            const double ds = (double) (start - st0) * F / n;
            double rateNow = r0, stNow = (double) st0 * F;

            auto fetch = [&] (const float* c, int i) -> float
            {
                if (i >= 0 && i < F) return c[i];
                if (oneShot) return 0.0f;
                if (i < 0) i += F;
                else if (i >= (int) le) i = (int) ls + (int) ((i - (int) ls) % (int) len);
                return i >= 0 && i < F ? c[i] : 0.0f;
            };

            int i = 0;
            double r = 0.0;
            for (; i < n; ++i)
            {
                r = pos + stNow;
                if (r >= le && ! oneShot)
                {
                    // Wrap into the loop (keeps pos bounded).
                    const double k = std::floor ((r - ls) / len);
                    pos -= k * len;
                    r -= k * len;
                }
                if (r >= (double) F) { finished = true; break; }   // one-shot end
                const int j = (int) r;
                const float f = (float) (r - j);
                if (j >= 1 && j + 2 < F)
                {
                    L[i] = hermite (cl[j - 1], cl[j], cl[j + 1], cl[j + 2], f);
                    R[i] = stereo ? hermite (cr[j - 1], cr[j], cr[j + 1], cr[j + 2], f) : L[i];
                }
                else
                {
                    L[i] = hermite (fetch (cl, j - 1), fetch (cl, j), fetch (cl, j + 1), fetch (cl, j + 2), f);
                    R[i] = stereo ? hermite (fetch (cr, j - 1), fetch (cr, j), fetch (cr, j + 1), fetch (cr, j + 2), f) : L[i];
                }
                pos += rateNow;
                rateNow += dr;
                stNow += ds;
            }
            for (; i < n; ++i) L[i] = R[i] = 0.0f;
            uiPos = (float) (r / F);
        }
    }
    else
    {
        const float st = clamp01 (p (ids.stereo));
        const float a = 1.0f - st, b = st;
        const float nrm = 1.0f / std::sqrt (a * a + b * b);
        const float ka = a * nrm, kb = b * nrm;
        switch (colour)
        {
            case White:
                for (int i = 0; i < n; ++i)
                {
                    const float l = rngL.bi(), r = rngR.bi();
                    L[i] = l; R[i] = ka * l + kb * r;
                }
                break;
            case Pink:
                for (int i = 0; i < n; ++i)
                {
                    const float l = pinkStep (pink[0], rngL.bi()), r = pinkStep (pink[1], rngR.bi());
                    L[i] = l; R[i] = ka * l + kb * r;
                }
                break;
            case Brown:
            {
                // Leaky integrator (corner ~15 Hz), RMS matched to white.
                const float leak = 1.0f - 95.0f / sr, g = std::sqrt (1.0f - leak * leak);
                float yl = brown[0], yr = brown[1];
                for (int i = 0; i < n; ++i)
                {
                    yl = yl * leak + g * rngL.bi();
                    yr = yr * leak + g * rngR.bi();
                    L[i] = yl; R[i] = ka * yl + kb * yr;
                }
                brown[0] = yl; brown[1] = yr;
                break;
            }
            default:   // Geiger: sparse random clicks; PITCH sets the density (2..512 per second).
            {
                const float prob = std::exp2 (1.0f + 8.0f * clamp01 (p (ids.pitch))) / sr;
                const float decay = std::exp (-1.0f / (0.0004f * sr));
                float cl = click[0], cr = click[1];
                for (int i = 0; i < n; ++i)
                {
                    if (rngL.uni() < prob) cl = (rngL.uni() < 0.5f ? -1.0f : 1.0f) * (0.5f + 0.5f * rngL.uni());
                    if (rngR.uni() < prob) cr = (rngR.uni() < 0.5f ? -1.0f : 1.0f) * (0.5f + 0.5f * rngR.uni());
                    L[i] = cl; R[i] = ka * cl + kb * cr;
                    cl *= decay; cr *= decay;
                }
                click[0] = std::abs (cl) < 1.0e-20f ? 0.0f : cl;
                click[1] = std::abs (cr) < 1.0e-20f ? 0.0f : cr;
                break;
            }
        }

        // FILTER: tilt. Negative = low-pass closing to ~150 Hz, positive = high-pass opening to ~10 kHz.
        const float tilt = clampf (p (ids.tilt), -1.0f, 1.0f);
        if (std::abs (tilt) > 0.005f)
        {
            const float fc = tilt < 0.0f ? 20000.0f * std::exp2 (7.0f * tilt) : 20.0f * std::exp2 (9.0f * tilt);
            const float c = 1.0f - std::exp (-kTwoPi * std::min (fc, 0.45f * sr) / sr);
            float zl = tiltZ[0], zr = tiltZ[1];
            if (tilt < 0.0f)
                for (int i = 0; i < n; ++i) { zl += c * (L[i] - zl); zr += c * (R[i] - zr); L[i] = zl; R[i] = zr; }
            else
                for (int i = 0; i < n; ++i) { zl += c * (L[i] - zl); zr += c * (R[i] - zr); L[i] -= zl; R[i] -= zr; }
            tiltZ[0] = std::abs (zl) < 1.0e-20f ? 0.0f : zl;
            tiltZ[1] = std::abs (zr) < 1.0e-20f ? 0.0f : zr;
        }
    }

    float tl, tr;
    panGains (p (ids.pan), tl, tr);
    const float level = std::max (0.0f, p (ids.level));
    tl *= level; tr *= level;
    const float l0 = gainsValid ? gL : tl, r0 = gainsValid ? gR : tr;
    const float dl = (tl - l0) / (float) n, drr = (tr - r0) / (float) n;
    for (int i = 0; i < n; ++i)
    {
        const float fi = (float) (i + 1);
        tick.outL[i] = L[i] * (l0 + dl * fi);
        tick.outR[i] = R[i] * (r0 + drr * fi);
    }
    if (tick.outMono != nullptr)
        for (int i = 0; i < n; ++i) tick.outMono[i] = 0.5f * (tick.outL[i] + tick.outR[i]);
    gL = tl; gR = tr; gainsValid = true;
    tick.activeUnison = 1;
}

// ---------------------------------------------------------------------------
// Factory noises (generated; 48 kHz)
// ---------------------------------------------------------------------------
namespace
{
constexpr double kRate = 48000.0;
constexpr double kPiD = 3.14159265358979323846;

struct Gen
{
    std::vector<float> l, r;
};

// Make a seamless loop: generate len + fade samples, crossfade the tail into the head.
void loopify (std::vector<float>& x, int len)
{
    const int fade = (int) x.size() - len;
    if (fade <= 0) return;
    for (int i = 0; i < fade; ++i)
    {
        const double t = (double) i / fade;
        const double a = std::sqrt (t), b = std::sqrt (1.0 - t);
        x[(size_t) i] = (float) (x[(size_t) i] * a + x[(size_t) (len + i)] * b);
    }
    x.resize ((size_t) len);
}

void normalise (Gen& g, float peak)
{
    float m = 0.0f;
    for (float v : g.l) m = std::max (m, std::abs (v));
    for (float v : g.r) m = std::max (m, std::abs (v));
    if (m < 1.0e-9f) return;
    const float k = peak / m;
    for (auto& v : g.l) v *= k;
    for (auto& v : g.r) v *= k;
}

struct OnePole
{
    double z = 0.0, c = 0.0;
    explicit OnePole (double hz) : c (1.0 - std::exp (-2.0 * kPiD * hz / kRate)) {}
    double lp (double x) { z += c * (x - z); return z; }
    double hp (double x) { return x - lp (x); }
};

struct Svf   // band-pass, for the organic noises
{
    double ic1 = 0, ic2 = 0;
    double bp (double x, double hz, double q)
    {
        const double g = std::tan (kPiD * std::min (hz, 0.45 * kRate) / kRate), k = 1.0 / q;
        const double a1 = 1.0 / (1.0 + g * (g + k)), a2 = g * a1, a3 = g * a2;
        const double v3 = x - ic2, v1 = a1 * ic1 + a2 * v3, v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2 * v1 - ic1; ic2 = 2 * v2 - ic2;
        return v1;
    }
};

Gen hum (double hz)
{
    const int len = (int) (2.0 * kRate);   // a whole number of cycles at 50 and 60 Hz
    Gen g;
    g.l.resize ((size_t) len);
    static const double amps[12] { 1.0, 0.45, 0.4, 0.15, 0.22, 0.08, 0.12, 0.04, 0.06, 0.03, 0.04, 0.02 };
    Rng rng (77u);
    OnePole hp (2000.0);
    for (int i = 0; i < len; ++i)
    {
        const double t = i / kRate;
        double y = 0.0;
        for (int k = 0; k < 12; ++k) y += amps[k] * std::sin (2.0 * kPiD * hz * (k + 1) * t + 0.4 * k);
        g.l[(size_t) i] = (float) (y + 0.02 * hp.hp (rng.bi()));
    }
    normalise (g, 0.9f);
    return g;
}

Gen tapeHiss()
{
    const int len = (int) (3.0 * kRate), fade = (int) (0.1 * kRate);
    Gen g;
    Rng a (11u), b (12u);
    OnePole hl (250.0), hr (250.0), sl (4000.0), sr (4000.0);
    for (int i = 0; i < len + fade; ++i)
    {
        const double xl = hl.hp (a.bi()), xr = hr.hp (b.bi());
        g.l.push_back ((float) (xl + 0.6 * sl.hp (xl)));
        g.r.push_back ((float) (xr + 0.6 * sr.hp (xr)));
    }
    loopify (g.l, len); loopify (g.r, len);
    normalise (g, 0.8f);
    return g;
}

Gen vinyl()
{
    const int len = (int) (3.0 * kRate), fade = (int) (0.05 * kRate);
    Gen g;
    Rng rng (21u);
    double cl = 0, cr = 0, rum = 0;
    OnePole hl (800.0), hr (800.0), rumLp (25.0), hiss (6000.0);
    for (int i = 0; i < len + fade; ++i)
    {
        if (rng.uni() < 40.0 / kRate) { const double v = rng.uni(); cl = (rng.bi() > 0 ? 1 : -1) * v * v * v; }
        if (rng.uni() < 40.0 / kRate) { const double v = rng.uni(); cr = (rng.bi() > 0 ? 1 : -1) * v * v * v; }
        rum = rumLp.lp (rng.bi());
        const double h = 0.03 * hiss.hp (rng.bi());
        g.l.push_back ((float) (hl.hp (cl) + 3.0 * rum + h));
        g.r.push_back ((float) (hr.hp (cr) + 3.0 * rum + h));
        cl *= 0.7; cr *= 0.7;
    }
    loopify (g.l, len); loopify (g.r, len);
    normalise (g, 0.9f);
    return g;
}

Gen rain()
{
    const int len = (int) (4.0 * kRate), fade = (int) (0.2 * kRate);
    Gen g;
    g.l.assign ((size_t) (len + fade), 0.0f);
    g.r.assign ((size_t) (len + fade), 0.0f);
    Rng rng (31u);
    // Bed of filtered noise.
    float pk[2][7] {};
    OnePole ll (3000.0), lr (3000.0);
    for (int i = 0; i < len + fade; ++i)
    {
        g.l[(size_t) i] = (float) (0.4 * ll.lp (pinkStep (pk[0], rng.bi())));
        g.r[(size_t) i] = (float) (0.4 * lr.lp (pinkStep (pk[1], rng.bi())));
    }
    // Droplets: short decaying sine blips.
    const int drops = (int) (300.0 * (len + fade) / kRate);
    for (int d = 0; d < drops; ++d)
    {
        const int at = (int) (rng.uni() * (len + fade - 2000));
        const double f = 1500.0 + 5000.0 * rng.uni(), dec = 0.002 + 0.006 * rng.uni();
        const double amp = 0.05 + 0.25 * rng.uni() * rng.uni(), pan = rng.uni();
        for (int i = 0; i < 2000 && at + i < len + fade; ++i)
        {
            const double t = i / kRate;
            const double v = amp * std::exp (-t / dec) * std::sin (2.0 * kPiD * f * t * (1.0 - 0.3 * t / (t + dec)));
            g.l[(size_t) (at + i)] += (float) (v * (1.0 - pan));
            g.r[(size_t) (at + i)] += (float) (v * pan);
        }
    }
    loopify (g.l, len); loopify (g.r, len);
    normalise (g, 0.85f);
    return g;
}

Gen wind()
{
    const int len = (int) (4.0 * kRate), fade = (int) (0.4 * kRate);
    Gen g;
    Rng a (41u), b (42u);
    Svf fl, fr;
    for (int i = 0; i < len + fade; ++i)
    {
        const double t = i / kRate;
        const double c = 500.0 + 250.0 * std::sin (2.0 * kPiD * 0.31 * t) + 150.0 * std::sin (2.0 * kPiD * 0.83 * t + 1.0);
        const double gust = 0.6 + 0.4 * std::sin (2.0 * kPiD * 0.25 * t) * std::sin (2.0 * kPiD * 0.11 * t + 0.5);
        g.l.push_back ((float) (gust * fl.bp (a.bi(), c, 4.0)));
        g.r.push_back ((float) (gust * fr.bp (b.bi(), c * 1.07, 4.0)));
    }
    loopify (g.l, len); loopify (g.r, len);
    normalise (g, 0.85f);
    return g;
}

Gen surf()
{
    const int len = (int) (6.0 * kRate), fade = (int) (0.5 * kRate);
    Gen g;
    Rng a (51u), b (52u);
    float pk[2][7] {};
    OnePole ll (500.0), lr (500.0);
    for (int i = 0; i < len + fade; ++i)
    {
        const double t = std::fmod (i / kRate, 3.0) / 3.0;          // one wave every 3 s
        const double env = std::pow (std::sin (kPiD * std::pow (t, 0.6)), 2.0);
        ll.c = lr.c = 1.0 - std::exp (-2.0 * kPiD * (300.0 + 4000.0 * env) / kRate);
        g.l.push_back ((float) (env * ll.lp (pinkStep (pk[0], a.bi()))));
        g.r.push_back ((float) (env * lr.lp (pinkStep (pk[1], b.bi()))));
    }
    loopify (g.l, len); loopify (g.r, len);
    normalise (g, 0.85f);
    return g;
}

template <typename F>
Gen oneShot (double seconds, F&& f)
{
    Gen g;
    const int len = (int) (seconds * kRate);
    for (int i = 0; i < len; ++i) g.l.push_back ((float) f (i, i / kRate));
    normalise (g, 0.95f);
    return g;
}

Gen clickHit()
{
    Rng rng (61u);
    OnePole hp (3000.0);
    return oneShot (0.05, [&] (int i, double t) { return (i == 0 ? 1.0 : 0.0) + 0.5 * std::exp (-t / 0.0015) * hp.hp (rng.bi()); });
}

Gen thump()
{
    double ph = 0.0;
    return oneShot (0.3, [&] (int i, double t)
    {
        const double f = 45.0 + 115.0 * std::exp (-t / 0.03);
        ph += f / kRate;
        return std::exp (-t / 0.08) * std::sin (2.0 * kPiD * ph) + (i < 24 ? 0.3 * (1.0 - i / 24.0) : 0.0);
    });
}

Gen snap()
{
    Rng rng (71u);
    Svf bp;
    return oneShot (0.15, [&] (int, double t) { return std::min (1.0, t / 0.0005) * std::exp (-t / 0.025) * bp.bp (rng.bi(), 2200.0, 2.0); });
}

Gen tickHit()
{
    return oneShot (0.04, [] (int, double t) { return std::exp (-t / 0.003) * std::sin (2.0 * kPiD * 6000.0 * t); });
}

struct NoiseEntry { const char* category; const char* name; std::function<Gen()> make; };

const std::vector<NoiseEntry>& noiseEntries()
{
    static const std::vector<NoiseEntry> list {
        { "Analog", "AC Hum 50", [] { return hum (50.0); } },
        { "Analog", "AC Hum 60", [] { return hum (60.0); } },
        { "Analog", "Tape Hiss", tapeHiss },
        { "Analog", "Vinyl Crackle", vinyl },
        { "Organics", "Rain", rain },
        { "Organics", "Wind", wind },
        { "Organics", "Surf", surf },
        { "Attacks", "Click", clickHit },
        { "Attacks", "Thump", thump },
        { "Attacks", "Snap", snap },
        { "Attacks", "Tick", tickHit },
    };
    return list;
}

std::mutex& noiseLock() { static std::mutex m; return m; }
std::vector<std::shared_ptr<const SampleData>>& noiseCache()
{
    static std::vector<std::shared_ptr<const SampleData>> c (noiseEntries().size());
    return c;
}
} // namespace

const std::vector<std::string>& FactoryNoises::categories()
{
    static const std::vector<std::string> c { "Analog", "Organics", "Attacks" };
    return c;
}

std::vector<std::string> FactoryNoises::names (const std::string& category)
{
    std::vector<std::string> out;
    for (const auto& e : noiseEntries())
        if (category == e.category) out.push_back (e.name);
    return out;
}

std::shared_ptr<const SampleData> FactoryNoises::get (const std::string& category, const std::string& name)
{
    const auto& list = noiseEntries();
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (category != list[i].category || name != list[i].name) continue;
        std::lock_guard<std::mutex> lock (noiseLock());
        auto& slot = noiseCache()[i];
        if (! slot)
        {
            Gen g = list[i].make();
            auto s = std::make_shared<SampleData>();
            s->name = list[i].name;
            s->category = list[i].category;
            s->sampleRate = kRate;
            s->rootNote = 60;
            s->left = std::move (g.l);
            s->right = std::move (g.r);
            slot = std::move (s);
        }
        return slot;
    }
    return nullptr;
}

std::shared_ptr<const SampleData> FactoryNoises::defaultNoise() { return get ("Analog", "Tape Hiss"); }

bool FactoryNoises::isFactory (const SampleData& s)
{
    std::lock_guard<std::mutex> lock (noiseLock());
    for (const auto& c : noiseCache())
        if (c.get() == &s) return true;
    return false;
}
} // namespace tincture
