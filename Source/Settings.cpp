#include "Settings.h"

namespace tincture
{
static juce::PropertiesFile::Options options()
{
    juce::PropertiesFile::Options o;
    o.applicationName = "Tincture";
    o.filenameSuffix = ".settings";
    o.osxLibrarySubFolder = "Application Support";
    o.folderName = "Tincture";
    o.storageFormat = juce::PropertiesFile::storeAsXML;
    return o;
}

SettingsStore::SettingsStore() : props (options()) {}

#define TINCTURE_BOOL(getter, setter, key, def) \
    bool Settings::getter() const { return store->props.getBoolValue (key, def); } \
    void Settings::setter (bool v) { store->props.setValue (key, v); store->props.saveIfNeeded(); }

TINCTURE_BOOL (tooltips, setTooltips, "tooltips", true)
TINCTURE_BOOL (doubleClickResets, setDoubleClickResets, "doubleClickResets", false)
TINCTURE_BOOL (showModHalos, setShowModHalos, "modHalos", true)
TINCTURE_BOOL (loadMidiMapFromPreset, setLoadMidiMapFromPreset, "loadMidiMap", false)
TINCTURE_BOOL (presetChangeResetsArp, setPresetChangeResetsArp, "presetResetsArp", true)
#undef TINCTURE_BOOL

float Settings::defaultScale() const { return (float) store->props.getDoubleValue ("scale", 1.0); }
void Settings::setDefaultScale (float s) { store->props.setValue ("scale", (double) s); store->props.saveIfNeeded(); }
juce::String Settings::artistName() const { return store->props.getValue ("artist", {}); }
void Settings::setArtistName (const juce::String& s) { store->props.setValue ("artist", s); store->props.saveIfNeeded(); }
int Settings::defaultQuality() const { return store->props.getIntValue ("quality", 2); /* High */ }
void Settings::setDefaultQuality (int q) { store->props.setValue ("quality", q); store->props.saveIfNeeded(); }

juce::String Settings::serumFolder() const { return store->props.getValue ("serumFolder", {}); }
void Settings::setSerumFolder (const juce::String& s) { store->props.setValue ("serumFolder", s); store->props.saveIfNeeded(); }
juce::String Settings::lastSerumImport() const { return store->props.getValue ("lastSerumImport", {}); }
void Settings::setLastSerumImport (const juce::String& s) { store->props.setValue ("lastSerumImport", s); store->props.saveIfNeeded(); }

juce::File Settings::folder() { return options().getDefaultFile().getParentDirectory(); }
static juce::File sub (const char* n) { auto f = Settings::folder().getChildFile (n); f.createDirectory(); return f; }
juce::File Settings::userPresets() { return sub ("Presets"); }
juce::File Settings::userTables()  { return sub ("Tables"); }
juce::File Settings::userSamples() { return sub ("Samples"); }
juce::File Settings::userShapes()  { return sub ("LFO Shapes"); }
juce::File Settings::userBanks()   { return sub ("Banks"); }
juce::File Settings::defaultPreset() { return folder().getChildFile ("Default.tincture"); }
} // namespace tincture
