// Delay.cpp — the Delay FX module: Normal (independent L/R), Ping-Pong
// (mono in, L and R feed each other) and Tap->Delay (mono: one tap at the L
// time, then a feedback delay at the R time). Delay times glide when changed
// (tape-like). A band-pass (HP + LP, 12 dB each) around FREQ colours every
// repeat; Q = width in octaves, max = widest = least filtering. High Quality
// switches the read interpolation from linear to cubic.
//
// Slots: 0 MODE, 1 HQ, 2 BPM (else ms), 3 LINK, 4 L TIME, 5 L SCALAR (0..2),
//        6 R TIME, 7 R SCALAR, 8 FEEDBACK, 9 FREQ, 10 Q (width), 15 LEVEL.
// Time limit: 20 s per line (longer synced values are clamped).
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
using namespace fxchoices;
constexpr FxType T = FxType::Delay;
constexpr float kMaxSeconds = 20.0f;

// Band edges of the feedback filter: FREQ / 2^(w/2) .. FREQ * 2^(w/2), w = 0.5..10 octaves.
void bandEdges (const float* v, float& hp, float& lp)
{
    const float f = fxPlain (T, 9, v[9]);
    const float w = 0.5f + 9.5f * clamp01 (v[10]);
    hp = f * std::exp2 (-0.5f * w);
    lp = f * std::exp2 (0.5f * w);
}

class Delay final : public FxModule
{
public:
    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        for (auto& d : line) d.init ((int) (kMaxSeconds * sr) + 8);
        written = 0;
        glideA = 1.0f - std::exp (-(float) kTick / (0.06f * sr));
    }
    // The 20 s lines aren't zeroed here (that would be a CPU spike when a
    // module is bypassed): reads beyond what was written since the reset
    // return silence instead.
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { hp[c].clear(); lp[c].clear(); }
        written = 0;
        dCur[0] = dCur[1] = -1.0f;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const auto& P = fxInfo (T).params;
        const int mode = P[0].index (b.v[0]);
        const bool hq = b.v[1] >= 0.5f, link = b.v[3] >= 0.5f;
        const float maxD = (float) line[0].capacity() - 4.0f;
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            // Times: base (ms or synced; discrete values aren't smoothed) x scalar.
            const float tl = fxTimeSeconds (T, 4, b.v, b.ctx->bpm) * fxPlain (T, 5, v[5]);
            const float tr = link ? tl : fxTimeSeconds (T, 6, b.v, b.ctx->bpm) * fxPlain (T, 7, v[7]);
            const float target[2] { clampf (tl * sr, 1.0f, maxD), clampf (tr * sr, 1.0f, maxD) };
            float start[2], step[2];
            for (int c = 0; c < 2; ++c)
            {
                if (dCur[c] < 0.0f) dCur[c] = target[c];
                start[c] = dCur[c];
                // Glide; jump when far off (avoids minute-long sweeps).
                float next = dCur[c] + (target[c] - dCur[c]) * glideA;
                if (std::abs (target[c] - dCur[c]) > sr * 0.5f || std::abs (target[c] - next) < 0.01f) next = target[c];
                dCur[c] = next;
                step[c] = (next - start[c]) / (float) m;
            }
            const float fb = std::min (v[8], 1.0f);
            float hz, lz;
            bandEdges (v, hz, lz);
            const bool useHp = hz > 20.0f, useLp = lz < 19000.0f;
            hpc.set (hz, 0.7071f, sr);
            lpc.set (std::min (lz, sr * 0.45f), 0.7071f, sr);
            float* l = L + off;
            float* r = R + off;
            if (hq) run<true> (mode, l, r, m, start, step, fb, useHp, useLp);
            else    run<false> (mode, l, r, m, start, step, fb, useHp, useLp);
            written = std::min (written + m, line[0].capacity());
        }
    }

private:
    inline float filt (int c, float x, bool useHp, bool useLp) noexcept
    {
        if (useHp) x = hp[c].hp (hpc, x);
        if (useLp) x = lp[c].lp (lpc, x);
        return x;
    }

    template <bool HQ>
    inline float rd (int c, float d) const noexcept
    {
        // Read before push: a delay of d samples sits at position d - 1.
        if (d + 3.0f >= (float) written) return 0.0f;
        return HQ ? line[c].hermite (std::max (d - 1.0f, 1.0f)) : line[c].linear (std::max (d - 1.0f, 0.0f));
    }

    template <bool HQ>
    void run (int mode, float* l, float* r, int m, const float* start, const float* step, float fb, bool useHp, bool useLp)
    {
        float dl = start[0], dr = start[1];
        switch (mode)
        {
            case PingPong:
                for (int i = 0; i < m; ++i)
                {
                    dl += step[0]; dr += step[1];
                    const float yl = rd<HQ> (0, dl), yr = rd<HQ> (1, dr);
                    const float mono = 0.5f * (l[i] + r[i]);
                    line[0].push (filt (0, mono + fb * yr, useHp, useLp) + kDenorm);
                    line[1].push (filt (1, yl, useHp, useLp) + kDenorm);
                    l[i] = yl; r[i] = yr;
                }
                break;
            case TapDelay:
                for (int i = 0; i < m; ++i)
                {
                    dl += step[0]; dr += step[1];
                    const float tap = rd<HQ> (0, dl), y = rd<HQ> (1, dr);
                    line[0].push (filt (0, 0.5f * (l[i] + r[i]), useHp, useLp) + kDenorm);
                    line[1].push (filt (1, tap + fb * y, useHp, useLp) + kDenorm);
                    l[i] = r[i] = tap + y;
                }
                break;
            default:
                for (int i = 0; i < m; ++i)
                {
                    dl += step[0]; dr += step[1];
                    const float yl = rd<HQ> (0, dl), yr = rd<HQ> (1, dr);
                    line[0].push (filt (0, l[i] + fb * yl, useHp, useLp) + kDenorm);
                    line[1].push (filt (1, r[i] + fb * yr, useHp, useLp) + kDenorm);
                    l[i] = yl; r[i] = yr;
                }
                break;
        }
    }

    float sr = 48000.0f, glideA = 0.1f, dCur[2] { -1.0f, -1.0f };
    int written = 0;                    // samples pushed since reset (saturates)
    SlotSmoother sm;
    DelayLine line[2];
    SvfCoefs hpc, lpc;
    Svf hp[2], lp[2];
};
} // namespace

std::unique_ptr<FxModule> makeDelay() { return std::make_unique<Delay>(); }

float delayFilterMagnitudeDb (const float* v, float hz, double sr)
{
    float h, l;
    bandEdges (v, h, l);
    float db = 0.0f;
    fxdsp::BiquadCoefs c;
    if (h > 20.0f) { c.highPass (h, 0.7071f, (float) sr); db += c.magnitudeDb (hz, (float) sr); }
    if (l < 19000.0f) { c.lowPass (std::min (l, (float) sr * 0.45f), 0.7071f, (float) sr); db += c.magnitudeDb (hz, (float) sr); }
    return db;
}
} // namespace tincture
