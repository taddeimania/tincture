// Flanger.cpp — the Flanger FX module: a short delay swept exponentially
// (0.15 .. ~7 ms) by a triangle LFO, with feedback; MIX against the dry
// signal makes the comb. PHASE offsets the right channel's LFO.
//
// Slots: 0 RATE (Hz or synced), 1 BPM, 2 DEPTH, 3 FEEDBACK (0..95%), 4 PHASE, 15 LEVEL.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
constexpr FxType T = FxType::Flanger;

class Flanger final : public FxModule
{
public:
    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        for (auto& d : line) d.init ((int) (0.012f * sr) + 8);
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { line[c].clear(); fbk[c] = 0.0f; }
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
            const float rate = fxRateHz (T, 0, b.v, b.ctx->bpm);
            const float depth = v[2], fb = v[3] * 0.95f, phase = v[4];
            lfo.advance (rate, m, sr);
            const float minD = 0.00015f * sr;
            float end[2];
            for (int c = 0; c < 2; ++c)
            {
                const float t = 0.5f + 0.5f * tri01 (lfo.phase + (c == 1 ? phase : 0.0f));
                end[c] = minD * std::exp2 (t * depth * 5.5f);
            }
            if (! primed) { cur[0] = end[0]; cur[1] = end[1]; primed = true; }
            float* ch[2] { L + off, R + off };
            for (int c = 0; c < 2; ++c)
            {
                DelayLine& dl = line[c];
                float* x = ch[c];
                float d = cur[c];
                const float st = (end[c] - cur[c]) / (float) m;
                float f = fbk[c];
                for (int i = 0; i < m; ++i)
                {
                    d += st;
                    const float y = dl.hermite (d - 1.0f);      // read before push
                    dl.push (x[i] + fb * f);
                    f = y;
                    x[i] = y;
                }
                fbk[c] = f + kDenorm;
                cur[c] = end[c];
            }
        }
    }

private:
    float sr = 48000.0f, cur[2] {}, fbk[2] {};
    bool primed = false;
    SlotSmoother sm;
    Lfo lfo;
    DelayLine line[2];
};
} // namespace

std::unique_ptr<FxModule> makeFlanger() { return std::make_unique<Flanger>(); }
} // namespace tincture
