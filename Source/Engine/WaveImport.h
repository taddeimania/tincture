// WaveImport.h — turning audio (and images) into wavetable frames, and frames
// back into audio for export (spec E §3.6, §3.7, §4). The plugin layer reads
// and writes the files (WAV/AIFF/PNG decoding, the .txt override files, the
// WAV `clm ` chunk); everything here works on plain float buffers.
//
// Stereo files: the plugin imports the left channel into OSC A and C and the
// right channel into OSC B (call importAudio once per channel).
// Every method yields 1..256 frames of 2048 samples. When a file holds more
// cycles than that, 256 cycles spaced evenly over the file are kept.
#pragma once

#include "WaveEdit.h"

#include <string>
#include <vector>

namespace tincture::wtedit
{
// ---------------------------------------------------------------------------
// Pitch detection (YIN, FFT-accelerated difference function)
// ---------------------------------------------------------------------------
struct PitchEstimate
{
    double period = 0.0;        // samples per cycle (fractional), 0 = none found
    float confidence = 0.0f;    // 1 - normalised difference at the chosen lag (0..1)
};

class PitchDetector
{
public:
    // Searches periods for minHz..maxHz. Window = the longest period.
    PitchDetector (double sampleRate, double minHz = 20.0, double maxHz = 3000.0);
    // Estimate at x[start ...]; needs window + maxLag samples (shorter input
    // just searches fewer lags).
    PitchEstimate detect (const float* x, int len, int start) const;
    int hopHint() const noexcept { return window / 2; }
    int windowSize() const noexcept { return window; }
    float threshold = 0.15f;

private:
    int minLag, maxLag, window, fftSize;
    Fft fft;
};

// A pitch map: estimates every `hop` samples (unvoiced spots filled in from
// their neighbours, then median-smoothed). period == 0 everywhere = no pitch.
struct PitchMap
{
    int hop = 0;
    std::vector<double> period;
    double at (double pos) const noexcept;        // interpolated period at a sample position
    double median() const;                        // average-pitch estimate
};
PitchMap buildPitchMap (const float* x, int len, double sampleRate, double minHz = 20.0, double maxHz = 3000.0);

// ---------------------------------------------------------------------------
// Overrides (§4.4, §4.5)
// ---------------------------------------------------------------------------
struct ImportOverride
{
    int samplesPerCycle = 0;    // 0 = none
    int interpolate = -1;       // -1 = not given, 0 = [no interp], 1 = [interp]
};
// Text file (name.txt or FolderInfo.txt) contents: "[<32..9999>]" and an
// optional "[interp]" / "[no interp]". Returns true if anything was found.
bool parseTextOverride (const std::string& text, ImportOverride& out);
// Formula field: a 1-4 digit sample count or a note name (B0, C#2, Db6) at
// 44.1 kHz, rounded to whole samples. Note names: octave numbering with C0 =
// MIDI 0, which reproduces the guide's example (2048 samples -> F1).
bool parseFieldOverride (const std::string& field, int& samples);
// The field echo, e.g. "split at: 2048 samples (22 Hz, note: F1 and -23 cents)".
std::string splitEcho (int samples);
std::string noteName (int midi);                           // C0 = 0 convention above
// Serum-style `clm ` chunk text ("<!>2048 ..."): frame size, or 0 if absent.
int parseClmChunk (const std::string& chunk);
std::string makeClmChunk (int frameSize = kFrameSize, bool interpolate = true);

// ---------------------------------------------------------------------------
// Audio import
// ---------------------------------------------------------------------------
enum class ImportMethod : int
{
    DynamicZeroSnap = 0, DynamicFollow, FixedFrameSize, PitchAverage, FrequencyEstimation,
    Fft256, Fft512, Fft1024, Fft2048
};

// IMPORT menu (§3.6), in order. singleCycle = "Audio to Single-Cycle".
struct ImportMenuItem { const char* label; ImportMethod method; bool singleCycle; };
const std::vector<ImportMenuItem>& importMenu();
// The drag-and-drop overlay targets (§4.1, minus the osc-type switches).
struct DropTarget { const char* group; const char* label; ImportMethod method; };
const std::vector<DropTarget>& dropTargets();

struct ImportResult
{
    bool ok = false;
    std::string error;
    FrameSet set;
    double samplesPerCycle = 0.0;  // the cycle length used (average for dynamic modes)
    double detectedHz = 0.0;
    int nearestNote = -1;          // MIDI note of detectedHz (Frequency Estimation)
    std::string fieldText;         // what to write into the Formula field (Pitch Average), else ""
};

// Mono input. `ov` (from the formula field or a .txt) forces fixed segments of
// that many samples for every method except the FFT ones; ov.interpolate sets
// FrameSet::interpolate.
ImportResult importAudio (const float* x, int len, double sampleRate, ImportMethod method,
                          const ImportOverride& ov = {}, const std::string& name = {});

// Single cycles: a whole file is one cycle, resampled exactly to 2048.
void importSingleCycle (const float* x, int len, Frame& out);
FrameSet importSingleCycles (const std::vector<std::vector<float>>& files, const std::string& name = {});

// Images (§4.3): `lum` is row-major height x width luminance 0..1 (the plugin
// decodes the PNG and converts colour to luminance). Each column is a frame
// (width capped at 256).
enum class ImageMode : int
{
    Waveform = 0,    // column top->bottom = time, resampled to 2048; luminance 0..1 -> amplitude 0..1 (black = silence)
    Bipolar,         // as Waveform, but luminance 0..1 -> -1..+1
    Spectral         // column bottom->top = harmonics 1..height (max 512), luminance = FFT bar height
};
FrameSet importImage (const float* lum, int width, int height, ImageMode mode = ImageMode::Waveform,
                      const std::string& name = {});

// ---------------------------------------------------------------------------
// Export (§3.7). The plugin writes the files.
// ---------------------------------------------------------------------------
struct ExportMenuItem { const char* label; int id; };
const std::vector<ExportMenuItem>& exportMenu();        // ids: 0 .256, 1 16-bit, 2 32-bit, 3 single cycles, 4 selection

// Frames first..last (inclusive, clamped) concatenated: mono, 2048 per frame.
std::vector<float> exportAudio (const FrameSet& s, int first = 0, int last = -1);
// One buffer per frame ("Export All As Single-Cycle Waves").
std::vector<std::vector<float>> exportSingleCycles (const FrameSet& s);
// 8-bit .256: each frame band-limited to 127 harmonics and decimated to 256
// unsigned bytes (128 = 0).
std::vector<uint8_t> export256 (const FrameSet& s);
std::string singleCycleFileName (const std::string& table, int index);   // "<table>_NNN.wav"
} // namespace tincture::wtedit
