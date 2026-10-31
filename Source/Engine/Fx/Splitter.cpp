// Splitter.cpp — the three splitter modules. L/H and L/M/H use
// Linkwitz-Riley 4th-order crossovers (bands sum to an all-pass: flat
// magnitude); M/S encodes to mid/side and decodes back. The engine runs each
// band's module chain between split() and join().
//
// Slots: L/H: 0 SPLIT FREQ.  L/M/H: 0 SPLIT FREQ (low), 1 SPLIT FREQ (high).  M/S: none.  15 LEVEL.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;

class Splitter final : public FxSplitter
{
public:
    explicit Splitter (FxType t) : type (t) {}

    void prepare (double s, int maxBlock) override
    {
        sr = (float) s;
        sm.prepare (s);
        for (auto& b : buf) for (auto& c : b) c.assign ((size_t) maxBlock, 0.0f);
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { x1[c].clear(); x2[c].clear(); ap[c].clear(); }
        sm.reset();
    }

    void split (const float* L, const float* R, int n, const FxBlock& b) override
    {
        bands = fxInfo (type).numBands;
        if (type == FxType::SplitterMS)
        {
            float *mL = bandL (0), *mR = bandR (0), *sL = bandL (1), *sR = bandR (1);
            for (int i = 0; i < n; ++i)
            {
                const float mid = 0.5f * (L[i] + R[i]), side = 0.5f * (L[i] - R[i]);
                mL[i] = mR[i] = mid;
                sL[i] = side; sR[i] = -side;            // side carried as a stereo pair (L = S, R = -S)
            }
            return;
        }
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            float f1 = fxPlain (type, 0, v[0]);
            c1.set (f1, 0.70710678f, sr);
            if (type == FxType::SplitterLH)
            {
                float *lL = bandL (0) + off, *lR = bandR (0) + off, *hL = bandL (1) + off, *hR = bandR (1) + off;
                for (int i = 0; i < m; ++i)
                {
                    x1[0].split (c1, L[off + i], lL[i], hL[i]);
                    x1[1].split (c1, R[off + i], lR[i], hR[i]);
                }
            }
            else
            {
                const float f2 = std::max (f1, fxPlain (type, 1, v[1]));   // enforce low <= high
                c2.set (f2, 0.70710678f, sr);
                float *lL = bandL (0) + off, *lR = bandR (0) + off, *mL = bandL (1) + off, *mR = bandR (1) + off,
                      *hL = bandL (2) + off, *hR = bandR (2) + off;
                for (int i = 0; i < m; ++i)
                {
                    float lo, rest;
                    x1[0].split (c1, L[off + i], lo, rest);
                    x2[0].split (c2, rest, mL[i], hL[i]);
                    lL[i] = ap[0].allpass (c2, lo);
                    x1[1].split (c1, R[off + i], lo, rest);
                    x2[1].split (c2, rest, mR[i], hR[i]);
                    lR[i] = ap[1].allpass (c2, lo);
                }
            }
        }
    }

    void join (float* L, float* R, int n) override
    {
        if (type == FxType::SplitterMS)
        {
            const float *mL = bandL (0), *mR = bandR (0), *sL = bandL (1), *sR = bandR (1);
            for (int i = 0; i < n; ++i)
            {
                // Mid chain output (stereo) plus side chain decoded (L - R)/2 per side pair.
                const float side = 0.5f * (sL[i] - sR[i]);
                L[i] = mL[i] + side;
                R[i] = mR[i] - side;
            }
            return;
        }
        for (int i = 0; i < n; ++i) { L[i] = bandL (0)[i] + bandL (1)[i]; R[i] = bandR (0)[i] + bandR (1)[i]; }
        if (bands == 3)
            for (int i = 0; i < n; ++i) { L[i] += bandL (2)[i]; R[i] += bandR (2)[i]; }
    }

private:
    FxType type;
    int bands = 2;
    float sr = 48000.0f;
    SlotSmoother sm;
    SvfCoefs c1, c2;
    Lr4 x1[2], x2[2], ap[2];
};
} // namespace

std::unique_ptr<FxModule> makeSplitter (FxType t) { return std::make_unique<Splitter> (t); }
} // namespace tincture
