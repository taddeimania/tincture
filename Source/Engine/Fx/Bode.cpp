// Bode.cpp — the Bode frequency shifter FX module. A Hilbert transformer
// (two 8th-order all-pass chains, ~90 deg apart over 20 Hz..20 kHz at
// 44.1/48 kHz) and a quadrature oscillator give the up- and down-shifted
// sidebands. A delay in the feedback loop turns it into spiralling pitched
// echoes; BLUR wobbles that delay.
//
// DIR (bipolar): +1 = both channels up, -1 = both down, centre = L up / R
// down, continuous in between. WIDTH mixes in the opposite sideband (100% =
// both sidebands = ring-modulation-like). BALANCE: down (-1) .. up (+1)
// sideband fed into the delay.
//
// Slots: 0 SHIFT (% of RANGE), 1 RANGE, 2 DIR, 3 WIDTH, 4 DELAY (ms or synced),
//        5 BPM, 6 FEED, 7 BALANCE, 8 BLUR, 9 MONO INPUT, 10 RETRIG, 15 LEVEL.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
constexpr FxType T = FxType::Bode;

// Olli Niemitalo's 90-degree phase-difference network.
constexpr float kHa[4] { 0.6923878f, 0.9360654322959f, 0.9882295226860f, 0.9987488452737f };
constexpr float kHb[4] { 0.4021921162426f, 0.8561710882420f, 0.9722909545651f, 0.9952884791278f };

struct Hilbert
{
    float xa[4][2] {}, ya[4][2] {}, xb[4][2] {}, yb[4][2] {};
    float aDelay = 0.0f;
    void clear() { *this = Hilbert(); }
    // In: x. Out: in-phase i, quadrature q.
    inline void tick (float x, float& i, float& q) noexcept
    {
        float a = x, b = x;
        for (int k = 0; k < 4; ++k)
        {
            const float c = kHa[k] * kHa[k];
            const float y = c * (a + ya[k][1]) - xa[k][1];
            xa[k][1] = xa[k][0]; xa[k][0] = a;
            ya[k][1] = ya[k][0]; ya[k][0] = y;
            a = y;
            const float cb = kHb[k] * kHb[k];
            const float yb2 = cb * (b + yb[k][1]) - xb[k][1];
            xb[k][1] = xb[k][0]; xb[k][0] = b;
            yb[k][1] = yb[k][0]; yb[k][0] = yb2;
            b = yb2;
        }
        i = aDelay;          // chain A, delayed one sample
        aDelay = a;
        q = b;
    }
};

class Bode final : public FxModule
{
public:
    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        for (auto& d : line) d.init ((int) (4.2f * sr) + 8);
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { hil[c].clear(); line[c].clear(); fbk[c] = 0.0f; }
        oc = 1.0f; os = 0.0f;
        blur.phase = 0.0;
        dCur = -1.0f;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        if (b.v[10] >= 0.5f && b.noteCount != lastCount) { oc = 1.0f; os = 0.0f; }
        lastCount = b.noteCount;
        const bool mono = b.v[9] >= 0.5f;
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            float* l = L + off;
            float* r = R + off;
            const float hz = v[0] * fxPlain (T, 1, v[1]);
            const float dir = fxPlain (T, 2, v[2]), width = v[3];
            const float fb = v[6] * 0.98f, bal = fxPlain (T, 7, v[7]), blurAmt = v[8];
            // Sideband gains per channel.
            float gu[2], gd[2];
            for (int c = 0; c < 2; ++c)
            {
                const float s = clampf (2.0f * dir + (c == 0 ? 1.0f : -1.0f), -1.0f, 1.0f);
                const float u = 0.5f * (1.0f + s), d = 0.5f * (1.0f - s);
                const float norm = 1.0f / (1.0f + width);
                gu[c] = (u + width * (1.0f - u)) * norm;
                gd[c] = (d + width * (1.0f - d)) * norm;
            }
            const float bu = 0.5f * (1.0f + bal), bd = 0.5f * (1.0f - bal);
            // Delay time (+ blur wobble), ramped.
            blur.advance (0.9f, m, sr);
            float t = fxTimeSeconds (T, 4, b.v, b.ctx->bpm) * sr;
            t += blurAmt * 0.004f * sr * (0.5f + 0.35f * sine01 (blur.phase) + 0.15f * sine01 (blur.phase * 6.3));
            t = clampf (t, 1.0f, (float) line[0].capacity() - 4.0f);
            if (dCur < 0.0f) dCur = t;
            const float dStep = (t - dCur) / (float) m;
            // Oscillator rotation for this tick.
            const float w = kTwoPi * hz / sr;
            const float rc = std::cos (w), rs = std::sin (w);
            const bool useFb = fb > 0.0f;

            for (int i = 0; i < m; ++i)
            {
                const float nc = oc * rc - os * rs;
                os = oc * rs + os * rc;
                oc = nc;
                dCur += dStep;
                float in[2] { l[i], r[i] };
                if (mono) in[0] = in[1] = 0.5f * (l[i] + r[i]);
                float out[2];
                for (int c = 0; c < 2; ++c)
                {
                    float x = in[c];
                    if (useFb) x += fbk[c];
                    float ii, qq;
                    hil[c].tick (x, ii, qq);
                    const float up = ii * oc + qq * os;
                    const float dn = ii * oc - qq * os;
                    out[c] = gu[c] * up + gd[c] * dn;
                    if (useFb)
                    {
                        line[c].push (bu * up + bd * dn);
                        fbk[c] = fastTanh (fb * line[c].linear (dCur)) + kDenorm;
                    }
                }
                l[i] = out[0];
                r[i] = out[1];
            }
            // Keep the phasor on the unit circle.
            const float mag = 1.0f / std::sqrt (oc * oc + os * os);
            oc *= mag; os *= mag;
        }
    }

private:
    float sr = 48000.0f, oc = 1.0f, os = 0.0f, fbk[2] {}, dCur = -1.0f;
    uint32_t lastCount = 0;
    SlotSmoother sm;
    Hilbert hil[2];
    DelayLine line[2];
    Lfo blur;
};
} // namespace

std::unique_ptr<FxModule> makeBode() { return std::make_unique<Bode>(); }
} // namespace tincture
