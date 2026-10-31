// FxPage.cpp — the FX page: rack tabs (MAIN / BUS 1 / BUS 2) with meters, the
// "+ FX" button, rack preset save/menu, the module list (reorder by drag,
// bypass, remove, splitter bands with their own add buttons) and the rack
// view with every module's editor stacked (spec C §3).
#include "FxModules.h"

namespace tincture::ui
{
using namespace juce;
using namespace fxui;

namespace
{
// ---------------------------------------------------------------------------
// Factory rack presets (Factory > category > name). Values are plain units
// (dB, Hz, %, ms as the slot shows them); slot -1 = the unit's MIX (0..1).
// ---------------------------------------------------------------------------
struct FactoryModule { FxType type; std::vector<std::pair<int, float>> plain; int band = -1; };   // band: into the last splitter
struct FactoryRack { const char* category; const char* name; std::vector<FactoryModule> modules; };

const std::vector<FactoryRack>& factoryRacks()
{
    using T = FxType;
    static const std::vector<FactoryRack> racks {
        { "Bass Processing", "Sub Focus",        { { T::Utility, { { 4, 1 }, { 5, 140 } } }, { T::Compressor, { { 1, -22 }, { 2, 4 } } }, { T::Equalizer, { { 3, 3 }, { 7, -2 } } } } },
        { "Bass Processing", "Grit Bass",        { { T::Distortion, { { 0, 0 }, { 5, 0.45f }, { -1, 0.6f } } }, { T::Equalizer, { { 0, 2 }, { 1, 40 } } }, { T::Compressor, { { 1, -18 } } } } },
        { "Bus Processing",  "Glue Bus",         { { T::Compressor, { { 1, -14 }, { 2, 2 }, { 3, 30 }, { 4, 200 } } }, { T::Utility, { { 6, 1.1f } } } } },
        { "Bus Processing",  "Wide Bus",         { { T::Hyper, { { 1, 3 }, { 5, 0.4f }, { -1, 0.3f } } }, { T::Utility, { { 6, 1.4f }, { 4, 1 } } } } },
        { "Distorted",       "Acid Dist Delay",  { { T::Distortion, { { 0, 0 }, { 1, 2 }, { 3, 900 }, { 5, 0.6f } } }, { T::Delay, { { 0, 1 }, { 8, 0.45f }, { -1, 0.25f } } } } },
        { "Distorted",       "Folded",           { { T::Distortion, { { 0, 6 }, { 5, 0.5f } } }, { T::Filter, { { 0, 5 }, { 1, 3000 } } } } },
        { "Drums",           "Punch",            { { T::Compressor, { { 1, -20 }, { 2, 6 }, { 3, 15 }, { 4, 80 } } }, { T::Equalizer, { { 1, 80 }, { 3, 3 }, { 5, 5000 }, { 7, 2 } } } } },
        { "Drums",           "Room Kit",         { { T::Compressor, { { 1, -16 } } }, { T::Reverb, { { 0, 1 }, { 3, 0.3f }, { -1, 0.15f } } } } },
        { "Dynamics",        "OTT Style",        { { T::Compressor, { { 0, 1 }, { 1, -30 }, { 2, 10 }, { 7, 0.5f }, { -1, 0.5f } } } } },
        { "Dynamics",        "Limiter",          { { T::Compressor, { { 1, -6 }, { 2, 1.0e9f } } } } },
        { "Main FX Bus",     "Classic Chain",    { { T::Chorus, {} }, { T::Delay, { { -1, 0.2f } } }, { T::Reverb, { { -1, 0.2f } } } } },
        { "Main FX Bus",     "Polish",           { { T::Equalizer, { { 7, 2 } } }, { T::Compressor, { { 1, -12 }, { 2, 2 } } }, { T::Utility, { { 6, 1.2f } } } } },
        { "Modulated",       "Phase Swirl",      { { T::Phaser, { { 3, 0.7f }, { 6, 0.4f } } }, { T::Flanger, { { 2, 0.4f }, { -1, 0.3f } } } } },
        { "Modulated",       "Shifted Space",    { { T::Bode, { { 0, 0.2f }, { 6, 0.5f }, { -1, 0.4f } } }, { T::Reverb, { { 0, 3 }, { -1, 0.3f } } } } },
        { "Reverb",          "Big Hall",         { { T::Reverb, { { 0, 1 }, { 3, 0.8f }, { 8, 0.7f }, { -1, 0.35f } } } } },
        { "Reverb",          "Plate Split",      { { T::SplitterLH, { { 0, 300 } } }, { T::Reverb, { { 0, 0 }, { -1, 0.35f } }, 1 } } },
    };
    return racks;
}

Node buildFactoryRack (const FactoryRack& fr)
{
    FxLayout L;
    std::map<int, FxUnitValues> vals;
    int lastSplitter = -1;
    for (auto& m : fr.modules)
    {
        const int u = m.band >= 0 && lastSplitter >= 0 ? L.add (FxList { kFxMain, lastSplitter, m.band }, m.type)
                                                       : L.add (kFxMain, m.type);
        if (m.band < 0 && fxInfo (m.type).splitter) lastSplitter = (int) L.racks[0].modules.size() - 1;
        if (u < 0) continue;
        FxUnitValues v;
        v.unit = u;
        v.p = fxDefaults (m.type);
        v.mix = fxDefaultMix (m.type);
        for (auto [k, plain] : m.plain)
        {
            if (k < 0) v.mix = plain;
            else v.p[(size_t) k] = fxInfo (m.type).params[(size_t) k].toNorm (plain);
        }
        vals[u] = v;
    }
    L.racks[0].presetName = fr.name;
    Node n = L.saveRack (kFxMain, [&vals] (int u) { return vals[u]; });
    n.set ("name", fr.name);
    return n;
}

Node& rackClipboard() { static Node n; return n; }
bool& rackClipboardFull() { static bool b = false; return b; }

// Folder tree of user files as a menu.
void addFileTree (Menu& m, const File& folder, const String& pattern, const std::function<void (const File&)>& pick)
{
    auto dirs = folder.findChildFiles (File::findDirectories, false);
    dirs.sort();
    for (auto& d : dirs)
    {
        Menu sub;
        addFileTree (sub, d, pattern, pick);
        if (! sub.empty()) m.sub (d.getFileName(), sub);
    }
    auto files = folder.findChildFiles (File::findFiles, false, pattern);
    files.sort();
    for (auto& f : files) m.add (f.getFileNameWithoutExtension(), [pick, f] { pick (f); });
}

constexpr int kRowH = 26;
} // namespace

// ===========================================================================
class FxPage final : public Component, public Refreshable, public FxHost
{
public:
    explicit FxPage (PanelContext& c) : ctx (c), proc (c.proc)
    {
        ctx.add (this);
        rack = jlimit (0, 2, (int) proc.uiState.getProperty ("fxRack", 0));
        const char* names[] { "MAIN", "BUS 1", "BUS 2" };
        for (int r = 0; r < 3; ++r)
        {
            tabs[r].setLabel (names[r]);
            tabs[r].onClick = [this, r] (const ModifierKeys&) { selectRack (r); };
            tabs[r].setTooltip (String ("Show the ") + names[r] + " FX rack");
            tabs[r].setLitColour (Colour (0xffe0a050));
            addAndMakeVisible (tabs[r]);
            for (auto* m : { &meters[r][0], &meters[r][1] })
            {
                m->setInterceptsMouseClicks (false, false);
                addAndMakeVisible (*m);
            }
        }
        addFx.onClick = [this] (const ModifierKeys&) { showMenu (addFx, addMenu (proc, FxList { rack, -1, -1 }), MenuOptions { MenuOptions::Place::Below, {}, 170, false }); };
        addFx.setTooltip ("Add an FX module at the end of this rack");
        addAndMakeVisible (addFx);
        saveBtn.onClick = [this] (const ModifierKeys&) { saveRackDialog(); };
        saveBtn.setTooltip ("Save this rack as a user rack preset");
        addAndMakeVisible (saveBtn);
        presetBox.getText = [this] { return String (proc.patch().fx.racks[(size_t) rack].presetName).toUpperCase(); };
        presetBox.onOpen = [this] { showMenu (presetBox, presetMenu()); };
        presetBox.fontH = 13.0f;
        presetBox.setTooltip ("Rack presets");
        addAndMakeVisible (presetBox);

        list.page = this;
        listView.setViewedComponent (&list, false);
        listView.setScrollBarsShown (true, false);
        listView.setScrollBarThickness (8);
        addAndMakeVisible (listView);
        content.page = this;
        rackView.setViewedComponent (&content, false);
        rackView.setScrollBarsShown (true, false);
        rackView.setScrollBarThickness (10);
        addAndMakeVisible (rackView);
        addChildComponent (popover);
        rebuild();
    }
    ~FxPage() override { ctx.remove (this); }

    // --- FxHost ---------------------------------------------------------------
    void openCurveEditor (const String& title, Colour col, std::function<Curve()> get, std::function<void (const Curve&, bool)> set) override
    {
        popover.title = title;
        popover.editor.colour = col;
        popover.editor.get = std::move (get);
        popover.editor.set = std::move (set);
        popover.setBounds (rackView.getBounds().withSizeKeepingCentre (460, 360));
        popover.setVisible (true);
        popover.toFront (false);
        popover.repaint();
    }
    int selectedBand (int unit) const override
    {
        auto it = bandSel.find (unit);
        return it == bandSel.end() ? 0 : it->second;
    }
    void selectBand (int unit, int band) override
    {
        if (selectedBand (unit) == band) return;
        bandSel[unit] = band;
        rebuild();
    }
    void moduleDragged (const FxPath& from, Point<int> pos, bool finished, bool alt) override
    {
        const auto local = content.getLocalPoint (this, pos);
        FxList to;
        int at = 0;
        content.dropAt (local.y, to, at);
        content.dropLine = finished ? -1 : content.lineFor (to, at);
        content.repaint();
        if (finished) applyDrop (from, to, at, alt);
    }

    // --- Refresh --------------------------------------------------------------
    void refresh() override
    {
        if (! fxui::panelVisible (*this)) return;
        const auto sig = layoutSignature (proc.patch().fx) + "#" + String (rack);
        if (sig != shownSig) rebuild();
        presetBox.refresh();
        for (int r = 0; r < 3; ++r)
            for (int ch = 0; ch < 2; ++ch) meters[r][ch].push (proc.fxEngine().rackPeak (r, ch));
        if (popover.isVisible()) popover.editor.repaint();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (colours::panel);
        drawSection (g, listView.getBounds().toFloat().expanded (4.0f, 8.0f).withTrimmedTop (-2.0f), "MODULES");
        // Pointer under the selected tab.
        const auto t = tabs[rack].getBounds().toFloat();
        Path p;
        p.addTriangle (t.getCentreX() - 6, t.getBottom() + 1, t.getCentreX() + 6, t.getBottom() + 1, t.getCentreX(), t.getBottom() + 7);
        g.setColour (Colour (0xffe0a050));
        g.fillPath (p);
        // Rack rails behind the rack view.
        const auto rv = rackView.getBounds().toFloat();
        g.setColour (colours::panelEdge);
        g.fillRect (rv.expanded (4.0f));
    }

    void resized() override
    {
        for (int r = 0; r < 3; ++r)
        {
            tabs[r].setBounds (12 + r * 107, 12, 103, 30);
            meters[r][0].setBounds (12 + r * 107 + 88, 17, 4, 20);
            meters[r][1].setBounds (12 + r * 107 + 94, 17, 4, 20);
        }
        addFx.setBounds (12, 54, 66, 28);
        saveBtn.setBounds (84, 54, 56, 28);
        presetBox.setBounds (146, 54, 186, 28);
        listView.setBounds (18, 104, 310, 680);
        rackView.setBounds (346, 12, 1042, 776);
        layoutContent();
    }

    // --- Operations -----------------------------------------------------------
    void selectRack (int r)
    {
        rack = jlimit (0, 2, r);
        proc.uiState.setProperty ("fxRack", rack, nullptr);
        for (int i = 0; i < 3; ++i) tabs[i].setLit (i == rack);
        rebuild();
        repaint();
    }

    void applyDrop (const FxPath& from, const FxList& to, int at, bool alt)
    {
        if (alt) duplicateModule (proc, from, to, at);
        else moveModule (proc, from, to, at);
        list.dropLine = content.dropLine = -1;
        list.repaint();
        content.repaint();
    }

    void saveRackDialog()
    {
        if (! ctx.editValue) return;
        const auto cur = String (proc.patch().fx.racks[(size_t) rack].presetName);
        const int r = rack;
        ctx.editValue (presetBox, cur == "Init" || cur == "Custom" ? String ("My Rack") : cur, [this, r] (const String& name)
        {
            const auto clean = File::createLegalFileName (name.trim());
            if (clean.isEmpty()) return;
            writeNode (rackPresetFolder().getChildFile (clean + ".tfxrack"), saveRackNode (proc, r, clean));
            proc.editPatch ("Name FX Bus", [r, clean] (Patch& p) { p.fx.racks[(size_t) r].presetName = clean.toStdString(); });
        });
    }

    Menu presetMenu()
    {
        Menu factory;
        std::map<String, Menu> byCat;
        std::vector<String> order;
        const auto cur = String (proc.patch().fx.racks[(size_t) rack].presetName);
        const int r = rack;
        for (size_t i = 0; i < factoryRacks().size(); ++i)
        {
            const auto& fr = factoryRacks()[i];
            const String cat (fr.category);
            if (byCat.find (cat) == byCat.end()) order.push_back (cat);
            byCat[cat].add (fr.name, [this, r, i] { loadRackPreset (proc, r, buildFactoryRack (factoryRacks()[i]), factoryRacks()[i].name); }, cur == fr.name);
        }
        for (auto& c : order) factory.sub (c, byCat[c]);
        factory.add ("Init", [this, r] { clearRack (r, "Init FX Bus"); }, cur == "Init");
        Menu user;
        addFileTree (user, rackPresetFolder(), "*.tfxrack", [this, r] (const File& f)
        {
            Node n;
            if (readNode (f, n)) loadRackPreset (proc, r, n, f.getFileNameWithoutExtension());
        });
        if (user.empty()) user.add ("(no user rack presets)", {}, false, false);
        Menu m;
        m.sub ("Factory", factory);
        m.sub ("User", user);
        return m;
    }

    void clearRack (int r, const String& label)
    {
        proc.editPatch (label, [r] (Patch& p)
        {
            const auto freed = p.fx.clearRack (r);
            p.matrix.removeRowsWhere ([&freed] (int dest)
            {
                for (int u : freed)
                {
                    if (dest == ids().fx[u].mix) return true;
                    for (int k = 0; k < kFxParams; ++k) if (dest == ids().fx[u].p[k]) return true;
                }
                return false;
            });
        });
    }

    Menu rackMenu()
    {
        Menu m;
        const int r = rack;
        m.sub ("Add FX Module", addMenu (proc, FxList { r, -1, -1 }));
        m.separator();
        const bool any = ! proc.patch().fx.racks[(size_t) r].modules.empty();
        m.add ("Cut FX Bus", [this, r]
        {
            rackClipboard() = saveRackNode (proc, r, String (proc.patch().fx.racks[(size_t) r].presetName));
            rackClipboardFull() = true;
            clearRack (r, "Cut FX Bus");
        }, false, any);
        m.add ("Copy FX Bus", [this, r]
        {
            rackClipboard() = saveRackNode (proc, r, String (proc.patch().fx.racks[(size_t) r].presetName));
            rackClipboardFull() = true;
        }, false, any);
        m.add ("Paste FX Bus", [this, r]
        {
            if (rackClipboardFull()) loadRackPreset (proc, r, rackClipboard(), String (rackClipboard().str ("name", "Custom")));
        }, false, rackClipboardFull());
        m.add ("Clear FX Bus", [this, r] { clearRack (r, "Clear FX Bus"); }, false, any);
        m.separator();
        const bool locked = proc.patch().fx.racks[(size_t) r].locked;
        m.toggle ("Lock FX Bus", locked, [this, r, locked] { proc.editPatch ("Lock FX Bus", [r, locked] (Patch& p) { p.fx.setLocked (r, ! locked); }); });
        const bool all = proc.patch().fx.allLocked();
        m.toggle ("Lock All FX Busses", all, [this, all] { proc.editPatch ("Lock All FX Busses", [all] (Patch& p) { p.fx.setAllLocked (! all); }); });
        m.separator();
        m.add ("Load FX Bus...", [this, r]
        {
            rackPresetFolder().createDirectory();
            chooser = std::make_unique<FileChooser> ("Load FX Bus", rackPresetFolder(), "*.tfxrack");
            chooser->launchAsync (FileBrowserComponent::openMode | FileBrowserComponent::canSelectFiles, [this, r] (const FileChooser& fc)
            {
                Node n;
                const auto f = fc.getResult();
                if (f.existsAsFile() && readNode (f, n)) loadRackPreset (proc, r, n, f.getFileNameWithoutExtension());
            });
        });
        m.add ("Save FX Bus...", [this] { saveRackDialog(); });
        return m;
    }

    // --- Building -------------------------------------------------------------
    void rebuild()
    {
        shownSig = layoutSignature (proc.patch().fx) + "#" + String (rack);
        for (int i = 0; i < 3; ++i) tabs[i].setLit (i == rack);
        list.build();
        const auto scroll = rackView.getViewPosition();
        content.build();
        layoutContent();
        rackView.setViewPosition (scroll);
        presetBox.refresh();
    }

    void layoutContent()
    {
        list.setSize (listView.getWidth() - 10, jmax (listView.getHeight(), (int) list.rows.size() * kRowH + 40));
        list.layout();
        content.setSize (rackView.getWidth() - 12, content.totalHeight());
        content.layout();
    }

    void scrollTo (const FxPath& p)
    {
        for (auto& e : content.editors)
            if (e->path().list.rack == p.list.rack && e->path().list.splitter == p.list.splitter && e->path().list.band == p.list.band && e->path().index == p.index)
            {
                rackView.setViewPosition (0, jmax (0, e->getY() - 6));
                return;
            }
    }

    // =========================================================================
    // The module list.
    // =========================================================================
    struct Row
    {
        bool band = false;
        FxPath path;            // module rows: the module; band rows: the splitter (path.index) in the rack
        int bandIndex = -1;
        int depth = 0;
        FxType type {};
        String name;
        bool bypass = false;
    };

    struct ListComp final : public Component
    {
        FxPage* page = nullptr;
        std::vector<Row> rows;
        std::vector<std::unique_ptr<BypassButton>> bypass;
        std::vector<std::unique_ptr<PanelButton>> extra;     // × (modules) or + (bands)
        int dropLine = -1, dragRow = -1;
        bool dragging = false;

        void build()
        {
            rows.clear();
            bypass.clear();
            extra.clear();
            const int rk = page->rack;
            const auto& mods = page->proc.patch().fx.racks[(size_t) rk].modules;
            for (int i = 0; i < (int) mods.size(); ++i)
            {
                const auto& m = mods[(size_t) i];
                rows.push_back ({ false, FxPath { FxList { rk, -1, -1 }, i }, -1, 0, m.type, fxInfo (m.type).caps, m.bypass });
                if (m.isSplitter())
                    for (int b = 0; b < fxInfo (m.type).numBands; ++b)
                    {
                        rows.push_back ({ true, FxPath { FxList { rk, -1, -1 }, i }, b, 1, m.type,
                                          fxInfo (m.type).bandNames[(size_t) b], m.bandBypass[(size_t) b] });
                        for (int j = 0; j < (int) m.bands[(size_t) b].size(); ++j)
                        {
                            const auto& c = m.bands[(size_t) b][(size_t) j];
                            rows.push_back ({ false, FxPath { FxList { rk, i, b }, j }, -1, 2, c.type, fxInfo (c.type).caps, c.bypass });
                        }
                    }
            }
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const auto row = rows[i];
                auto b = std::make_unique<BypassButton>();
                b->set (row.bypass);
                auto* pg = page;
                if (row.band)
                    b->onClick = [pg, row] (const ModifierKeys&) { toggleBandBypass (pg->proc, row.path.list.rack, row.path.index, row.bandIndex); };
                else
                    b->onClick = [pg, row] (const ModifierKeys& m) { toggleBypass (pg->proc, row.path, m.isAltDown()); };
                addAndMakeVisible (*b);
                bypass.push_back (std::move (b));
                auto x = std::make_unique<PanelButton> (row.band ? "+" : "x");
                x->setFontHeight (12.0f);
                if (row.band)
                {
                    PanelButton* raw = x.get();
                    x->onClick = [pg, row, raw] (const ModifierKeys&)
                    {
                        showMenu (*raw, addMenu (pg->proc, FxList { row.path.list.rack, row.path.index, row.bandIndex }),
                                  MenuOptions { MenuOptions::Place::Below, {}, 170, false });
                    };
                    x->setTooltip ("Add a module to " + row.name);
                }
                else
                {
                    x->onClick = [pg, row] (const ModifierKeys&) { removeModule (pg->proc, row.path); };
                    x->setTooltip ("Remove this module");
                }
                addAndMakeVisible (*x);
                extra.push_back (std::move (x));
            }
        }
        void layout()
        {
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const int y = (int) i * kRowH;
                bypass[i]->setBounds (getWidth() - 52, y + 3, 20, 20);
                extra[i]->setBounds (getWidth() - 28, y + 3, 22, 20);
            }
        }
        void paint (Graphics& g) override
        {
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const auto& r = rows[i];
                const auto a = Rectangle<float> (0.0f, (float) i * kRowH, (float) getWidth(), (float) kRowH);
                const float indent = (float) r.depth * 14.0f;
                g.setColour (i % 2 ? colours::panel : colours::panel.brighter (0.04f));
                g.fillRect (a);
                if (r.band)
                {
                    drawText (g, "+ " + r.name, a.withTrimmedLeft (indent + 6).withTrimmedRight (60), 12.0f,
                              r.bypass ? colours::inkDim : colours::ink, true, Justification::centredLeft);
                }
                else
                {
                    g.setColour (typeColour (r.type));
                    g.fillRect (Rectangle<float> (indent + 4.0f, a.getY() + 5.0f, 4.0f, kRowH - 10.0f));
                    drawText (g, r.name, a.withTrimmedLeft (indent + 14).withTrimmedRight (60), 13.0f,
                              r.bypass ? colours::inkDim : colours::ink, true, Justification::centredLeft);
                }
            }
            if (rows.empty())
                drawText (g, "Empty rack: click + FX", getLocalBounds().toFloat().withHeight (40.0f), 12.0f, colours::inkDim, false);
            if (dropLine >= 0)
            {
                g.setColour (colours::ledYellow);
                g.fillRect (0, dropLine - 1, getWidth(), 3);
            }
        }
        int rowAt (int y) const { return y < 0 ? -1 : y / kRowH; }
        void dropAt (int y, FxList& to, int& at) const
        {
            const int rk = page->rack;
            const int i = rowAt (y);
            if (i < 0 || i >= (int) rows.size())
            {
                to = FxList { rk, -1, -1 };
                at = (int) page->proc.patch().fx.racks[(size_t) rk].modules.size();
                return;
            }
            const auto& r = rows[(size_t) i];
            const bool lower = (y % kRowH) >= kRowH / 2;
            if (r.band) { to = FxList { rk, r.path.index, r.bandIndex }; at = 0; return; }
            to = r.path.list;
            at = r.path.index + (lower ? 1 : 0);
        }
        int lineFor (const FxList& to, int at) const
        {
            // y of the insertion point for (to, at).
            int lastInList = -1;
            for (size_t i = 0; i < rows.size(); ++i)
            {
                const auto& r = rows[i];
                if (r.band && to.splitter == r.path.index && to.band == r.bandIndex && at == 0) return (int) (i + 1) * kRowH;
                if (! r.band && r.path.list.rack == to.rack && r.path.list.splitter == to.splitter && r.path.list.band == to.band)
                {
                    if (r.path.index == at) return (int) i * kRowH;
                    lastInList = (int) i;
                }
            }
            return lastInList >= 0 ? (lastInList + 1) * kRowH : (int) rows.size() * kRowH;
        }
        void mouseDown (const MouseEvent& e) override
        {
            dragRow = rowAt (e.y);
            dragging = false;
            if (e.mods.isPopupMenu())
            {
                if (dragRow >= 0 && dragRow < (int) rows.size() && ! rows[(size_t) dragRow].band)
                    showContextMenu (*this, e.getPosition(), moduleMenu (page->ctx, rows[(size_t) dragRow].path, *this));
                else
                    showContextMenu (*this, e.getPosition(), page->rackMenu());
                dragRow = -1;
            }
        }
        void mouseDrag (const MouseEvent& e) override
        {
            if (dragRow < 0 || dragRow >= (int) rows.size() || rows[(size_t) dragRow].band) return;
            if (! dragging && e.getDistanceFromDragStart() < 4) return;
            dragging = true;
            FxList to;
            int at = 0;
            dropAt (e.y, to, at);
            dropLine = lineFor (to, at);
            repaint();
        }
        void mouseUp (const MouseEvent& e) override
        {
            if (dragRow < 0 || dragRow >= (int) rows.size()) return;
            const auto row = rows[(size_t) dragRow];
            if (dragging)
            {
                FxList to;
                int at = 0;
                dropAt (e.y, to, at);
                dropLine = -1;
                dragging = false;
                page->applyDrop (row.path, to, at, e.mods.isAltDown());
                return;
            }
            // Click: show it in the rack view.
            if (row.band) page->selectBand (page->unitOf (row.path), row.bandIndex);
            else if (row.path.list.splitter >= 0)
                page->selectBand (page->unitOf (FxPath { FxList { row.path.list.rack, -1, -1 }, row.path.list.splitter }), row.path.list.band);
            page->scrollTo (row.path);
        }
    };

    int unitOf (const FxPath& p) const
    {
        const auto* m = proc.patch().fx.get (p);
        return m != nullptr ? m->unit : -1;
    }

    // =========================================================================
    // The rack view.
    // =========================================================================
    struct RackContent final : public Component
    {
        FxPage* page = nullptr;
        std::vector<std::unique_ptr<FxModuleEditor>> editors;
        std::vector<int> indent;
        std::vector<Colour> bracket;
        int dropLine = -1;

        void build()
        {
            editors.clear();
            indent.clear();
            bracket.clear();
            const int rk = page->rack;
            const auto& mods = page->proc.patch().fx.racks[(size_t) rk].modules;
            for (int i = 0; i < (int) mods.size(); ++i)
            {
                const auto& m = mods[(size_t) i];
                const FxPath p { FxList { rk, -1, -1 }, i };
                editors.push_back (createModuleEditor (page->ctx, *page, p, m));
                indent.push_back (0);
                bracket.push_back ({});
                if (m.isSplitter())
                {
                    const int b = jlimit (0, fxInfo (m.type).numBands - 1, page->selectedBand (m.unit));
                    for (int j = 0; j < (int) m.bands[(size_t) b].size(); ++j)
                    {
                        const FxPath cp { FxList { rk, i, b }, j };
                        editors.push_back (createModuleEditor (page->ctx, *page, cp, m.bands[(size_t) b][(size_t) j]));
                        indent.push_back (26);
                        bracket.push_back (colours::band (b));
                    }
                }
            }
            for (auto& e : editors) addAndMakeVisible (*e);
        }
        int totalHeight() const { return jmax (page->rackView.getHeight(), (int) editors.size() * (kModuleH + 6) + 80); }
        void layout()
        {
            int y = 0;
            for (size_t i = 0; i < editors.size(); ++i)
            {
                editors[i]->setBounds (indent[i], y, getWidth() - indent[i], kModuleH);
                y += kModuleH + 6;
            }
        }
        void paint (Graphics& g) override
        {
            g.fillAll (colours::panelEdge);
            // Rails.
            g.setColour (colours::socket);
            const int end = (int) editors.size() * (kModuleH + 6);
            for (int y = end + 10; y < getHeight(); y += 44)
            {
                g.fillRect (6, y, 10, 3);
                g.fillRect (getWidth() - 16, y, 10, 3);
            }
            if (editors.empty())
                drawText (g, "Click + FX (or right-click here) to add a module", { 0.0f, 30.0f, (float) getWidth(), 20.0f }, 13.0f, colours::inkDim, false);
            for (size_t i = 0; i < editors.size(); ++i)
                if (indent[i] > 0)
                {
                    const auto r = editors[i]->getBounds();
                    g.setColour (bracket[i]);
                    g.fillRect (8, r.getY() - 6, 4, r.getHeight() + 6);
                    g.fillRect (8, r.getBottom() - 4, 14, 3);
                }
        }
        void paintOverChildren (Graphics& g) override
        {
            if (dropLine >= 0)
            {
                g.setColour (colours::ledYellow);
                g.fillRect (0, dropLine - 2, getWidth(), 3);
            }
        }
        void mouseDown (const MouseEvent& e) override
        {
            if (e.mods.isPopupMenu()) showContextMenu (*this, e.getPosition(), page->rackMenu());
        }
        void dropAt (int y, FxList& to, int& at) const
        {
            const int rk = page->rack;
            for (size_t i = 0; i < editors.size(); ++i)
            {
                const auto r = editors[i]->getBounds();
                if (y < r.getBottom() + 3)
                {
                    const auto& p = editors[i]->path();
                    to = p.list;
                    at = p.index + (y >= r.getCentreY() ? 1 : 0);
                    return;
                }
            }
            to = FxList { rk, -1, -1 };
            at = (int) page->proc.patch().fx.racks[(size_t) rk].modules.size();
        }
        int lineFor (const FxList& to, int at) const
        {
            int last = -1;
            for (size_t i = 0; i < editors.size(); ++i)
            {
                const auto& p = editors[i]->path();
                if (p.list.rack == to.rack && p.list.splitter == to.splitter && p.list.band == to.band)
                {
                    if (p.index == at) return editors[i]->getY() - 3;
                    last = (int) i;
                }
            }
            return last >= 0 ? editors[(size_t) last]->getBottom() + 3 : (int) editors.size() * (kModuleH + 6);
        }
    };

    // A floating curve editor (X-Shaper A / B).
    struct Popover final : public Component
    {
        Popover()
        {
            addAndMakeVisible (editor);
            addAndMakeVisible (close);
            close.onClick = [this] (const ModifierKeys&) { setVisible (false); };
        }
        void paint (Graphics& g) override
        {
            g.fillAll (colours::panel);
            g.setColour (colours::ink);
            g.drawRect (getLocalBounds(), 2);
            drawText (g, title, { 14.0f, 8.0f, 300.0f, 18.0f }, 14.0f, editor.colour, true, Justification::centredLeft);
            drawText (g, "Double-click: add/remove point. Drag a segment to bend it.", { 14.0f, (float) getHeight() - 26.0f, 360.0f, 16.0f },
                      11.0f, colours::inkDim, false, Justification::centredLeft);
        }
        void resized() override
        {
            close.setBounds (getWidth() - 74, 6, 64, 24);
            editor.setBounds (14, 36, getWidth() - 28, getHeight() - 70);
        }
        String title;
        CurveEditor editor;
        PanelButton close { "CLOSE" };
    };

    PanelContext& ctx;
    TinctureProcessor& proc;
    int rack = 0;
    PanelButton tabs[3];
    Meter meters[3][2];
    PanelButton addFx { "+ FX" }, saveBtn { "SAVE" };
    ChoiceDisplay presetBox;
    ListComp list;
    Viewport listView;
    RackContent content;
    Viewport rackView;
    Popover popover;
    std::map<int, int> bandSel;
    String shownSig;
    std::unique_ptr<FileChooser> chooser;
};

std::unique_ptr<juce::Component> createFxPage (PanelContext& ctx)
{
    return std::make_unique<FxPage> (ctx);
}
} // namespace tincture::ui
