// Compressor.cpp — the Compressor FX module.
//   SINGLE: stereo-linked peak compressor (6 dB soft knee), gain computed in
//   dB and smoothed with ATTACK / RELEASE, makeup GAIN.
//   MULTIBAND: OTT-style 3 bands (LR4 crossovers at X-LOW / X-HIGH); each
//   band compresses downward above THRESH (RATIO) and upward below it
//   (BELOW, at most +24 dB, nothing below -90 dB), then gets its H/M/L gain.
//   Limit (RATIO at maximum): look-ahead brick-wall limiter, ceiling = THRESH,
//   ATTACK = look-ahead 0..10 ms, GAIN 0..36 dB after the limiter. Peaks are
//   estimated between samples too (cubic midpoint) for an approximate true
//   peak. Limit is broadband in both modes. With LAT COMP on, the
//   look-ahead is reported as latency.
// The module applies MIX itself (the dry path is delayed by the look-ahead).
//
// Slots: 0 MODE, 1 THRESH, 2 RATIO, 3 ATTACK, 4 RELEASE, 5 GAIN, 6 X-LOW,
//        7 BELOW, 8 X-HIGH, 9 H, 10 M, 11 L, 12 LAT COMP, 15 LEVEL.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
constexpr FxType T = FxType::Compressor;
constexpr float kKnee = 6.0f;
constexpr float kMaxLookMs = 10.0f;

inline float fastDb (float x) noexcept { return 6.0205999f * std::log2 (x + 1.0e-9f); }

// Static curve: gain change in dB for a level in dB.
inline float curveDb (float lvl, float thr, float slope, float upSlope) noexcept
{
    const float over = lvl - thr;
    if (over > kKnee * 0.5f) return -over * slope;
    if (over > -kKnee * 0.5f)
    {
        const float t = over + kKnee * 0.5f;
        return -slope * t * t / (2.0f * kKnee);
    }
    if (upSlope <= 0.0f || lvl < -90.0f) return 0.0f;
    return std::min (-over * upSlope, 24.0f);
}

struct Detector
{
    float gr = 0.0f;                 // current gain change, dB
    inline float tick (float peak, float thr, float slope, float up, float aA, float aR) noexcept
    {
        const float target = curveDb (fastDb (peak), thr, slope, up);
        // Moving towards less gain = attack; towards more gain = release.
        gr += (target - gr) * (target < gr ? aA : aR);
        return gr;
    }
};

class Compressor final : public FxModule
{
public:
    bool mixesItself() const noexcept override { return true; }
    int latency() const noexcept override { return reportLatency ? look : 0; }

    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        cap = (int) (kMaxLookMs * 0.001f * sr) + 4;
        for (auto& d : delay) d.init (cap + 4);
        dqVal.assign ((size_t) cap + 2, 0.0f);
        dqIdx.assign ((size_t) cap + 2, 0);
        box.assign ((size_t) cap + 2, 1.0f);
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { x1[c].clear(); x2[c].clear(); ap[c].clear(); delay[c].clear(); }
        for (auto& d : det) d = Detector();
        resetLimiter (look);
        hist[0] = hist[1] = hist[2] = 0.0f;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const bool multi = fxInfo (T).params[0].index (b.v[0]) == 1;
        const bool limit = b.v[2] >= 0.985f;
        reportLatency = limit && b.v[12] >= 0.5f;
        const float mix = b.mix;
        float grOut[3] { 0.0f, 0.0f, 0.0f };
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            float* l = L + off;
            float* r = R + off;
            const float thr = fxPlain (T, 1, v[1]);
            const float relMs = fxPlain (T, 4, v[4]);
            const float aR = 1.0f - std::exp (-1.0f / (relMs * 0.001f * sr));
            if (limit)
            {
                const int la = (int) std::lround (clamp01 (b.v[3]) * kMaxLookMs * 0.001f * sr);
                if (la != look) resetLimiter (la);
                const float makeup = dbToGain (v[5] * 36.0f);
                grOut[1] = limitBlock (l, r, m, dbToGain (thr), aR, makeup, mix);
                grOut[0] = grOut[2] = grOut[1];
                continue;
            }
            if (look != 0) resetLimiter (0);
            const float t = std::min (clamp01 (v[2]) / 0.985f, 0.999f);
            const float slope = t;                               // 1 - 1/ratio
            const float aA = 1.0f - std::exp (-1.0f / (fxPlain (T, 3, v[3]) * 0.001f * sr));
            const float makeup = dbToGain (fxPlain (T, 5, v[5]));
            if (! multi)
            {
                Detector& d = det[1];
                for (int i = 0; i < m; ++i)
                {
                    const float pk = std::max (std::abs (l[i]), std::abs (r[i]));
                    const float g = dbToGainFast (d.tick (pk, thr, slope, 0.0f, aA, aR)) * makeup;
                    l[i] += (l[i] * g - l[i]) * mix;
                    r[i] += (r[i] * g - r[i]) * mix;
                }
                grOut[1] = std::min (0.0f, det[1].gr);
                continue;
            }
            const float rb = 1.0f + 19.0f * v[7] * v[7];
            const float up = 1.0f - 1.0f / rb;
            const float f1 = fxPlain (T, 6, v[6]);
            const float f2 = std::max (fxPlain (T, 8, v[8]), f1 * 1.05f);
            c1.set (f1, 0.70710678f, sr);
            c2.set (f2, 0.70710678f, sr);
            const float gH = dbToGain (fxPlain (T, 9, v[9])) * makeup;
            const float gM = dbToGain (fxPlain (T, 10, v[10])) * makeup;
            const float gL = dbToGain (fxPlain (T, 11, v[11])) * makeup;
            for (int i = 0; i < m; ++i)
            {
                float lo[2], mid[2], hi[2];
                const float in[2] { l[i], r[i] };
                for (int c = 0; c < 2; ++c)
                {
                    float rest, lo0;
                    x1[c].split (c1, in[c], lo0, rest);
                    x2[c].split (c2, rest, mid[c], hi[c]);
                    lo[c] = ap[c].allpass (c2, lo0);
                }
                const float kH = dbToGainFast (det[0].tick (std::max (std::abs (hi[0]), std::abs (hi[1])), thr, slope, up, aA, aR)) * gH;
                const float kM = dbToGainFast (det[1].tick (std::max (std::abs (mid[0]), std::abs (mid[1])), thr, slope, up, aA, aR)) * gM;
                const float kL = dbToGainFast (det[2].tick (std::max (std::abs (lo[0]), std::abs (lo[1])), thr, slope, up, aA, aR)) * gL;
                const float ol = hi[0] * kH + mid[0] * kM + lo[0] * kL;
                const float orr = hi[1] * kH + mid[1] * kM + lo[1] * kL;
                l[i] += (ol - l[i]) * mix;
                r[i] += (orr - r[i]) * mix;
            }
            for (int k = 0; k < 3; ++k) grOut[k] = det[k].gr;
        }
        if (b.telemetry)
            for (int k = 0; k < 3; ++k) b.telemetry->gainReductionDb[k].store (grOut[k], std::memory_order_relaxed);
    }

private:
    static inline float dbToGainFast (float db) noexcept { return std::exp2 (db * 0.16609640f); }

    void resetLimiter (int la)
    {
        look = std::clamp (la, 0, cap - 2);
        head = tail = 0;
        counter = 0;
        boxPos = 0;
        boxSum = (double) (look + 1);
        std::fill (box.begin(), box.end(), 1.0f);
        env = 1.0f;
        for (auto& d : delay) d.clear();
        hist[0] = hist[1] = hist[2] = 0.0f;
    }

    // Returns the gain reduction (dB) at the end of the block.
    float limitBlock (float* l, float* r, int m, float ceil, float aR, float makeup, float mix)
    {
        const int W = look + 1;
        const int Q = (int) dqVal.size();
        float g = 1.0f;
        for (int i = 0; i < m; ++i)
        {
            // Peak incl. an inter-sample estimate (cubic midpoint of the last 4 mono-max samples).
            const float pk = std::max (std::abs (l[i]), std::abs (r[i]));
            const float mid = std::abs (-0.0625f * hist[0] + 0.5625f * hist[1] + 0.5625f * hist[2] - 0.0625f * pk);
            hist[0] = hist[1]; hist[1] = hist[2]; hist[2] = pk;
            const float p = std::max (pk, mid);
            const float req = p > ceil ? ceil / p : 1.0f;
            // Running minimum over the last W samples (monotonic deque).
            while (head != tail && dqVal[(size_t) ((tail - 1 + Q) % Q)] >= req) tail = (tail - 1 + Q) % Q;
            dqVal[(size_t) tail] = req; dqIdx[(size_t) tail] = counter; tail = (tail + 1) % Q;
            while (dqIdx[(size_t) head] <= counter - W) head = (head + 1) % Q;
            const float mn = dqVal[(size_t) head];
            ++counter;
            // Release.
            env = mn < env ? mn : env + (mn - env) * aR;
            // Box average over W keeps the attack smooth and never above the minimum.
            boxSum += (double) env - (double) box[(size_t) boxPos];
            box[(size_t) boxPos] = env;
            boxPos = boxPos + 1 >= W ? 0 : boxPos + 1;
            g = std::min ((float) (boxSum / (double) W), 1.0f);
            // Delay the audio by the look-ahead.
            delay[0].push (l[i]);
            delay[1].push (r[i]);
            const float dl = delay[0].tap (look), dr = delay[1].tap (look);
            const float wl = dl * g * makeup, wr = dr * g * makeup;
            l[i] = dl + (wl - dl) * mix;
            r[i] = dr + (wr - dr) * mix;
        }
        if (counter > (1 << 30)) { for (auto& x : dqIdx) x -= counter; counter = 0; }
        return gainToDb (g);
    }

    float sr = 48000.0f;
    SlotSmoother sm;
    SvfCoefs c1, c2;
    Lr4 x1[2], x2[2], ap[2];
    Detector det[3];
    // Limiter.
    int cap = 0, look = 0, head = 0, tail = 0, counter = 0, boxPos = 0;
    bool reportLatency = false;
    double boxSum = 1.0;
    float env = 1.0f, hist[3] {};
    std::vector<float> dqVal, box;
    std::vector<int> dqIdx;
    DelayLine delay[2];
};
} // namespace

std::unique_ptr<FxModule> makeCompressor() { return std::make_unique<Compressor>(); }
} // namespace tincture
