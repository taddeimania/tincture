// SampleData.h — audio held in memory for the sample-based oscillator modes
// and the noise oscillator. Immutable once built: shared between the patch,
// the voices and the UI through shared_ptr<const SampleData>.
#pragma once

#include "../Model/Tree.h"

#include <memory>
#include <string>
#include <vector>

namespace tincture
{
struct SampleData
{
    std::string name;                   // shown in the UI
    std::string path;                   // file it came from ("" for generated)
    std::string category;               // factory folder ("Analog", "Color", ...)
    double sampleRate = 48000.0;
    int rootNote = 60;                  // MIDI note it plays at its own rate
    std::vector<float> left, right;     // right empty = mono
    int loopStart = -1, loopEnd = -1;   // from file metadata, -1 = none
    std::vector<int> slices;            // slice start frames (empty = none)
    bool embedded = false;              // saved inside the preset

    int frames() const noexcept { return (int) left.size(); }
    bool stereo() const noexcept { return ! right.empty(); }
    const float* channel (int c) const noexcept { return c == 1 && stereo() ? right.data() : left.data(); }

    // Embedded samples save their audio; others save the path (and name).
    void save (Node& n) const;
    static std::shared_ptr<const SampleData> load (const Node& n);
};

// Parse a root note from a file name ("Pad C3.wav", "bass_F#2", "lead-60"),
// or -1 if none. C3 = MIDI 60 (Ableton convention: C-2 = 0).
int rootNoteFromName (const std::string& fileName);
} // namespace tincture
