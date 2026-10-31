// PresetStore.h — the preset library for the browser: factory sounds and the
// user's .tincture files, with their metadata. Message thread only.
#pragma once

#include <juce_core/juce_core.h>

#include <vector>

namespace tincture
{
struct PresetEntry
{
    juce::String name, category, author, description;
    juce::StringArray tags;
    int rating = 0;
    bool factory = false;
    int factoryIndex = -1;
    juce::File file;                  // user presets
    juce::String folder;              // sub-folder under the user folder ("" = top)
};

class PresetStore
{
public:
    static constexpr const char* extension = ".tincture";

    void rescan();
    const std::vector<PresetEntry>& all() { if (! scanned) rescan(); return entries; }
    int indexOfFile (const juce::File&) const;
    int indexOfFactory (int factoryIndex) const;
    juce::StringArray categories();   // every category in use, sorted
    void setRating (int entry, int rating);   // user presets: rewrites the file's metadata

private:
    std::vector<PresetEntry> entries;
    bool scanned = false;
};
} // namespace tincture
