// ShotsSerum.cpp — Serum 2 import: the menu entries, finding and decoding
// library files (a fake library with a `clm ` wavetable), and, when
// TINCTURE_SERUM_PRESETS names a folder of .SerumPreset files, importing one
// into the live processor (pages saved as PNGs) and converting the folder.
#include "Shots.h"
#include "SerumLibrary.h"
#include "Engine/Wavetable.h"

namespace
{
void put32 (juce::MemoryOutputStream& o, uint32_t v) { o.writeInt ((int) v); }

// A 16-bit mono WAV of `frames` cycles of 2048 samples with Serum's clm marker.
void writeTable (const juce::File& f, int frames)
{
    juce::MemoryOutputStream data;
    for (int fr = 0; fr < frames; ++fr)
        for (int i = 0; i < 2048; ++i)
        {
            const double ph = (double) i / 2048.0;
            const double sine = std::sin (juce::MathConstants<double>::twoPi * ph), saw = 2.0 * ph - 1.0;
            const double t = frames > 1 ? (double) fr / (frames - 1) : 0.0;
            data.writeShort ((short) juce::roundToInt (30000.0 * ((1.0 - t) * sine + t * saw)));
        }
    const juce::String clm = "<!>2048 10000000 wavetable (www.xferrecords.com)";
    juce::MemoryOutputStream o;
    o.write ("RIFF", 4);
    put32 (o, (uint32_t) (4 + 24 + 8 + clm.length() + (clm.length() & 1) + 8 + data.getDataSize()));
    o.write ("WAVE", 4);
    o.write ("fmt ", 4); put32 (o, 16); o.writeShort (1); o.writeShort (1); put32 (o, 44100); put32 (o, 88200); o.writeShort (2); o.writeShort (16);
    o.write ("clm ", 4); put32 (o, (uint32_t) clm.length()); o.write (clm.toRawUTF8(), (size_t) clm.length());
    if (clm.length() & 1) o.writeByte (0);
    o.write ("data", 4); put32 (o, (uint32_t) data.getDataSize()); o.write (data.getData(), data.getDataSize());
    f.getParentDirectory().createDirectory();
    f.replaceWithData (o.getData(), o.getDataSize());
}
} // namespace

TINCTURE_SHOT ("serum", [] (Shot& s)
{
    using namespace tincture;

    // Menu entries.
    s.click (s.panel, { 800, 30 });
    s.check (s.menuHas ("Import Serum 2 Preset..."), "preset menu: Import Serum 2 Preset");
    s.check (s.menuHas ("Import Serum 2 Folder..."), "preset menu: Import Serum 2 Folder");
    s.closeMenu();
    s.click (*s.button ("MENU"));
    s.check (s.menuHas ("Serum Library Folder..."), "main menu: Serum Library Folder");
    s.closeMenu();

    // A fake Serum library: Tables/Analog/Basic Shapes.wav (4 frames, clm 2048).
    const auto lib = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("tincture-serum-lib");
    lib.deleteRecursively();
    writeTable (lib.getChildFile ("Tables/Analog/Basic Shapes.wav"), 4);
    writeTable (lib.getChildFile ("Tables/User/Deep/ANALOG SAW.wav"), 1);
    SerumLibrary library (lib.getFullPathName());
    const juce::File preset = lib.getChildFile ("x.SerumPreset");
    const auto f1 = library.find ("/Analog/Basic Shapes.wav", true, preset);
    s.check (f1 == lib.getChildFile ("Tables/Analog/Basic Shapes.wav"), "relative path under Tables (leading slash)");
    s.check (library.find ("//Analog/Basic Shapes.wav", true, preset) == f1, "double leading slash");
    s.check (library.find ("Analog\\Basic Shapes.wav", true, preset) == f1, "backslashes");
    s.check (library.find ("/User/ANALOG SAW.wav", true, preset) == lib.getChildFile ("Tables/User/Deep/ANALOG SAW.wav"), "by name in the index");
    s.check (! library.find ("Nope/Missing.wav", true, preset).exists(), "missing file");
    const auto table = SerumLibrary::decodeTable (f1);
    s.check (table != nullptr && table->numFrames() == 4, "clm table decodes to 4 frames");
    auto assets = library.assetsFor (preset);
    s.check (assets.wavetable ("/Analog/Basic Shapes.wav") != nullptr, "assets: table found");
    s.check (assets.wavetable ("/Analog/Basic Shapes.wav") == assets.wavetable ("Analog/Basic Shapes.wav"), "assets: cached");
    lib.deleteRecursively();

    // Real presets, if a folder is given.
    const auto folder = juce::SystemStats::getEnvironmentVariable ("TINCTURE_SERUM_PRESETS", {});
    if (folder.isEmpty()) return;
    const juce::File dir (folder);
    const auto files = dir.findChildFiles (juce::File::findFiles, true, "*.SerumPreset");
    s.check (! files.isEmpty(), "corpus has presets");
    if (files.isEmpty()) return;

    juce::String summary;
    const auto first = files[0];
    s.check (TinctureProcessor::isSerumPreset (first), "detected as a Serum preset");
    s.check (s.proc.importSerumPreset (first, summary), "import " + first.getFileName());
    std::printf ("%s\n\n", summary.toRawUTF8());
    s.check (s.proc.presetName().isNotEmpty() && s.proc.presetName() != "Init", "preset named");
    s.check (! s.proc.currentPresetFile().exists(), "not tied to a Tincture file");
    s.play (40);
    for (auto page : { "OSC", "MIX", "FX", "MATRIX" })
    {
        s.click (*s.button (page));
        s.play (4);
        s.save (juce::String ("serum_") + page);
    }
    s.click (*s.button ("OSC"));

    // loadPreset() takes Serum files too.
    s.check (s.proc.loadPreset (files[files.size() - 1]), "loadPreset accepts .SerumPreset");

    // Folder conversion into User/Serum Imports/<folder>.
    const int n = s.proc.importSerumFolder (dir, summary);
    std::printf ("%s\n", summary.toRawUTF8());
    s.check (n == files.size(), "every preset converted");
    const auto out = Settings::userPresets().getChildFile ("Serum Imports").getChildFile (dir.getFileName());
    s.check (out.findChildFiles (juce::File::findFiles, true, juce::String ("*") + PresetStore::extension).size() == n, "files written");
    int listed = 0;
    for (const auto& e : s.proc.presets.all()) if (! e.factory && e.file.isAChildOf (out)) ++listed;
    s.check (listed == n, "converted presets show in the browser");
    // A converted file loads like any Tincture preset.
    const auto converted = out.findChildFiles (juce::File::findFiles, true, juce::String ("*") + PresetStore::extension);
    if (! converted.isEmpty()) s.check (s.proc.loadPreset (converted[0]), "converted preset loads");
    out.deleteRecursively();
    s.proc.presets.rescan();
});
