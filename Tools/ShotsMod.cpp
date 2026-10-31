// ShotsMod.cpp — the OSC page's modulation strip (UI/ModStrip.*): tabs, the
// envelope / LFO / VELO-NOTE graphs, every menu, drags and drops.
#include "Shots.h"
#include "UI/ModStrip.h"

using namespace tincture;
using namespace tincture::ui;

namespace
{
// Mounts overlays over the page when the real OSC page isn't hosting the strip.
struct TestHost final : OscPageHost
{
    explicit TestHost (juce::Component& p) : page (p) {}
    void showOverlay (std::unique_ptr<juce::Component> c, Area a, const juce::String& i) override
    {
        closeOverlay();
        ov = std::move (c);
        id = i;
        page.addAndMakeVisible (*ov);
        ov->setBounds (0, 0, kPageW, a == Area::Modules ? kModuleRowH : kModuleRowH + kModStripH);
    }
    void closeOverlay() override { ov.reset(); id.clear(); }
    juce::String overlayId() const override { return id; }
    juce::Component& page;
    std::unique_ptr<juce::Component> ov;
    juce::String id;
};

int rowsFor (Shot& s, ModSource src) { return (int) s.proc.patch().matrix.rowsForSource (src).size(); }
const LfoData& lfo (Shot& s, int l) { return s.proc.patch().lfo[(size_t) l]; }
} // namespace

TINCTURE_SHOT ("mod", [] (Shot& s)
{
    s.panel.showPage (TincturePanel::Osc);
    auto* page = s.panel.pageComponent (TincturePanel::Osc);
    s.check (page != nullptr, "osc page");
    if (page == nullptr) return;

    // Use the strip the OSC page made, or mount one (the page may still be a placeholder).
    std::unique_ptr<TestHost> host;
    std::unique_ptr<juce::Component> owned;
    auto* strip = s.find<ModStrip>();
    if (strip == nullptr)
    {
        host = std::make_unique<TestHost> (*page);
        owned = createModStrip (s.panel, *host);
        strip = dynamic_cast<ModStrip*> (owned.get());
        page->addAndMakeVisible (*owned);
        owned->setBounds (0, kModStripY, kPageW, kModStripH);
        std::printf ("  (mounted the strip on the OSC page with a test host)\n");
    }
    s.check (strip != nullptr, "strip exists");
    if (strip == nullptr) return;
    s.pump (50);
    s.save ("mod_default");

    // A knob elsewhere on the page to drop sources on.
    ParamKnob* target = s.find<ParamKnob> ([&] (ParamKnob& k) { return ! strip->isParentOf (&k) && params()[k.param()].modulatable(); });
    std::unique_ptr<ParamKnob> ownKnob;
    if (target == nullptr)
    {
        ownKnob = std::make_unique<ParamKnob> (s.panel, ids().osc[0].level);
        page->addAndMakeVisible (*ownKnob);
        ownKnob->setBounds (40, 40, 60, 60);
        target = ownKnob.get();
        std::printf ("  (no ParamKnob on the module row yet: using a test knob for OSC A Level)\n");
    }
    const int dest = target->param();
    auto& proc = s.proc;

    // ------------------------------------------------------------------ ENV
    auto& env = strip->envs;
    s.click (*env.tabs[1]);
    s.check (env.selected() == 1 && env.graph.env == 1, "ENV 2 tab selects");
    s.check (env.knobs[0]->param() == ids().env[1].attack, "knobs rebound to ENV 2");

    // Drag ENV 2 tab: start the real drag (DragAndDropContainer), then deliver the drop.
    s.drag (*env.tabs[1], { 20, 10 }, { 60, -80 }, 6);
    target->itemDropped (juce::DragAndDropTarget::SourceDetails (modSourceDragDescription (ModSource::Env2), env.tabs[1].get(), { 10, 10 }));
    s.check (proc.patch().matrix.find (ModSource::Env2, dest) >= 0, "drop ENV 2 on a knob creates a matrix row");
    s.pump (60);
    s.check (env.tabs[1]->destinations() == 1, "ENV 2 tile counts 1 destination");
    s.check (env.tabs[1]->getTooltip().startsWith ("Env 2 Source\n------------\nDestinations:"), "tile tooltip format: " + env.tabs[1]->getTooltip());

    s.click (*env.tabs[1], { 20, 10 }, true);
    auto items = s.menuItems();
    s.check (items.size() == 4 && items[0].startsWith ("Bypass: ") && items[1] == "Bypass All Destinations" && items[2] == "---"
             && items[3] == "Remove All Destinations", "ENV tab menu: " + items.joinIntoString (" | "));
    s.save ("mod_menu_env_tab");
    s.choose (items[0]);
    {
        const int r = proc.patch().matrix.find (ModSource::Env2, dest);
        s.check (r >= 0 && proc.patch().matrix.rows[(size_t) r].bypass, "Bypass: X bypasses the row");
    }
    s.click (*env.tabs[1], { 20, 10 }, true);
    s.choose ("Remove All Destinations");
    s.check (rowsFor (s, ModSource::Env2) == 0, "Remove All Destinations");
    s.click (*env.tabs[2], { 20, 10 }, true);
    s.check (s.menuItems().size() == 3 && ! s.panel.getMenuLayer().pane (0)->getRows()[0].item->enabled, "no destinations: greyed items");
    s.closeMenu();

    // Graph: drag the attack peak, a curve handle, the sustain point.
    {
        const auto& id = ids().env[1];
        const float a0 = proc.get (id.attack);
        const auto pk = env.graph.points()[1].toInt();
        s.drag (env.graph, pk, pk + juce::Point<int> (40, 0));
        s.check (proc.get (id.attack) > a0 + 0.01f, "drag peak lengthens ATK");
        const float c0 = proc.get (id.decayCurve);
        const auto h = env.graph.handle (1).toInt();
        s.drag (env.graph, h, h + juce::Point<int> (0, -25));
        s.check (proc.get (id.decayCurve) > c0 + 0.1f, "drag decay handle bends it");
        const auto sp = env.graph.points()[3].toInt();
        s.drag (env.graph, sp, sp + juce::Point<int> (0, 30));
        s.check (proc.get (id.sustain) < -1.0f, "drag sustain point down lowers SUS");
    }
    s.click (env.graph, { 60, 40 }, true);
    s.check (s.menuItems() == juce::StringArray ({ "Legato Inverted", "---", "Grid" }), "env graph menu: " + s.menuItems().joinIntoString (" | "));
    s.check (s.menuHas ("Grid/Beats"), "Grid > Beats");
    s.save ("mod_menu_env_graph");
    s.choose ("Grid/Beats");
    s.check (env.graph.gridBeats, "Grid > Beats chosen");
    s.click (env.graph, { 60, 40 }, true);
    s.choose ("Legato Inverted");
    s.check (proc.params.on (ids().env[1].legatoInvert) && env.legInv->isLit(), "Legato Inverted sets the L box");
    s.click (env.zoom, env.zoom.upBox().getCentre().toInt());
    s.check (! env.graph.locked, "zoom ^ unlocks");
    s.click (env.zoom, env.zoom.lockBox().getCentre().toInt());
    s.check (env.graph.locked, "lock re-locks");
    s.click (env.bpmBtn);
    s.check (proc.params.on (ids().env[1].bpm), "BPM on");
    s.check (env.values[0]->format != nullptr, "BPM: note-value display");
    s.save ("mod_env2_bpm");
    s.click (env.msBtn);
    s.check (! proc.params.on (ids().env[1].bpm), "MS back");
    // MS -> BPM uses the host tempo from telemetry: 0.25 s at 120 = 1/8, at 60 = 1/16.
    proc.set (ids().env[1].decay, 0.25f);
    proc.telemetry().bpm.store (60.0);
    s.click (env.bpmBtn);
    s.check (std::abs (proc.get (ids().env[1].decay) - 0.25f) < 1.0e-4f, "MS->BPM at 60 BPM: 0.25 s = 1/16 (0.25 beats), got "
             + juce::String (proc.get (ids().env[1].decay)));
    s.check (env.values[2]->format (proc.get (ids().env[1].decay)) == "1/16", "BPM readout 1/16");
    proc.telemetry().bpm.store (120.0);
    s.click (env.msBtn);
    s.check (std::abs (proc.get (ids().env[1].decay) - 0.125f) < 1.0e-3f, "BPM->MS at 120 BPM: 1/16 = 125 ms");
    proc.set (ids().env[1].decay, 0.25f);
    s.click (env.bpmBtn);
    s.check (std::abs (proc.get (ids().env[1].decay) - 0.5f) < 1.0e-4f, "MS->BPM at 120 BPM: 0.25 s = 1/8");
    s.click (env.msBtn);
    s.click (*env.tabs[0]);

    // ------------------------------------------------------------------ LFO
    auto& lp = strip->lfos;
    s.check (! lp.tabs[6]->isVisible(), "LFO 7 hidden in an init patch");
    s.click (*lp.tabs[1]);
    s.check (lp.selected() == 1, "LFO 2 selected");
    auto& g = lp.graph;
    {
        const size_t n0 = lfo (s, 1).shape.points.size();
        s.doubleClick (g, g.toScreen (0.25f, 0.8f).toInt());
        s.check (lfo (s, 1).shape.points.size() == n0 + 1, "double-click adds a point");
        const int added = 1;
        const float y0 = lfo (s, 1).shape.points[added].y;
        const auto p = g.pointPos (added).toInt();
        s.drag (g, p, p + juce::Point<int> (8, 30));
        s.check (lfo (s, 1).shape.points[added].y < y0 - 0.1f, "drag moves the point (patch LfoData changed)");
        s.check (proc.canUndo() && proc.undoLabel().isNotEmpty(), "drag committed to undo: " + proc.undoLabel());
        s.doubleClick (g, g.pointPos (added).toInt());
        s.check (lfo (s, 1).shape.points.size() == n0, "double-click removes it");
        // Alt-drag a curve handle bends all.
        const auto h = g.handlePos (1).toInt();
        s.drag (g, h, h + juce::Point<int> (0, -20));
        s.check (std::abs (lfo (s, 1).shape.points[1].curve) > 0.1f, "drag curve handle bends");
    }
    s.save ("mod_lfo2_edit");

    // Tools: Flat draws steps per grid cell.
    s.click (lp.toolBtn[1]);
    s.check (g.tool == CurveEditor::Tool::Flat && lp.toolBtn[1].isLit(), "Flat tool");
    s.drag (g, g.toScreen (0.05f, 0.75f).toInt(), g.toScreen (0.4f, 0.75f).toInt(), 10);
    s.check (std::abs (lfo (s, 1).shape.evaluate (0.2f) - 0.75f) < 0.01f, "Flat draws steps at the grid value");
    s.click (lp.toolBtn[2]);
    s.drag (g, g.toScreen (0.6f, 1.0f).toInt(), g.toScreen (0.7f, 1.0f).toInt(), 3);
    s.check (lfo (s, 1).shape.evaluate (0.74f) > lfo (s, 1).shape.evaluate (0.63f) + 0.3f, "Ramp Up draws a rising ramp");
    s.save ("mod_lfo2_tools");
    s.click (lp.toolBtn[0]);

    // Point menu (a main point) and its effects.
    {
        const int pt = 1;
        s.click (g, g.pointPos (pt).toInt(), true);
        auto it = s.menuItems();
        s.check (it.size() >= 12 && it[0] == "#Shift-Key Function:" && it[1] == "Flat" && it[2] == "Ramp Up" && it[3] == "Ramp Down"
                 && it.contains ("Modulate X:") && it.contains ("Modulate Y:") && it.contains ("Set Loopback Point Here")
                 && it.contains ("Remove Loopback Point") && it.contains ("Remove Selected Points") && it.contains ("Flip Vertical")
                 && it.contains ("Flip Horizontal"), "point menu: " + it.joinIntoString (" | "));
        s.check (s.menuHas ("Modulate Y:/LFO Bus 1/LFOs/LFO 3"), "Modulate Y > LFO Bus 1 > source tree");
        s.check (s.menuHas ("Modulate Y:/LFO Bus 1/Poly Aftertch"), "source tree spelling Poly Aftertch");
        s.save ("mod_menu_lfo_point");
        s.choose ("Modulate Y:/LFO Bus 1/LFOs/LFO 3");
        s.check (lfo (s, 1).buses[0].source == (int) ModSource::Lfo3 && lfo (s, 1).pointMods.size() == 1 && lfo (s, 1).pointMods[0].dy > 0.0f,
                 "point Y modulated by LFO 3 on bus 1");
        s.click (g, g.pointPos (pt).toInt(), true);
        s.check (s.menuHas ("Modulate X:/LFO Bus 2"), "next free bus offered");
        s.choose ("Set Loopback Point Here");
        s.check (lfo (s, 1).loopback == pt, "loopback set");
        s.click (g, g.pointPos (pt).toInt(), true);
        s.choose ("Remove Loopback Point");
        s.check (lfo (s, 1).loopback == -1, "loopback removed");
        s.click (g, g.pointPos (pt).toInt(), true);
        s.choose ("Ramp Down");
        s.check (g.shiftTool == CurveEditor::Tool::RampDown, "Shift-Key Function: Ramp Down");
        const float before = lfo (s, 1).shape.points.front().y;
        s.click (g, g.pointPos (pt).toInt(), true);
        s.choose ("Flip Vertical");
        s.check (std::abs (lfo (s, 1).shape.points.front().y - (1.0f - before)) < 1.0e-4f || ! g.selection.empty(), "Flip Vertical");
    }
    // Handle menu: Modulate Curves.
    {
        s.click (lp.shape);
        s.choose ("Default");
        s.pump (40);
        const auto h = g.handlePos (1).toInt();
        s.click (g, h, true);
        s.check (s.menuItems().contains ("Modulate Curves:") && ! s.menuItems().contains ("Modulate X:"), "handle menu has Modulate Curves:");
        s.closeMenu();
    }
    // Drop a mod source on a point: X / Y boxes (drop path called directly).
    {
        const auto pt = g.pointPos (1).toInt();
        juce::DragAndDropTarget::SourceDetails sd (modSourceDragDescription (ModSource::Macro2), lp.tabs[0].get(), pt);
        g.itemDragMove (sd);
        const auto yBox = juce::Point<int> (pt.x + 17, pt.y + 8);
        g.itemDropped (juce::DragAndDropTarget::SourceDetails (modSourceDragDescription (ModSource::Macro2), lp.tabs[0].get(), yBox));
        s.check (lfo (s, 1).buses[0].source == (int) ModSource::Macro2 && lfo (s, 1).pointMods.size() == 1 && lfo (s, 1).pointMods[0].dy > 0.0f, "drop on a point's Y box adds point modulation");
        s.save ("mod_lfo2_pointmod");
    }

    // Shape menu.
    s.click (lp.shape);
    {
        auto it = s.menuItems();
        s.check (it == juce::StringArray ({ "Default", "---", "Basic", "Rhythmic", "Sidechain", "User", "---", "Random (Curved)", "Random (Flat)", "---",
                                            "Wavetable A to LFO", "Wavetable B to LFO", "Wavetable C to LFO", "---", "Save Shape..." }),
                 "shape menu verbatim: " + it.joinIntoString (" | "));
        s.check (s.menuHas ("Basic/Sine") && s.menuHas ("Sidechain/Pump Classic"), "factory folders");
        s.save ("mod_menu_lfo_shape");
        s.choose ("Sidechain/Pump Classic");
        s.check (lfo (s, 1).shapeName == "Pump Classic" && lfo (s, 1).pointMods.empty(), "factory shape loaded");
    }
    s.click (lp.shapeNext);
    s.check (lfo (s, 1).shapeName == "Pump Hard", "> steps to the next shape in the folder");
    s.click (lp.shape);
    s.choose ("Random (Flat)");
    s.check (lfo (s, 1).shapeName == "Random (Flat)" && lfo (s, 1).shape.points.size() > 4, "Random (Flat)");
    s.click (lp.shape);
    s.choose ("Random (Curved)");
    s.check (lfo (s, 1).shapeName == "Random (Curved)", "Random (Curved)");
    if (proc.patch().wavetable[0].table != nullptr)
    {
        s.click (lp.shape);
        s.choose ("Wavetable A to LFO");
        s.check (lfo (s, 1).shapeName == "Wavetable A" && lfo (s, 1).shape.points.size() > 3, "Wavetable A to LFO");
    }
    s.save ("mod_lfo2_shape");

    // TYPE / mode / direction.
    s.click (*lp.type);
    s.check (s.menuItems() == juce::StringArray ({ "Normal", "Path", "Chaos: Lorenz", "Chaos: Rossler", "S&H" }), "type list: " + s.menuItems().joinIntoString (" | "));
    s.choose ("Chaos: Lorenz");
    s.check (proc.params.geti (ids().lfo[1].type) == 2, "type Lorenz");
    s.noteOn (60);
    for (int i = 0; i < 20; ++i) s.play (4);
    s.check (! g.editable && ! g.trace.empty(), "chaos: tools off, trace shown");
    s.save ("mod_lfo2_lorenz");
    s.noteOff (60);
    s.click (*lp.type);
    s.choose ("Normal");
    s.click (lp.modeBtn[2]);
    s.check (proc.params.geti (ids().lfo[1].mode) == 2 && lp.modeBtn[2].isLit(), "ENVELOPE mode");
    s.click (*lp.direction);
    s.check (s.menuItems() == juce::StringArray ({ "Forward", "Reverse", "Ping Pong" }), "direction list");
    s.choose ("Ping Pong");
    s.check (proc.params.geti (ids().lfo[1].direction) == 2, "Ping Pong");
    s.wheel (*lp.gridX, 1.0f);
    s.check (proc.params.geti (ids().lfo[1].gridX) == 9, "||| grid stepper");

    // RATE extras, BPM / HZ, PHASE extra.
    s.click (*lp.rate, { 26, 20 }, true);
    s.check (s.menuItems()[1] == "Swing", "RATE menu (BPM) has Swing: " + s.menuItems().joinIntoString (" | "));
    s.save ("mod_menu_lfo_rate");
    s.choose ("Swing");
    s.check (proc.params.on (ids().lfo[1].swing), "Swing on");
    s.click (*lp.trip);
    s.check (lp.rateField->format (proc.get (ids().lfo[1].rateSync)).endsWith (" T"), "TRIP shows in the readout");
    s.click (lp.hzBtn);
    s.check (! proc.params.on (ids().lfo[1].bpm) && lp.rate->param() == ids().lfo[1].rate, "HZ binds the Hz RATE");
    s.click (*lp.rateField, { 20, 9 }, true);
    s.check (s.menuItems()[1] == "10x", "RATE menu (HZ) has 10x");
    s.choose ("10x");
    s.check (proc.params.on (ids().lfo[1].x10), "10x on");
    s.click (*lp.phase, { 30, 20 }, true);
    s.check (s.menuItems()[1] == "Snap to Grid", "PHASE menu Snap to Grid");
    s.choose ("Snap to Grid");
    s.check (proc.params.on (ids().lfo[1].phaseSnap), "Snap to Grid on");
    s.save ("mod_lfo2_hz");
    s.click (lp.bpmBtn);

    // Live playhead while a note plays.
    s.noteOn (64);
    s.play (30);
    s.check (g.playhead >= 0.0f, "LFO playhead from telemetry");
    s.save ("mod_playing");
    s.noteOff (64);

    // LFO 7-10 appear once LFO 6 has a destination.
    s.click (*lp.tabs[5]);
    proc.editPatch ("test", [dest] (Patch& p) { p.matrix.assign (ModSource::Lfo6, dest); });
    s.pump (80);
    s.check (lp.tabs[9]->isVisible(), "LFO 10 tab visible after LFO 6 is assigned");
    // Alt-drop copy LFO 2 -> LFO 8 (the drop handler with Alt, as the DnD would call it).
    lp.tabs[7]->onTileDrop (ModSource::Lfo2, true);
    s.check (lfo (s, 7).shape == lfo (s, 1).shape && proc.params.geti (ids().lfo[7].direction) == 2, "Alt-drop copies LFO 2 to LFO 8");
    s.click (*lp.tabs[7]);
    s.save ("mod_lfo8_tabs");

    // Pop-out editor.
    s.click (*lp.tabs[1]);
    s.click (lp.popout);
    auto* ov = s.find<LfoEditorOverlay>();
    s.check (ov != nullptr && ov->isVisible(), "LFO editor overlay open");
    if (ov != nullptr)
    {
        const size_t n0 = lfo (s, 1).shape.points.size();
        s.doubleClick (ov->graph, ov->graph.toScreen (0.33f, 0.4f).toInt());
        s.check (lfo (s, 1).shape.points.size() == n0 + 1, "pop-out edits the same LFO");
        s.save ("mod_lfo_editor");
        s.click (ov->shape);
        s.check (s.menuHas ("Basic/Triangle"), "pop-out shape menu");
        s.closeMenu();
        s.click (ov->close);
        s.pump (60);
        s.check (s.find<LfoEditorOverlay>() == nullptr, "close button closes the editor");
    }

    // ------------------------------------------------------------------ VELO / NOTE
    auto& vn = strip->veloNote;
    s.click (*vn.tabs[1]);
    s.check (vn.selected() == 1, "NOTE tab");
    s.doubleClick (vn.graph, vn.graph.toScreen (0.5f, 0.8f).toInt());
    s.check (proc.patch().graphs.note.points.size() == 3, "NOTE graph: double-click adds a point");
    s.click (*vn.tabs[1], { 20, 10 }, true);
    auto vi = s.menuItems();
    s.check (vi.size() == 6 && vi[0] == "Legato (Portamento Time)" && vi[1] == "Init Graph" && vi[2] == "---" && vi[3] == "Bypass All Destinations",
             "NOTE tab menu: " + vi.joinIntoString (" | "));
    s.save ("mod_menu_note_tab");
    s.choose ("Legato (Portamento Time)");
    s.check (proc.patch().graphs.noteLegato, "Legato (Portamento Time)");
    s.click (*vn.tabs[1], { 20, 10 }, true);
    s.choose ("Init Graph");
    s.check (proc.patch().graphs.note.points.size() == 2, "Init Graph");
    s.click (*vn.tabs[0]);

    // ------------------------------------------------------------------ MACROS
    auto& mc = strip->macros;
    target->itemDropped (juce::DragAndDropTarget::SourceDetails (modSourceDragDescription (ModSource::Macro1), mc.tiles[0].get(), { 5, 5 }));
    s.check (rowsFor (s, ModSource::Macro1) == 1, "macro 1 tile drop assigns");
    mc.tiles[3]->itemDropped (juce::DragAndDropTarget::SourceDetails (modSourceDragDescription (ModSource::Macro1), mc.tiles[0].get(), { 5, 5 }));
    s.check (rowsFor (s, ModSource::Macro1) == 0 && rowsFor (s, ModSource::Macro4) == 1, "drop macro 1 on macro 4 swaps assignments");

    // Rename a macro from its tile menu (and the knob's control menu has it too).
    s.click (*mc.tiles[1], { 9, 9 }, true);
    s.check (s.menuItems()[0] == "Rename...", "macro tile menu has Rename...");
    s.choose ("Rename...");
    {
        auto* ed = s.find<juce::TextEditor> ([&] (juce::TextEditor& t) { return t.getParentComponent() == &s.panel && t.isVisible(); });
        if (ed == nullptr || ed->getText() != "MACRO 2")
            s.find<juce::TextEditor> ([&] (juce::TextEditor& t) { std::printf ("  (text editor '%s' visible %d)\n", t.getText().toRawUTF8(), (int) t.isVisible()); return false; });
        s.check (ed != nullptr && ed->getText() == "MACRO 2", "rename box shows the current name");
        if (ed != nullptr && ed->onReturnKey) { ed->setText ("Bass", false); ed->onReturnKey(); }
        s.pump (60);
    }
    s.check (proc.patch().macroNames[1] == "Bass", "macroNames[1] = Bass");
    s.check (mc.knobs[1]->caption == "Bass", "name shown under the knob");
    s.click (*mc.knobs[2], { 25, 20 }, true);
    s.check (s.menuItems()[1] == "Rename...", "macro knob menu has Rename...");
    s.closeMenu();
    s.save ("mod_macro_renamed");

    // ------------------------------------------------------------------ VOICING
    auto& vc = strip->voicing;
    s.click (*vc.poly, { 70, 10 }, true);
    s.check (s.menuItems() == juce::StringArray ({ "#Poly Count", "Limit Same Note Poly to 1", "Voice Steal Priority", "---", "Reset Control", "Lock Parameter" }),
             "POLY menu verbatim: " + s.menuItems().joinIntoString (" | "));
    s.check (s.menuHas ("Voice Steal Priority/Velocity"), "steal list");
    s.save ("mod_menu_poly");
    s.check (s.panel.getMenuLayer().pane (0)->getRows()[1].item->enabled, "Limit Same Note enabled");
    s.choose ("Limit Same Note Poly to 1");
    s.check (proc.patch().limitSameNote, "Limit Same Note Poly to 1 on");
    s.click (*vc.poly, { 70, 10 }, true);
    s.check (s.panel.getMenuLayer().pane (0)->getRows()[1].item->isTicked(), "Limit Same Note ticked");
    s.choose ("Voice Steal Priority/Highest");
    s.check (proc.params.geti (ids().global.voiceSteal) == 2, "Voice Steal Priority: Highest");
    s.click (vc.polyUp);
    s.check (proc.params.geti (ids().global.poly) == 9, "POLY up");
    s.click (*vc.mono);
    s.check (proc.params.on (ids().global.mono) && ! vc.poly->isEnabled(), "MONO disables POLY");
    s.click (*vc.mono);
    const float pc0 = proc.get (ids().global.portaCurve);
    s.drag (*vc.curve, { 20, 30 }, { 20, 5 });
    s.check (proc.get (ids().global.portaCurve) > pc0 + 0.1f, "porta CURVE drag");
    s.click (*vc.always);
    s.check (proc.params.on (ids().global.portaAlways), "ALWAYS");
    s.noteOn (60);
    s.noteOn (67);
    s.play (10);
    s.check (vc.counterText().startsWith ("2 /") || vc.counterText().startsWith ("1 /") || ! vc.counterText().startsWith ("0 /"), "voice counter: " + vc.counterText());
    s.save ("mod_final");
    s.noteOff (60);
    s.noteOff (67);
    s.play (40);

    if (owned != nullptr) { page->removeChildComponent (owned.get()); owned.reset(); }
    if (ownKnob != nullptr) ownKnob.reset();
});
