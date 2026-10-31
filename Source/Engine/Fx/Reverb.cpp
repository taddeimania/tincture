// Reverb.cpp — the Reverb FX module, five types:
//   PLATE    Dattorro's figure-of-eight plate tank (input diffusers, two
//            modulated decay-diffusion all-passes, damping). SIZE = decay,
//            DAMP = tank damping, WIDTH = output M/S width.
//   HALL     8-line FDN (Hadamard feedback), input diffusion, per-line
//            RT60-matched gains (DECAY = RT60 0.2..25 s), SIZE scales the line
//            lengths, SPIN modulates them.
//   VINTAGE  shorter FDN plus an early-reflection tap line (ER SIZE),
//            diffusion (DIFF A) with damped diffusers (DIFF B), DAMP, CHORUS.
//   NITROUS  FDN whose line-length set is the MODE geometry (Space, Marble,
//            Rectangle, Hexagon, Box), FEEDBACK, DIFFUSION (input + in-loop
//            all-passes), CHORUS.
//   BASIN    dense FDN with an all-pass inside every line, FEEDBACK, CHORUS.
// All: LO CUT / HI CUT (12 dB SVFs on the wet), PRE-DLY (ms or synced, 2 s max).
//
// Slots: 0 TYPE, 1 LO CUT, 2 HI CUT, 3 SIZE, 4 PRE-DLY, 5 BPM, 6 DAMP, 7 WIDTH,
//        8 DECAY/FEEDBACK, 9 SPIN/CHORUS RATE, 10 SPIN/CHORUS DEPTH, 11 ER SIZE,
//        12 DIFF A/DIFFUSION, 13 DIFF B, 14 NITROUS MODE, 15 LEVEL.
#include "FxEngine.h"

namespace tincture
{
namespace
{
using namespace fxdsp;
using namespace fxchoices;
constexpr FxType T = FxType::Reverb;
constexpr int kLines = 8;
constexpr float kMaxLineSec = 0.32f;

// Schroeder all-pass on a DelayLine with a fixed length.
struct Allpass
{
    DelayLine d;
    int len = 1;
    void init (int maxLen) { d.init (maxLen + 2); }
    inline float tick (float x, float g) noexcept
    {
        const float z = d.tap (len - 1);
        const float w = x + g * z;
        d.push (w + kDenorm);
        return z - g * w;
    }
    inline float tickFrac (float x, float g, float lenF) noexcept
    {
        const float z = d.linear (lenF - 1.0f);
        const float w = x + g * z;
        d.push (w + kDenorm);
        return z - g * w;
    }
    // Inner tap, counted like the line (0 = newest).
    inline float tap (int i) const noexcept { return d.tap (i); }
};

// ---------------------------------------------------------------------------
// Plate (Dattorro 1997), lengths at 29761 Hz.
// ---------------------------------------------------------------------------
struct PlateTank
{
    Allpass in[4], ap1[2], ap2[2];
    DelayLine dA[2], dB[2];
    float s = 1.0f, bwZ = 0.0f, damp[2] {}, end[2] {};
    int lA[2] {}, lB[2] {}, ap1Len[2] {};
    Lfo lfo;
    int tapL[7] {}, tapR[7] {};

    void prepare (float sr)
    {
        s = sr / 29761.0f;
        auto sc = [&] (int x) { return std::max (1, (int) std::lround ((float) x * s)); };
        const int inLen[4] { 142, 107, 379, 277 };
        for (int i = 0; i < 4; ++i) { in[i].init (sc (inLen[i])); in[i].len = sc (inLen[i]); }
        ap1Len[0] = sc (672); ap1Len[1] = sc (908);
        for (int c = 0; c < 2; ++c) ap1[c].init (ap1Len[c] + sc (16) + 4);
        ap2[0].init (sc (1800)); ap2[0].len = sc (1800);
        ap2[1].init (sc (2656)); ap2[1].len = sc (2656);
        lA[0] = sc (4453); lA[1] = sc (4217);
        lB[0] = sc (3720); lB[1] = sc (3163);
        for (int c = 0; c < 2; ++c) { dA[c].init (lA[c]); dB[c].init (lB[c]); }
        const int tl[7] { 266, 2974, 1913, 1996, 1990, 187, 1066 };
        const int tr[7] { 353, 3627, 1228, 2673, 2111, 335, 121 };
        for (int i = 0; i < 7; ++i) { tapL[i] = sc (tl[i]); tapR[i] = sc (tr[i]); }
    }
    void clear()
    {
        for (auto& a : in) a.d.clear();
        for (int c = 0; c < 2; ++c) { ap1[c].d.clear(); ap2[c].d.clear(); dA[c].clear(); dB[c].clear(); damp[c] = end[c] = 0.0f; }
        bwZ = 0.0f;
    }
    // In: mono x; out: l, r.
    inline void tick (float x, float decay, float dampA, float exc0, float exc1, float& l, float& r) noexcept
    {
        bwZ += 0.9995f * (x - bwZ);
        float v = bwZ;
        v = in[0].tick (v, 0.75f);
        v = in[1].tick (v, 0.75f);
        v = in[2].tick (v, 0.625f);
        v = in[3].tick (v, 0.625f);
        const float dd2 = clampf (decay + 0.15f, 0.25f, 0.5f);
        const float exc[2] { exc0, exc1 };
        float newEnd[2];
        for (int c = 0; c < 2; ++c)
        {
            float t = v + decay * end[1 - c];
            t = ap1[c].tickFrac (t, -0.7f, (float) ap1Len[c] + exc[c]);
            dA[c].push (t);
            float a = dA[c].tap (lA[c] - 1);
            damp[c] += dampA * (a - damp[c]);
            a = damp[c] * decay;
            a = ap2[c].tick (a, dd2);
            dB[c].push (a);
            newEnd[c] = dB[c].tap (lB[c] - 1);
        }
        end[0] = newEnd[0] + kDenorm; end[1] = newEnd[1] + kDenorm;
        // Output taps: c = 1 (right tank) and c = 0 (left tank) per Dattorro's table.
        l = 0.6f * (dA[1].tap (tapL[0]) + dA[1].tap (tapL[1]) - ap2[1].tap (tapL[2]) + dB[1].tap (tapL[3])
                    - dA[0].tap (tapL[4]) - ap2[0].tap (tapL[5]) - dB[0].tap (tapL[6]));
        r = 0.6f * (dA[0].tap (tapR[0]) + dA[0].tap (tapR[1]) - ap2[0].tap (tapR[2]) + dB[0].tap (tapR[3])
                    - dA[1].tap (tapR[4]) - ap2[1].tap (tapR[5]) - dB[1].tap (tapR[6]));
    }
};

// ---------------------------------------------------------------------------
// FDN for Hall / Vintage / Nitrous / Basin.
// ---------------------------------------------------------------------------
struct FdnConfig
{
    float ms[kLines];
    float sizeLo, sizeHi;            // SIZE 0..1 -> length scale
    float loopApMs;                  // > 0: all-pass inside each line (scaled per line)
};

const FdnConfig kHall { { 43.1f, 51.7f, 61.3f, 69.9f, 79.7f, 89.1f, 97.3f, 107.9f }, 0.4f, 1.6f, 0.0f };
const FdnConfig kVintage { { 23.3f, 29.9f, 33.7f, 41.9f, 47.3f, 53.9f, 59.3f, 67.1f }, 0.5f, 1.5f, 0.0f };
const FdnConfig kBasin { { 37.3f, 45.9f, 53.1f, 62.3f, 71.9f, 81.7f, 93.1f, 104.9f }, 0.3f, 1.7f, 4.0f };
const FdnConfig kNitrous[5] {
    { { 53.1f, 67.9f, 79.3f, 91.7f, 103.1f, 113.9f, 127.3f, 139.7f }, 0.3f, 2.0f, 5.0f },    // Space
    { { 7.1f, 8.9f, 10.3f, 11.9f, 13.7f, 15.1f, 16.7f, 18.3f }, 0.5f, 2.0f, 1.3f },          // Marble
    { { 20.0f, 28.0f, 38.0f, 34.4f, 42.7f, 47.3f, 53.9f, 62.1f }, 0.4f, 2.0f, 3.0f },         // Rectangle
    { { 30.0f, 34.6f, 40.0f, 45.8f, 52.0f, 60.0f, 69.3f, 79.4f }, 0.4f, 2.0f, 3.0f },         // Hexagon
    { { 24.0f, 24.6f, 25.3f, 26.1f, 26.9f, 27.6f, 28.4f, 29.3f }, 0.4f, 2.0f, 2.0f },         // Box
};
constexpr float kDiffMs[2][4] { { 4.77f, 3.59f, 12.73f, 9.30f }, { 5.11f, 3.83f, 11.87f, 8.71f } };
constexpr float kErMs[8] { 4.3f, 10.7f, 15.1f, 21.5f, 27.0f, 29.8f, 37.9f, 45.8f };
constexpr float kErGain[8] { 0.84f, 0.70f, 0.61f, 0.52f, 0.45f, 0.40f, 0.33f, 0.28f };

struct Fdn
{
    DelayLine line[kLines], apLine[kLines], er;
    Allpass diff[2][4];
    float len[kLines] {}, damp[kLines] {}, diffLp[2] {};
    float sr = 48000.0f;
    bool primed = false;
    Lfo lfo;

    void prepare (float s)
    {
        sr = s;
        for (auto& l : line) l.init ((int) (kMaxLineSec * sr) + 8);
        for (auto& a : apLine) a.init ((int) (0.012f * sr) + 4);
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < 4; ++k) diff[c][k].init ((int) (kDiffMs[c][k] * 0.001f * sr * 1.6f) + 4);
        er.init ((int) (0.1f * sr) + 4);
    }
    void clear()
    {
        for (auto& l : line) l.clear();
        for (auto& a : apLine) a.clear();
        for (auto& c : diff) for (auto& a : c) a.d.clear();
        er.clear();
        for (int i = 0; i < kLines; ++i) damp[i] = 0.0f;
        diffLp[0] = diffLp[1] = 0.0f;
        primed = false;
    }

    // One tick. g[i]: per-line loop gains; dampA: loop low-pass coefficient;
    // diffG: input diffusion; diffDamp: low-pass in the diffusers (0 = off);
    // loopApG: in-line all-pass gain (0 = off); modDepth in samples; er: 0 = off.
    void run (float* l, float* r, int m, const FdnConfig& cfg, float size, const float* g, float dampA,
              float diffG, float diffDamp, float loopApG, float modRate, float modDepth, float erSize, float erAmt)
    {
        const float scale = cfg.sizeLo + (cfg.sizeHi - cfg.sizeLo) * size;
        float end[kLines], step[kLines];
        lfo.advance (modRate, m, sr);
        for (int i = 0; i < kLines; ++i)
        {
            const float mod = modDepth * sine01 (lfo.phase + 0.137 * i * 7.0);
            end[i] = clampf (cfg.ms[i] * scale * 0.001f * sr + mod, 8.0f, (float) line[i].capacity() - 4.0f);
            if (! primed) len[i] = end[i];
            step[i] = (end[i] - len[i]) / (float) m;
        }
        primed = true;
        int apLen[kLines];
        for (int i = 0; i < kLines; ++i)
            apLen[i] = std::max (2, std::min ((int) (cfg.loopApMs * (0.7f + 0.09f * (float) i) * 0.001f * sr), apLine[i].capacity() - 2));
        int dl[2][4];
        const float dScale = 0.6f + 0.6f * size;
        for (int c = 0; c < 2; ++c)
            for (int k = 0; k < 4; ++k)
            {
                dl[c][k] = std::max (1, (int) (kDiffMs[c][k] * dScale * 0.001f * sr));
                diff[c][k].len = dl[c][k];
            }
        int erTap[8];
        for (int k = 0; k < 8; ++k) erTap[k] = std::max (1, (int) (kErMs[k] * erSize * 0.001f * sr));
        const float outG = 0.35f;

        for (int n = 0; n < m; ++n)
        {
            // Input diffusion per channel.
            float in[2] { l[n], r[n] };
            for (int c = 0; c < 2; ++c)
            {
                float v = in[c];
                for (int k = 0; k < 4; ++k) v = diff[c][k].tick (v, diffG);
                if (diffDamp > 0.0f) { diffLp[c] += (1.0f - diffDamp) * (v - diffLp[c]); v = diffLp[c]; }
                in[c] = v;
            }
            // Read the lines.
            float o[kLines];
            for (int i = 0; i < kLines; ++i)
            {
                len[i] += step[i];
                o[i] = line[i].linear (len[i] - 1.0f);
            }
            float yl = 0.0f, yr = 0.0f;
            for (int i = 0; i < kLines; ++i)
            {
                yl += (i & 1) ? -o[i] : o[i];
                yr += (i & 2) ? -o[i] : o[i];
            }
            // Damping, gain, optional in-loop all-pass.
            float x[kLines];
            for (int i = 0; i < kLines; ++i)
            {
                damp[i] += dampA * (o[i] - damp[i]);
                float v = damp[i] * g[i];
                if (loopApG > 0.0f)
                {
                    const float z = apLine[i].tap (apLen[i] - 1);
                    const float w = v + loopApG * z;
                    apLine[i].push (w + kDenorm);
                    v = z - loopApG * w;
                }
                x[i] = v;
            }
            // Hadamard 8 (orthogonal, scaled).
            for (int h = 1; h < kLines; h <<= 1)
                for (int i = 0; i < kLines; i += h << 1)
                    for (int j = i; j < i + h; ++j)
                    {
                        const float a = x[j], b = x[j + h];
                        x[j] = a + b; x[j + h] = a - b;
                    }
            const float hs = 0.35355339f;
            for (int i = 0; i < kLines; ++i)
                line[i].push (x[i] * hs + ((i & 1) ? in[1] : in[0]) + kDenorm);
            float ol = yl * outG, orr = yr * outG;
            if (erAmt > 0.0f)
            {
                er.push (0.5f * (l[n] + r[n]));
                for (int k = 0; k < 8; k += 2)
                {
                    ol += erAmt * kErGain[k] * er.tap (erTap[k]);
                    orr += erAmt * kErGain[k + 1] * er.tap (erTap[k + 1]);
                }
            }
            l[n] = ol;
            r[n] = orr;
        }
        for (int i = 0; i < kLines; ++i) len[i] = end[i];
    }
};

class Reverb final : public FxModule
{
public:
    void prepare (double s, int) override
    {
        sr = (float) s;
        sm.prepare (s);
        for (auto& p : pre) p.init ((int) (2.0f * sr) + 8);
        plate.prepare (sr);
        fdn.prepare (sr);
    }
    void reset() override
    {
        for (int c = 0; c < 2; ++c) { pre[c].clear(); lo[c].clear(); hi[c].clear(); }
        plate.clear();
        fdn.clear();
        preCur = -1.0f;
        sm.reset();
    }

    void process (float* L, float* R, int n, const FxBlock& b) override
    {
        const auto& P = fxInfo (T).params;
        const int type = P[0].index (b.v[0]);
        if (type != lastType) { plate.clear(); fdn.clear(); lastType = type; }
        const int nmode = P[14].index (b.v[14]);
        for (int off = 0; off < n; off += kTick)
        {
            const int m = std::min (kTick, n - off);
            const float* v = sm.tick (b.v);
            float* l = L + off;
            float* r = R + off;

            // Pre-delay (ramped).
            const float pd = clampf (fxTimeSeconds (T, 4, b.v, b.ctx->bpm) * sr, 0.0f, (float) pre[0].capacity() - 4.0f);
            if (preCur < 0.0f) preCur = pd;
            const float pdStep = (pd - preCur) / (float) m;
            for (int i = 0; i < m; ++i)
            {
                preCur += pdStep;
                pre[0].push (l[i]); pre[1].push (r[i]);
                if (preCur >= 0.5f) { l[i] = pre[0].linear (preCur); r[i] = pre[1].linear (preCur); }
            }

            const float size = v[3];
            const float rateHz = 0.05f + 4.0f * v[9] * v[9];
            switch (type)
            {
                case Plate: runPlate (l, r, m, v); break;
                case Hall:
                {
                    float g[kLines];
                    rt60Gains (g, fdnLens (kHall, size), rt60 (v[8]));
                    fdn.run (l, r, m, kHall, size, g, onePoleCoef (7000.0f, sr), 0.7f, 0.0f, 0.0f,
                             rateHz, v[10] * 0.0015f * sr, 1.0f, 0.0f);
                    break;
                }
                case Vintage:
                {
                    float g[kLines];
                    rt60Gains (g, fdnLens (kVintage, size), rt60 (v[8]));
                    const float dampHz = 18000.0f * std::exp2 (-v[6] * 3.6f);
                    fdn.run (l, r, m, kVintage, size, g, onePoleCoef (dampHz, sr), 0.4f + 0.35f * v[12], 0.85f * v[13], 0.0f,
                             rateHz, v[10] * 0.002f * sr, 0.3f + 1.4f * v[11], 0.6f);
                    break;
                }
                case Nitrous:
                {
                    float g[kLines];
                    fbGains (g, fdnLens (kNitrous[nmode], size), v[8]);
                    fdn.run (l, r, m, kNitrous[nmode], size, g, onePoleCoef (12000.0f, sr), 0.75f * v[12], 0.0f, 0.6f * v[12],
                             rateHz, v[10] * 0.002f * sr, 1.0f, 0.0f);
                    break;
                }
                default:
                {
                    float g[kLines];
                    fbGains (g, fdnLens (kBasin, size), v[8]);
                    fdn.run (l, r, m, kBasin, size, g, onePoleCoef (9000.0f, sr), 0.75f, 0.0f, 0.6f,
                             rateHz, v[10] * 0.002f * sr, 1.0f, 0.0f);
                    break;
                }
            }

            // LO / HI cut on the wet.
            const float loHz = 20.0f * std::exp2 (v[1] * 7.0f);
            const float hiHz = 20000.0f * std::exp2 (-v[2] * 7.0f);
            if (v[1] > 0.005f)
            {
                loC.set (loHz, 0.7071f, sr);
                for (int i = 0; i < m; ++i) { l[i] = lo[0].hp (loC, l[i]); r[i] = lo[1].hp (loC, r[i]); }
            }
            if (v[2] > 0.005f)
            {
                hiC.set (hiHz, 0.7071f, sr);
                for (int i = 0; i < m; ++i) { l[i] = hi[0].lp (hiC, l[i]); r[i] = hi[1].lp (hiC, r[i]); }
            }
        }
    }

private:
    static float rt60 (float n) noexcept { return 0.2f * std::exp2 (n * 7.0f); }
    void fdnLensImpl (const FdnConfig& c, float size, float* out) const
    {
        const float scale = c.sizeLo + (c.sizeHi - c.sizeLo) * size;
        for (int i = 0; i < kLines; ++i) out[i] = c.ms[i] * scale * 0.001f * sr;
    }
    struct Lens { float v[kLines]; };
    Lens fdnLens (const FdnConfig& c, float size) const { Lens l; fdnLensImpl (c, size, l.v); return l; }
    void rt60Gains (float* g, const Lens& l, float rt) const
    {
        for (int i = 0; i < kLines; ++i) g[i] = std::pow (10.0f, -3.0f * l.v[i] / (rt * sr));
    }
    void fbGains (float* g, const Lens& l, float n) const
    {
        float mean = 0.0f;
        for (int i = 0; i < kLines; ++i) mean += l.v[i];
        mean /= (float) kLines;
        const float fb = 0.3f + 0.695f * clamp01 (n);
        for (int i = 0; i < kLines; ++i) g[i] = std::pow (fb, l.v[i] / mean);
    }

    void runPlate (float* l, float* r, int m, const float* v)
    {
        const float decay = 0.2f + 0.78f * v[3];
        const float dampA = 1.0f - 0.85f * v[6];
        const float width = v[7];
        plate.lfo.advance (0.9f, m, sr);
        const float exc = 16.0f * plate.s;
        const float e0 = exc * sine01 (plate.lfo.phase), e1 = exc * sine01 (plate.lfo.phase + 0.25);
        for (int i = 0; i < m; ++i)
        {
            float a, bb;
            plate.tick (0.5f * (l[i] + r[i]), decay, dampA, e0, e1, a, bb);
            const float mid = 0.5f * (a + bb), side = 0.5f * (a - bb) * width;
            l[i] = mid + side;
            r[i] = mid - side;
        }
    }

    float sr = 48000.0f, preCur = -1.0f;
    int lastType = -1;
    SlotSmoother sm;
    DelayLine pre[2];
    PlateTank plate;
    Fdn fdn;
    SvfCoefs loC, hiC;
    Svf lo[2], hi[2];
};
} // namespace

std::unique_ptr<FxModule> makeReverb() { return std::make_unique<Reverb>(); }
} // namespace tincture
