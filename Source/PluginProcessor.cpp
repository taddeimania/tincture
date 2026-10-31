#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginEditor.h"
#include "Engine/WaveImport.h"
#include "Engine/Wavetable.h"
#include "Model/FactoryPresets.h"
#include "Model/SerumImport.h"
#include "SerumLibrary.h"

using namespace tincture;

namespace
{
constexpr int kMaxHistory = 200;

std::vector<bool> maskFor (const std::set<int>& locked, const std::set<juce::String>& modules)
{
    const auto& reg = tincture::params();
    std::vector<bool> m ((size_t) reg.size(), false);
    for (int p : locked) if (p >= 0 && p < reg.size()) m[(size_t) p] = true;
    for (int i = 0; i < reg.size(); ++i)
        if (modules.count (juce::String (reg[i].group)) > 0) m[(size_t) i] = true;
    return m;
}
} // namespace

TinctureProcessor::TinctureProcessor()
    : AudioProcessor (BusesProperties().withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      params (*this),
      patchNow (std::make_shared<Patch>())
{
    for (auto& c : ccMap) c.store (-1);
    pv.resetToDefaults();
    oversample = juce::jlimit (1, 4, settings.defaultQuality());
    synth.setOversample (oversample);

    // A saved default preset replaces the init sound for new instances.
    if (Settings::defaultPreset().existsAsFile())
        loadPreset (Settings::defaultPreset());
    else
        publish();
    history.clear();
    historyPos = -1;
    recordHistory ("Open");
    edited = false;
    startTimerHz (30);
}

TinctureProcessor::~TinctureProcessor() { stopTimer(); }

bool TinctureProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    return l.getMainOutputChannelSet() == juce::AudioChannelSet::stereo();
}

void TinctureProcessor::prepareToPlay (double sampleRate, int block)
{
    pv.resetToDefaults();
    midiScratch.reserve (2048);
    synth.prepare (sampleRate, juce::jmax (block, 64));
    setLatencySamples (synth.latencySamples());
}

// ---------------------------------------------------------------------------
// Audio
// ---------------------------------------------------------------------------
void TinctureProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int n = buffer.getNumSamples();
    midiScratch.clear();

    // On-screen keyboard and wheels first, at the start of the block.
    MidiEvent ui;
    while (uiMidi.pop (ui)) midiScratch.push_back (ui);

    for (const auto meta : midi)
    {
        const auto m = meta.getMessage();
        if (m.getRawDataSize() < 1 || m.getRawDataSize() > 3) continue;
        const auto* d = m.getRawData();
        MidiEvent e;
        e.offset = juce::jlimit (0, juce::jmax (0, n - 1), meta.samplePosition);
        e.status = d[0];
        e.data1 = m.getRawDataSize() > 1 ? d[1] : 0;
        e.data2 = m.getRawDataSize() > 2 ? d[2] : 0;
        if ((e.status & 0xF0) == 0xB0)
        {
            // MIDI learn, then learned CCs move their parameter.
            const int target = learning.load();
            if (target >= 0)
            {
                for (auto& c : ccMap) if (c.load() == target) c.store (-1);
                ccMap[(size_t) (e.data1 & 127)].store (target);
                learning.store (-1);
            }
            const int p = ccMap[(size_t) (e.data1 & 127)].load();
            if (p >= 0)
                if (auto* h = params.host (p))
                    h->setValueNotifyingHost ((float) e.data2 / 127.0f);
        }
        midiScratch.push_back (e);
    }
    midi.clear();

    params.copyTo (pv);
    HostInfo host;
    if (auto* ph = getPlayHead())
        if (auto pos = ph->getPosition())
        {
            if (auto bpm = pos->getBpm()) host.bpm = *bpm;
            if (auto ppq = pos->getPpqPosition()) host.ppq = *ppq;
            host.playing = pos->getIsPlaying();
        }

    midiOutScratch.clear();
    auto* L = buffer.getWritePointer (0);
    auto* R = buffer.getNumChannels() > 1 ? buffer.getWritePointer (1) : L;
    synth.process (L, R, n, midiScratch.data(), (int) midiScratch.size(), pv, host, midiOutScratch);
    for (int c = 2; c < buffer.getNumChannels(); ++c) buffer.clear (c, 0, n);

    for (const auto& e : midiOutScratch)
    {
        const int size = (e.status & 0xF0) == 0xC0 || (e.status & 0xF0) == 0xD0 ? 2 : 3;
        const juce::uint8 raw[3] { e.status, e.data1, e.data2 };
        midi.addEvent (raw, size, juce::jlimit (0, n - 1, e.offset));
    }
}

// ---------------------------------------------------------------------------
// Patch and history
// ---------------------------------------------------------------------------
void TinctureProcessor::publish()
{
    synth.setPatch (CompiledPatch::compile (patchNow));
    synth.setKeyboardSettings (patchNow->keyboard);
    setLatencySamples (synth.latencySamples());
}

void TinctureProcessor::editPatch (const juce::String& label, const std::function<void (Patch&)>& fn)
{
    auto copy = std::make_shared<Patch> (*patchNow);
    fn (*copy);
    patchNow = copy;
    publish();
    edited = true;
    if (label.isNotEmpty()) recordHistory (label);
}

void TinctureProcessor::editAll (const juce::String& label, const std::function<void (Patch&, ParamValues&)>& fn)
{
    ParamValues values;
    values.resetToDefaults();
    for (int i = 0; i < params.size(); ++i) values[i] = params.get (i);
    auto copy = std::make_shared<Patch> (*patchNow);
    fn (*copy, values);
    for (int i = 0; i < params.size(); ++i)
        if (! same (values[i], params.get (i))) params.set (i, values[i]);
    patchNow = copy;
    publish();
    edited = true;
    if (label.isNotEmpty()) recordHistory (label);
}

void TinctureProcessor::commitHistory (const juce::String& label) { recordHistory (label); }

juce::MemoryBlock TinctureProcessor::snapshot() const
{
    ParamValues values;
    values.resetToDefaults();
    for (int i = 0; i < params.size(); ++i) values[i] = params.get (i);
    const auto bin = encodePreset (*patchNow, values).toBinary();
    return juce::MemoryBlock (bin.data(), bin.size());
}

void TinctureProcessor::restore (const juce::MemoryBlock& mb, bool keepLocks)
{
    Node root;
    if (! Node::fromBinary ((const uint8_t*) mb.getData(), mb.getSize(), root)) return;
    auto next = std::make_shared<Patch>();
    ParamValues values;
    if (! decodePreset (root, *next, values)) return;

    if (keepLocks)
    {
        const auto mask = maskFor (locked, lockedModules);
        for (int i = 0; i < params.size(); ++i)
            if (mask[(size_t) i]) values[i] = params.get (i);
        const char* oscGroups[] { "OSC A", "OSC B", "OSC C" };
        for (int o = 0; o < kNumOscs; ++o)
            if (lockedModules.count (oscGroups[o]))
            {
                next->wavetable[(size_t) o] = patchNow->wavetable[(size_t) o];
                next->sample[(size_t) o] = patchNow->sample[(size_t) o];
            }
        if (lockedModules.count ("NOISE")) next->noise = patchNow->noise;
        for (int l = 0; l < kNumLfos; ++l)
            if (lockedModules.count ("LFO " + juce::String (l + 1))) next->lfo[(size_t) l] = patchNow->lfo[(size_t) l];
        if (lockedModules.count ("ARP") || lockedModules.count ("Arp") || patchNow->arp.lockModule) next->arp = patchNow->arp;
        if (lockedModules.count ("CLIP") || lockedModules.count ("Clip") || patchNow->clips.lockModule) next->clips = patchNow->clips;
        if (patchNow->matrix.locked) next->matrix = patchNow->matrix;
    }
    for (int i = 0; i < params.size(); ++i) params.set (i, values[i]);
    patchNow = next;
    publish();
}

void TinctureProcessor::recordHistory (const juce::String& label)
{
    if (historyPos + 1 < (int) history.size())
        history.erase (history.begin() + historyPos + 1, history.end());
    history.push_back ({ label, snapshot() });
    if ((int) history.size() > kMaxHistory) history.erase (history.begin());
    historyPos = (int) history.size() - 1;
}

juce::String TinctureProcessor::undoLabel() const { return canUndo() ? history[(size_t) historyPos].label : juce::String(); }
juce::String TinctureProcessor::redoLabel() const { return canRedo() ? history[(size_t) historyPos + 1].label : juce::String(); }

void TinctureProcessor::undo()
{
    if (! canUndo()) return;
    --historyPos;
    restore (history[(size_t) historyPos].state, false);
    edited = true;
}

void TinctureProcessor::redo()
{
    if (! canRedo()) return;
    ++historyPos;
    restore (history[(size_t) historyPos].state, false);
    edited = true;
}

void TinctureProcessor::beginGesture (int p)
{
    params.beginGesture (p);
    ++gestureDepth;
}

void TinctureProcessor::endGesture (int p)
{
    params.endGesture (p);
    gestureDepth = juce::jmax (0, gestureDepth - 1);
    edited = true;
    recordHistory (juce::String (params.info (p).name));
}

void TinctureProcessor::setWithUndo (int p, float plain)
{
    if (same (params.get (p), params.info (p).snap (plain))) return;
    params.set (p, plain);
    edited = true;
    recordHistory (juce::String (params.info (p).name));
}

// ---------------------------------------------------------------------------
// Presets
// ---------------------------------------------------------------------------
void TinctureProcessor::initPatch()
{
    locked.clear();
    lockedModules.clear();
    auto keepMatrix = patchNow->matrix.locked ? patchNow->matrix : ModMatrixData();
    params.resetAll();
    patchNow = std::make_shared<Patch>();
    if (keepMatrix.locked) patchNow->matrix = keepMatrix;
    presetFile = juce::File();
    publish();
    edited = false;
    recordHistory ("Init Preset");
}

void TinctureProcessor::initModule (const juce::String& group)
{
    editAll ("Init " + group, [group] (Patch& p, ParamValues& v)
    {
        const auto& reg = tincture::params();
        for (int i = 0; i < reg.size(); ++i)
            if (juce::String (reg[i].group) == group) v[i] = reg[i].def;
        p.matrix.removeRowsForGroup (group.toStdString());
        const char* oscGroups[] { "OSC A", "OSC B", "OSC C" };
        for (int o = 0; o < kNumOscs; ++o)
            if (group == oscGroups[o])
            {
                p.wavetable[(size_t) o] = Patch().wavetable[(size_t) o];
                p.sample[(size_t) o] = SampleOscData();
            }
        if (group == "NOISE") p.noise = NoiseOscData();
        for (int l = 0; l < kNumLfos; ++l)
            if (group == "LFO " + juce::String (l + 1)) p.lfo[(size_t) l] = LfoData();
    });
}

bool TinctureProcessor::loadPresetData (const juce::MemoryBlock& mb, const juce::String& nameIfMissing)
{
    Node root;
    if (! Node::fromBinary ((const uint8_t*) mb.getData(), mb.getSize(), root)) return false;
    restore (mb, true);
    if (patchNow->info.name.empty() || patchNow->info.name == "Init")
        if (nameIfMissing.isNotEmpty()) patchNow->info.name = nameIfMissing.toStdString();
    edited = false;
    recordHistory ("Load " + presetName());
    return true;
}

bool TinctureProcessor::loadPreset (const juce::File& f)
{
    if (isSerumPreset (f))
    {
        juce::String summary;
        return importSerumPreset (f, summary);
    }
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb)) return false;
    // Name the patch after the file.
    Node root;
    if (! Node::fromBinary ((const uint8_t*) mb.getData(), mb.getSize(), root)) return false;
    if (! loadPresetData (mb, f.getFileNameWithoutExtension())) return false;
    auto copy = std::make_shared<Patch> (*patchNow);
    copy->info.name = f.getFileNameWithoutExtension().toStdString();
    patchNow = copy;
    presetFile = f;
    edited = false;
    return true;
}

bool TinctureProcessor::isSerumPreset (const juce::File& f)
{
    if (f.hasFileExtension (".SerumPreset")) return true;
    juce::FileInputStream in (f);
    char magic[9] {};
    return in.openedOk() && in.read (magic, 9) == 9 && serum::looksLikeSerumPreset ((const uint8_t*) magic, 9);
}

namespace
{
bool convertSerum (const juce::File& f, SerumLibrary& lib, Patch& p, ParamValues& v, serum::Report& report, juce::String& error)
{
    juce::MemoryBlock mb;
    if (! f.loadFileAsData (mb)) { error = "couldn't read the file"; return false; }
    std::string err;
    if (! serum::importPreset ((const uint8_t*) mb.getData(), mb.getSize(), p, v, lib.assetsFor (f), report, err))
    {
        error = err;
        return false;
    }
    if (p.info.name.empty()) p.info.name = f.getFileNameWithoutExtension().toStdString();
    return true;
}
} // namespace

bool TinctureProcessor::importSerumPreset (const juce::File& f, juce::String& summary)
{
    SerumLibrary lib (settings.serumFolder());
    Patch p;
    ParamValues v;
    serum::Report report;
    juce::String error;
    if (! convertSerum (f, lib, p, v, report, error))
    {
        summary = f.getFileName() + ": " + error;
        return false;
    }
    if (lib.roots().isEmpty() && ! report.missingFiles.empty())
        report.note ("no Serum library folder found: set one with Serum Library Folder... in the main menu");
    const auto bin = encodePreset (p, v).toBinary();
    if (! loadPresetData (juce::MemoryBlock (bin.data(), bin.size()), juce::String (p.info.name))) return false;
    presetFile = juce::File();
    summary = juce::String (report.summary());
    return true;
}

int TinctureProcessor::importSerumFolder (const juce::File& folder, juce::String& summary)
{
    SerumLibrary lib (settings.serumFolder());
    const auto dest = Settings::userPresets().getChildFile ("Serum Imports").getChildFile (juce::File::createLegalFileName (folder.getFileName()));
    int written = 0, failed = 0;
    juce::StringArray missing;
    for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*.SerumPreset", juce::File::findFiles))
    {
        const auto& f = entry.getFile();
        Patch p;
        ParamValues v;
        serum::Report report;
        juce::String error;
        if (! convertSerum (f, lib, p, v, report, error)) { ++failed; continue; }
        for (const auto& m : report.missingFiles) missing.addIfNotAlreadyThere (juce::String (m));
        const auto out = dest.getChildFile (f.getParentDirectory().getRelativePathFrom (folder))
                             .getChildFile (juce::File::createLegalFileName (f.getFileNameWithoutExtension()) + PresetStore::extension);
        out.getParentDirectory().createDirectory();
        const auto bin = encodePreset (p, v).toBinary();
        if (out.replaceWithData (bin.data(), bin.size())) ++written;
        else ++failed;
    }
    presets.rescan();
    summary = juce::String (written) + " preset" + (written == 1 ? "" : "s") + " converted into User/Serum Imports/" + dest.getFileName() + ".";
    if (failed > 0) summary << "\n" << failed << " couldn't be read.";
    if (! missing.isEmpty())
    {
        summary << "\n\n" << missing.size() << " referenced file" << (missing.size() == 1 ? " was" : "s were")
                << " not found (Tincture tables used instead):";
        for (int i = 0; i < juce::jmin (12, missing.size()); ++i) summary << "\n  " << missing[i];
        if (missing.size() > 12) summary << "\n  ... and " << (missing.size() - 12) << " more";
        if (lib.roots().isEmpty()) summary << "\n\nNo Serum library folder was found. Set one with Serum Library Folder... in the main menu.";
    }
    return written;
}

bool TinctureProcessor::savePreset (const juce::File& f)
{
    auto copy = std::make_shared<Patch> (*patchNow);
    copy->info.name = f.getFileNameWithoutExtension().toStdString();
    if (copy->info.author.empty()) copy->info.author = settings.artistName().toStdString();
    patchNow = copy;
    const auto mb = snapshot();
    f.getParentDirectory().createDirectory();
    if (! f.replaceWithData (mb.getData(), mb.getSize())) return false;
    presetFile = f;
    edited = false;
    presets.rescan();
    return true;
}

void TinctureProcessor::stepPreset (int delta)
{
    const auto& all = presets.all();
    if (all.empty()) return;
    int at = presetFile.existsAsFile() ? presets.indexOfFile (presetFile) : -1;
    if (at < 0)
        for (size_t i = 0; i < all.size(); ++i)
            if (all[i].factory && all[i].name == presetName()) { at = (int) i; break; }
    const int next = ((at < 0 ? (delta > 0 ? -1 : 0) : at) + delta + (int) all.size()) % (int) all.size();
    const auto& e = all[(size_t) next];
    if (e.factory)
    {
        Patch p;
        ParamValues v;
        v.resetToDefaults();
        factoryPresets()[(size_t) e.factoryIndex].build (p, v);
        p.info.name = e.name.toStdString();
        p.info.category = e.category.toStdString();
        p.info.author = "Tincture";
        const auto bin = encodePreset (p, v).toBinary();
        loadPresetData (juce::MemoryBlock (bin.data(), bin.size()), e.name);
        presetFile = juce::File();
    }
    else
        loadPreset (e.file);
}

void TinctureProcessor::revertToSaved()
{
    if (presetFile.existsAsFile()) loadPreset (presetFile);
}

void TinctureProcessor::saveAsDefault()
{
    const auto mb = snapshot();
    Settings::defaultPreset().replaceWithData (mb.getData(), mb.getSize());
}

// ---------------------------------------------------------------------------
// Module clipboard
// ---------------------------------------------------------------------------
void TinctureProcessor::copyModule (const juce::String& group, bool withMods)
{
    Node n ("module");
    n.set ("group", group.toStdString());
    const auto& reg = tincture::params();
    auto& pn = n.add ("params");
    for (int i = 0; i < reg.size(); ++i)
        if (juce::String (reg[i].group) == group)
            pn.set (reg[i].shortName + "|" + reg[i].id.substr (reg[i].id.find ('.') + 1), (double) params.get (i));
    const char* oscGroups[] { "OSC A", "OSC B", "OSC C" };
    for (int o = 0; o < kNumOscs; ++o)
        if (group == oscGroups[o])
        {
            patchNow->wavetable[(size_t) o].save (n.add ("wavetable"));
            patchNow->sample[(size_t) o].save (n.add ("sample"));
        }
    if (withMods)
    {
        auto& rows = n.add ("mods");
        for (const auto& r : patchNow->matrix.rows)
            if (r.dest >= 0 && juce::String (reg[r.dest].group) == group)
            {
                auto& rn = rows.add ("row");
                r.save (rn);
                rn.set ("destSuffix", reg[r.dest].id.substr (reg[r.dest].id.find ('.') + 1));
            }
    }
    const auto bin = n.toBinary();
    moduleClipboard = juce::MemoryBlock (bin.data(), bin.size());
    moduleClipboardGroup = group;
}

static juce::String moduleKind (const juce::String& g)
{
    if (g.startsWith ("OSC")) return "osc";
    if (g.startsWith ("FILTER")) return "filter";
    if (g.startsWith ("ENV")) return "env";
    if (g.startsWith ("LFO")) return "lfo";
    return g;
}

bool TinctureProcessor::canPasteModule (const juce::String& group) const
{
    return moduleClipboard.getSize() > 0 && moduleKind (group) == moduleKind (moduleClipboardGroup);
}

void TinctureProcessor::pasteModule (const juce::String& group)
{
    if (! canPasteModule (group)) return;
    Node n;
    if (! Node::fromBinary ((const uint8_t*) moduleClipboard.getData(), moduleClipboard.getSize(), n)) return;
    editAll ("Paste " + group, [&] (Patch& p, ParamValues& v)
    {
        const auto& reg = tincture::params();
        std::map<std::string, int> bySuffix;
        for (int i = 0; i < reg.size(); ++i)
            if (juce::String (reg[i].group) == group)
                bySuffix[reg[i].id.substr (reg[i].id.find ('.') + 1)] = i;
        if (auto* pn = n.child ("params"))
            for (const auto& [key, val] : pn->properties())
            {
                const auto suffix = key.substr (key.find ('|') + 1);
                if (auto it = bySuffix.find (suffix); it != bySuffix.end())
                    if (auto* d = std::get_if<double> (&val)) v[it->second] = reg[it->second].snap ((float) *d);
            }
        const char* oscGroups[] { "OSC A", "OSC B", "OSC C" };
        for (int o = 0; o < kNumOscs; ++o)
            if (group == oscGroups[o])
            {
                if (auto* w = n.child ("wavetable")) p.wavetable[(size_t) o].load (*w);
                if (auto* s = n.child ("sample")) { p.sample[(size_t) o].load (*s); p.sample[(size_t) o].rebuild(); }
            }
        if (auto* rows = n.child ("mods"))
            for (auto* rn : rows->children ("row"))
            {
                ModRow r;
                r.load (*rn);
                if (auto it = bySuffix.find (rn->str ("destSuffix")); it != bySuffix.end())
                {
                    r.dest = it->second;
                    p.matrix.add (r);
                }
            }
    });
}

// ---------------------------------------------------------------------------
// Locks and MIDI learn
// ---------------------------------------------------------------------------
void TinctureProcessor::setLocked (int p, bool on) { if (on) locked.insert (p); else locked.erase (p); }
bool TinctureProcessor::isModuleLocked (const juce::String& g) const { return lockedModules.count (g) > 0; }
void TinctureProcessor::setModuleLocked (const juce::String& g, bool on) { if (on) lockedModules.insert (g); else lockedModules.erase (g); }

void TinctureProcessor::clearMidiLearn (int p)
{
    for (auto& c : ccMap) if (c.load() == p) c.store (-1);
    if (learning.load() == p) learning.store (-1);
}

int TinctureProcessor::ccFor (int p) const
{
    for (int c = 0; c < 128; ++c) if (ccMap[(size_t) c].load() == p) return c;
    return -1;
}

// ---------------------------------------------------------------------------
// Keyboard
// ---------------------------------------------------------------------------
void TinctureProcessor::uiNoteOn (int note, float vel)
{
    MidiEvent e;
    e.offset = 0;
    e.status = 0x90;
    e.data1 = (uint8_t) juce::jlimit (0, 127, note);
    e.data2 = (uint8_t) juce::jlimit (1, 127, juce::roundToInt (vel * 127.0f));
    uiMidi.push (e);
}

void TinctureProcessor::uiNoteOff (int note)
{
    MidiEvent e;
    e.offset = 0;
    e.status = 0x80;
    e.data1 = (uint8_t) juce::jlimit (0, 127, note);
    e.data2 = 64;
    uiMidi.push (e);
}

void TinctureProcessor::uiController (int cc, int value)
{
    MidiEvent e;
    e.status = 0xB0;
    e.data1 = (uint8_t) (cc & 127);
    e.data2 = (uint8_t) juce::jlimit (0, 127, value);
    uiMidi.push (e);
}

void TinctureProcessor::uiPitchBend (float b)
{
    const int v = juce::jlimit (0, 16383, juce::roundToInt ((b + 1.0f) * 8192.0f));
    MidiEvent e;
    e.status = 0xE0;
    e.data1 = (uint8_t) (v & 127);
    e.data2 = (uint8_t) (v >> 7);
    uiMidi.push (e);
}

void TinctureProcessor::setQuality (int f)
{
    oversample = f >= 4 ? 4 : (f >= 2 ? 2 : 1);
    synth.setOversample (oversample);
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
std::shared_ptr<const SampleData> TinctureProcessor::decodeAudio (const juce::File& f)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
    if (r == nullptr || r->lengthInSamples <= 0) return nullptr;
    const int len = (int) juce::jmin<juce::int64> (r->lengthInSamples, (juce::int64) r->sampleRate * 600);
    juce::AudioBuffer<float> buf ((int) juce::jmin (2u, r->numChannels), len);
    r->read (&buf, 0, len, 0, true, buf.getNumChannels() > 1);
    auto s = std::make_shared<SampleData>();
    s->name = f.getFileNameWithoutExtension().toStdString();
    s->path = f.getFullPathName().toStdString();
    s->sampleRate = r->sampleRate;
    s->left.assign (buf.getReadPointer (0), buf.getReadPointer (0) + len);
    if (buf.getNumChannels() > 1)
    {
        s->right.assign (buf.getReadPointer (1), buf.getReadPointer (1) + len);
        if (s->right == s->left) s->right.clear();
    }
    const int root = rootNoteFromName (f.getFileName().toStdString());
    s->rootNote = root >= 0 ? root : 60;
    const auto& md = r->metadataValues;
    if (md.containsKey ("Loop0Start") && md.containsKey ("Loop0End"))
    {
        s->loopStart = md["Loop0Start"].getIntValue();
        s->loopEnd = md["Loop0End"].getIntValue();
    }
    return s;
}

bool TinctureProcessor::loadSampleFile (int osc, const juce::File& f)
{
    auto s = decodeAudio (f);
    if (s == nullptr || osc < 0 || osc >= kNumOscs) return false;
    editPatch ("Load Sample", [&] (Patch& p)
    {
        auto& d = p.sample[(size_t) osc];
        d.sample = s;
        d.rebuild ((int) params.get (ids().osc[osc].type) == 4);
    });
    return true;
}

bool TinctureProcessor::loadNoiseFile (const juce::File& f)
{
    auto s = decodeAudio (f);
    if (s == nullptr) return false;
    editPatch ("Load Noise", [&] (Patch& p) { p.noise.sample = s; });
    params.set (ids().noise.colour, 0.0f);
    return true;
}

bool TinctureProcessor::loadWavetableFile (int osc, const juce::File& f, int importMode)
{
    auto s = decodeAudio (f);
    if (s == nullptr || osc < 0 || osc >= kNumOscs) return false;
    const auto res = wtedit::importAudio (s->left.data(), s->frames(), s->sampleRate, (wtedit::ImportMethod) importMode, {}, s->name);
    if (! res.ok || res.set.frames.empty()) return false;
    std::vector<std::vector<float>> frames;
    for (const auto& fr : res.set.frames) frames.emplace_back (fr.begin(), fr.end());
    auto table = Wavetable::create (s->name, "User", std::move (frames),
                                    res.set.interpolate ? Wavetable::Morph::None : Wavetable::Morph::None, true);
    editPatch ("Load Wavetable", [&] (Patch& p) { p.wavetable[(size_t) osc].table = table; });
    return true;
}

// ---------------------------------------------------------------------------
// Host state
// ---------------------------------------------------------------------------
void TinctureProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    ParamValues values;
    values.resetToDefaults();
    for (int i = 0; i < params.size(); ++i) values[i] = params.get (i);
    Node root = encodePreset (*patchNow, values);
    auto& s = root.add ("session");
    std::vector<float> lk;
    for (int p : locked) lk.push_back ((float) p);
    s.set ("locked", lk);
    std::string mods;
    for (auto& m : lockedModules) { mods += m.toStdString(); mods += "\n"; }
    s.set ("lockedModules", mods);
    std::vector<float> cc;
    for (int c = 0; c < 128; ++c) cc.push_back ((float) ccMap[(size_t) c].load());
    s.set ("ccMap", cc);
    s.set ("quality", (double) oversample);
    s.set ("presetFile", presetFile.getFullPathName().toStdString());
    s.set ("edited", edited ? 1.0 : 0.0);
    s.set ("ui", uiState.toXmlString().toStdString());
    const auto bin = root.toBinary();
    dest.replaceAll (bin.data(), bin.size());
}

void TinctureProcessor::setStateInformation (const void* data, int size)
{
    Node root;
    if (! Node::fromBinary ((const uint8_t*) data, (size_t) size, root)) return;
    if (auto* s = root.child ("session"))
    {
        locked.clear();
        if (auto* lk = s->floats ("locked")) for (float p : *lk) locked.insert ((int) p);
        lockedModules.clear();
        for (auto& m : juce::StringArray::fromLines (juce::String (s->str ("lockedModules"))))
            if (m.isNotEmpty()) lockedModules.insert (m);
        if (auto* cc = s->floats ("ccMap"))
            for (size_t c = 0; c < 128 && c < cc->size(); ++c) ccMap[c].store ((int) (*cc)[c]);
        setQuality ((int) s->num ("quality", 1.0));
        presetFile = juce::File (juce::String (s->str ("presetFile")));
        if (auto xml = juce::parseXML (juce::String (s->str ("ui"))))
            uiState = juce::ValueTree::fromXml (*xml);
    }
    const auto bin = root.toBinary();
    restore (juce::MemoryBlock (bin.data(), bin.size()), false);
    edited = root.child ("session") != nullptr && root.child ("session")->flag ("edited");
    history.clear();
    historyPos = -1;
    recordHistory ("Open");
}

// ---------------------------------------------------------------------------
void TinctureProcessor::timerCallback()
{
    synth.collectGarbage();
    // Notes recorded into a clip land in the patch.
    ClipBank bank = patchNow->clips;
    if (synth.noteProcessor().drainRecorded (bank) > 0)
    {
        auto copy = std::make_shared<Patch> (*patchNow);
        copy->clips = std::move (bank);
        patchNow = copy;
        publish();
        edited = true;
    }
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new TinctureProcessor(); }
