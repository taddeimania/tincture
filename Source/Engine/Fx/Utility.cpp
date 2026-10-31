// Utility.cpp — the Utility FX module: polarity, LPF/HPF (12 dB/oct),
// mono bass (LR4 split), stereo width and balance.
//
// Slots: 0 INV L, 1 INV R, 2 LPF, 3 HPF, 4 MONO BASS, 5 FREQ, 6 WIDTH, 7 PAN, 15 LEVEL.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;

class Utility final : public FxModule
{
public:
    void prepare (double s, int) override { sr = (float) s; sm.prepare (s); }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { lp[c].clear(); hp[c].clear(); xo[c].clear(); }
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const FxType T = FxType::Utility;
        const float invL = b.v[0] >= 0.5f ? -1.0f : 1.0f;
        const float invR = b.v[1] >= 0.5f ? -1.0f : 1.0f;
        const bool mono = b.v[4] >= 0.5f;
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            float* l = L + off;
            float* r = R + off;
            const float* v = sm.tick (b.v);
            const float lpHz = fxPlain (T, 2, v[2]), hpHz = fxPlain (T, 3, v[3]);
            const float width = fxPlain (T, 6, v[6]), pan = fxPlain (T, 7, v[7]);
            const float gl = invL * (pan > 0.0f ? 1.0f - pan : 1.0f);
            const float gr = invR * (pan < 0.0f ? 1.0f + pan : 1.0f);

            for (int i = 0; i < m; ++i) { l[i] *= gl; r[i] *= gr; }
            if (lpHz < 19500.0f)
            {
                lpc.set (lpHz, 0.7071f, sr);
                for (int i = 0; i < m; ++i) { l[i] = lp[0].lp (lpc, l[i]); r[i] = lp[1].lp (lpc, r[i]); }
            }
            if (hpHz > 20.5f)
            {
                hpc.set (hpHz, 0.7071f, sr);
                for (int i = 0; i < m; ++i) { l[i] = hp[0].hp (hpc, l[i]); r[i] = hp[1].hp (hpc, r[i]); }
            }
            if (mono)
            {
                xoc.set (fxPlain (T, 5, v[5]), 0.70710678f, sr);
                for (int i = 0; i < m; ++i)
                {
                    float lo0, hi0, lo1, hi1;
                    xo[0].split (xoc, l[i], lo0, hi0);
                    xo[1].split (xoc, r[i], lo1, hi1);
                    const float lo = 0.5f * (lo0 + lo1);
                    const float mid = 0.5f * (hi0 + hi1), side = 0.5f * (hi0 - hi1) * width;
                    l[i] = lo + mid + side;
                    r[i] = lo + mid - side;
                }
            }
            else if (std::abs (width - 1.0f) > 1.0e-4f)
            {
                for (int i = 0; i < m; ++i)
                {
                    const float mid = 0.5f * (l[i] + r[i]), side = 0.5f * (l[i] - r[i]) * width;
                    l[i] = mid + side;
                    r[i] = mid - side;
                }
            }
        }
    }

private:
    float sr = 48000.0f;
    SlotSmoother sm;
    SvfCoefs lpc, hpc, xoc;
    Svf lp[2], hp[2];
    Lr4 xo[2];
};
} // namespace

std::unique_ptr<FxModule> makeUtility() { return std::make_unique<Utility>(); }
} // namespace tincture
