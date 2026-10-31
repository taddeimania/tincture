// OscModulesSample.cpp — the sample-based oscillator modes: the browser
// menus, the operations / loop / SCAN / DENS / LENGTH / DIR menus, the
// waveform + spectrogram display with its markers, the multisample zone
// map and the loop crossfade button.
#include "OscModules.h"

namespace tincture::ui::oscui
{
using namespace juce;

namespace
{
String letterOf (int osc) { return String::charToString ((juce_wchar) ('A' + osc)); }

std::unique_ptr<FileChooser>& chooser()
{
    static std::unique_ptr<FileChooser> c;
    return c;
}

void browse (const String& title, const String& patterns, std::function<void (const File&)> done)
{
    chooser() = std::make_unique<FileChooser> (title, File(), patterns);
    chooser()->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [done] (const FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (f.existsAsFile()) done (f);
    });
}

// Replace the shared sample of Sample/Granular/Spectral (edits reset).
void setSample (TinctureProcessor& proc, int osc, std::shared_ptr<const SampleData> s, const String& label = "Load Sample")
{
    if (s == nullptr) return;
    const bool spectral = proc.params.geti (ids().osc[osc].type) == 4;
    proc.editPatch (label, [osc, s, spectral] (Patch& p)
    {
        auto& d = p.sample[(size_t) osc];
        d.sample = s;
        d.edits.clear();
        d.fadeEdges = 0;
        d.manualSlices.clear();
        d.rebuild (spectral);
    });
}

// A factory wavetable as a sample: its frames end to end, one cycle per
// frame at C3.
std::shared_ptr<const SampleData> tableAsSample (const std::string& cat, const std::string& name)
{
    auto t = FactoryTables::get (cat, name);
    if (t == nullptr) return nullptr;
    auto s = std::make_shared<SampleData>();
    s->name = name;
    s->category = "Wavetables/" + cat;
    s->rootNote = 60;
    s->sampleRate = (double) Wavetable::kFrameSize * 261.6255653;
    const int n = jmin (t->numFrames(), 64);
    s->left.resize ((size_t) (n * Wavetable::kFrameSize));
    for (int f = 0; f < n; ++f)
        t->renderFrame (n <= 1 ? 0 : roundToInt ((float) f / (float) (n - 1) * (float) (t->numFrames() - 1)),
                        s->left.data() + (size_t) f * Wavetable::kFrameSize);
    return s;
}

struct LibItem { std::string kind, category, name; };   // kind: "noise" / "table"

std::vector<LibItem> sampleLibrary()
{
    std::vector<LibItem> l;
    for (const auto& c : FactoryNoises::categories())
        for (const auto& n : FactoryNoises::names (c)) l.push_back ({ "noise", c, n });
    for (const auto& c : FactoryTables::categories())
        for (const auto& n : FactoryTables::names (c)) l.push_back ({ "table", c, n });
    return l;
}

void loadLibItem (TinctureProcessor& proc, int osc, const LibItem& it)
{
    if (it.kind == "noise") setSample (proc, osc, FactoryNoises::get (it.category, it.name));
    else setSample (proc, osc, tableAsSample (it.category, it.name));
}

// Spectral "Import PNG...": columns = time, rows = log frequency, brightness = level.
std::shared_ptr<const SampleData> sampleFromImage (const File& f)
{
    auto img = ImageFileFormat::loadFrom (f);
    if (! img.isValid()) return nullptr;
    const int cols = jmin (img.getWidth(), 512), rows = jmin (img.getHeight(), 96);
    img = img.rescaled (cols, rows);
    constexpr double sr = 48000.0;
    constexpr int hop = 512;
    auto s = std::make_shared<SampleData>();
    s->name = f.getFileNameWithoutExtension().toStdString();
    s->path = f.getFullPathName().toStdString();
    s->sampleRate = sr;
    s->left.assign ((size_t) (cols * hop), 0.0f);
    Random rng (1);
    for (int r = 0; r < rows; ++r)
    {
        const double hz = 40.0 * std::pow (16000.0 / 40.0, (double) (rows - 1 - r) / (double) jmax (1, rows - 1));
        const double inc = MathConstants<double>::twoPi * hz / sr;
        double ph = rng.nextDouble() * MathConstants<double>::twoPi;
        float prev = 0.0f;
        for (int c = 0; c < cols; ++c)
        {
            const float a = img.getPixelAt (c, r).getBrightness();
            for (int i = 0; i < hop; ++i)
            {
                const float amp = prev + (a - prev) * (float) i / (float) hop;
                if (amp > 0.002f) s->left[(size_t) (c * hop + i)] += amp * (float) std::sin (ph);
                ph += inc;
            }
            prev = a;
        }
    }
    float peak = 1e-6f;
    for (float v : s->left) peak = jmax (peak, std::abs (v));
    for (auto& v : s->left) v *= 0.9f / peak;
    s->embedded = true;
    return s;
}

int yTargetParam (int osc, int type, int target)
{
    const auto& id = ids().osc[osc];
    if (type == 3)
    {
        const int p[] { -1, id.level, id.warp1, id.warp2, id.density, id.grainLen, id.winAmt, id.winSkew, id.grOffset, id.grDir,
                        id.grPitch, id.grLenRand, id.grPanRand, id.grGainRand, id.winAmtRand, id.winSkewRand, -1, -1 };
        return target >= 0 && target < (int) std::size (p) ? p[target] : -1;
    }
    const int p[] { -1, id.level, id.warp1, id.warp2, id.specCut, id.specMix, id.specLo, id.specHi };
    return target >= 0 && target < (int) std::size (p) ? p[target] : -1;
}

using DataFn = std::function<void (SampleOscData&)>;
void editData (TinctureProcessor& proc, int osc, const String& label, DataFn fn, bool rebuild = false)
{
    const bool spectral = proc.params.geti (ids().osc[osc].type) == 4;
    proc.editPatch (label, [osc, fn, rebuild, spectral] (Patch& p)
    {
        auto& d = p.sample[(size_t) osc];
        fn (d);
        if (rebuild) d.rebuild (spectral);
    });
}

void toggleItem (Menu& m, TinctureProcessor& proc, int osc, const String& text, bool SampleOscData::* member, bool enabled = true, bool rebuild = false)
{
    const bool on = proc.patch().sample[(size_t) osc].*member;
    m.toggle (text, on, [&proc, osc, text, member, rebuild] { editData (proc, osc, text, [member] (SampleOscData& d) { d.*member = ! (d.*member); }, rebuild); }, enabled);
}
} // namespace

// ===========================================================================
// Browser
// ===========================================================================
String sampleName (const TinctureProcessor& proc, int osc, int type)
{
    const auto& d = proc.patch().sample[(size_t) osc];
    if (type == 1) return d.multi != nullptr ? String (d.multi->name) : String ("-");
    return d.sample != nullptr ? String (d.sample->name) : String ("-");
}

Menu sampleBrowserMenu (PanelContext& ctx, int osc, int type, Component&)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    Menu m;
    if (type == 1)
    {
        for (auto c : { "Bass", "Choir", "Drums", "Keys", "Mallet", "Plucked", "Strings", "Synth", "Winds" })
        {
            Menu sub;
            sub.add ("No instruments installed", [] {}, false, false);
            m.sub (c, sub);
        }
        m.separator();
        if (d.multi != nullptr)
        {
            const bool hasFile = ! d.multi->zones.empty() && d.multi->zones.front().sample != nullptr && ! d.multi->zones.front().sample->path.empty();
            const String path = hasFile ? String (d.multi->zones.front().sample->path) : String();
            m.add ("Show SFZ in Finder", [path] { File (path).revealToUser(); }, false, hasFile);
        }
        m.add ("Load SFZ...", [&proc, osc]
        {
            browse ("Load an SFZ instrument", "*.sfz;*.wav;*.aif;*.aiff;*.flac", [&proc, osc] (const File& f) { loadFileInto (proc, osc, f); });
        });
        if (d.multi != nullptr)
        {
            int zone = d.lastPlayedZone;
            if (zone < 0 && proc.telemetry().lastNote.load() >= 0)
                zone = d.multi->findZone (proc.telemetry().lastNote.load(), jlimit (1, 127, roundToInt (proc.telemetry().lastVelocity.load() * 127.0f)));
            const auto ms = d.multi;
            m.add ("Switch to Single Sample", [&proc, osc, zone, ms]
            {
                if (zone < 0 || zone >= (int) ms->zones.size()) return;
                auto s = ms->zones[(size_t) zone].sample;
                proc.editAll ("Switch to Single Sample", [osc, s] (Patch& p, ParamValues& v)
                {
                    v[ids().osc[osc].type] = 2.0f;
                    auto& dd = p.sample[(size_t) osc];
                    dd.sample = s;
                    dd.type = 2;
                    dd.edits.clear();
                    dd.rebuild();
                });
            }, false, zone >= 0);
        }
        return m;
    }

    const auto* cur = d.sample.get();
    const String curCat = cur != nullptr ? String (cur->category) : String();
    {
        Menu sub;
        sub.add ("No factory samples installed", [] {}, false, false);
        m.sub ("Factory", sub);
    }
    {
        Menu sub;
        bool any = false;
        for (const auto& c : FactoryNoises::categories())
        {
            Menu cs;
            bool has = false;
            for (const auto& n : FactoryNoises::names (c))
            {
                const bool t = cur != nullptr && curCat == String (c) && cur->name == n && FactoryNoises::isFactory (*cur);
                cs.add (n, [&proc, osc, c, n] { loadLibItem (proc, osc, { "noise", c, n }); }, t);
                has |= t;
            }
            sub.sub (c, cs, has);
            any |= has;
        }
        m.sub ("Factory Non-Tonal", sub, any);
    }
    m.separator();
    {
        Menu sub;
        bool any = false;
        for (const auto& c : FactoryTables::categories())
        {
            Menu cs;
            bool has = false;
            for (const auto& n : FactoryTables::names (c))
            {
                const bool t = cur != nullptr && curCat == "Wavetables/" + String (c) && cur->name == n;
                cs.add (n, [&proc, osc, c, n] { loadLibItem (proc, osc, { "table", c, n }); }, t);
                has |= t;
            }
            sub.sub (c, cs, has);
            any |= has;
        }
        m.sub ("Wavetables", sub, any);
    }
    m.separator();
    if (cur != nullptr)
    {
        const bool library = FactoryNoises::isFactory (*cur) || curCat.startsWith ("Wavetables/");
        if (! library)
            m.toggle ("Embed in Preset", cur->embedded, [&proc, osc]
            {
                const auto& s = proc.patch().sample[(size_t) osc].sample;
                if (s == nullptr) return;
                auto copy = std::make_shared<SampleData> (*s);
                copy->embedded = ! s->embedded;
                editData (proc, osc, "Embed in Preset", [copy] (SampleOscData& dd) { dd.sample = copy; });
            });
        const String path (cur->path);
        m.add ("Show Sample in Finder", [path] { File (path).revealToUser(); }, false, path.isNotEmpty());
        m.add ("Reload Sample", [&proc, osc, path]
        {
            std::shared_ptr<const SampleData> fresh;
            if (path.isNotEmpty() && File (path).existsAsFile()) fresh = TinctureProcessor::decodeAudio (File (path));
            editData (proc, osc, "Reload Sample", [fresh] (SampleOscData& dd)
            {
                if (fresh != nullptr) dd.sample = fresh;
                dd.edits.clear();
                dd.fadeEdges = 0;
            }, true);
        });
    }
    m.add ("Load Sample...", [&proc, osc]
    {
        browse ("Load a sample", "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3", [&proc, osc] (const File& f) { proc.loadSampleFile (osc, f); });
    });
    if (type == 4)
        m.add ("Import PNG...", [&proc, osc]
        {
            browse ("Import a spectrogram image", "*.png;*.jpg;*.jpeg", [&proc, osc] (const File& f) { setSample (proc, osc, sampleFromImage (f), "Import PNG"); });
        });
    if (cur != nullptr)
    {
        Menu sw;
        const auto& names = SampleOscData::wavetableImportChoices();
        for (int i = 0; i < (int) names.size(); ++i)
            sw.add (names[(size_t) i], [&proc, osc, i]
            {
                const auto& dd = proc.patch().sample[(size_t) osc];
                auto src = dd.processed != nullptr ? dd.processed : dd.sample;
                if (src == nullptr) return;
                auto frames = SampleOscData::toWavetable (*src, i);
                if (frames.empty()) return;
                auto table = Wavetable::create (src->name, "User", std::move (frames));
                proc.editAll ("Switch to Wavetable", [osc, table] (Patch& p, ParamValues& v)
                {
                    v[ids().osc[osc].type] = 0.0f;
                    p.wavetable[(size_t) osc].table = table;
                });
            });
        m.sub ("Switch to Wavetable", sw);
    }
    return m;
}

void stepSample (PanelContext& ctx, int osc, int delta)
{
    auto& proc = ctx.proc;
    if (proc.params.geti (ids().osc[osc].type) == 1) return;
    const auto lib = sampleLibrary();
    if (lib.empty()) return;
    const auto* cur = proc.patch().sample[(size_t) osc].sample.get();
    int i = -1;
    if (cur != nullptr)
        for (int k = 0; k < (int) lib.size(); ++k)
        {
            const auto cat = lib[(size_t) k].kind == "table" ? "Wavetables/" + lib[(size_t) k].category : lib[(size_t) k].category;
            if (cur->name == lib[(size_t) k].name && cur->category == cat) i = k;
        }
    i = i < 0 ? (delta > 0 ? 0 : (int) lib.size() - 1) : (i + delta + (int) lib.size()) % (int) lib.size();
    loadLibItem (proc, osc, lib[(size_t) i]);
}

// ===========================================================================
// Operations menu (right-click the display)
// ===========================================================================
Menu sampleOperationsMenu (PanelContext& ctx, int osc, int type)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const auto& id = ids().osc[osc];
    Menu m;
    toggleItem (m, proc, osc, "Show Marker Animation", &SampleOscData::showMarkerAnimation);
    toggleItem (m, proc, osc, "Zoom to Start and End", &SampleOscData::zoomToStartEnd);
    if (type == 4) toggleItem (m, proc, osc, "Show Waveform Display", &SampleOscData::showWaveform);
    m.separator();
    const char* snaps[] { "Snap Off", "Snap to Zero", "Snap to Beats", "Snap to Loop" };
    for (int i = 0; i < 4; ++i)
        m.add (snaps[i], [&proc, osc, i] { editData (proc, osc, "Snap", [i] (SampleOscData& dd) { dd.snap = i; }); }, d.snap == i);
    m.separator();
    {
        Menu fe;
        const auto& names = SampleOscData::fadeEdgeChoices();
        for (int i = 0; i < (int) names.size(); ++i)
            fe.add (names[(size_t) i], [&proc, osc, i] { editData (proc, osc, "Fade Edges", [i] (SampleOscData& dd) { dd.fadeEdges = i; }, true); }, d.fadeEdges == i);
        m.sub ("Fade Edges", fe, d.fadeEdges > 0, d.sample != nullptr);
    }
    const bool has = d.sample != nullptr;
    m.add ("Normalize", [&proc, osc] { editData (proc, osc, "Normalize", [] (SampleOscData& dd) { dd.edits.push_back ({ SampleEdit::Normalize, 0.0f, 1.0f }); }, true); }, false, has);
    m.add ("Reverse", [&proc, osc] { editData (proc, osc, "Reverse", [] (SampleOscData& dd) { dd.edits.push_back ({ SampleEdit::Reverse, 0.0f, 1.0f }); }, true); }, false, has);
    m.add ("Trim", [&proc, osc, type]
    {
        proc.editAll ("Trim", [osc, type] (Patch& p, ParamValues& v)
        {
            const auto& i = ids().osc[osc];
            const float a = jmin (v[i.start], v[i.end]), b = jmax (v[i.start], v[i.end]);
            if (b - a < 0.001f) return;
            auto& dd = p.sample[(size_t) osc];
            dd.edits.push_back ({ SampleEdit::Trim, a, b });
            dd.rebuild (type == 4);
            v[i.start] = 0.0f; v[i.end] = 1.0f;
            v[i.loopStart] = jlimit (0.0f, 1.0f, (v[i.loopStart] - a) / (b - a));
            v[i.loopEnd] = jlimit (0.0f, 1.0f, (v[i.loopEnd] - a) / (b - a));
        });
    }, false, has);
    m.separator();
    const bool manual = d.manualLoop && type >= 3;
    const char* slicing[] { "Slicing Off", "Slice Auto", "Slice Manual" };
    for (int i = 0; i < 3; ++i)
        m.add (slicing[i], [&proc, osc, i, label = String (slicing[i])]
        {
            editData (proc, osc, label, [i] (SampleOscData& dd)
            {
                if (i == SampleOscData::SliceManual && dd.manualSlices.empty() && dd.processed != nullptr && dd.processed->frames() > 0)
                {
                    // Start from the auto-detected slices.
                    for (int f : SampleOscData::detectSlices (*dd.processed, dd.sliceThreshold))
                        if (f > 0) dd.manualSlices.push_back ((float) f / (float) dd.processed->frames());
                }
                dd.slicing = i;
            }, true);
        }, d.slicing == i, i == 0 || (! manual && has));
    if (d.slicing != SampleOscData::SlicingOff)
    {
        m.separator();
        toggleItem (m, proc, osc, "Play Slice To End", &SampleOscData::playSliceToEnd);
        toggleItem (m, proc, osc, "Play Single Slice", &SampleOscData::playSingleSlice);
        Menu rn;
        for (int o = -1; o <= 8; ++o)
        {
            const int note = (o + 2) * 12;     // C3 = 60
            rn.add ("C" + String (o), [&proc, osc, note] { editData (proc, osc, "Slice Root Note", [note] (SampleOscData& dd) { dd.sliceRoot = note; }); }, d.sliceRoot == note);
        }
        m.sub ("Root Note", rn);
        const int sel = proc.patch().clips.selected;
        m.add ("Send to Selected Clip (" + String (sel + 1) + ")", [&proc, osc]
        {
            const auto& dd = proc.patch().sample[(size_t) osc];
            if (dd.processed == nullptr || dd.processed->slices.empty()) return;
            const auto slices = dd.processed->slices;
            const double sr = dd.processed->sampleRate, beatsPerSec = 2.0;   // 120 BPM
            const int frames = dd.processed->frames();
            const int root = dd.sliceRoot;
            proc.editPatch ("Send Slices to Clip", [slices, sr, beatsPerSec, frames, root] (Patch& p)
            {
                auto& clip = p.clips.clips[(size_t) p.clips.selected];
                clip.notes.clear();
                for (size_t k = 0; k < slices.size(); ++k)
                {
                    ClipNote n;
                    n.start = (double) slices[k] / sr * beatsPerSec;
                    const int endF = k + 1 < slices.size() ? slices[k + 1] : frames;
                    n.length = jmax (0.0625, (double) (endF - slices[k]) / sr * beatsPerSec);
                    n.pitch = jlimit (0, 127, root + (int) k);
                    clip.addNote (n);
                }
                clip.length = jmax (1.0, std::ceil ((double) frames / sr * beatsPerSec));
            });
        });
        toggleItem (m, proc, osc, "Auto-Sync to Clip", &SampleOscData::autoSyncToClip);
    }
    if (type >= 3)
    {
        m.separator();
        toggleItem (m, proc, osc, "Show X|Y Control", &SampleOscData::showXY);
        if (d.showXY)
        {
            const auto& targets = type == 3 ? SampleOscData::granularYTargets() : SampleOscData::spectralYTargets();
            const int cur = type == 3 ? d.yTargetGranular : d.yTargetSpectral;
            Menu ya;
            for (int i = 0; i < (int) targets.size(); ++i)
                ya.add (targets[(size_t) i], [&proc, osc, type, i]
                {
                    editData (proc, osc, "Y Axis", [type, i] (SampleOscData& dd) { (type == 3 ? dd.yTargetGranular : dd.yTargetSpectral) = i; });
                }, i == cur);
            m.sub ("Y Axis", ya);
            if (cur != 0)
            {
                const int param = yTargetParam (osc, type, cur);
                m.sub ("Modulate Y:", sourceMenu ([&proc, param] (ModSource s)
                {
                    if (param >= 0) proc.editPatch ("Add Modulation", [s, param] (Patch& p) { p.matrix.assign (s, param); });
                }), false, param >= 0);
            }
        }
    }
    (void) id;
    return m;
}

// ===========================================================================
// Loop menu
// ===========================================================================
String loopFace (const TinctureProcessor& proc, int osc)
{
    const auto& d = proc.patch().sample[(size_t) osc];
    const int t = proc.params.geti (ids().osc[osc].type);
    if (d.manualLoop && t >= 3) return "MANUAL";
    return String (choices::loopModes()[(size_t) jlimit (0, 4, proc.params.geti (ids().osc[osc].loopMode))]).toUpperCase();
}

Menu loopMenu (PanelContext& ctx, int osc, int type)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const int cur = proc.params.geti (ids().osc[osc].loopMode);
    const bool manual = d.manualLoop && type >= 3;
    Menu m;
    const auto& names = choices::loopModes();
    for (int i = 0; i < (int) names.size(); ++i)
        m.add (names[(size_t) i], [&proc, osc, i]
        {
            proc.editAll ("Loop Mode", [osc, i] (Patch& p, ParamValues& v)
            {
                v[ids().osc[osc].loopMode] = (float) i;
                p.sample[(size_t) osc].manualLoop = false;
            });
        }, ! manual && cur == i);
    if (type >= 3)
    {
        m.separator();
        m.add ("Manual", [&proc, osc, type]
        {
            editData (proc, osc, "Loop Mode", [] (SampleOscData& dd) { dd.manualLoop = true; dd.slicing = SampleOscData::SlicingOff; }, true);
            (void) type;
        }, manual);
    }
    m.separator();
    toggleItem (m, proc, osc, "Relative Loop", &SampleOscData::relativeLoop);
    toggleItem (m, proc, osc, "Link Loop Length", &SampleOscData::linkLoopLength);
    toggleItem (m, proc, osc, "Exit Loop on Release", &SampleOscData::exitLoopOnRelease);
    if (type == 3) toggleItem (m, proc, osc, "Loop Grains", &SampleOscData::loopGrains);
    return m;
}

// ===========================================================================
// Knob menu extras
// ===========================================================================
void addScanItems (Menu& m, PanelContext& ctx, int osc, int type)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    Menu range;
    const char* names[] { "+/- 200%", "+/- 400%", "+/- 800%" };
    for (int i = 0; i < 3; ++i)
        range.add (names[i], [&proc, osc, i] { editData (proc, osc, "Scan Range", [i] (SampleOscData& dd) { dd.scanRange = i; }); }, d.scanRange == i);
    m.sub ("Range", range);
    toggleItem (m, proc, osc, "Reverse", &SampleOscData::scanReverse);
    if (type >= 3) toggleItem (m, proc, osc, "Key Track", &SampleOscData::scanKeyTrack);
    toggleItem (m, proc, osc, "Lock Scan Rate (to Tempo)", &SampleOscData::scanTempoLock);
    toggleItem (m, proc, osc, "Sample Length to BPM", &SampleOscData::lengthToBpm);
    if (type == 4)
    {
        m.separator();
        toggleItem (m, proc, osc, "Phase Lock", &SampleOscData::phaseLock);
        toggleItem (m, proc, osc, "Transients", &SampleOscData::transients);
    }
}

void addDensityItems (Menu& m, PanelContext& ctx, int osc)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const char* modes[] { "Free", "BPM Sync", "Grains" };
    for (int i = 0; i < 3; ++i)
        m.add (modes[i], [&proc, osc, i]
        {
            editData (proc, osc, "Density Mode", [i] (SampleOscData& dd)
            {
                dd.densityMode = i;
                if (i == SampleOscData::DensGrains && dd.lengthMode == SampleOscData::LenPercent) dd.lengthMode = SampleOscData::LenFree;
            });
        }, d.densityMode == i);
    if (d.densityMode == SampleOscData::DensSync)
    {
        m.separator();
        toggleItem (m, proc, osc, "Triplet", &SampleOscData::densTriplet);
        toggleItem (m, proc, osc, "Dotted", &SampleOscData::densDotted);
    }
    m.separator();
    toggleItem (m, proc, osc, "Jump Start", &SampleOscData::jumpStart);
    Menu mg;
    for (int n : { 8, 16, 32, 64, 128, 256 })
        mg.add (n == SampleOsc::kMaxGrains ? String ("Unlimited (") + String (n) + ")" : String (n),
                [&proc, osc, n] { editData (proc, osc, "Max Grains", [n] (SampleOscData& dd) { dd.maxGrains = n; }); }, d.maxGrains == n);
    m.sub ("Max Grains", mg);
}

void addLengthItems (Menu& m, PanelContext& ctx, int osc)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const char* modes[] { "Free", "BPM Sync", "Percent" };
    for (int i = 0; i < 3; ++i)
        m.add (modes[i], [&proc, osc, i] { editData (proc, osc, "Length Mode", [i] (SampleOscData& dd) { dd.lengthMode = i; }); },
               d.lengthMode == i, i != SampleOscData::LenPercent || d.densityMode != SampleOscData::DensGrains);
    if (d.lengthMode == SampleOscData::LenSync)
    {
        m.separator();
        toggleItem (m, proc, osc, "Triplet", &SampleOscData::lenTriplet);
        toggleItem (m, proc, osc, "Dotted", &SampleOscData::lenDotted);
    }
}

void addDirItems (Menu& m, PanelContext& ctx, int osc)
{
    toggleItem (m, ctx.proc, osc, "Reverse Grains", &SampleOscData::reverseGrains);
}

// ===========================================================================
// SampleDisplay
// ===========================================================================
SampleDisplay::SampleDisplay (PanelContext& c, int o, int t) : ctx (c), osc (o), type (t)
{
    ctx.add (this);
    setTooltip ("Drag the yellow start/end markers and the blue loop markers; drag up/down to zoom, sideways to pan; "
                "right-click for sample operations; drop an audio file to load it");
}

SampleDisplay::~SampleDisplay() { ctx.remove (this); }

Rectangle<float> SampleDisplay::waveArea() const
{
    auto r = getLocalBounds().toFloat().reduced (2.0f);
    if (type == 4)
    {
        r.removeFromRight (16.0f);
        if (ctx.proc.patch().sample[(size_t) osc].showWaveform) r.removeFromBottom (20.0f);
    }
    return r;
}

float SampleDisplay::xOf (float f) const
{
    const auto r = waveArea();
    return r.getX() + (f - viewStart) / jmax (1e-6f, viewEnd - viewStart) * r.getWidth();
}

float SampleDisplay::fracAt (float x) const
{
    const auto r = waveArea();
    return jlimit (0.0f, 1.0f, viewStart + (x - r.getX()) / r.getWidth() * (viewEnd - viewStart));
}

void SampleDisplay::resized() { image = {}; }

void SampleDisplay::refresh()
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const auto& id = ids().osc[osc];
    if (d.zoomToStartEnd)
    {
        const float a = jmin (proc.params.get (id.start), proc.params.get (id.end)), b = jmax (proc.params.get (id.start), proc.params.get (id.end));
        if (b - a > 0.001f) { viewStart = a; viewEnd = b; }
    }
    // Hash of everything drawn.
    uint32 h = 2166136261u;
    auto mix = [&h] (float v) { uint32 b; std::memcpy (&b, &v, 4); h = (h ^ b) * 16777619u; };
    for (int p : { id.start, id.end, id.loopStart, id.loopEnd, id.loopMode, id.xfade, id.specLo, id.specHi, id.scan })
        mix (proc.params.get (p));
    mix ((float) d.slicing); mix (d.sliceThreshold); mix ((float) d.manualSlices.size()); mix ((float) d.manualLoop);
    mix ((float) d.showXY); mix (d.yValue); mix ((float) d.showWaveform); mix (viewStart); mix (viewEnd);
    for (float s : d.manualSlices) mix (s);
    std::vector<float> heads;
    if (proc.telemetry().activeVoices.load() > 0)
    {
        const float ph = proc.telemetry().oscPlayhead[osc].load();
        if (ph > 0.0f) heads.push_back (ph);
    }
    for (float v : heads) mix (v);
    const void* key = type == 4 ? (const void*) d.spectral.get() : (const void*) d.processed.get();
    if (key != imageKey || ! same (viewStart, imageV0) || ! same (viewEnd, imageV1)) image = {};
    if (h != stateKey || heads != shownPlayheads || image.isNull())
    {
        stateKey = h;
        shownPlayheads = heads;
        repaint();
    }
}

void SampleDisplay::rebuildImage()
{
    const auto& d = ctx.proc.patch().sample[(size_t) osc];
    const auto r = waveArea().toNearestInt();
    imageV0 = viewStart; imageV1 = viewEnd;
    imageKey = type == 4 ? (const void*) d.spectral.get() : (const void*) d.processed.get();
    image = Image (Image::ARGB, jmax (1, r.getWidth()), jmax (1, r.getHeight()), true);
    const int W = image.getWidth(), H = image.getHeight();
    if (type == 4 && d.spectral != nullptr && d.spectral->frames > 0)
    {
        const auto& sd = *d.spectral;
        Image::BitmapData bm (image, Image::BitmapData::writeOnly);
        const float binHz = (float) (sd.sampleRate / SpectralData::kFftSize);
        for (int x = 0; x < W; ++x)
        {
            const float f = viewStart + (viewEnd - viewStart) * (float) x / (float) W;
            const int frame = jlimit (0, sd.frames - 1, roundToInt (f * (float) (sd.sampleFrames / SpectralData::kHop)) + SpectralData::kPreFrames);
            const float* mag = sd.magAt (frame);
            for (int y = 0; y < H; ++y)
            {
                const float hz = 20.0f * std::pow (1000.0f, 1.0f - (float) y / (float) H);
                const int bin = jlimit (1, SpectralData::kBins - 1, roundToInt (hz / binHz));
                const float db = Decibels::gainToDecibels (mag[bin] / (float) SpectralData::kFftSize * 4.0f, -90.0f);
                const float v = jlimit (0.0f, 1.0f, (db + 80.0f) / 75.0f);
                bm.setPixelColour (x, y, colours::brand.withAlpha (v * v));
            }
        }
        return;
    }
    const auto* s = d.processed.get();
    if (s == nullptr || s->frames() == 0) return;
    Graphics g (image);
    const int n = s->frames();
    const int chans = s->stereo() ? 2 : 1;
    const float laneH = (float) H / (float) chans;
    for (int c = 0; c < chans; ++c)
    {
        const float* data = s->channel (c);
        const float cy = laneH * ((float) c + 0.5f);
        g.setColour (colours::grid);
        g.drawHorizontalLine ((int) cy, 0.0f, (float) W);
        g.setColour (colours::brand.withAlpha (0.85f));
        for (int x = 0; x < W; ++x)
        {
            const int a = jlimit (0, n - 1, (int) ((viewStart + (viewEnd - viewStart) * (float) x / (float) W) * (float) n));
            const int b = jlimit (a + 1, n, (int) ((viewStart + (viewEnd - viewStart) * (float) (x + 1) / (float) W) * (float) n));
            float lo = 0.0f, hi = 0.0f;
            const int step = jmax (1, (b - a) / 64);
            for (int i = a; i < b; i += step) { lo = jmin (lo, data[i]); hi = jmax (hi, data[i]); }
            g.drawVerticalLine (x, cy - hi * laneH * 0.48f, cy - lo * laneH * 0.48f + 1.0f);
        }
    }
}

void SampleDisplay::paint (Graphics& g)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const auto& id = ids().osc[osc];
    drawLcdBox (g, getLocalBounds().toFloat());
    const auto r = waveArea();
    const bool empty = type == 4 ? d.spectral == nullptr : d.processed == nullptr;
    if (empty)
    {
        drawText (g, d.sample == nullptr ? "Drop a sample here or use the browser" : "Analysing...", r, 11.0f, colours::lcdDim, true);
        return;
    }
    if (image.isNull()) rebuildImage();
    g.drawImageAt (image, (int) r.getX(), (int) r.getY());

    // Spectral: optional waveform strip.
    if (type == 4 && d.showWaveform && d.processed != nullptr)
    {
        const auto strip = Rectangle<float> (r.getX(), r.getBottom() + 1.0f, r.getWidth(), 18.0f);
        const auto* s = d.processed.get();
        const int n = s->frames();
        g.setColour (colours::brand.withAlpha (0.7f));
        for (int x = 0; x < (int) strip.getWidth(); x += 1)
        {
            const int a = jlimit (0, n - 1, (int) (fracAt (strip.getX() + (float) x) * (float) n));
            float pk = 0.0f;
            for (int i = a; i < jmin (n, a + 256); i += 8) pk = jmax (pk, std::abs (s->left[(size_t) i]));
            g.drawVerticalLine ((int) strip.getX() + x, strip.getCentreY() - pk * 8.0f, strip.getCentreY() + pk * 8.0f + 1.0f);
        }
    }

    const float st = proc.params.get (id.start), en = proc.params.get (id.end);
    // Dim outside start..end.
    g.setColour (colours::black.withAlpha (0.55f));
    const float xs = jlimit (r.getX(), r.getRight(), xOf (jmin (st, en))), xe = jlimit (r.getX(), r.getRight(), xOf (jmax (st, en)));
    g.fillRect (Rectangle<float>::leftTopRightBottom (r.getX(), r.getY(), xs, r.getBottom()));
    g.fillRect (Rectangle<float>::leftTopRightBottom (xe, r.getY(), r.getRight(), r.getBottom()));

    // Slices.
    const auto* ps = d.processed.get();
    if (d.slicing != SampleOscData::SlicingOff && ps != nullptr && ps->frames() > 0)
    {
        const auto& sl = ps->slices;
        for (size_t k = 0; k < sl.size(); ++k)
        {
            const float f = (float) sl[k] / (float) ps->frames();
            const float x = xOf (f);
            if (x < r.getX() || x > r.getRight()) continue;
            g.setColour (colours::ink.withAlpha (0.6f));
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            if (d.slicing == SampleOscData::SliceManual)
            {
                Path h;
                h.addTriangle (x - 3.0f, r.getBottom(), x + 3.0f, r.getBottom(), x, r.getBottom() - 5.0f);
                g.fillPath (h);
            }
        }
        if (hovering && hoverSlice >= 0 && hoverSlice < (int) sl.size())
        {
            const float a = xOf ((float) sl[(size_t) hoverSlice] / (float) ps->frames());
            const float b = xOf (hoverSlice + 1 < (int) sl.size() ? (float) sl[(size_t) hoverSlice + 1] / (float) ps->frames() : 1.0f);
            g.setColour (colours::ink.withAlpha (0.08f));
            g.fillRect (Rectangle<float>::leftTopRightBottom (jmax (a, r.getX()), r.getY(), jmin (b, r.getRight()), r.getBottom()));
            const int note = d.sliceRoot + hoverSlice;
            static const char* nn[] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            drawText (g, String (nn[note % 12]) + String (note / 12 - 2), { jmax (a, r.getX()), r.getBottom() - 14.0f, 30.0f, 13.0f }, 10.5f, colours::ink, true);
        }
        if (d.slicing == SampleOscData::SliceAuto)
        {
            const float y = r.getCentreY() - d.sliceThreshold * r.getHeight() * 0.48f;
            g.setColour (colours::ledYellow.withAlpha (0.8f));
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
        }
    }

    // Loop region (blue, along the top) and crossfade.
    const bool looping = proc.params.geti (id.loopMode) != 0 && ! (d.manualLoop && type >= 3);
    if (looping)
    {
        const float a = st + (en - st) * proc.params.get (id.loopStart), b = st + (en - st) * proc.params.get (id.loopEnd);
        const float xa = xOf (a), xb = xOf (b);
        g.setColour (loopBlue().withAlpha (0.12f));
        g.fillRect (Rectangle<float>::leftTopRightBottom (jmin (xa, xb), r.getY(), jmax (xa, xb), r.getBottom()));
        g.setColour (loopBlue());
        g.fillRect (Rectangle<float>::leftTopRightBottom (jmin (xa, xb), r.getY(), jmax (xa, xb), r.getY() + 3.0f));
        for (float x : { xa, xb })
        {
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            Path t;
            t.addTriangle (x - 4.0f, r.getY(), x + 4.0f, r.getY(), x, r.getY() + 6.0f);
            g.fillPath (t);
        }
        const float xf = proc.params.get (id.xfade);
        if (xf > 0.0f)
        {
            const float w = std::abs (xb - xa) * xf;
            Path fan;
            fan.startNewSubPath (xb - w, r.getBottom());
            fan.lineTo (xb, r.getY());
            fan.lineTo (xb, r.getBottom());
            fan.closeSubPath();
            g.setColour (loopBlue().withAlpha (0.25f));
            g.fillPath (fan);
        }
    }

    // Start / end markers (yellow).
    g.setColour (colours::ledYellow);
    for (int k = 0; k < 2; ++k)
    {
        const float x = xOf (k == 0 ? st : en);
        if (x < r.getX() - 1.0f || x > r.getRight() + 1.0f) continue;
        g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
        if (hovering || grab == Grab::Start || grab == Grab::End)
        {
            Path t;
            const float y = r.getCentreY();
            if (k == 0) t.addTriangle (x, y - 6.0f, x, y + 6.0f, x + 7.0f, y);
            else t.addTriangle (x, y - 6.0f, x, y + 6.0f, x - 7.0f, y);
            g.fillPath (t);
        }
    }

    // Playheads.
    g.setColour (colours::ink);
    for (float p : shownPlayheads)
        g.drawVerticalLine ((int) xOf (p), r.getY(), r.getBottom());

    // X|Y dot (and the manual-mode position).
    const bool manual = d.manualLoop && type >= 3;
    if (d.showXY || manual)
    {
        const float px = manual ? jlimit (0.0f, 1.0f, (proc.params.get (id.scan) + 2.0f) * 0.25f)
                                : (shownPlayheads.empty() ? st : shownPlayheads.front());
        const float x = xOf (px);
        if (manual) { g.setColour (colours::ledYellow); g.drawVerticalLine ((int) x, r.getY(), r.getBottom()); }
        const float y = r.getBottom() - d.yValue * r.getHeight();
        g.setColour (colours::ledRed);
        g.fillEllipse (Rectangle<float> (9.0f, 9.0f).withCentre ({ x, d.showXY ? y : r.getCentreY() }));
    }

    // Spectral band limits.
    if (type == 4)
    {
        const auto strip = Rectangle<float> (r.getRight() + 2.0f, r.getY(), 13.0f, r.getHeight());
        g.setColour (colours::panelEdge);
        g.fillRect (strip);
        const auto& lo = params()[id.specLo];
        const float yh = strip.getBottom() - lo.toNorm (proc.params.get (id.specHi)) * strip.getHeight();
        const float yl = strip.getBottom() - lo.toNorm (proc.params.get (id.specLo)) * strip.getHeight();
        g.setColour (colours::brand.withAlpha (0.25f));
        g.fillRect (Rectangle<float>::leftTopRightBottom (strip.getX(), jmin (yh, yl), strip.getRight(), jmax (yh, yl)));
        g.setColour (colours::ledYellow);
        for (float y : { yh, yl })
            g.fillRect (strip.getX(), y - 1.5f, strip.getWidth(), 3.0f);
        // Muted regions on the spectrogram.
        g.setColour (colours::black.withAlpha (0.5f));
        g.fillRect (Rectangle<float>::leftTopRightBottom (r.getX(), r.getY(), r.getRight(), jlimit (r.getY(), r.getBottom(), yh)));
        g.fillRect (Rectangle<float>::leftTopRightBottom (r.getX(), jlimit (r.getY(), r.getBottom(), yl), r.getRight(), r.getBottom()));
    }

    if (viewStart > 0.0f || viewEnd < 1.0f)
        drawText (g, "x" + String (1.0f / jmax (0.001f, viewEnd - viewStart), 1), r.reduced (3.0f).removeFromTop (12.0f), 10.5f, colours::lcdDim, true, Justification::topRight);
}

SampleDisplay::Grab SampleDisplay::hitTestGrab (Point<float> p) const
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const auto& id = ids().osc[osc];
    const auto r = waveArea();
    if (type == 4 && p.x > r.getRight())
    {
        const auto& lo = params()[id.specLo];
        const float yh = r.getBottom() - lo.toNorm (proc.params.get (id.specHi)) * r.getHeight();
        const float yl = r.getBottom() - lo.toNorm (proc.params.get (id.specLo)) * r.getHeight();
        return std::abs (p.y - yh) < std::abs (p.y - yl) ? Grab::HiFreq : Grab::LoFreq;
    }
    if (d.showXY)
    {
        const float y = r.getBottom() - d.yValue * r.getHeight();
        if (std::abs (p.y - y) < 6.0f && d.showXY) { /* the dot: handled as View with Y drag below */ }
    }
    const float st = proc.params.get (id.start), en = proc.params.get (id.end);
    if (! d.showMarkerAnimation)
    {
        if (proc.params.geti (id.loopMode) != 0 && ! (d.manualLoop && type >= 3) && p.y < r.getY() + 10.0f)
        {
            const float a = st + (en - st) * proc.params.get (id.loopStart), b = st + (en - st) * proc.params.get (id.loopEnd);
            if (std::abs (p.x - xOf (a)) < 5.0f) return Grab::LoopStart;
            if (std::abs (p.x - xOf (b)) < 5.0f) return Grab::LoopEnd;
            if (p.x > jmin (xOf (a), xOf (b)) && p.x < jmax (xOf (a), xOf (b))) return Grab::LoopBar;
        }
        if (std::abs (p.x - xOf (st)) < 5.0f) return Grab::Start;
        if (std::abs (p.x - xOf (en)) < 5.0f) return Grab::End;
    }
    if (d.slicing == SampleOscData::SliceAuto)
    {
        const float y = r.getCentreY() - d.sliceThreshold * r.getHeight() * 0.48f;
        if (std::abs (p.y - y) < 4.0f) return Grab::Threshold;
    }
    if (d.slicing == SampleOscData::SliceManual && d.processed != nullptr && d.processed->frames() > 0)
        for (int s : d.processed->slices)
            if (s > 0 && std::abs (p.x - xOf ((float) s / (float) d.processed->frames())) < 4.0f) return Grab::Slice;
    return Grab::View;
}

void SampleDisplay::mouseMove (const MouseEvent& e)
{
    hovering = true;
    const auto& d = ctx.proc.patch().sample[(size_t) osc];
    int hs = -1;
    if (d.processed != nullptr && d.processed->frames() > 0 && d.slicing != SampleOscData::SlicingOff)
    {
        const float f = fracAt (e.position.x);
        const auto& sl = d.processed->slices;
        for (int k = 0; k < (int) sl.size(); ++k)
            if ((float) sl[(size_t) k] / (float) d.processed->frames() <= f) hs = k;
    }
    const auto h = hitTestGrab (e.position);
    if (h != hover || hs != hoverSlice)
    {
        hover = h;
        hoverSlice = hs;
        setMouseCursor (h == Grab::View || h == Grab::None ? MouseCursor::NormalCursor
                        : (h == Grab::Threshold || h == Grab::HiFreq || h == Grab::LoFreq ? MouseCursor::UpDownResizeCursor : MouseCursor::LeftRightResizeCursor));
    }
    repaint();
}

void SampleDisplay::mouseExit (const MouseEvent&) { hovering = false; hoverSlice = -1; repaint(); }

namespace
{
int paramForGrab (int osc, SampleDisplay::Grab g)
{
    const auto& id = ids().osc[osc];
    switch (g)
    {
        case SampleDisplay::Grab::Start:     return id.start;
        case SampleDisplay::Grab::End:       return id.end;
        case SampleDisplay::Grab::LoopStart: return id.loopStart;
        case SampleDisplay::Grab::LoopEnd:   return id.loopEnd;
        case SampleDisplay::Grab::HiFreq:    return id.specHi;
        case SampleDisplay::Grab::LoFreq:    return id.specLo;
        case SampleDisplay::Grab::None:      case SampleDisplay::Grab::LoopBar: case SampleDisplay::Grab::Threshold:
        case SampleDisplay::Grab::Slice:     case SampleDisplay::Grab::View:    return -1;
    }
    return -1;
}

// Snap a position (fraction) by the display's snap mode.
float snapFrac (const SampleOscData& d, const TinctureProcessor& proc, int osc, float f)
{
    if (d.processed == nullptr || d.processed->frames() < 2) return f;
    const auto& s = *d.processed;
    const int n = s.frames();
    if (d.snap == SampleOscData::SnapZero)
    {
        const int c = jlimit (1, n - 1, (int) (f * (float) n));
        for (int k = 0; k < 4096; ++k)
            for (int i : { c - k, c + k })
                if (i > 0 && i < n && (s.left[(size_t) i - 1] <= 0.0f) != (s.left[(size_t) i] <= 0.0f)) return (float) i / (float) n;
    }
    else if (d.snap == SampleOscData::SnapLoop)
    {
        const auto& id = ids().osc[osc];
        std::vector<float> pts { 0.0f, 1.0f };
        if (s.loopStart >= 0) pts.push_back ((float) s.loopStart / (float) n);
        if (s.loopEnd >= 0) pts.push_back ((float) s.loopEnd / (float) n);
        const float st = proc.params.get (id.start), en = proc.params.get (id.end);
        pts.push_back (st + (en - st) * proc.params.get (id.loopStart));
        pts.push_back (st + (en - st) * proc.params.get (id.loopEnd));
        float best = f;
        for (float p : pts) if (std::abs (p - f) < std::abs (best - f) || same (best, f)) best = p;
        return best;
    }
    else if (d.snap == SampleOscData::SnapBeats)
    {
        // The host tempo isn't known on the message thread: beats at 120 BPM.
        const double beat = s.sampleRate * 0.5 / (double) n;
        if (beat > 0.0 && beat < 1.0) return (float) (std::round (f / beat) * beat);
    }
    return f;
}
}

void SampleDisplay::mouseDown (const MouseEvent& e)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const auto& id = ids().osc[osc];
    const auto r = waveArea();
    if (e.mods.isPopupMenu())
    {
        if (type == 4 && e.position.x > r.getRight())
        {
            Menu m;
            toggleItem (m, proc, osc, "Smooth", &SampleOscData::specSmooth);
            toggleItem (m, proc, osc, "Post Warp", &SampleOscData::specPostWarp);
            showContextMenu (*this, e.getPosition(), m);
            return;
        }
        showContextMenu (*this, e.getPosition(), sampleOperationsMenu (ctx, osc, type));
        return;
    }
    grab = hitTestGrab (e.position);
    view0 = viewStart; view1 = viewEnd;
    grabFrac0 = fracAt (e.position.x);
    // Alt-click in Slice Manual: add or remove a slice.
    if (d.slicing == SampleOscData::SliceManual && e.mods.isAltDown())
    {
        const float f = grabFrac0;
        const bool onSlice = grab == Grab::Slice;
        editData (proc, osc, onSlice ? "Remove Slice" : "Add Slice", [f, onSlice] (SampleOscData& dd)
        {
            if (onSlice)
            {
                auto it = std::min_element (dd.manualSlices.begin(), dd.manualSlices.end(), [f] (float a, float b) { return std::abs (a - f) < std::abs (b - f); });
                if (it != dd.manualSlices.end()) dd.manualSlices.erase (it);
            }
            else dd.manualSlices.push_back (f);
        }, true);
        grab = Grab::None;
        return;
    }
    if (d.showXY && std::abs (e.position.y - (r.getBottom() - d.yValue * r.getHeight())) < 7.0f && grab == Grab::View)
        grab = Grab::None, grabA = -2.0f;   // the X|Y dot: drag sets Y
    if (grab == Grab::Slice && d.processed != nullptr)
    {
        grabSlice = -1;
        float best = 1e9f;
        for (int k = 0; k < (int) d.manualSlices.size(); ++k)
            if (std::abs (d.manualSlices[(size_t) k] - grabFrac0) < best) { best = std::abs (d.manualSlices[(size_t) k] - grabFrac0); grabSlice = k; }
    }
    if (grab == Grab::LoopBar) { grabA = proc.params.get (id.loopStart); grabB = proc.params.get (id.loopEnd); proc.beginGesture (id.loopStart); proc.beginGesture (id.loopEnd); }
    const int p = paramForGrab (osc, grab);
    if (p >= 0) proc.beginGesture (p);
}

void SampleDisplay::mouseDrag (const MouseEvent& e)
{
    auto& proc = ctx.proc;
    const auto& d = proc.patch().sample[(size_t) osc];
    const auto& id = ids().osc[osc];
    const auto r = waveArea();
    const float f = fracAt (e.position.x);
    const float st = proc.params.get (id.start), en = proc.params.get (id.end);
    switch (grab)
    {
        case Grab::Start: proc.set (id.start, jlimit (0.0f, en, snapFrac (d, proc, osc, f))); break;
        case Grab::End:   proc.set (id.end, jlimit (st, 1.0f, snapFrac (d, proc, osc, f))); break;
        case Grab::LoopStart:
        case Grab::LoopEnd:
        {
            const float rel = jlimit (0.0f, 1.0f, (snapFrac (d, proc, osc, f) - st) / jmax (1e-6f, en - st));
            proc.set (grab == Grab::LoopStart ? id.loopStart : id.loopEnd, rel);
            break;
        }
        case Grab::LoopBar:
        {
            const float dRel = (f - grabFrac0) / jmax (1e-6f, en - st);
            const float len = grabB - grabA;
            const float a = jlimit (0.0f, 1.0f - jmax (0.0f, len), grabA + dRel);
            proc.set (id.loopStart, a);
            proc.set (id.loopEnd, a + len);
            break;
        }
        case Grab::HiFreq:
        case Grab::LoFreq:
            proc.params.setNorm (grab == Grab::HiFreq ? id.specHi : id.specLo, jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight()));
            break;
        case Grab::Threshold:
        {
            const float t = jlimit (0.01f, 1.0f, (r.getCentreY() - e.position.y) / (r.getHeight() * 0.48f));
            editData (proc, osc, {}, [t] (SampleOscData& dd) { dd.sliceThreshold = t; }, true);
            break;
        }
        case Grab::Slice:
            if (grabSlice >= 0)
            {
                const int k = grabSlice;
                editData (proc, osc, {}, [k, f] (SampleOscData& dd) { if (k < (int) dd.manualSlices.size()) dd.manualSlices[(size_t) k] = f; }, true);
            }
            break;
        case Grab::View:
        {
            if (d.zoomToStartEnd || d.slicing == SampleOscData::SliceManual) break;
            const float dy = (float) e.getDistanceFromDragStartY();
            const float zoom = std::exp (dy * 0.02f);
            const float span = jlimit (0.002f, 1.0f, (view1 - view0) * zoom);
            const float centre = grabFrac0 - (float) e.getDistanceFromDragStartX() / r.getWidth() * span;
            const float rel = (grabFrac0 - view0) / jmax (1e-6f, view1 - view0);
            float a = centre - rel * span;
            a = jlimit (0.0f, 1.0f - span, a);
            viewStart = a; viewEnd = a + span;
            break;
        }
        case Grab::None:
            if (same (grabA, -2.0f))
                editData (proc, osc, {}, [y = jlimit (0.0f, 1.0f, (r.getBottom() - e.position.y) / r.getHeight())] (SampleOscData& dd) { dd.yValue = y; });
            break;
    }
    refresh();
    repaint();
}

void SampleDisplay::mouseUp (const MouseEvent&)
{
    auto& proc = ctx.proc;
    const auto& id = ids().osc[osc];
    const int p = paramForGrab (osc, grab);
    if (p >= 0) proc.endGesture (p);
    if (grab == Grab::LoopBar) { proc.endGesture (id.loopStart); proc.endGesture (id.loopEnd); }
    if (grab == Grab::Threshold) proc.commitHistory ("Slice Threshold");
    if (grab == Grab::Slice) proc.commitHistory ("Move Slice");
    if (grab == Grab::None && same (grabA, -2.0f)) proc.commitHistory ("X|Y");
    grab = Grab::None;
    grabA = 0.0f;
    grabSlice = -1;
}

void SampleDisplay::mouseDoubleClick (const MouseEvent& e)
{
    if (hitTestGrab (e.position) == Grab::LoopBar)
    {
        const auto& id = ids().osc[osc];
        ctx.proc.editAll ("Reset Loop", [&id] (Patch&, ParamValues& v) { v[id.loopStart] = 0.0f; v[id.loopEnd] = 1.0f; });
    }
}

void SampleDisplay::mouseWheelMove (const MouseEvent& e, const MouseWheelDetails& w)
{
    if (ctx.proc.patch().sample[(size_t) osc].zoomToStartEnd) return;
    const float f = fracAt (e.position.x);
    const float span = jlimit (0.002f, 1.0f, (viewEnd - viewStart) * (w.deltaY > 0 ? 0.8f : 1.25f));
    const float rel = (f - viewStart) / jmax (1e-6f, viewEnd - viewStart);
    const float a = jlimit (0.0f, 1.0f - span, f - rel * span);
    viewStart = a; viewEnd = a + span;
    refresh();
}

bool SampleDisplay::isInterestedInFileDrag (const StringArray& files)
{
    for (auto& f : files) if (isAudioFile (f) || (type == 4 && File (f).hasFileExtension (".png"))) return true;
    return false;
}

void SampleDisplay::filesDropped (const StringArray& files, int, int)
{
    for (auto& f : files)
    {
        if (type == 4 && File (f).hasFileExtension (".png")) { setSample (ctx.proc, osc, sampleFromImage (File (f)), "Import PNG"); return; }
        if (isAudioFile (f)) { ctx.proc.loadSampleFile (osc, File (f)); return; }
    }
}

// ===========================================================================
// ZoneMapDisplay
// ===========================================================================
ZoneMapDisplay::ZoneMapDisplay (PanelContext& c, int o) : ctx (c), osc (o) { ctx.add (this); }
ZoneMapDisplay::~ZoneMapDisplay() { ctx.remove (this); }

void ZoneMapDisplay::refresh()
{
    const auto& d = ctx.proc.patch().sample[(size_t) osc];
    int z = -1;
    if (d.multi != nullptr && ctx.proc.telemetry().activeVoices.load() > 0)
        z = d.multi->findZone (ctx.proc.telemetry().lastNote.load(), jlimit (1, 127, roundToInt (ctx.proc.telemetry().lastVelocity.load() * 127.0f)));
    if (z >= 0) lastZone = z;
    if (z != shownZone || d.multi.get() != shownMulti) { shownZone = z; shownMulti = d.multi.get(); repaint(); }
}

void ZoneMapDisplay::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    drawLcdBox (g, r);
    const auto& d = ctx.proc.patch().sample[(size_t) osc];
    const auto in = r.reduced (4.0f);
    if (d.multi == nullptr || d.multi->zones.empty())
    {
        drawText (g, "Drop an SFZ or audio file here", in, 11.0f, colours::lcdDim, true);
        return;
    }
    const auto& zones = d.multi->zones;
    if (showWave)
    {
        const int z = jlimit (0, (int) zones.size() - 1, lastZone < 0 ? 0 : lastZone);
        const auto& s = zones[(size_t) z].sample;
        if (s != nullptr && s->frames() > 0)
        {
            g.setColour (colours::brand.withAlpha (0.85f));
            const int n = s->frames();
            for (int x = 0; x < (int) in.getWidth(); ++x)
            {
                const int a = (int) ((float) x / in.getWidth() * (float) n), b = jmin (n, (int) ((float) (x + 1) / in.getWidth() * (float) n) + 1);
                float lo = 0, hi = 0;
                for (int i = a; i < b; i += jmax (1, (b - a) / 32)) { lo = jmin (lo, s->left[(size_t) i]); hi = jmax (hi, s->left[(size_t) i]); }
                g.drawVerticalLine ((int) in.getX() + x, in.getCentreY() - hi * in.getHeight() * 0.45f, in.getCentreY() - lo * in.getHeight() * 0.45f + 1.0f);
            }
            drawText (g, s->name, in.withHeight (13.0f), 10.5f, colours::lcd, true, Justification::topLeft);
        }
        return;
    }
    int lo = 127, hi = 0;
    for (auto& z : zones) { lo = jmin (lo, z.loKey); hi = jmax (hi, z.hiKey); }
    const float kw = in.getWidth() / (float) jmax (1, hi - lo + 1);
    for (int i = 0; i < (int) zones.size(); ++i)
    {
        const auto& z = zones[(size_t) i];
        const auto box = Rectangle<float>::leftTopRightBottom (in.getX() + (float) (z.loKey - lo) * kw, in.getBottom() - (float) z.hiVel / 127.0f * in.getHeight(),
                                                              in.getX() + (float) (z.hiKey - lo + 1) * kw, in.getBottom() - (float) (z.loVel - 1) / 127.0f * in.getHeight());
        g.setColour (i == shownZone ? colours::lcd.withAlpha (0.6f) : colours::brand.withAlpha (0.18f));
        g.fillRect (box.reduced (0.5f));
        g.setColour (colours::brand.withAlpha (0.7f));
        g.drawRect (box.reduced (0.5f), 1.0f);
    }
    drawText (g, String (d.multi->name) + "  " + String ((int) zones.size()) + " zones", in.withHeight (13.0f), 10.5f, colours::lcd, true, Justification::topLeft);
}

void ZoneMapDisplay::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    showWave = ! showWave;
    repaint();
}

bool ZoneMapDisplay::isInterestedInFileDrag (const StringArray& files)
{
    for (auto& f : files) if (isAudioFile (f) || File (f).hasFileExtension (".sfz")) return true;
    return false;
}

void ZoneMapDisplay::filesDropped (const StringArray& files, int, int)
{
    for (auto& f : files)
        if (isAudioFile (f) || File (f).hasFileExtension (".sfz")) { loadFileInto (ctx.proc, osc, File (f)); return; }
}

// ===========================================================================
// XFadeButton
// ===========================================================================
XFadeButton::XFadeButton (PanelContext& c, int o) : ctx (c), osc (o) { ctx.add (this); refresh(); }
XFadeButton::~XFadeButton() { ctx.remove (this); }

void XFadeButton::refresh()
{
    const float v = ctx.proc.params.get (ids().osc[osc].xfade);
    if (! same (v, shown))
    {
        shown = v;
        setTooltip (letterOf (osc) + " Loop X-Fade : " + String (roundToInt (v * 100.0f)) + " % (drag up)");
        repaint();
    }
}

void XFadeButton::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = shown > 0.0f;
    g.setColour (on ? loopBlue() : colours::black);
    g.fillRect (r);
    g.setColour (colours::inkDim);
    g.drawRect (r, 1.0f);
    drawIcon (g, IconButton::XFade, r.reduced (3.0f), on ? colours::black : colours::ink);
}

void XFadeButton::mouseDown (const MouseEvent& e)
{
    const int p = ids().osc[osc].xfade;
    if (e.mods.isPopupMenu()) { showContextMenu (*this, e.getPosition(), controlMenu (ctx, p)); return; }
    startV = ctx.proc.params.get (p);
    dragging = false;
}

void XFadeButton::mouseDrag (const MouseEvent& e)
{
    const int p = ids().osc[osc].xfade;
    if (e.mods.isPopupMenu()) return;
    if (! dragging) { dragging = true; ctx.proc.beginGesture (p); }
    ctx.proc.set (p, jlimit (0.0f, 1.0f, startV - (float) e.getDistanceFromDragStartY() / 120.0f));
    refresh();
    if (ctx.setStatus) ctx.setStatus (getTooltip());
}

void XFadeButton::mouseUp (const MouseEvent&)
{
    if (dragging) ctx.proc.endGesture (ids().osc[osc].xfade);
    dragging = false;
}
} // namespace tincture::ui::oscui
