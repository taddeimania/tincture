// WaveImport.cpp — audio / image import into wavetable frames, pitch
// detection, import overrides and export helpers. See WaveImport.h.
#include "WaveImport.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

namespace tincture::wtedit
{
namespace
{
using cpx = std::complex<float>;

int nextPow2 (int v)
{
    int n = 1;
    while (n < v) n <<= 1;
    return n;
}

std::string trim (const std::string& s)
{
    size_t a = 0, b = s.size();
    while (a < b && std::isspace ((unsigned char) s[a])) ++a;
    while (b > a && std::isspace ((unsigned char) s[b - 1])) --b;
    return s.substr (a, b - a);
}

std::string lower (std::string s)
{
    for (auto& c : s) c = (char) std::tolower ((unsigned char) c);
    return s;
}

// Rising zero crossing nearest to `target` within +-radius (fractional), or -1.
double risingZeroNear (const float* x, int len, double target, double radius)
{
    const int a = std::max (1, (int) std::floor (target - radius));
    const int b = std::min (len - 1, (int) std::ceil (target + radius));
    double best = -1.0, bestDist = 1.0e30;
    for (int i = a; i <= b; ++i)
    {
        const float p = x[i - 1], c = x[i];
        if (! (p < 0.0f && c >= 0.0f)) continue;
        const double pos = (double) (i - 1) + (double) p / (double) (p - c);
        const double d = std::abs (pos - target);
        if (d < bestDist) { bestDist = d; best = pos; }
    }
    return best;
}

// Sub-sample period refinement: correlate the start of the file with the
// region K cycles later and take the best lag / K.
double refinePeriod (const float* x, int len, double p)
{
    if (p <= 2.0) return p;
    const int w = (int) std::min (4096.0, std::round (2.0 * p));
    const int k = (int) std::min (64.0, std::floor ((len - w - p) / p));
    if (k < 2) return p;
    const double l0 = k * p;
    const int lo = std::max (1, (int) std::floor (l0 - p * 0.25)), hi = std::min (len - w - 1, (int) std::ceil (l0 + p * 0.25));
    if (hi <= lo + 2) return p;
    double ea = 0.0;
    for (int j = 0; j < w; ++j) ea += (double) x[j] * x[j];
    if (ea <= 1.0e-12) return p;
    std::vector<double> c ((size_t) (hi - lo + 1));
    for (int l = lo; l <= hi; ++l)
    {
        double s = 0.0, eb = 0.0;
        for (int j = 0; j < w; ++j) { s += (double) x[j] * x[l + j]; eb += (double) x[l + j] * x[l + j]; }
        c[(size_t) (l - lo)] = eb > 1.0e-12 ? s / std::sqrt (ea * eb) : -1.0;
    }
    int bi = 0;
    for (int i = 1; i < (int) c.size(); ++i) if (c[(size_t) i] > c[(size_t) bi]) bi = i;
    if (bi == 0 || bi == (int) c.size() - 1 || c[(size_t) bi] < 0.8) return p;
    const double a = c[(size_t) bi - 1], b = c[(size_t) bi], d = c[(size_t) bi + 1];
    const double den = a - 2.0 * b + d;
    const double off = den != 0.0 ? 0.5 * (a - d) / den : 0.0;
    return (lo + bi + off) / k;
}

struct Cycle { double start, len; };

void keepAtMost256 (std::vector<Cycle>& cycles)
{
    if ((int) cycles.size() <= kMaxFrames) return;
    std::vector<Cycle> out ((size_t) kMaxFrames);
    const double step = (double) (cycles.size() - 1) / (kMaxFrames - 1);
    for (int i = 0; i < kMaxFrames; ++i) out[(size_t) i] = cycles[(size_t) std::lround (i * step)];
    cycles = std::move (out);
}

void cyclesToFrames (const float* x, int len, const std::vector<Cycle>& cycles, FrameSet& set)
{
    set.frames.resize (cycles.size());
    for (size_t i = 0; i < cycles.size(); ++i)
    {
        const Cycle& c = cycles[i];
        if (c.len == (double) kFrameSize && c.start == std::floor (c.start) && c.start + kFrameSize <= len)
            std::copy (x + (ptrdiff_t) c.start, x + (ptrdiff_t) c.start + kFrameSize, set.frames[i].begin());
        else
            readCycle (x, len, c.start, c.len, set.frames[i]);
    }
}

double firstRisingZero (const float* x, int len, double maxSearch)
{
    const double z = risingZeroNear (x, len, 0.0, maxSearch);
    // risingZeroNear picks the nearest to 0, i.e. the first one in [0, maxSearch].
    return z < 0.0 ? 0.0 : z;
}
} // namespace

// ---------------------------------------------------------------------------
// Pitch detection
// ---------------------------------------------------------------------------
PitchDetector::PitchDetector (double sampleRate, double minHz, double maxHz)
    : minLag (std::max (2, (int) std::floor (sampleRate / maxHz))),
      maxLag (std::max (8, (int) std::ceil (sampleRate / minHz))),
      window (maxLag),
      fftSize (nextPow2 (2 * maxLag + 2)),
      fft (fftSize)
{
}

PitchEstimate PitchDetector::detect (const float* x, int len, int start) const
{
    PitchEstimate est;
    const int avail = len - start;
    const int w = window;
    const int lagMax = std::min (maxLag, avail - w - 2);
    if (lagMax < minLag + 2) return est;

    // Both real inputs in one complex FFT: z = a + i*b.
    std::vector<cpx> z ((size_t) fftSize), a ((size_t) fftSize);
    for (int j = 0; j < w + lagMax; ++j) z[(size_t) j] = cpx (j < w ? x[start + j] : 0.0f, x[start + j]);
    fft.forward (z.data());
    for (int k = 0; k < fftSize; ++k)
    {
        const cpx zk = z[(size_t) k], zn = std::conj (z[(size_t) ((fftSize - k) & (fftSize - 1))]);
        const cpx A = (zk + zn) * 0.5f;
        const cpx B = (zk - zn) * cpx (0.0f, -0.5f);
        a[(size_t) k] = std::conj (A) * B;
    }
    fft.inverse (a.data());     // a[tau] = sum_j x[j] x[j + tau]

    std::vector<double> ps ((size_t) (w + lagMax + 1), 0.0);
    for (int j = 0; j < w + lagMax; ++j) ps[(size_t) j + 1] = ps[(size_t) j] + (double) x[start + j] * x[start + j];
    const double m0 = ps[(size_t) w];
    if (m0 < 1.0e-9 * w) return est;      // silence

    std::vector<float> dn ((size_t) lagMax + 1);
    dn[0] = 1.0f;
    double run = 0.0;
    for (int t = 1; t <= lagMax; ++t)
    {
        const double mt = ps[(size_t) (t + w)] - ps[(size_t) t];
        const double d = std::max (0.0, m0 + mt - 2.0 * (double) a[(size_t) t].real());
        run += d;
        dn[(size_t) t] = run > 0.0 ? (float) (d * t / run) : 1.0f;
    }

    int tau = -1;
    for (int t = minLag; t <= lagMax; ++t)
    {
        if (dn[(size_t) t] < threshold)
        {
            while (t + 1 <= lagMax && dn[(size_t) t + 1] < dn[(size_t) t]) ++t;
            tau = t;
            break;
        }
    }
    if (tau < 0)
    {
        tau = minLag;
        for (int t = minLag; t <= lagMax; ++t) if (dn[(size_t) t] < dn[(size_t) tau]) tau = t;
    }
    double refined = tau;
    if (tau > 1 && tau < lagMax)
    {
        const double y0 = dn[(size_t) tau - 1], y1 = dn[(size_t) tau], y2 = dn[(size_t) tau + 1];
        const double den = y0 - 2.0 * y1 + y2;
        if (den > 0.0) refined = tau + 0.5 * (y0 - y2) / den;
    }
    est.period = refined;
    est.confidence = clamp01 (1.0f - dn[(size_t) tau]);
    return est;
}

double PitchMap::at (double pos) const noexcept
{
    if (period.empty() || hop <= 0) return 0.0;
    const double f = std::max (0.0, pos / hop);
    const size_t i = (size_t) f;
    if (i + 1 >= period.size()) return period.back();
    const double t = f - (double) i;
    return period[i] + (period[i + 1] - period[i]) * t;
}

double PitchMap::median() const
{
    std::vector<double> v;
    for (double p : period) if (p > 0.0) v.push_back (p);
    if (v.empty()) return 0.0;
    std::nth_element (v.begin(), v.begin() + (ptrdiff_t) (v.size() / 2), v.end());
    return v[v.size() / 2];
}

PitchMap buildPitchMap (const float* x, int len, double sampleRate, double minHz, double maxHz)
{
    PitchDetector det (sampleRate, minHz, maxHz);
    PitchMap map;
    map.hop = std::max (64, det.hopHint());
    std::vector<double> raw;
    std::vector<bool> voiced;
    for (int s = 0; s < len; s += map.hop)
    {
        // Short tail: analyse the last full window instead.
        const int st = std::max (0, std::min (s, len - det.windowSize() * 2));
        const PitchEstimate e = det.detect (x, len, st);
        raw.push_back (e.period);
        voiced.push_back (e.period > 0.0 && e.confidence > 0.5f);
    }
    // Fill unvoiced spots from the nearest voiced neighbour.
    int lastV = -1;
    for (int i = 0; i < (int) raw.size(); ++i)
    {
        if (voiced[(size_t) i]) { lastV = i; continue; }
        int nextV = -1;
        for (int j = i + 1; j < (int) raw.size(); ++j) if (voiced[(size_t) j]) { nextV = j; break; }
        if (lastV < 0 && nextV < 0) { raw[(size_t) i] = 0.0; continue; }
        const int src = lastV < 0 ? nextV : (nextV < 0 ? lastV : (i - lastV <= nextV - i ? lastV : nextV));
        raw[(size_t) i] = raw[(size_t) src];
    }
    // 5-point median.
    map.period.resize (raw.size());
    for (int i = 0; i < (int) raw.size(); ++i)
    {
        std::array<double, 5> w {};
        int n = 0;
        for (int j = std::max (0, i - 2); j <= std::min ((int) raw.size() - 1, i + 2); ++j)
        {
            // insertion sort into w[0..n]
            int k = n++;
            while (k > 0 && w[(size_t) k - 1] > raw[(size_t) j]) { w[(size_t) k] = w[(size_t) k - 1]; --k; }
            w[(size_t) k] = raw[(size_t) j];
        }
        map.period[(size_t) i] = w[(size_t) n / 2];
    }
    return map;
}

// ---------------------------------------------------------------------------
// Overrides
// ---------------------------------------------------------------------------
bool parseTextOverride (const std::string& text, ImportOverride& out)
{
    bool any = false;
    size_t p = 0;
    while ((p = text.find ('[', p)) != std::string::npos)
    {
        const size_t e = text.find (']', p + 1);
        if (e == std::string::npos) break;
        const std::string tok = lower (trim (text.substr (p + 1, e - p - 1)));
        p = e + 1;
        if (! tok.empty() && std::all_of (tok.begin(), tok.end(), [] (char c) { return std::isdigit ((unsigned char) c); }))
        {
            const int v = std::atoi (tok.c_str());
            if (v >= 32 && v <= 9999) { out.samplesPerCycle = v; any = true; }
        }
        else if (tok == "interp") { out.interpolate = 1; any = true; }
        else if (tok == "no interp" || tok == "nointerp" || tok == "no_interp") { out.interpolate = 0; any = true; }
    }
    return any;
}

std::string noteName (int midi)
{
    static const char* names[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    const int oct = (int) std::floor (midi / 12.0);
    return std::string (names[((midi % 12) + 12) % 12]) + std::to_string (oct);
}

bool parseFieldOverride (const std::string& field, int& samples)
{
    const std::string s = trim (field);
    if (s.empty()) return false;
    if (s.size() <= 4 && std::all_of (s.begin(), s.end(), [] (char c) { return std::isdigit ((unsigned char) c); }))
    {
        const int v = std::atoi (s.c_str());
        if (v <= 0) return false;
        samples = v;
        return true;
    }
    static const int semis[7] { 9, 11, 0, 2, 4, 5, 7 };     // A..G
    const char c = (char) std::toupper ((unsigned char) s[0]);
    if (c < 'A' || c > 'G') return false;
    size_t i = 1;
    int note = semis[c - 'A'];
    if (i < s.size() && s[i] == '#') { ++note; ++i; }
    else if (i < s.size() && s[i] == 'b') { --note; ++i; }
    if (i >= s.size()) return false;
    bool neg = false;
    if (s[i] == '-') { neg = true; ++i; }
    if (i >= s.size() || ! std::all_of (s.begin() + (ptrdiff_t) i, s.end(), [] (char ch) { return std::isdigit ((unsigned char) ch); }))
        return false;
    const int oct = std::atoi (s.c_str() + i) * (neg ? -1 : 1);
    const int midi = oct * 12 + note;            // C0 = 0
    const double hz = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
    const int n = (int) std::lround (44100.0 / hz);
    if (n < 1) return false;
    samples = n;
    return true;
}

std::string splitEcho (int samples)
{
    if (samples <= 0) return {};
    const double hz = 44100.0 / samples;
    const double m = 69.0 + 12.0 * std::log2 (hz / 440.0);
    const int note = (int) std::lround (m);
    const int cents = (int) std::lround ((m - note) * 100.0);
    return "split at: " + std::to_string (samples) + " samples (" + std::to_string ((int) std::lround (hz))
         + " Hz, note: " + noteName (note) + " and " + (cents > 0 ? "+" : "") + std::to_string (cents) + " cents)";
}

int parseClmChunk (const std::string& chunk)
{
    const size_t p = chunk.find ("<!>");
    if (p == std::string::npos) return 0;
    const int v = std::atoi (chunk.c_str() + p + 3);
    return v > 0 ? v : 0;
}

std::string makeClmChunk (int frameSize, bool interpolate)
{
    return "<!>" + std::to_string (frameSize) + (interpolate ? " 10000000" : " 00000000") + " wavetable (tincture)";
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------
const std::vector<ImportMenuItem>& importMenu()
{
    static const std::vector<ImportMenuItem> m {
        { "Audio (via Dynamic Pitch Zero-Snap)...", ImportMethod::DynamicZeroSnap, false },
        { "Audio (via Dynamic Pitch Follow)...", ImportMethod::DynamicFollow, false },
        { "Audio (via Fixed Frame Size)...", ImportMethod::FixedFrameSize, false },
        { "Audio (via FFT Resynth 256)...", ImportMethod::Fft256, false },
        { "Audio (via FFT Resynth 512)...", ImportMethod::Fft512, false },
        { "Audio (via FFT Resynth 1024)...", ImportMethod::Fft1024, false },
        { "Audio (via FFT Resynth 2048)...", ImportMethod::Fft2048, false },
        { "Audio to Single-Cycle (Replaces Selection)...", ImportMethod::FixedFrameSize, true },
        { "Audio Frequency Estimation...", ImportMethod::FrequencyEstimation, false },
    };
    return m;
}

const std::vector<DropTarget>& dropTargets()
{
    static const std::vector<DropTarget> d {
        { "Normal", "DYNAMIC PITCH - ZERO SNAP", ImportMethod::DynamicZeroSnap },
        { "Normal", "DYNAMIC PITCH - FOLLOW", ImportMethod::DynamicFollow },
        { "Normal", "FREQUENCY ESTIMATION", ImportMethod::FrequencyEstimation },
        { "Constant framesize", "PITCH AVERAGE", ImportMethod::PitchAverage },
        { "FFT", "256", ImportMethod::Fft256 },
        { "FFT", "512", ImportMethod::Fft512 },
        { "FFT", "1024", ImportMethod::Fft1024 },
        { "FFT", "2048", ImportMethod::Fft2048 },
    };
    return d;
}

const std::vector<ExportMenuItem>& exportMenu()
{
    static const std::vector<ExportMenuItem> m {
        { "Export All As 8-bit (.256)", 0 },
        { "Export All As 16-bit (.wav)", 1 },
        { "Export All As 32-bit (.wav)", 2 },
        { "Export All As Single-Cycle Waves", 3 },
        { "Export Selection", 4 },
    };
    return m;
}

// ---------------------------------------------------------------------------
// Audio import
// ---------------------------------------------------------------------------
namespace
{
void importFft (const float* x, int len, int n, FrameSet& set)
{
    const Fft fft (n);
    const int frames = std::clamp (len <= n ? 1 : 1 + (len - n) / (n / 2), 1, kMaxFrames);
    const int harmonics = std::min (n / 2 - 1, kNyquist - 1);
    std::vector<cpx> buf ((size_t) n);
    std::vector<float> win ((size_t) n);
    for (int i = 0; i < n; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) n);
    set.frames.resize ((size_t) frames);
    Spectrum s;
    for (int f = 0; f < frames; ++f)
    {
        const int start = frames == 1 ? 0 : (int) std::lround ((double) f * std::max (0, len - n) / (frames - 1));
        for (int i = 0; i < n; ++i)
        {
            const int idx = start + i;
            buf[(size_t) i] = cpx (idx < len ? x[idx] * win[(size_t) i] : 0.0f, 0.0f);
        }
        fft.forward (buf.data());
        s.mag.fill (0.0f);
        s.phase.fill (0.0f);
        for (int k = 1; k <= harmonics; ++k)
        {
            const cpx c = buf[(size_t) k];
            s.mag[(size_t) k] = 4.0f * std::abs (c) / (float) n;       // Hann coherent gain 0.5
            s.phase[(size_t) k] = std::atan2 (c.imag(), c.real()) + 0.5f * kPi;
        }
        synthesise (s, set.frames[(size_t) f]);
    }
    float peak = 0.0f;
    for (auto& f : set.frames) peak = std::max (peak, peakOf (f));
    if (peak > 1.0e-9f) for (auto& f : set.frames) for (auto& v : f) v /= peak;
}

void importFrequencyEstimation (const float* x, int len, double p, double sampleRate, FrameSet& set)
{
    // Analyse 4-cycle Hann windows resampled to `spc` samples per cycle (just
    // enough for every harmonic below Nyquist): harmonic k lands exactly on
    // FFT bin 4k, everything inharmonic is dropped. Frames are resynthesised
    // from the harmonics, phase-referenced to whole cycles so they line up.
    constexpr int cyclesPerWin = 4;
    const int harmonics = std::clamp ((int) std::floor (0.5 * sampleRate / (sampleRate / p)), 1, kNyquist - 1);
    const int spc = std::clamp (nextPow2 (2 * (harmonics + 1)), 64, kFrameSize);
    const int wn = cyclesPerWin * spc;
    const int maxK = std::min (harmonics, spc / 2 - 1);
    const Fft fft (wn);
    const double winLen = cyclesPerWin * p;
    const int numCycles = (int) std::floor ((len - winLen) / p) + 1;
    if (numCycles < 1) { set.frames.assign (1, Frame {}); readCycle (x, len, 0.0, p, set.frames[0]); return; }
    const int stride = std::max (1, (numCycles + kMaxFrames - 1) / kMaxFrames);
    std::vector<cpx> buf ((size_t) wn);
    std::vector<float> win ((size_t) wn);
    for (int i = 0; i < wn; ++i) win[(size_t) i] = 0.5f - 0.5f * std::cos (kTwoPi * (float) i / (float) wn);
    const double step = p / spc;
    const float cutoff = (float) std::min (1.0, 1.0 / step) * (step > 1.0 ? 0.97f : 1.0f);
    set.frames.clear();
    Spectrum s;
    for (int c = 0; c < numCycles && (int) set.frames.size() < kMaxFrames; c += stride)
    {
        const double start = c * p;
        for (int i = 0; i < wn; ++i)
            buf[(size_t) i] = cpx (sincRead (x, len, start + i * step, cutoff, false) * win[(size_t) i], 0.0f);
        fft.forward (buf.data());
        s.mag.fill (0.0f);
        s.phase.fill (0.0f);
        for (int k = 1; k <= maxK; ++k)
        {
            const cpx v = buf[(size_t) (cyclesPerWin * k)];
            s.mag[(size_t) k] = 4.0f * std::abs (v) / (float) wn;
            s.phase[(size_t) k] = std::atan2 (v.imag(), v.real()) + 0.5f * kPi;
        }
        set.frames.push_back (Frame {});
        synthesise (s, set.frames.back());
    }
}
} // namespace

ImportResult importAudio (const float* x, int len, double sampleRate, ImportMethod method,
                          const ImportOverride& ov, const std::string& name)
{
    ImportResult r;
    r.set.name = name;
    r.set.modified = true;
    if (ov.interpolate >= 0) r.set.interpolate = ov.interpolate == 1;
    if (x == nullptr || len <= 0) { r.error = "empty audio"; return r; }

    const bool fftMethod = method == ImportMethod::Fft256 || method == ImportMethod::Fft512
                        || method == ImportMethod::Fft1024 || method == ImportMethod::Fft2048;
    if (fftMethod)
    {
        const int n = method == ImportMethod::Fft256 ? 256 : method == ImportMethod::Fft512 ? 512
                    : method == ImportMethod::Fft1024 ? 1024 : 2048;
        importFft (x, len, n, r.set);
        r.samplesPerCycle = n;
        r.detectedHz = sampleRate / n;
        r.ok = true;
        return r;
    }

    std::vector<Cycle> cycles;
    auto fixed = [&] (double size)
    {
        r.samplesPerCycle = size;
        for (double s = 0.0; s + size <= len + 1.0e-9; s += size) cycles.push_back ({ s, size });
        if (cycles.empty()) cycles.push_back ({ 0.0, size });       // short file: one (zero-padded) cycle
    };

    if (ov.samplesPerCycle > 0 || method == ImportMethod::FixedFrameSize)
    {
        fixed (ov.samplesPerCycle > 0 ? ov.samplesPerCycle : kFrameSize);
    }
    else
    {
        const PitchMap map = buildPitchMap (x, len, sampleRate);
        double avg = map.median();
        if (avg <= 0.0)
        {
            fixed (kFrameSize);      // nothing pitched found: plain 2048-sample frames
        }
        else
        {
            avg = refinePeriod (x, len, avg);
            r.detectedHz = sampleRate / avg;
            r.nearestNote = (int) std::lround (hzToNote ((float) r.detectedHz));
            if (method == ImportMethod::FrequencyEstimation)
            {
                importFrequencyEstimation (x, len, avg, sampleRate, r.set);
                r.samplesPerCycle = avg;
                r.ok = true;
                return r;
            }
            double pos = firstRisingZero (x, len, avg);
            if (method == ImportMethod::PitchAverage)
            {
                for (double s = pos; s + avg <= len; s += avg) cycles.push_back ({ s, avg });
                r.samplesPerCycle = avg;
                r.fieldText = std::to_string ((int) std::lround (avg));
            }
            else
            {
                double total = 0.0;
                for (;;)
                {
                    const double p = std::max (2.0, map.at (pos + avg * 0.5));
                    double next = pos + p;
                    if (method == ImportMethod::DynamicZeroSnap)
                    {
                        const double z = risingZeroNear (x, len, next, p * 0.25);
                        if (z > pos + p * 0.5) next = z;
                    }
                    if (next > len) break;
                    cycles.push_back ({ pos, next - pos });
                    total += next - pos;
                    pos = next;
                }
                r.samplesPerCycle = cycles.empty() ? avg : total / (double) cycles.size();
            }
            if (cycles.empty()) cycles.push_back ({ 0.0, avg });
        }
    }
    keepAtMost256 (cycles);
    cyclesToFrames (x, len, cycles, r.set);
    r.ok = true;
    return r;
}

void importSingleCycle (const float* x, int len, Frame& out)
{
    resampleCycle (x, len, out);
}

FrameSet importSingleCycles (const std::vector<std::vector<float>>& files, const std::string& name)
{
    FrameSet s;
    s.name = name;
    s.modified = true;
    for (auto& f : files)
    {
        if ((int) s.frames.size() >= kMaxFrames) break;
        s.frames.push_back (Frame {});
        resampleCycle (f.data(), (int) f.size(), s.frames.back());
    }
    if (s.frames.empty()) s.frames.push_back (Frame {});
    return s;
}

// ---------------------------------------------------------------------------
// Images
// ---------------------------------------------------------------------------
FrameSet importImage (const float* lum, int width, int height, ImageMode mode, const std::string& name)
{
    FrameSet s;
    s.name = name;
    s.modified = true;
    if (lum == nullptr || width <= 0 || height <= 0) { s.frames.push_back (Frame {}); return s; }
    const int frames = std::min (width, kMaxFrames);
    s.frames.resize ((size_t) frames);
    std::vector<float> col ((size_t) height);
    for (int c = 0; c < frames; ++c)
    {
        // Columns beyond 256 are dropped; narrower images map 1:1.
        const int srcCol = width > kMaxFrames ? (int) std::lround ((double) c * (width - 1) / (kMaxFrames - 1)) : c;
        for (int r = 0; r < height; ++r) col[(size_t) r] = clamp01 (lum[(size_t) r * (size_t) width + (size_t) srcCol]);
        Frame& f = s.frames[(size_t) c];
        if (mode == ImageMode::Spectral)
        {
            Spectrum sp;
            const int bins = std::min (height, kFftBins);
            for (int k = 1; k <= bins; ++k)
            {
                const float bar = col[(size_t) (height - k)];       // bottom row = fundamental
                sp.mag[(size_t) k] = bar * bar;
            }
            synthesise (sp, f);
            continue;
        }
        for (int k = 0; k < kFrameSize; ++k)
        {
            const double pos = std::clamp (((double) k + 0.5) * height / kFrameSize - 0.5, 0.0, (double) (height - 1));
            const int i = (int) pos;
            const float t = (float) (pos - i);
            const float v = lerp (col[(size_t) i], col[(size_t) std::min (i + 1, height - 1)], t);
            f[(size_t) k] = mode == ImageMode::Bipolar ? 2.0f * v - 1.0f : v;
        }
    }
    if (mode == ImageMode::Spectral)
    {
        float peak = 0.0f;
        for (auto& f : s.frames) peak = std::max (peak, peakOf (f));
        if (peak > 1.0f) for (auto& f : s.frames) for (auto& v : f) v /= peak;
    }
    return s;
}

// ---------------------------------------------------------------------------
// Export
// ---------------------------------------------------------------------------
std::vector<float> exportAudio (const FrameSet& s, int first, int last)
{
    if (s.frames.empty()) return {};
    if (last < 0) last = s.size() - 1;
    first = std::clamp (first, 0, s.size() - 1);
    last = std::clamp (last, first, s.size() - 1);
    std::vector<float> out;
    out.reserve ((size_t) (last - first + 1) * kFrameSize);
    for (int i = first; i <= last; ++i) out.insert (out.end(), s.frames[(size_t) i].begin(), s.frames[(size_t) i].end());
    return out;
}

std::vector<std::vector<float>> exportSingleCycles (const FrameSet& s)
{
    std::vector<std::vector<float>> out;
    for (auto& f : s.frames) out.emplace_back (f.begin(), f.end());
    return out;
}

std::vector<uint8_t> export256 (const FrameSet& s)
{
    std::vector<uint8_t> out;
    out.reserve (s.frames.size() * 256);
    Frame tmp;
    for (auto& f : s.frames)
    {
        tmp = f;
        lowpassHarmonics (tmp, 127);
        Spectrum sp;
        analyse (f, sp);
        const float dc = sp.mag[0];                         // keep the DC the lowpass removed
        for (int i = 0; i < 256; ++i)
        {
            const float v = clampf (tmp[(size_t) (i * 8)] + dc, -1.0f, 1.0f);
            out.push_back ((uint8_t) std::clamp ((int) std::lround (v * 127.0f + 128.0f), 0, 255));
        }
    }
    return out;
}

std::string singleCycleFileName (const std::string& table, int index)
{
    char buf[32];
    std::snprintf (buf, sizeof (buf), "_%03d.wav", std::clamp (index + 1, 0, 999999));
    return (table.empty() ? std::string ("Table") : table) + buf;
}
} // namespace tincture::wtedit
