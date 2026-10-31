// SerumLibrary.h — finding the files a Serum 2 preset refers to (wavetables,
// noise and sample files) and decoding them, for serum::importPreset.
//
// Serum stores those as paths relative to its own library folders
// ("S2 Tables/Default Shapes.wav", "/Analog/Basic Shapes.wav",
// "Organics/AC hum1.wav", "Factory/Plucked/x.flac"). They are looked up in:
//   - the folder set with "Serum Library Folder..." (Settings::serumFolder),
//   - the standard Serum 2 and Serum 1 locations
//       macOS:   /Library/Audio/Presets/Xfer Records/Serum 2 Presets (and ~/Library/...),
//                .../Serum Presets (Serum 1)
//       Windows: Documents\Xfer\Serum 2 Presets, Documents\Xfer\Serum Presets
//   - the preset's own folder and the two above it (packs often ship their tables).
// First by relative path under Tables/, Noises/, Samples/ (and the library
// root), then by file name anywhere below those folders (an index built on
// first use).
//
// Message thread only.
#pragma once

#include "Model/SerumImport.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <map>

namespace tincture
{
class SerumLibrary
{
public:
    explicit SerumLibrary (juce::String extraFolder = {});

    // The lookups for a preset at `presetFile` (its folder is searched too).
    serum::Assets assetsFor (const juce::File& presetFile);

    // The standard locations that exist on this machine.
    static juce::Array<juce::File> standardRoots();
    juce::Array<juce::File> roots() const { return libraryRoots; }

    juce::File find (const std::string& relPath, bool table, const juce::File& presetFile);

    // A Serum wavetable file (2048-sample frames unless its `clm ` chunk says otherwise).
    static std::shared_ptr<const Wavetable> decodeTable (const juce::File&);

private:
    juce::Array<juce::File> libraryRoots;
    std::map<juce::String, juce::File> byName;      // lower-case file name -> file
    bool indexed = false;
    std::map<juce::String, std::shared_ptr<const Wavetable>> tableCache;
    std::map<juce::String, std::shared_ptr<const SampleData>> sampleCache;

    void buildIndex();
};
} // namespace tincture
