// Chorus.cpp — the Chorus FX module: four modulated taps (two per side,
// base delays DELAY 1 / DELAY 2, LFO phases 0/180 deg on the left and
// 90/270 on the right), feedback, and a LPF/HPF on the wet signal.
//
// Slots: 0 RATE (Hz or synced), 1 BPM, 2 DELAY 1, 3 DELAY 2, 4 DEPTH,
//        5 FEEDBACK (0..95%), 6 FILTER FREQ, 7 FILTER TYPE (LPF/HPF), 15 LEVEL.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
constexpr FxType T = FxType::Chorus;
constexpr float kModMs = 6.0f;          // DEPTH 100% sweeps each tap over 0..6 ms

class Chorus final : public FxModule
{
public:
    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        for (auto& d : line) d.init ((int) (0.06f * sr) + 8);
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { line[c].clear(); flt[c].clear(); fbk[c] = 0.0f; }
        lfo.phase = 0.0;
        primed = false;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const bool hpf = fxInfo (T).params[7].index (b.v[7]) == 1;
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            const float rate = fxRateHz (T, 0, b.v, b.ctx->bpm);
            const float msToS = sr * 0.001f;
            const float d1 = fxPlain (T, 2, v[2]) * msToS, d2 = fxPlain (T, 3, v[3]) * msToS;
            const float depth = v[4] * kModMs * msToS;
            const float fb = v[5] * 0.95f;

            // Tap delays at the start / end of the tick, ramped across it.
            float end[4];
            lfo.advance (rate, m, sr);
            static const double ph[4] { 0.0, 0.5, 0.25, 0.75 };
            for (int t = 0; t < 4; ++t)
                end[t] = ((t & 1) ? d2 : d1) + depth * (0.5f + 0.5f * sine01 (lfo.phase + ph[t]));
            if (! primed) { std::copy (end, end + 4, cur); primed = true; }
            float step[4];
            for (int t = 0; t < 4; ++t) step[t] = (end[t] - cur[t]) / (float) m;

            float* ch[2] { L + off, R + off };
            for (int c = 0; c < 2; ++c)
            {
                DelayLine& dl = line[c];
                float* x = ch[c];
                float da = cur[c * 2], db = cur[c * 2 + 1];
                const float sa = step[c * 2], sb = step[c * 2 + 1];
                float f = fbk[c];
                for (int i = 0; i < m; ++i)
                {
                    dl.push (x[i] + f * fb);
                    da += sa; db += sb;
                    const float y = 0.5f * (dl.hermite (std::max (da, 1.0f)) + dl.hermite (std::max (db, 1.0f)));
                    f = y;
                    x[i] = y;
                }
                fbk[c] = f + kDenorm;
            }
            std::copy (end, end + 4, cur);

            // Wet filter.
            const float fhz = fxPlain (T, 6, v[6]);
            if (hpf ? fhz > 20.5f : fhz < 19500.0f)
            {
                fc.set (fhz, 0.7071f, sr);
                for (int c = 0; c < 2; ++c)
                {
                    float* x = ch[c];
                    Svf& s = flt[c];
                    if (hpf) for (int i = 0; i < m; ++i) x[i] = s.hp (fc, x[i]);
                    else     for (int i = 0; i < m; ++i) x[i] = s.lp (fc, x[i]);
                }
            }
        }
    }

private:
    float sr = 48000.0f, cur[4] {}, fbk[2] {};
    bool primed = false;
    SlotSmoother sm;
    Lfo lfo;
    DelayLine line[2];
    SvfCoefs fc;
    Svf flt[2];
};
} // namespace

std::unique_ptr<FxModule> makeChorus() { return std::make_unique<Chorus>(); }
} // namespace tincture
