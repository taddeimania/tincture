// Envelope.h — ENV 1-4: the per-voice AHDSR envelope generators.
//
// Stages: Attack (from the current level up to 1), Hold (at 1), Decay (down
// to the sustain level), Sustain (until note-off), Release (from the current
// level down to 0), then Idle. Times are read once per tick, so they can be
// modulated; stage progress is kept as a 0..1 fraction so a time change never
// makes the level jump. Stage boundaries are handled inside the tick to the
// exact sample, so timing is sample accurate whatever the tick size.
//
// Curves are -1..1 and describe how the segment is drawn: > 0 bows the
// segment upwards, < 0 downwards (so the default decay/release curve of -0.6
// gives the usual fast-start "exponential" fall, and the attack's +0.2 a
// slightly convex rise).
//
// BPM mode: the ATK/HOLD/DEC/REL plain values are read as beats (quarter
// notes) instead of seconds and converted at the host tempo every tick.
//
// ENV 1 is the amp envelope: the voice is finished when ENV 1 is.
#pragma once

#include "Module.h"
#include "Model/Curve.h"

#include <vector>

namespace tincture
{
// Sustain is registered as -60..0 dB; the bottom of the travel means -inf.
inline float envSustainGain (float db) noexcept { return db <= -59.95f ? 0.0f : dbToGain (db); }

// BPM mode helpers. MS -> BPM switching converts each time to the nearest
// note subdivision (1/64 .. 8 bars, straight, dotted or triplet) in beats.
inline double envBeatsToSeconds (double beats, double bpm) noexcept { return beats * 60.0 / (bpm > 1.0 ? bpm : 1.0); }
float envSnapSecondsToBeats (float seconds, double bpm);

// Everything the envelope needs for one tick (read from the voice's ParamView).
struct EnvSettings
{
    float attack = 0.001f, hold = 0.0f, decay = 1.0f, release = 0.015f;   // seconds
    float sustain = 1.0f;                                                   // gain 0..1
    float attackCurve = 0.2f, decayCurve = -0.6f, releaseCurve = -0.6f;    // -1..1
    bool legatoInvert = false;

    static EnvSettings read (const ParamView& p, const EnvIds& id, double bpm) noexcept;
};

struct EnvTick
{
    float start = 0.0f, end = 0.0f;    // level at the first and after the last sample of the tick
};

class Envelope
{
public:
    enum class Stage : uint8_t { Idle, Attack, Hold, Decay, Sustain, Release };

    void reset() noexcept { stage = Stage::Idle; level = 0.0f; frac = 0.0f; }

    // Starts the attack from the current level (0 for an idle voice; a
    // stolen voice rises from where it was, without a click).
    void trigger() noexcept { stage = Stage::Attack; frac = 0.0f; from = level; }

    // A note-on that reaches a sounding voice (mono re-pitch, or a stolen
    // voice): `legato` is NoteInfo::legato (the core's "don't retrigger"
    // decision from the global LEGATO switch). Legato Inverted does the
    // opposite. Idle or released envelopes always restart.
    static bool shouldRetrigger (bool legato, bool legatoInverted) noexcept { return legato == legatoInverted; }
    void noteOn (bool legato, bool legatoInverted) noexcept
    {
        if (stage == Stage::Idle || stage == Stage::Release || shouldRetrigger (legato, legatoInverted))
            trigger();
    }

    void noteOff() noexcept
    {
        if (stage != Stage::Idle && stage != Stage::Release) { stage = Stage::Release; frac = 0.0f; from = level; }
    }

    // Advances n samples. Returns the level at the start and end of the tick.
    EnvTick process (const EnvSettings& s, int n, float sampleRate) noexcept;

    float value() const noexcept { return level; }
    float progress() const noexcept { return frac; }     // 0..1 through the current stage
    Stage currentStage() const noexcept { return stage; }
    bool released() const noexcept { return stage == Stage::Release || stage == Stage::Idle; }
    bool finished() const noexcept { return stage == Stage::Idle; }

private:
    float segment (const EnvSettings& s) const noexcept;

    Stage stage = Stage::Idle;
    float level = 0.0f;
    float frac = 0.0f;      // progress through the current stage, 0..1
    float from = 0.0f;      // start level of the attack or release segment
};

// ---------------------------------------------------------------------------
// Drawing: the envelope's shape as a pure function of time (for the graph).
// `releaseAt` is the note-off time in seconds; pass a negative value to draw
// with the note-off right after the decay (the usual graph).
// ---------------------------------------------------------------------------
float envShapeAt (const EnvSettings& s, float tSeconds, float releaseAt = -1.0f) noexcept;
float envShapeLength (const EnvSettings& s, float releaseAt = -1.0f) noexcept;    // end of release
// `count` (time, level) pairs over 0..envShapeLength, interleaved.
void envShapePolyline (const EnvSettings& s, int count, std::vector<float>& xy, float releaseAt = -1.0f);
} // namespace tincture
