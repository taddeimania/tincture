// EnginePresetsTests.cpp — renders every factory preset offline and checks it:
// level (peak / RMS), NaN/Inf, DC, silence, release to silence, category
// character (bass low end, pad attack, pluck decay, pitch), CPU cost, and that
// encodePreset / decodePreset round-trips each preset exactly. Each render is
// written to /tmp/presets/<name>.wav for listening.
//
//   TinctureEnginePresetsTests            all presets
//   TinctureEnginePresetsTests <name>     only presets whose name contains <name>
// Returns non-zero on failure.
#include "Engine/Synth.h"
#include "Engine/Wavetable.h"
#include "Model/FactoryPresets.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

using namespace tincture;

namespace
{
int failures = 0, checks = 0;
std::vector<std::string> problems;

void check (bool ok, const std::string& preset, const std::string& what)
{
    ++checks;
    if (! ok)
    {
        ++failures;
        problems.push_back (preset + ": " + what);
    }
}

constexpr double kRate = 48000.0;
constexpr int kBlock = 512;
constexpr double kHeld = 2.0;            // seconds the notes are held
constexpr double kMaxTail = 12.0;        // seconds rendered after note-off at most

double toDb (double g) { return g > 1.0e-9 ? 20.0 * std::log10 (g) : -180.0; }

// ---- WAV writer (16-bit PCM stereo) -------------------------------------------
void put32 (std::FILE* f, uint32_t v) { uint8_t b[4] { (uint8_t) v, (uint8_t) (v >> 8), (uint8_t) (v >> 16), (uint8_t) (v >> 24) }; std::fwrite (b, 1, 4, f); }
void put16 (std::FILE* f, uint16_t v) { uint8_t b[2] { (uint8_t) v, (uint8_t) (v >> 8) }; std::fwrite (b, 1, 2, f); }

void writeWav (const std::string& path, const std::vector<float>& L, const std::vector<float>& R)
{
    std::FILE* f = std::fopen (path.c_str(), "wb");
    if (f == nullptr) return;
    const uint32_t frames = (uint32_t) L.size(), bytes = frames * 4;
    std::fwrite ("RIFF", 1, 4, f); put32 (f, 36 + bytes); std::fwrite ("WAVE", 1, 4, f);
    std::fwrite ("fmt ", 1, 4, f); put32 (f, 16); put16 (f, 1); put16 (f, 2);
    put32 (f, (uint32_t) kRate); put32 (f, (uint32_t) kRate * 4); put16 (f, 4); put16 (f, 16);
    std::fwrite ("data", 1, 4, f); put32 (f, bytes);
    std::vector<int16_t> buf ((size_t) frames * 2);
    for (uint32_t i = 0; i < frames; ++i)
    {
        buf[2 * i]     = (int16_t) std::lrint (std::clamp (L[i], -1.0f, 1.0f) * 32767.0f);
        buf[2 * i + 1] = (int16_t) std::lrint (std::clamp (R[i], -1.0f, 1.0f) * 32767.0f);
    }
    std::fwrite (buf.data(), 2, buf.size(), f);
    std::fclose (f);
}

// ---- What to play per category --------------------------------------------------
std::vector<int> notesFor (const FactoryPreset& p)
{
    const auto& c = p.category;
    if (c == "Bass") return { 36 };                       // C2 (65 Hz)
    if (c == "Lead") return { 60 };
    if (c == "Pad" || c == "Keys" || c == "Atmosphere") return { 48, 55, 60, 64 };   // C3 G3 C4 E4
    if (c == "Pluck") return { 60, 64, 67 };
    if (c == "Arp / Seq") return { 48, 51, 55, 58 };
    if (c == "Drum") return { 60 };
    return { 60 };                                        // FX, Init
}

struct Render
{
    std::vector<float> L, R;
    int noteOff = 0;
    double seconds = 0.0, cpu = 0.0;
    bool finite = true;
};

Render render (const FactoryPreset& preset, double held = kHeld, int macro = -1)
{
    auto patch = std::make_shared<Patch>();
    ParamValues pv;
    pv.resetToDefaults();
    preset.build (*patch, pv);
    if (macro >= 0)
    {
        const int id = ids().macro[macro];
        pv[id] = pv[id] < 0.5f ? 1.0f : 0.0f;
    }

    Synth synth;
    synth.prepare (kRate, kBlock);
    synth.setKeyboardSettings (patch->keyboard);
    synth.setPatch (CompiledPatch::compile (patch));

    const auto notes = notesFor (preset);
    Render r;
    r.noteOff = (int) (held * kRate);
    const int maxLen = r.noteOff + (int) (kMaxTail * kRate);
    std::vector<float> L (kBlock), R (kBlock);
    MidiBuffer out;
    HostInfo host;
    host.playing = true;
    host.bpm = 120.0;
    int pos = 0, quiet = 0;
    double busy = 0.0;
    while (pos < maxLen)
    {
        std::vector<MidiEvent> ev;
        if (pos == 0)
            for (int n : notes) ev.push_back (MidiEvent::noteOn (0, n, 100));
        if (pos <= r.noteOff && r.noteOff < pos + kBlock)
            for (int n : notes) ev.push_back (MidiEvent::noteOff (r.noteOff - pos, n));
        out.clear();
        const auto t0 = std::chrono::steady_clock::now();
        synth.process (L.data(), R.data(), kBlock, ev.data(), (int) ev.size(), pv, host, out);
        busy += std::chrono::duration<double> (std::chrono::steady_clock::now() - t0).count();
        host.ppq += kBlock / kRate * host.bpm / 60.0;
        float pk = 0.0f;
        for (int i = 0; i < kBlock; ++i)
        {
            if (! std::isfinite (L[(size_t) i]) || ! std::isfinite (R[(size_t) i])) r.finite = false;
            pk = std::max (pk, std::max (std::abs (L[(size_t) i]), std::abs (R[(size_t) i])));
            r.L.push_back (L[(size_t) i]);
            r.R.push_back (R[(size_t) i]);
        }
        pos += kBlock;
        // Stop once it has been silent (-100 dB) for 0.5 s after note-off.
        if (pos > r.noteOff) quiet = pk < 1.0e-5f ? quiet + kBlock : 0;
        if (quiet > (int) (0.5 * kRate)) break;
    }
    synth.collectGarbage();
    r.seconds = (double) r.L.size() / kRate;
    r.cpu = 100.0 * busy / r.seconds;
    return r;
}

// ---- Analysis -------------------------------------------------------------------
struct Analysis
{
    double peakDb = -180, rmsDb = -180, dc = 0, centroid = 0, lowFrac = 0;
    double attack = 0;          // s to reach -3 dB of the held maximum (10 ms RMS frames)
    double drop1s = 0;          // dB: level at 1.0 s relative to the held maximum
    double tail = 0;            // s after note-off until the last sample above -60 dBFS
    double f0 = 0;              // estimated fundamental (Hz), 0 = none found
    int onsets = 0;             // rises of > 6 dB within 30 ms in the held part (arp / gate steps)
    bool zero = true;
};

// Magnitude spectrum of a Hann window at `start` (mono), size n.
std::vector<double> spectrum (const Render& r, int start, int n)
{
    std::vector<float> re ((size_t) n), im ((size_t) n, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        const size_t k = (size_t) std::min ((int) r.L.size() - 1, start + i);
        const double w = 0.5 - 0.5 * std::cos (2.0 * 3.14159265358979 * i / (n - 1));
        re[(size_t) i] = (float) (0.5 * (r.L[k] + r.R[k]) * w);
    }
    fft (re.data(), im.data(), n, false);
    std::vector<double> m ((size_t) n / 2);
    for (int i = 0; i < n / 2; ++i) m[(size_t) i] = std::sqrt ((double) re[(size_t) i] * re[(size_t) i] + (double) im[(size_t) i] * im[(size_t) i]);
    return m;
}

// YIN-style fundamental estimate on the mono signal.
double pitchOf (const Render& r, int start, int n)
{
    if ((int) r.L.size() < start + 2 * n) return 0.0;
    std::vector<double> x ((size_t) (2 * n));
    for (int i = 0; i < 2 * n; ++i) x[(size_t) i] = 0.5 * (r.L[(size_t) (start + i)] + r.R[(size_t) (start + i)]);
    const int minLag = (int) (kRate / 2000.0), maxLag = std::min (n - 1, (int) (kRate / 25.0));
    std::vector<double> d ((size_t) maxLag + 1, 0.0);
    for (int lag = 1; lag <= maxLag; ++lag)
    {
        double s = 0.0;
        for (int i = 0; i < n; ++i) { const double e = x[(size_t) i] - x[(size_t) (i + lag)]; s += e * e; }
        d[(size_t) lag] = s;
    }
    double run = 0.0;
    std::vector<double> c ((size_t) maxLag + 1, 1.0);
    for (int lag = 1; lag <= maxLag; ++lag) { run += d[(size_t) lag]; c[(size_t) lag] = run > 0 ? d[(size_t) lag] * lag / run : 1.0; }
    for (int lag = minLag; lag < maxLag; ++lag)
        if (c[(size_t) lag] < 0.15)
        {
            while (lag + 1 < maxLag && c[(size_t) (lag + 1)] < c[(size_t) lag]) ++lag;
            // Parabolic refinement.
            const double a = c[(size_t) (lag - 1)], b = c[(size_t) lag], e = c[(size_t) (lag + 1)];
            const double den = a - 2 * b + e;
            const double off = std::abs (den) > 1e-12 ? 0.5 * (a - e) / den : 0.0;
            return kRate / (lag + off);
        }
    return 0.0;
}

Analysis analyse (const Render& r)
{
    Analysis a;
    double peak = 0.0, sumL = 0.0, sumR = 0.0;
    for (size_t i = 0; i < r.L.size(); ++i)
    {
        peak = std::max (peak, (double) std::max (std::abs (r.L[i]), std::abs (r.R[i])));
        sumL += r.L[i]; sumR += r.R[i];
        if (r.L[i] != 0.0f || r.R[i] != 0.0f) a.zero = false;
    }
    a.peakDb = toDb (peak);
    a.dc = std::max (std::abs (sumL), std::abs (sumR)) / (double) r.L.size();

    const int h1 = r.noteOff;
    // 10 ms RMS envelope over the held part.
    const int fr = (int) (0.01 * kRate);
    std::vector<double> env;
    for (int i = 0; i + fr <= h1; i += fr)
    {
        double s = 0.0;
        for (int k = i; k < i + fr; ++k) s += 0.5 * ((double) r.L[(size_t) k] * r.L[(size_t) k] + (double) r.R[(size_t) k] * r.R[(size_t) k]);
        env.push_back (std::sqrt (s / fr));
    }
    const double emax = env.empty() ? 0.0 : *std::max_element (env.begin(), env.end());
    for (size_t i = 0; i < env.size(); ++i)
        if (env[i] >= emax * 0.7071) { a.attack = (double) (i + 1) * 0.01; break; }
    // RMS while the sound is active: the 10 ms frames of the held part within
    // 30 dB of the loudest (so a short drum is measured over its hit, a pad
    // over its whole sustain).
    {
        double s2 = 0.0;
        int count = 0;
        for (double e : env)
            if (e >= emax * 0.0316) { s2 += e * e; ++count; }
        a.rmsDb = toDb (std::sqrt (s2 / std::max (1, count)));
    }
    {
        bool wasRising = false;
        for (size_t i = 3; i < env.size(); ++i)
        {
            const bool rising = env[i] > emax * 0.05 && env[i] > 2.0 * env[i - 3];
            if (rising && ! wasRising) ++a.onsets;
            wasRising = rising;
        }
    }
    const size_t at1 = (size_t) std::min ((int) env.size() - 1, 100);
    a.drop1s = toDb (env.empty() ? 0.0 : env[at1]) - toDb (emax);

    // Tail: last sample above -60 dBFS.
    int last = r.noteOff;
    for (int i = (int) r.L.size() - 1; i >= r.noteOff; --i)
        if (std::max (std::abs (r.L[(size_t) i]), std::abs (r.R[(size_t) i])) > 0.001f) { last = i; break; }
    a.tail = (double) (last - r.noteOff) / kRate;

    // Spectrum over the held part, averaged over a few windows.
    const int n = 8192;
    std::vector<double> mag ((size_t) n / 2, 0.0);
    int windows = 0;
    // Short sounds (more than 20 dB down after 1 s) are measured from the start.
    for (int s = a.drop1s < -20.0 ? 0 : (int) (0.2 * kRate); s + n <= h1 && (windows == 0 || a.drop1s >= -20.0); s += n / 2, ++windows)
    {
        const auto m = spectrum (r, s, n);
        for (size_t k = 0; k < m.size(); ++k) mag[k] += m[k] * m[k];
    }
    double tot = 0.0, wsum = 0.0, low = 0.0;
    for (int k = 1; k < n / 2; ++k)
    {
        const double f = k * kRate / n;
        tot += mag[(size_t) k];
        wsum += mag[(size_t) k] * f;
        if (f < 200.0) low += mag[(size_t) k];
    }
    a.centroid = tot > 0 ? wsum / tot : 0.0;
    a.lowFrac = tot > 0 ? low / tot : 0.0;
    a.f0 = pitchOf (r, (int) (0.5 * kRate), 8192);
    return a;
}

double midiHz (int n) { return 440.0 * std::pow (2.0, (n - 69) / 12.0); }

// Cents from `hz` to the nearest octave of `ref`.
double centsToOctave (double hz, double ref)
{
    double c = 1200.0 * std::log2 (hz / ref);
    c = std::fmod (c, 1200.0);
    if (c > 600.0) c -= 1200.0;
    if (c < -600.0) c += 1200.0;
    return c;
}

// Energy of (b - a) relative to a, in dB, over the shorter of the two.
double diffDb (const Render& a, const Render& b)
{
    const size_t n = std::min (a.L.size(), b.L.size());
    double e = 0.0, d = 0.0;
    for (size_t i = 0; i < n; ++i)
    {
        e += (double) a.L[i] * a.L[i] + (double) a.R[i] * a.R[i];
        const double x = (double) b.L[i] - a.L[i], y = (double) b.R[i] - a.R[i];
        d += x * x + y * y;
    }
    return 10.0 * std::log10 ((d + 1.0e-30) / (e + 1.0e-30));
}

// ---- Round trip -----------------------------------------------------------------
bool roundTrip (const FactoryPreset& preset, std::string& why)
{
    Patch p;
    ParamValues pv;
    pv.resetToDefaults();
    preset.build (p, pv);
    p.info.name = preset.name;
    p.info.category = preset.category;
    const auto bin1 = encodePreset (p, pv).toBinary();
    Node n;
    if (! Node::fromBinary (bin1.data(), bin1.size(), n)) { why = "fromBinary failed"; return false; }
    Patch q;
    ParamValues qv;
    qv.resetToDefaults();
    if (! decodePreset (n, q, qv)) { why = "decode failed"; return false; }
    const auto bin2 = encodePreset (q, qv).toBinary();
    if (bin1 != bin2) { why = "re-encode differs"; return false; }
    for (int i = 0; i < (int) pv.v.size(); ++i)
        if (pv[i] != qv[i])
        {
            why = "param " + params()[i].id + " " + std::to_string (pv[i]) + " -> " + std::to_string (qv[i]);
            return false;
        }
    return true;
}
} // namespace

int main (int argc, char** argv)
{
    const std::string only = argc > 1 ? argv[1] : "";
    const auto& list = factoryPresets();
    std::filesystem::create_directories ("/tmp/presets");

    // Library-level checks.
    check (! list.empty() && list.front().name == "Init", "library", "Init is first");
    {
        std::vector<std::string> names;
        for (auto& p : list) names.push_back (p.name);
        std::sort (names.begin(), names.end());
        check (std::adjacent_find (names.begin(), names.end()) == names.end(), "library", "names are unique");
        static const std::vector<std::string> cats { "Init", "Bass", "Lead", "Pad", "Pluck", "Keys", "Arp / Seq", "FX", "Atmosphere", "Drum" };
        for (auto& p : list)
        {
            check (std::find (cats.begin(), cats.end(), p.category) != cats.end(), p.name, "known category '" + p.category + "'");
            check (! p.description.empty() && p.description.back() == '.', p.name, "description is a sentence");
        }
    }

    std::printf ("%-20s %-11s %7s %7s %6s %6s %6s %6s %6s %7s %7s %6s %4s %6s\n",
                 "preset", "category", "peak", "rms", "atk s", "1s dB", "tail", "cent", "low%", "f0", "cents", "cpu%", "ons", "macros");
    double cpuTotal = 0.0, cpuMax = 0.0;
    int rendered = 0;
    for (const auto& preset : list)
    {
        if (! only.empty() && preset.name.find (only) == std::string::npos) continue;
        const auto& name = preset.name;
        const auto& cat = preset.category;

        std::string why;
        check (roundTrip (preset, why), name, "round trip: " + why);

        const Render r = render (preset);
        const Analysis a = analyse (r);
        ++rendered;
        cpuTotal += r.cpu;
        cpuMax = std::max (cpuMax, r.cpu);

        std::string file = name;
        for (auto& ch : file) if (ch == '/' || ch == ' ') ch = '_';
        writeWav ("/tmp/presets/" + file + ".wav", r.L, r.R);

        // Macros: each of MACRO 1-4 flipped (0 <-> 1) must change the sound
        // (difference energy above -30 dB of the base render, 1 s notes).
        std::string macroStr = "----";
        if (name != "Init")
        {
            const Render base = render (preset, 1.0);
            check (diffDb (base, render (preset, 1.0)) < -120.0, name, "renders are deterministic");
            int working = 0;
            for (int m = 0; m < 4; ++m)
                if (diffDb (base, render (preset, 1.0, m)) > -30.0) { macroStr[(size_t) m] = (char) ('1' + m); ++working; }
            check (working == 4, name, "macros with an audible effect: " + macroStr);
        }

        const auto notes = notesFor (preset);
        double cents = 0.0;
        const bool pitched = notes.size() == 1 && cat != "Drum" && cat != "FX";
        if (pitched && a.f0 > 0.0) cents = centsToOctave (a.f0, midiHz (notes[0]));

        std::printf ("%-20s %-11s %7.1f %7.1f %6.2f %6.1f %6.2f %6.0f %6.1f %7.1f %7.0f %6.2f %4d %6s\n",
                     name.c_str(), cat.c_str(), a.peakDb, a.rmsDb, a.attack, a.drop1s, a.tail, a.centroid,
                     100.0 * a.lowFrac, a.f0, cents, r.cpu, a.onsets, macroStr.c_str());

        // ---- Checks -----------------------------------------------------------
        check (r.finite, name, "no NaN/Inf");
        check (! a.zero, name, "not constant zero");
        check (a.dc < 0.05, name, "DC offset " + std::to_string (a.dc));
        check (a.peakDb <= toDb (0.95), name, "peak " + std::to_string (a.peakDb) + " dB clips");
        if (name != "Init")
        {
            check (a.peakDb >= -12.5 && a.peakDb <= -2.5, name, "peak " + std::to_string (a.peakDb) + " dB outside -12..-3");
            check (a.rmsDb >= -30.0, name, "active RMS " + std::to_string (a.rmsDb) + " dB too quiet");
        }
        check (a.tail < 10.0, name, "tail " + std::to_string (a.tail) + " s does not end within 10 s");
        if (cat == "Bass") check (a.lowFrac > 0.25, name, "bass: energy below 200 Hz " + std::to_string (a.lowFrac));
        if (cat == "Pad") check (a.attack >= 0.15, name, "pad: attack " + std::to_string (a.attack) + " s");
        if (cat == "Pluck") check (a.drop1s < -12.0, name, "pluck: level after 1 s only " + std::to_string (a.drop1s) + " dB down");
        if (cat == "Drum") check (a.drop1s < -20.0, name, "drum: level after 1 s only " + std::to_string (a.drop1s) + " dB down");
        if (cat == "Arp / Seq") check (a.onsets >= 4, name, "arp/seq: only " + std::to_string (a.onsets) + " note steps in 2 s");
        if (pitched && (cat == "Bass" || cat == "Lead"))
            check (a.f0 > 0.0 && std::abs (cents) < 35.0, name, "pitch: f0 " + std::to_string (a.f0) + " Hz, " + std::to_string (cents) + " cents");
    }

    std::printf ("\n%d presets rendered; CPU mean %.2f %%, max %.2f %% of one core (48 kHz, %d-sample blocks)\n",
                 rendered, rendered ? cpuTotal / rendered : 0.0, cpuMax, kBlock);
    for (auto& p : problems) std::printf ("  FAIL %s\n", p.c_str());
    std::printf ("%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
