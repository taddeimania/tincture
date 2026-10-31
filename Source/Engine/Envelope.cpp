// Envelope.cpp — ENV 1-4 (see Envelope.h).
#include "Envelope.h"

#include <cmath>

namespace tincture
{
namespace
{
// Rising segment shape for a "drawn" curve (> 0 bows up).
inline float riseShape (float t, float c) noexcept { return Curve::bend (t, c); }
// Falling segment shape, 0..1 of the drop: a curve that bows up visually
// means a slow start of the fall, so the bend is mirrored.
inline float fallShape (float t, float c) noexcept { return Curve::bend (t, -c); }
}

float envSnapSecondsToBeats (float seconds, double bpm)
{
    const double beats = seconds * (bpm > 1.0 ? bpm : 1.0) / 60.0;
    if (beats <= 0.0) return 0.0f;
    // Candidate subdivisions: 1/64 .. 8 bars, straight, dotted and triplet.
    double best = 0.0625, bestErr = 1.0e9;
    for (double b = 0.0625; b <= 32.0; b *= 2.0)
        for (double m : { 1.0, 1.5, 2.0 / 3.0 })
        {
            const double v = b * m;
            if (v > 32.0) continue;
            const double err = std::abs (std::log (v / beats));
            if (err < bestErr) { bestErr = err; best = v; }
        }
    return (float) best;
}

EnvSettings EnvSettings::read (const ParamView& p, const EnvIds& id, double bpm) noexcept
{
    EnvSettings s;
    s.attack  = p (id.attack);
    s.hold    = p (id.hold);
    s.decay   = p (id.decay);
    s.release = p (id.release);
    if (p.on (id.bpm))
    {
        const float k = (float) (60.0 / (bpm > 1.0 ? bpm : 1.0));
        s.attack *= k; s.hold *= k; s.decay *= k; s.release *= k;
    }
    s.sustain = envSustainGain (p (id.sustain));
    s.attackCurve  = p (id.attackCurve);
    s.decayCurve   = p (id.decayCurve);
    s.releaseCurve = p (id.releaseCurve);
    s.legatoInvert = p.on (id.legatoInvert);
    return s;
}

float Envelope::segment (const EnvSettings& s) const noexcept
{
    switch (stage)
    {
        case Stage::Attack:  return from + (1.0f - from) * riseShape (frac, s.attackCurve);
        case Stage::Hold:    return 1.0f;
        case Stage::Decay:   return 1.0f + (s.sustain - 1.0f) * fallShape (frac, s.decayCurve);
        case Stage::Sustain: return s.sustain;
        case Stage::Release: return from * (1.0f - fallShape (frac, s.releaseCurve));
        default:             return 0.0f;
    }
}

EnvTick Envelope::process (const EnvSettings& s, int n, float sr) noexcept
{
    EnvTick out;
    out.start = level;
    float remaining = (float) n;

    while (remaining > 0.0f)
    {
        float seconds;
        switch (stage)
        {
            case Stage::Idle:    level = 0.0f; remaining = 0.0f; continue;
            case Stage::Sustain: level = s.sustain; remaining = 0.0f; continue;
            case Stage::Attack:  seconds = s.attack; break;
            case Stage::Hold:    seconds = s.hold; break;
            case Stage::Decay:   seconds = s.decay; break;
            default:             seconds = s.release; break;
        }

        const float len = seconds * sr;                 // stage length in samples
        const float need = (1.0f - frac) * len;         // samples left in this stage
        if (need <= remaining)
        {
            remaining -= need;
            frac = 0.0f;
            switch (stage)
            {
                case Stage::Attack:  level = 1.0f; stage = Stage::Hold; break;
                case Stage::Hold:    level = 1.0f; stage = Stage::Decay; break;
                case Stage::Decay:   level = s.sustain; stage = Stage::Sustain; break;
                default:             level = 0.0f; stage = Stage::Idle; break;
            }
        }
        else
        {
            frac += remaining / len;
            remaining = 0.0f;
            level = segment (s);
        }
    }

    out.end = level;
    return out;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------
float envShapeLength (const EnvSettings& s, float releaseAt) noexcept
{
    const float off = releaseAt >= 0.0f ? releaseAt : s.attack + s.hold + s.decay;
    return off + s.release;
}

float envShapeAt (const EnvSettings& s, float t, float releaseAt) noexcept
{
    const float off = releaseAt >= 0.0f ? releaseAt : s.attack + s.hold + s.decay;
    auto held = [&s] (float u) noexcept
    {
        if (u < s.attack) return riseShape (u / s.attack, s.attackCurve);
        u -= s.attack;
        if (u < s.hold) return 1.0f;
        u -= s.hold;
        if (u < s.decay) return 1.0f + (s.sustain - 1.0f) * fallShape (u / s.decay, s.decayCurve);
        return s.sustain;
    };
    if (t < 0.0f) return 0.0f;
    if (t < off) return held (t);
    const float r = t - off;
    if (r >= s.release) return 0.0f;
    return held (off) * (1.0f - fallShape (r / s.release, s.releaseCurve));
}

void envShapePolyline (const EnvSettings& s, int count, std::vector<float>& xy, float releaseAt)
{
    xy.clear();
    if (count < 2) count = 2;
    const float len = envShapeLength (s, releaseAt);
    xy.reserve ((size_t) count * 2);
    for (int i = 0; i < count; ++i)
    {
        const float t = len * (float) i / (float) (count - 1);
        xy.push_back (t);
        xy.push_back (envShapeAt (s, t, releaseAt));
    }
}
} // namespace tincture
