// Phaser.cpp — the Phaser FX module: 1..12 first-order all-pass stages per
// channel swept exponentially (+-3 octaves at DEPTH 100%) around FREQ;
// DEPTH 2 spreads the stages' frequencies (up to +-2 octaves across the
// chain); feedback around the chain; PHASE offsets the right LFO. MIX with
// the dry signal makes the notches. Coefficients ramp across each tick.
//
// Slots: 0 RATE (Hz or synced), 1 BPM, 2 POLES (1..12), 3 DEPTH, 4 DEPTH 2,
//        5 FREQ, 6 FEEDBACK (0..95%), 7 PHASE, 15 LEVEL.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
constexpr FxType T = FxType::Phaser;
constexpr int kMaxPoles = 12;

class Phaser final : public FxModule
{
public:
    void prepare (double s, int) override { sr = (float) s; sm.prepare (s); }
    void reset() override
    {
        for (int c = 0; c < 2; ++c)
        {
            for (int k = 0; k < kMaxPoles; ++k) st[c][k] = 0.0f;
            fbk[c] = 0.0f;
        }
        lfo.phase = 0.0;
        primed = false;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            const int poles = 1 + fxInfo (T).params[2].index (b.v[2]);
            const float rate = fxRateHz (T, 0, b.v, b.ctx->bpm);
            const float depth = v[3], spread = v[4], f0 = fxPlain (T, 5, v[5]);
            const float fb = v[6] * 0.95f, phase = v[7];
            lfo.advance (rate, m, sr);
            float end[2][kMaxPoles];
            for (int c = 0; c < 2; ++c)
            {
                const float mod = sine01 (lfo.phase + (c == 1 ? phase : 0.0f)) * depth * 3.0f;
                for (int k = 0; k < poles; ++k)
                {
                    const float pos = poles > 1 ? (float) k / (float) (poles - 1) * 2.0f - 1.0f : 0.0f;
                    const float f = clampf (f0 * std::exp2 (mod + spread * 2.0f * pos), 10.0f, sr * 0.45f);
                    const float t = std::tan (kPi * f / sr);
                    end[c][k] = (t - 1.0f) / (t + 1.0f);
                }
            }
            if (! primed || poles != lastPoles)
            {
                for (int c = 0; c < 2; ++c) for (int k = 0; k < kMaxPoles; ++k) a[c][k] = end[c][k];
                primed = true;
                lastPoles = poles;
            }
            const float inv = 1.0f / (float) m;
            float* ch[2] { L + off, R + off };
            for (int c = 0; c < 2; ++c)
            {
                float da[kMaxPoles];
                for (int k = 0; k < poles; ++k) da[k] = (end[c][k] - a[c][k]) * inv;
                float* x = ch[c];
                float* s = st[c];
                float* ac = a[c];
                float f = fbk[c];
                for (int i = 0; i < m; ++i)
                {
                    float y = x[i] + fb * f;
                    for (int k = 0; k < poles; ++k)
                    {
                        ac[k] += da[k];
                        const float o = ac[k] * y + s[k];
                        s[k] = y - ac[k] * o;
                        y = o;
                    }
                    f = y;
                    x[i] = y;
                }
                fbk[c] = f + kDenorm;
                for (int k = 0; k < poles; ++k) ac[k] = end[c][k];
            }
        }
    }

private:
    float sr = 48000.0f;
    float st[2][kMaxPoles] {}, a[2][kMaxPoles] {}, fbk[2] {};
    int lastPoles = 0;
    bool primed = false;
    SlotSmoother sm;
    Lfo lfo;
};
} // namespace

std::unique_ptr<FxModule> makePhaser() { return std::make_unique<Phaser>(); }
} // namespace tincture
