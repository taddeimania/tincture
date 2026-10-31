// Equalizer.cpp — the two-band Equalizer FX module (RBJ biquads).
//
// Slots: 0 LOW TYPE (Shelf/Peak/High Pass), 1 FREQ, 2 Q, 3 GAIN,
//        4 HIGH TYPE (Shelf/Peak/Low Pass), 5 FREQ, 6 Q, 7 GAIN, 15 LEVEL. No MIX.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
constexpr FxType T = FxType::Equalizer;

// Coefficients of both bands from (smoothed) slot values. Returns false for
// a band that is flat (shelf/peak at 0 dB) so it can be skipped.
bool bandCoefs (const float* v, int band, float sr, BiquadCoefs& c)
{
    const int o = band * 4;
    const int type = fxInfo (T).params[(size_t) o].index (v[o]);
    const float hz = fxPlain (T, o + 1, v[o + 1]);
    const float q = fxPlain (T, o + 2, v[o + 2]);
    const float db = fxPlain (T, o + 3, v[o + 3]);
    if (type == 2)
    {
        if (band == 0) c.highPass (hz, q, sr); else c.lowPass (hz, q, sr);
        return true;
    }
    if (type == 1) c.peak (hz, q, db, sr);
    else if (band == 0) c.lowShelf (hz, q, db, sr);
    else c.highShelf (hz, q, db, sr);
    return std::abs (db) > 1.0e-3f;
}

class Equalizer final : public FxModule
{
public:
    void prepare (double s, int) override { sr = (float) s; sm.prepare (s); }
    void reset() override
    {
        for (auto& b : bq) for (auto& c : b) c.clear();
        sm.reset();
    }
    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            float* l = L + off;
            float* r = R + off;
            const float* v = sm.tick (b.v);
            for (int band = 0; band < 2; ++band)
            {
                BiquadCoefs c;
                if (! bandCoefs (v, band, sr, c)) continue;
                Biquad& fl = bq[band][0];
                Biquad& fr = bq[band][1];
                for (int i = 0; i < m; ++i)
                {
                    l[i] = fl.tick (c, l[i]);
                    r[i] = fr.tick (c, r[i]);
                }
            }
        }
    }

private:
    float sr = 48000.0f;
    SlotSmoother sm;
    Biquad bq[2][2];
};
} // namespace

std::unique_ptr<FxModule> makeEqualizer() { return std::make_unique<Equalizer>(); }

float equalizerMagnitudeDb (const float* v, float hz, double sr)
{
    float db = 0.0f;
    for (int band = 0; band < 2; ++band)
    {
        BiquadCoefs c;
        if (bandCoefs (v, band, (float) sr, c)) db += c.magnitudeDb (hz, (float) sr);
    }
    return db;
}
} // namespace tincture
