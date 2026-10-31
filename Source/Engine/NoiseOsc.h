// NoiseOsc.h — the NOISE oscillator: real-time colour noises (White, Pink,
// Brown, Geiger) with STEREO decorrelation and a tilt FILTER, or playback of
// a noise sample (colour index 0 "Sample") with 4-point Hermite
// interpolation, loop / one-shot, START (live, so automating it scratches),
// per-note RAND start, PITCH (50% = original rate, +-48 semitones), FINE
// and optional key tracking. Also the generated factory noise library.
#pragma once

#include "Module.h"
#include "SampleData.h"

#include <memory>
#include <string>
#include <vector>

namespace tincture
{
struct NoiseOscData
{
    std::shared_ptr<const SampleData> sample;   // played when colour = 0 ("Sample"); null = silent

    // Factory noises are stored by reference (category + name), others via SampleData::save.
    void save (Node& n) const;
    void load (const Node& n);
};

class NoiseOsc
{
public:
    enum Colour { SampleMode = 0, White, Pink, Brown, Geiger };

    void prepare (const Context& ctx);
    void reset() noexcept;
    void noteOn (const NoteInfo& note, const OscTick& tick, const NoiseIds& ids, const NoiseOscData& data);
    // Writes tick.n samples to outL/outR (and outMono if set); activeUnison = 1.
    void render (OscTick& tick, const NoiseIds& ids, const NoiseOscData& data);

    // Playback position 0..1 in the sample (UI).
    float currentPosition() const noexcept { return uiPos; }

private:
    float sampleRate = 48000.0f;
    Rng rngL { 0x1234567u }, rngR { 0x89ABCDEu };
    // Colour state per channel: Pink (Kellet, 7 poles), Brown, Geiger click, tilt filter.
    float pink[2][7] {};
    float brown[2] {};
    float click[2] {};
    float tiltZ[2] {};
    // Sample playback.
    double pos = 0.0;                   // frames played since note-on (before START offset)
    float randStart = 0.0f;             // per note, fraction of the file
    float lastRate = -1.0f;
    float lastStart = -1.0f;
    bool finished = false;
    float uiPos = 0.0f;
    float gL = 0.0f, gR = 0.0f;
    bool gainsValid = false;
    alignas (32) float bufL[kTick] {};
    alignas (32) float bufR[kTick] {};
};

// ---------------------------------------------------------------------------
// Factory noises: procedurally generated, in folders ("Analog", "Organics",
// "Attacks"). Built on first request (message thread), cached, thread-safe.
// ---------------------------------------------------------------------------
struct FactoryNoises
{
    static const std::vector<std::string>& categories();
    static std::vector<std::string> names (const std::string& category);
    static std::shared_ptr<const SampleData> get (const std::string& category, const std::string& name);
    static std::shared_ptr<const SampleData> defaultNoise();      // Analog / "Tape Hiss"
    static bool isFactory (const SampleData& s);
};
} // namespace tincture
