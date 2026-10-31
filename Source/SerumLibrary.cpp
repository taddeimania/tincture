// SerumLibrary.cpp — see SerumLibrary.h.
#include "SerumLibrary.h"
#include "PluginProcessor.h"
#include "Engine/WaveImport.h"

namespace tincture
{
namespace
{
juce::String cleanRelative (const std::string& rel)
{
    // Serum writes "/Analog/x.wav", "//Analog/x.wav" and Windows separators.
    juce::String r = juce::String (rel).replaceCharacter ('\\', '/');
    while (r.startsWithChar ('/')) r = r.substring (1);
    return r;
}

const char* const kSubFolders[] { "Tables", "Noises", "Samples", "Samples/Factory Non-Tonal/Noises", "Samples/Factory Non-Tonal", "" };

// Frame size from a WAV file's `clm ` chunk (Serum's wavetable marker); 0 if none.
int clmFrameSize (const juce::File& f)
{
    juce::FileInputStream in (f);
    if (! in.openedOk()) return 0;
    char riff[12];
    if (in.read (riff, 12) != 12 || std::memcmp (riff, "RIFF", 4) != 0 || std::memcmp (riff + 8, "WAVE", 4) != 0) return 0;
    while (! in.isExhausted())
    {
        char id[4];
        if (in.read (id, 4) != 4) break;
        const auto size = (juce::int64) (juce::uint32) in.readInt();
        if (std::memcmp (id, "clm ", 4) == 0)
        {
            juce::MemoryBlock mb;
            in.readIntoMemoryBlock (mb, (ssize_t) juce::jmin<juce::int64> (size, 256));
            return wtedit::parseClmChunk (mb.toString().toStdString());
        }
        if (! in.setPosition (in.getPosition() + size + (size & 1))) break;
    }
    return 0;
}
} // namespace

SerumLibrary::SerumLibrary (juce::String extraFolder)
{
    if (extraFolder.isNotEmpty())
    {
        const juce::File f (extraFolder);
        if (f.isDirectory()) libraryRoots.add (f);
    }
    for (const auto& r : standardRoots()) libraryRoots.addIfNotAlreadyThere (r);
}

juce::Array<juce::File> SerumLibrary::standardRoots()
{
    juce::Array<juce::File> out;
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    const auto docs = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory);
    const juce::File candidates[] {
        juce::File ("/Library/Audio/Presets/Xfer Records/Serum 2 Presets"),
        home.getChildFile ("Library/Audio/Presets/Xfer Records/Serum 2 Presets"),
        docs.getChildFile ("Xfer/Serum 2 Presets"),
        juce::File ("/Library/Audio/Presets/Xfer Records/Serum Presets"),
        home.getChildFile ("Library/Audio/Presets/Xfer Records/Serum Presets"),
        docs.getChildFile ("Xfer/Serum Presets"),
    };
    for (const auto& c : candidates)
        if (c.isDirectory()) out.addIfNotAlreadyThere (c);
    return out;
}

void SerumLibrary::buildIndex()
{
    indexed = true;
    for (const auto& root : libraryRoots)
        for (const char* sub : { "Tables", "Noises", "Samples" })
        {
            const auto dir = root.getChildFile (sub);
            if (! dir.isDirectory()) continue;
            for (const auto& entry : juce::RangedDirectoryIterator (dir, true, "*.wav;*.flac;*.aif;*.aiff", juce::File::findFiles))
            {
                const auto key = entry.getFile().getFileName().toLowerCase();
                if (byName.find (key) == byName.end()) byName[key] = entry.getFile();
            }
        }
}

juce::File SerumLibrary::find (const std::string& relPath, bool table, const juce::File& presetFile)
{
    const auto rel = cleanRelative (relPath);
    if (rel.isEmpty()) return {};

    // 1. By relative path under each library folder.
    for (const auto& root : libraryRoots)
        for (const char* sub : kSubFolders)
        {
            if (table && juce::String (sub).startsWith ("Samples")) continue;
            const auto f = (juce::String (sub).isEmpty() ? root : root.getChildFile (sub)).getChildFile (rel);
            if (f.existsAsFile()) return f;
        }

    // 2. Beside the preset: the preset's folder and the two above it, and any
    //    Tables / Wavetables / Noises / Samples folder in them (packs ship their files so).
    const auto name = rel.fromLastOccurrenceOf ("/", false, false);
    const auto home = juce::File::getSpecialLocation (juce::File::userHomeDirectory);
    auto dir = presetFile.getParentDirectory();
    for (int up = 0; up < 3 && dir.isDirectory() && dir != home && ! dir.isRoot(); ++up, dir = dir.getParentDirectory())
    {
        if (const auto f = dir.getChildFile (name); f.existsAsFile()) return f;
        for (const char* sub : { "Tables", "Wavetables", "Noises", "Samples" })
        {
            const auto d = dir.getChildFile (sub);
            if (! d.isDirectory()) continue;
            const auto found = d.findChildFiles (juce::File::findFiles, true, name);
            if (! found.isEmpty()) return found.getFirst();
        }
    }

    // 3. By name anywhere in the libraries.
    if (! indexed) buildIndex();
    if (const auto it = byName.find (name.toLowerCase()); it != byName.end()) return it->second;
    return {};
}

std::shared_ptr<const Wavetable> SerumLibrary::decodeTable (const juce::File& f)
{
    auto s = TinctureProcessor::decodeAudio (f);
    if (s == nullptr || s->frames() <= 0) return nullptr;
    wtedit::ImportOverride ov;
    const int clm = clmFrameSize (f);
    ov.samplesPerCycle = clm > 0 ? clm : 2048;
    // A file shorter than one frame is a single cycle.
    const auto method = s->frames() < ov.samplesPerCycle ? wtedit::ImportMethod::DynamicZeroSnap : wtedit::ImportMethod::FixedFrameSize;
    const auto res = wtedit::importAudio (s->left.data(), s->frames(), s->sampleRate, method, ov, s->name);
    if (! res.ok || res.set.frames.empty()) return nullptr;
    std::vector<std::vector<float>> frames;
    for (const auto& fr : res.set.frames) frames.emplace_back (fr.begin(), fr.end());
    return Wavetable::create (s->name, "Serum", std::move (frames), Wavetable::Morph::None, true);
}

serum::Assets SerumLibrary::assetsFor (const juce::File& presetFile)
{
    serum::Assets a;
    a.wavetable = [this, presetFile] (const std::string& rel) -> std::shared_ptr<const Wavetable>
    {
        const auto key = cleanRelative (rel).toLowerCase();
        if (const auto it = tableCache.find (key); it != tableCache.end()) return it->second;
        std::shared_ptr<const Wavetable> t;
        const auto f = find (rel, true, presetFile);
        if (f.existsAsFile()) t = decodeTable (f);
        tableCache[key] = t;
        return t;
    };
    a.sample = [this, presetFile] (const std::string& rel) -> std::shared_ptr<const SampleData>
    {
        const auto key = cleanRelative (rel).toLowerCase();
        if (const auto it = sampleCache.find (key); it != sampleCache.end()) return it->second;
        std::shared_ptr<const SampleData> s;
        const auto f = find (rel, false, presetFile);
        if (f.existsAsFile()) s = TinctureProcessor::decodeAudio (f);
        sampleCache[key] = s;
        return s;
    };
    return a;
}
} // namespace tincture
