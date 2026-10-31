// Hyper.cpp — the Hyper/Dimension FX module.
//   HYPER: up to 7 detuned "unison" copies of the (mono-summed) input, each a
//   delay tap wobbled by its own LFO (rates spread around RATE), spread
//   across the stereo field; its wet/dry is the unit's MIX ("HYPER MIX").
//   RETRIG zeroes every voice's offset on a note-on (they then drift apart).
//   DIMENSION: four (eight with SIZE) delay taps with slow amplitude
//   modulation, summed with alternating signs and added out of phase to L
//   and R (pseudo-stereo widening without hollowing the centre).
//
// Slots: 0 RATE, 1 UNISON (0..7, 0 = Hyper off), 2 DETUNE, 3 RETRIG,
//        4 DIM SIZE, 5 DIM MIX, 15 LEVEL.  Unit MIX = HYPER MIX.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
constexpr FxType T = FxType::Hyper;
constexpr int kVoices = 7;
constexpr float kDimMs[8] { 7.3f, 11.9f, 17.1f, 23.7f, 13.1f, 19.3f, 29.9f, 37.3f };

class Hyper final : public FxModule
{
public:
    bool mixesItself() const noexcept override { return true; }

    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        hyperLine.init ((int) (0.03f * sr) + 8);
        dimLine.init ((int) (0.045f * sr) + 8);
    }
    void reset() override
    {
        hyperLine.clear();
        dimLine.clear();
        for (int i = 0; i < kVoices; ++i) { vl[i].phase = 0.37 * i + 0.11 * i * i; cur[i] = -1.0f; }
        for (int i = 0; i < 4; ++i) am[i].phase = 0.25 * i;
        lastCount = 0;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const int voices = fxInfo (T).params[1].index (b.v[1]);
        const bool retrig = b.v[3] >= 0.5f;
        if (retrig && b.noteCount != lastCount)
            for (int i = 0; i < kVoices; ++i) vl[i].phase = 0.0;
        lastCount = b.noteCount;
        const float hmix = b.mix;
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            float* l = L + off;
            float* r = R + off;
            const float rate = fxPlain (T, 0, v[0]);
            const float depth = v[2] * 0.003f * sr;              // up to +-3 ms
            const float base = 0.012f * sr;
            const float size = v[4], dmix = v[5];

            // Hyper voice delays (ramped across the tick) and pan gains.
            float end[kVoices], step[kVoices], gl[kVoices], gr[kVoices];
            const float norm = voices > 0 ? 1.0f / std::sqrt ((float) voices) : 0.0f;
            for (int i = 0; i < voices; ++i)
            {
                const float spreadI = voices > 1 ? (float) i / (float) (voices - 1) * 2.0f - 1.0f : 0.0f;
                vl[i].advance (rate * (1.0f + 0.23f * spreadI), m, sr);
                end[i] = base + depth * sine01 (vl[i].phase);
                if (cur[i] < 0.0f) cur[i] = end[i];
                step[i] = (end[i] - cur[i]) / (float) m;
                panGains (spreadI * 0.9f, gl[i], gr[i]);
                gl[i] *= norm * 0.7071f; gr[i] *= norm * 0.7071f;
            }
            for (int i = 0; i < m; ++i)
            {
                hyperLine.push (0.5f * (l[i] + r[i]));
                if (voices == 0) continue;
                float wl = 0.0f, wr = 0.0f;
                for (int k = 0; k < voices; ++k)
                {
                    cur[k] += step[k];
                    const float y = hyperLine.linear (cur[k]);
                    wl += y * gl[k];
                    wr += y * gr[k];
                }
                l[i] += (wl - l[i]) * hmix;
                r[i] += (wr - r[i]) * hmix;
            }
            for (int k = 0; k < voices; ++k) cur[k] = end[k];

            // Dimension.
            if (dmix > 1.0e-4f)
            {
                float a[8];
                for (int k = 0; k < 4; ++k)
                {
                    am[k].advance (0.11f + 0.07f * (float) k, m, sr);
                    a[k] = 0.6f + 0.4f * sine01 (am[k].phase);
                    a[k + 4] = size * (0.6f + 0.4f * sine01 (am[k].phase + 0.5));
                }
                int d[8];
                const float scale = 0.6f + 0.4f * size;
                for (int k = 0; k < 8; ++k) d[k] = (int) (kDimMs[k] * scale * 0.001f * sr);
                for (int i = 0; i < m; ++i)
                {
                    dimLine.push (0.5f * (l[i] + r[i]));
                    float s = 0.0f;
                    for (int k = 0; k < 8; ++k) s += ((k & 1) ? -a[k] : a[k]) * dimLine.tap (d[k]);
                    s *= 0.35f;
                    l[i] += s * dmix;
                    r[i] -= s * dmix;
                }
            }
        }
    }

private:
    float sr = 48000.0f, cur[kVoices] {};
    uint32_t lastCount = 0;
    SlotSmoother sm;
    Lfo vl[kVoices], am[4];
    DelayLine hyperLine, dimLine;
};
} // namespace

std::unique_ptr<FxModule> makeHyper() { return std::make_unique<Hyper>(); }
} // namespace tincture
