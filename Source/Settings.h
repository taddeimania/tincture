// Settings.h — per-user preferences shared by every Tincture on the machine
// (GLOBAL page preferences, panel size, tooltips). User presets and banks
// live in folders beside the settings file:
//
//   macOS:   ~/Library/Application Support/Tincture/
//   Windows: %APPDATA%\Tincture (folder)
//   Linux:   ~/.config/Tincture/
//
// Message thread only.
#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

namespace tincture
{
struct SettingsStore
{
    SettingsStore();
    juce::PropertiesFile props;
};

class Settings
{
public:
    bool tooltips() const;            void setTooltips (bool);
    float defaultScale() const;       void setDefaultScale (float);     // panel size, 1.0 = 100 %
    bool doubleClickResets() const;   void setDoubleClickResets (bool); // else double-click types a value
    bool showModHalos() const;        void setShowModHalos (bool);
    bool loadMidiMapFromPreset() const; void setLoadMidiMapFromPreset (bool);
    bool presetChangeResetsArp() const; void setPresetChangeResetsArp (bool);
    juce::String artistName() const;  void setArtistName (const juce::String&);
    int defaultQuality() const;       void setDefaultQuality (int);
    // Serum import: an extra folder to search for Serum's wavetables and
    // samples (empty = only the standard Serum locations), and the folder
    // the import dialogs start in.
    juce::String serumFolder() const; void setSerumFolder (const juce::String&);
    juce::String lastSerumImport() const; void setLastSerumImport (const juce::String&);

    static juce::File folder();       // Application Support/Tincture
    static juce::File userPresets();  // .../Presets
    static juce::File userTables();   // .../Tables
    static juce::File userSamples();  // .../Samples
    static juce::File userShapes();   // .../LFO Shapes
    static juce::File userBanks();    // .../Banks (arp, clip, FX rack)
    static juce::File defaultPreset();// .../Default.tincture

private:
    juce::SharedResourcePointer<SettingsStore> store;
};
} // namespace tincture
