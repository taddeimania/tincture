// OscModules.cpp — the OSC page's module row, module identity, the module
// (label) menu, routing codes, file import helpers and a small SFZ reader.
#include "OscModules.h"

namespace tincture::ui
{
using namespace juce;

namespace oscui
{
// ===========================================================================
// ModuleRef
// ===========================================================================
String ModuleRef::group() const
{
    switch (kind)
    {
        case Kind::Osc:    return "OSC " + String::charToString ((juce_wchar) ('A' + index));
        case Kind::Sub:    return "SUB";
        case Kind::Noise:  return "NOISE";
        case Kind::Filter: return "FILTER " + String (index + 1);
    }
    return {};
}

String ModuleRef::title() const { return group(); }

String ModuleRef::letter() const
{
    switch (kind)
    {
        case Kind::Osc:    return String::charToString ((juce_wchar) ('A' + index));
        case Kind::Sub:    return "Sub";
        case Kind::Noise:  return "Noise";
        case Kind::Filter: return "Filter " + String (index + 1);
    }
    return {};
}

int ModuleRef::enableParam() const
{
    switch (kind)
    {
        case Kind::Osc:    return ids().osc[index].enable;
        case Kind::Sub:    return ids().sub.enable;
        case Kind::Noise:  return ids().noise.enable;
        case Kind::Filter: return ids().filter[index].enable;
    }
    return -1;
}

int ModuleRef::routeParam() const
{
    switch (kind)
    {
        case Kind::Osc:    return ids().osc[index].route;
        case Kind::Sub:    return ids().sub.route;
        case Kind::Noise:  return ids().noise.route;
        case Kind::Filter: return ids().filter[index].route;
    }
    return -1;
}

int ModuleRef::balanceParam() const
{
    switch (kind)
    {
        case Kind::Osc:    return ids().osc[index].filterBal;
        case Kind::Sub:    return ids().sub.filterBal;
        case Kind::Noise:  return ids().noise.filterBal;
        case Kind::Filter: return -1;
    }
    return -1;
}

int ModuleRef::bus1Param() const
{
    switch (kind)
    {
        case Kind::Osc:    return ids().osc[index].bus1;
        case Kind::Sub:    return ids().sub.bus1;
        case Kind::Noise:  return ids().noise.bus1;
        case Kind::Filter: return ids().filter[index].bus1;
    }
    return -1;
}

int ModuleRef::bus2Param() const
{
    switch (kind)
    {
        case Kind::Osc:    return ids().osc[index].bus2;
        case Kind::Sub:    return ids().sub.bus2;
        case Kind::Noise:  return ids().noise.bus2;
        case Kind::Filter: return ids().filter[index].bus2;
    }
    return -1;
}

ModSource ModuleRef::source() const
{
    switch (kind)
    {
        case Kind::Osc:    return (ModSource) ((int) ModSource::OscA + index);
        case Kind::Sub:    return ModSource::Sub;
        case Kind::Noise:  return ModSource::Noise;
        case Kind::Filter: return index == 0 ? ModSource::Filter1 : ModSource::Filter2;
    }
    return ModSource::None;
}

Colour ModuleRef::accent() const { return kind == Kind::Filter ? filterAccent() : colours::brand; }

// ===========================================================================
// Module menu (spec A 2.5)
// ===========================================================================
Menu moduleMenu (PanelContext& ctx, ModuleRef ref)
{
    auto& proc = ctx.proc;
    const int en = ref.enableParam();
    const String group = ref.group();
    Menu m;
    m.header (params()[en].name);
    m.add ("Reset Control", [&proc, en] { proc.setWithUndo (en, params()[en].def); });
    if (proc.midiLearnTarget() == en)
        m.add ("Cancel MIDI Learn", [&proc, en] { proc.clearMidiLearn (en); });
    else
        m.add ("MIDI Learn", [&proc, en] { proc.startMidiLearn (en); });
    if (proc.ccFor (en) >= 0)
        m.add ("Clear MIDI Learn (CC " + String (proc.ccFor (en)) + ")", [&proc, en] { proc.clearMidiLearn (en); });
    m.toggle ("Lock Module", proc.isModuleLocked (group), [&proc, group] { proc.setModuleLocked (group, ! proc.isModuleLocked (group)); });
    m.add ("Init Module", [&proc, group] { proc.initModule (group); });
    m.separator();
    m.add ("Copy", [&proc, group] { proc.copyModule (group, false); });
    m.add ("Copy (w/mods)", [&proc, group] { proc.copyModule (group, true); });
    m.add ("Paste", [&proc, group] { proc.pasteModule (group); }, false, proc.canPasteModule (group));

    int track = -1, bend = -1;
    bool sampleType = false;
    if (ref.kind == Kind::Osc)
    {
        track = ids().osc[ref.index].pitchTrack;
        bend = ids().osc[ref.index].bendTrack;
        const int t = proc.params.geti (ids().osc[ref.index].type);
        sampleType = t >= 2 && t <= 4;
    }
    else if (ref.kind == Kind::Sub)   track = ids().sub.pitchTrack;
    else if (ref.kind == Kind::Noise) track = ids().noise.pitchTrack;
    if (track >= 0)
    {
        m.separator();
        m.toggle ("Enable Pitch Tracking", proc.params.on (track), [&proc, track] { proc.setWithUndo (track, proc.params.on (track) ? 0.0f : 1.0f); });
        if (bend >= 0 && ! proc.params.on (track) && sampleType)
            m.toggle ("Enable Pitch Bend Tracking", proc.params.on (bend), [&proc, bend] { proc.setWithUndo (bend, proc.params.on (bend) ? 0.0f : 1.0f); });
    }
    return m;
}

String routeCode (const TinctureProcessor& proc, ModuleRef ref)
{
    const int r = proc.params.geti (ref.routeParam());
    if (ref.kind == Kind::Filter)
    {
        switch (r)
        {
            case 0:  return ref.index == 0 ? "F2" : "F1";
            case 1:  return "M";
            case 2:  return "D";
            default: return "-";
        }
    }
    switch (r)
    {
        case 0:
        {
            const float b = proc.params.get (ref.balanceParam());
            return b <= 0.001f ? "F1" : (b >= 0.999f ? "F2" : "F1/2");
        }
        case 1:  return "M";
        case 2:  return "D";
        default: return "-";
    }
}

// ===========================================================================
// Files
// ===========================================================================
bool isAudioFile (const String& path)
{
    const auto ext = File (path).getFileExtension().toLowerCase();
    return ext == ".wav" || ext == ".aif" || ext == ".aiff" || ext == ".flac" || ext == ".ogg" || ext == ".mp3";
}

bool loadFileInto (TinctureProcessor& proc, int osc, const File& f)
{
    const int type = proc.params.geti (ids().osc[osc].type);
    if (type == 0) return proc.loadWavetableFile (osc, f);
    if (type == 1)
    {
        std::shared_ptr<const MultiSample> ms;
        if (f.hasFileExtension (".sfz")) ms = loadSfz (f);
        else if (auto s = TinctureProcessor::decodeAudio (f))
            ms = std::make_shared<const MultiSample> (MultiSample::fromFiles (f.getFileNameWithoutExtension().toStdString(),
                                                                              { { f.getFileName().toStdString(), s } }));
        if (ms == nullptr || ms->zones.empty()) return false;
        proc.editPatch ("Load Instrument", [osc, ms] (Patch& p)
        {
            auto& d = p.sample[(size_t) osc];
            d.multi = ms;
            d.lastPlayedZone = -1;
            d.rebuild();
        });
        return true;
    }
    return proc.loadSampleFile (osc, f);
}

// ===========================================================================
// SFZ
// ===========================================================================
namespace
{
int sfzKey (const String& v)
{
    const auto t = v.trim();
    if (t.isEmpty()) return -1;
    if (t.containsOnly ("-0123456789")) return jlimit (0, 127, t.getIntValue());
    static const int base[] { 9, 11, 0, 2, 4, 5, 7 };   // a b c d e f g
    const auto c = CharacterFunctions::toLowerCase (t[0]);
    if (c < 'a' || c > 'g') return -1;
    int n = base[c - 'a'];
    int i = 1;
    if (t[i] == '#') { ++n; ++i; }
    else if (t[i] == 'b' && t.length() > 2) { --n; ++i; }
    const int oct = t.substring (i).getIntValue();
    return jlimit (0, 127, (oct + 1) * 12 + n);              // SFZ: c4 = 60
}
}

std::shared_ptr<const MultiSample> loadSfz (const File& file)
{
    StringArray lines;
    file.readLines (lines);
    // Tokenise: headers <x>, opcodes key=value (sample= runs to the next opcode).
    struct Tok { String key, value; bool header = false; };
    std::vector<Tok> toks;
    for (auto line : lines)
    {
        line = line.upToFirstOccurrenceOf ("//", false, false);
        auto words = StringArray::fromTokens (line, " \t", "");
        for (int i = 0; i < words.size(); ++i)
        {
            const auto w = words[i];
            if (w.isEmpty()) continue;
            if (w.startsWithChar ('<'))
            {
                toks.push_back ({ w.removeCharacters ("<>").toLowerCase(), {}, true });
                continue;
            }
            if (! w.containsChar ('=')) continue;
            Tok t { w.upToFirstOccurrenceOf ("=", false, false).toLowerCase(), w.fromFirstOccurrenceOf ("=", false, false) };
            if (t.key == "sample")
                while (i + 1 < words.size() && ! words[i + 1].containsChar ('=') && ! words[i + 1].startsWithChar ('<'))
                    t.value << " " << words[++i];
            toks.push_back (t);
        }
    }

    using Ops = std::map<String, String>;
    Ops global, group, region, control;
    String section;
    MultiSample ms;
    ms.name = file.getFileNameWithoutExtension().toStdString();
    std::map<String, std::shared_ptr<const SampleData>> cache;

    auto flush = [&]
    {
        if (section != "region") return;
        Ops o = global;
        for (auto& [k, v] : group) o[k] = v;
        for (auto& [k, v] : region) o[k] = v;
        if (o.count ("sample") == 0) return;
        auto rel = (control.count ("default_path") ? control["default_path"] : String()) + o["sample"];
        rel = rel.replaceCharacter ('\\', '/');
        const auto f = file.getParentDirectory().getChildFile (rel);
        auto& s = cache[f.getFullPathName()];
        if (s == nullptr) s = TinctureProcessor::decodeAudio (f);
        if (s == nullptr) return;
        MultiSample::Zone z;
        z.sample = s;
        auto num = [&] (const char* k, float def) { return o.count (k) ? o[k].getFloatValue() : def; };
        auto key = [&] (const char* k, int def) { return o.count (k) ? sfzKey (o[k]) : def; };
        const int k = key ("key", -1);
        z.loKey = k >= 0 ? k : key ("lokey", 0);
        z.hiKey = k >= 0 ? k : key ("hikey", 127);
        z.root = key ("pitch_keycenter", k >= 0 ? k : s->rootNote);
        z.loVel = (int) num ("lovel", 1);
        z.hiVel = (int) num ("hivel", 127);
        z.tuneCents = num ("tune", 0.0f);
        z.gainDb = num ("volume", 0.0f);
        z.loop = o.count ("loop_mode") && (o["loop_mode"] == "loop_continuous" || o["loop_mode"] == "loop_sustain");
        for (auto& [kk, v] : o)
            if (kk.startsWith ("ampeg_")) z.hasEnv = true;
        z.env.delay = num ("ampeg_delay", 0.0f);
        z.env.attack = num ("ampeg_attack", 0.0f);
        z.env.hold = num ("ampeg_hold", 0.0f);
        z.env.decay = num ("ampeg_decay", 0.0f);
        z.env.sustain = num ("ampeg_sustain", 100.0f) * 0.01f;
        z.env.release = num ("ampeg_release", 0.015f);
        ms.zones.push_back (z);
    };

    for (const auto& t : toks)
    {
        if (t.header)
        {
            flush();
            section = t.key;
            if (section == "region") region.clear();
            else if (section == "group") group.clear();
            else if (section == "global") global.clear();
            continue;
        }
        if (section == "region") region[t.key] = t.value;
        else if (section == "group") group[t.key] = t.value;
        else if (section == "global") global[t.key] = t.value;
        else if (section == "control") control[t.key] = t.value;
    }
    flush();
    if (ms.zones.empty()) return nullptr;
    return std::make_shared<const MultiSample> (std::move (ms));
}
} // namespace oscui

// ===========================================================================
// The row
// ===========================================================================
namespace
{
class ModuleRow final : public Component
{
public:
    ModuleRow (PanelContext& ctx, OscPageHost& host)
    {
        using namespace oscui;
        add (createSubPanel (ctx), { 6, 6, 92, 380 });
        for (int o = 0; o < kNumOscs; ++o)
            add (createOscPanel (ctx, host, o), { 102 + o * 250, 6, 246, 380 });
        add (createNoisePanel (ctx), { 852, 6, 150, 380 });
        add (createFilterPanel (ctx, 0), { 1006, 6, 192, 380 });
        add (createFilterPanel (ctx, 1), { 1202, 6, 192, 380 });
    }

private:
    void add (std::unique_ptr<Component> c, Rectangle<int> r)
    {
        addAndMakeVisible (*c);
        c->setBounds (r);
        panels.push_back (std::move (c));
    }
    std::vector<std::unique_ptr<Component>> panels;
};
}

std::unique_ptr<Component> createModuleRow (PanelContext& ctx, OscPageHost& host)
{
    return std::make_unique<ModuleRow> (ctx, host);
}
} // namespace tincture::ui
