#include "PresetStore.h"
#include "Settings.h"
#include "Model/FactoryPresets.h"

namespace tincture
{
static bool readInfo (const juce::File& f, PresetEntry& e)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb)) return false;
    Node root;
    if (! Node::fromBinary ((const uint8_t*) mb.getData(), mb.getSize(), root)) return false;
    const Node* info = nullptr;
    if (auto* p = root.child ("patch")) info = p->child ("info");
    e.name = f.getFileNameWithoutExtension();
    if (info != nullptr)
    {
        e.category = info->str ("category");
        e.author = info->str ("author");
        e.description = info->str ("description");
        e.rating = info->integer ("rating");
        e.tags = juce::StringArray::fromTokens (juce::String (info->str ("tags")), ",", "");
        e.tags.removeEmptyStrings();
    }
    return true;
}

void PresetStore::rescan()
{
    entries.clear();
    const auto& f = factoryPresets();
    for (size_t i = 0; i < f.size(); ++i)
    {
        PresetEntry e;
        e.name = f[i].name;
        e.category = f[i].category;
        e.description = f[i].description;
        e.author = "Tincture";
        for (auto& t : f[i].tags) e.tags.add (t);
        e.factory = true;
        e.factoryIndex = (int) i;
        entries.push_back (e);
    }
    const auto root = Settings::userPresets();
    for (const auto& file : root.findChildFiles (juce::File::findFiles, true, juce::String ("*") + extension))
    {
        PresetEntry e;
        if (! readInfo (file, e)) continue;
        e.file = file;
        e.folder = file.getParentDirectory() == root ? juce::String() : file.getParentDirectory().getRelativePathFrom (root);
        entries.push_back (e);
    }
    scanned = true;
}

int PresetStore::indexOfFile (const juce::File& f) const
{
    for (size_t i = 0; i < entries.size(); ++i) if (! entries[i].factory && entries[i].file == f) return (int) i;
    return -1;
}

int PresetStore::indexOfFactory (int fi) const
{
    for (size_t i = 0; i < entries.size(); ++i) if (entries[i].factory && entries[i].factoryIndex == fi) return (int) i;
    return -1;
}

juce::StringArray PresetStore::categories()
{
    juce::StringArray c;
    for (auto& e : all()) if (e.category.isNotEmpty()) c.addIfNotAlreadyThere (e.category);
    c.sort (true);
    return c;
}

void PresetStore::setRating (int i, int rating)
{
    if (i < 0 || i >= (int) entries.size()) return;
    auto& e = entries[(size_t) i];
    e.rating = rating;
    if (e.factory) return;
    juce::MemoryBlock mb;
    if (! e.file.loadFileAsData (mb)) return;
    Node root;
    if (! Node::fromBinary ((const uint8_t*) mb.getData(), mb.getSize(), root)) return;
    for (auto& c : root.allChildren())
        if (c.type() == "patch")
            for (auto& inf : c.allChildren())
                if (inf.type() == "info") inf.set ("rating", (double) rating);
    const auto bin = root.toBinary();
    e.file.replaceWithData (bin.data(), bin.size());
}
} // namespace tincture
