// ShotsPages.cpp — the MIX, FX, MATRIX and GLOBAL pages and the preset browser.
//   TinctureSnapshot out-dir shot pages
#include "Shots.h"
#include "UI/FxModules.h"

using namespace tincture;
using namespace tincture::ui;

namespace
{
template <typename T> T* findIn (juce::Component& root, std::function<bool (T&)> pred = {})
{
    T* found = nullptr;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
    {
        if (found != nullptr) return;
        if (auto* t = dynamic_cast<T*> (&c)) if (fxui::panelVisible (c) && (! pred || pred (*t))) { found = t; return; }
        for (auto* ch : c.getChildren()) walk (*ch);
    };
    walk (root);
    return found;
}

std::vector<juce::Viewport*> viewports (juce::Component& page)
{
    std::vector<juce::Viewport*> v;
    for (auto* c : page.getChildren()) if (auto* vp = dynamic_cast<juce::Viewport*> (c)) v.push_back (vp);
    return v;
}

ParamChoice* choiceFor (Shot& s, int param)
{
    return s.find<ParamChoice> ([param] (ParamChoice& c) { return c.param() == param && fxui::panelVisible (c); });
}

bool itemsAre (Shot& s, const juce::StringArray& want, const juce::String& what)
{
    const auto got = s.menuItems();
    const bool ok = got == want;
    if (! ok) std::printf ("  %s menu: %s\n", what.toRawUTF8(), got.joinIntoString (" | ").toRawUTF8());
    s.check (ok, what + " menu items");
    return ok;
}

const FxLayout::Rack& rackOf (Shot& s, int r) { return s.proc.patch().fx.racks[(size_t) r]; }
} // namespace

static void pagesShot (Shot& s)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    s.proc.initPatch();
    s.pump (50);

    // =======================================================================
    // MIX
    // =======================================================================
    s.click (*s.button ("MIX"));
    s.check (s.panel.currentPage() == TincturePanel::Mix, "MIX shown");
    s.noteOn (60);
    s.play (10);
    s.save ("pages_mix");

    if (auto* r = choiceFor (s, ids().osc[1].route))
    {
        s.click (*r);
        itemsAre (s, { "Filter", "Main", "Direct", "None" }, "OSC B route");
        s.save ("pages_mix_route_menu");
        s.choose ("Filter");
        s.check (s.proc.params.geti (ids().osc[1].route) == 0, "OSC B routed to Filter");
    }
    else s.check (false, "OSC B route dropdown");
    if (auto* r = choiceFor (s, ids().filter[0].route))
    {
        s.click (*r);
        itemsAre (s, { "Filter 2", "Main", "Direct", "None" }, "FILTER 1 route");
        s.choose ("Direct");
        s.check (s.proc.params.geti (ids().filter[0].route) == 2, "FILTER 1 -> Direct");
    }
    else s.check (false, "FILTER 1 route dropdown");
    if (auto* r = choiceFor (s, ids().filter[1].route))
    {
        s.click (*r);
        itemsAre (s, { "Filter 1", "Main", "Direct", "None" }, "FILTER 2 route");
        s.closeMenu();
    }
    if (auto* r = choiceFor (s, ids().global.bus1Route))
    {
        s.click (*r);
        itemsAre (s, { "Main", "Direct", "Bus 2" }, "BUS 1 route");
        s.choose ("Bus 2");
        s.check (s.proc.params.geti (ids().global.bus1Route) == 2, "BUS 1 -> Bus 2");
        if (auto* r2 = choiceFor (s, ids().global.bus2Route))
        {
            s.click (*r2);
            itemsAre (s, { "Main", "Direct", "Bus 1" }, "BUS 2 route");
            s.choose ("Bus 1");
            s.check (s.proc.params.geti (ids().global.bus2Route) == 0, "BUS 2 can't route into BUS 1 while BUS 1 -> BUS 2 (disabled)");
            s.closeMenu();
        }
        s.proc.setWithUndo (ids().global.bus1Route, 0.0f);
    }
    // Header click enables SUB.
    {
        auto* page = s.panel.pageComponent (TincturePanel::Mix);
        auto* sub = page->getChildComponent (0);
        const bool before = s.proc.params.on (ids().sub.enable);
        s.click (*sub, { 50, 14 });
        s.check (s.proc.params.on (ids().sub.enable) != before, "SUB header toggles enable");
    }

    // =======================================================================
    // FX
    // =======================================================================
    s.click (*s.button ("FX"));
    s.check (s.panel.currentPage() == TincturePanel::Fx, "FX shown");
    auto* fxPage = s.panel.pageComponent (TincturePanel::Fx);
    s.save ("pages_fx_empty");
    auto addFx = [&] (const juce::String& name)
    {
        s.click (*s.button ("+ FX"));
        return s.choose (name);
    };
    s.click (*s.button ("+ FX"));
    itemsAre (s, { "Bode", "Chorus", "Compressor", "Convolve", "Delay", "Distortion", "Equalizer", "Filter", "Flanger",
                   "Hyper/Dimension", "Phaser", "Reverb", "Splitter L/H", "Splitter L/M/H", "Splitter M/S", "Utility" }, "+ FX");
    s.save ("pages_fx_add_menu");
    s.choose ("Distortion");
    s.check (rackOf (s, 0).modules.size() == 1 && rackOf (s, 0).modules[0].type == FxType::Distortion, "Distortion added");
    s.check (std::abs (s.proc.get (ids().fx[rackOf (s, 0).modules[0].unit].p[5]) - fxDefaults (FxType::Distortion)[5]) < 1e-4f, "defaults written");
    addFx ("Equalizer");
    addFx ("Compressor");
    // "Splitter L/H" has a '/' (the shot's menu path separator): add it directly.
    fxui::addModule (s.proc, tincture::FxList { 0, -1, -1 }, FxType::SplitterLH);
    s.pump (40);
    addFx ("Reverb");
    s.check (rackOf (s, 0).modules.size() == 5, "five modules");
    // Band "+" in the list adds into the splitter's first band.
    if (auto* plus = s.button ("+"))
    {
        s.click (*plus);
        s.check (s.menuOpen(), "band add menu");
        s.check (s.menuItems().contains ("Splitter L/H"), "band menu lists splitters (greyed)");
        s.choose ("Chorus");
        s.check (rackOf (s, 0).modules[3].bands[0].size() == 1, "Chorus in LOWS");
    }
    else s.check (false, "band + button");
    s.play (10);
    s.save ("pages_fx");

    // Module menu on the first editor.
    auto editors = [&]
    {
        std::vector<fxui::FxModuleEditor*> v;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            if (auto* e = dynamic_cast<fxui::FxModuleEditor*> (&c)) v.push_back (e);
            for (auto* ch : c.getChildren()) walk (*ch);
        };
        walk (*fxPage);
        return v;
    };
    if (auto ed = editors(); ! ed.empty())
    {
        s.click (*ed[0], { 14, 18 });
        itemsAre (s, { "Factory", "---", "Save FX Preset...", "Save as Default Preset", "---", "Duplicate FX Module", "Remove FX Module" }, "module");
        s.save ("pages_fx_module_menu");
        s.choose ("Duplicate FX Module");
        s.check (rackOf (s, 0).modules.size() == 6 && rackOf (s, 0).modules[1].type == FxType::Distortion, "duplicated below");
        s.check (std::abs (s.proc.get (ids().fx[rackOf (s, 0).modules[1].unit].p[5]) - s.proc.get (ids().fx[rackOf (s, 0).modules[0].unit].p[5])) < 1e-5f, "duplicate copies values");
        ed = editors();
        s.click (*ed[1], { 14, 18 });
        s.choose ("Remove FX Module");
        s.check (rackOf (s, 0).modules.size() == 5, "removed");
        // Factory > Default resets the module.
        const int u = rackOf (s, 0).modules[0].unit;
        s.proc.setWithUndo (ids().fx[u].p[5], 0.9f);
        ed = editors();
        s.click (*ed[0], { 14, 18 });
        s.choose ("Factory/Default");
        s.check (std::abs (s.proc.get (ids().fx[u].p[5]) - fxDefaults (FxType::Distortion)[5]) < 1e-4f, "Factory > Default");
        // Distortion mode menu, X-Shaper shows EDIT A.
        s.click (*ed[0], { 30, fxui::kModuleH / 2 }, true);
        s.check (s.menuOpen(), "right-click module background opens module menu");
        s.closeMenu();
    }
    if (auto* mode = s.findByTooltip ("Mode"))
    {
        s.click (*mode);
        s.check (s.menuItems().size() == 13 && s.menuItems()[11] == "X-Shaper", "distortion mode menu (13)");
        s.choose ("X-Shaper");
        s.check (fxInfo (FxType::Distortion).params[0].index (s.proc.get (ids().fx[rackOf (s, 0).modules[0].unit].p[0])) == fxchoices::XShaper, "X-Shaper set");
        s.play (2);
        if (auto* ea = s.button ("EDIT A"))
        {
            s.click (*ea);
            s.save ("pages_fx_xshaper");
            if (auto* ce = findIn<fxui::CurveEditor> (*fxPage))
            {
                const auto before = rackOf (s, 0).modules[0].shapeA.points.size();
                s.doubleClick (*ce, { ce->getWidth() / 2, ce->getHeight() / 3 });
                s.check (rackOf (s, 0).modules[0].shapeA.points.size() == before + 1, "X-Shaper curve point added");
            }
            if (auto* cl = s.button ("CLOSE")) s.click (*cl);
        }
        else s.check (false, "EDIT A visible in X-Shaper");
    }
    // Ratio right-click has Limiter Latency Comp; Filter cutoff has Key Track.
    if (auto ed = editors(); ed.size() > 2)
    {
        auto* k = findIn<ParamKnob> (*ed[2], [] (ParamKnob& kn) { return kn.caption == "RATIO"; });
        if (k != nullptr)
        {
            s.click (*k, { -1, -1 }, true);
            s.check (s.menuHas ("Limiter Latency Comp"), "RATIO menu: Limiter Latency Comp");
            s.closeMenu();
        }
        else s.check (false, "RATIO knob");
    }

    // Drag in the list: move the first module (Distortion) below the Equalizer.
    {
        auto vps = viewports (*fxPage);
        s.check (vps.size() >= 2, "list and rack viewports");
        if (vps.size() >= 2)
        {
            auto* list = vps[0]->getViewedComponent();
            s.drag (*list, { 100, 13 }, { 100, 26 + 22 });
            s.check (rackOf (s, 0).modules[0].type == FxType::Equalizer && rackOf (s, 0).modules[1].type == FxType::Distortion, "list drag reorders");
            s.save ("pages_fx_reordered");
            // Rack view: drag the 2nd editor (Distortion) by its grip above the first.
            auto ed = editors();
            s.drag (*ed[1], { 14, 80 }, { 14, 20 - fxui::kModuleH });
            s.check (rackOf (s, 0).modules[0].type == FxType::Distortion, "rack view drag reorders");
            // Rack background menu.
            auto* content = vps[1]->getViewedComponent();
            vps[1]->setViewPosition (0, 100000);
            s.pump (20);
            s.click (*content, { 300, content->getHeight() - 20 }, true);
            itemsAre (s, { "Add FX Module", "---", "Cut FX Bus", "Copy FX Bus", "Paste FX Bus", "Clear FX Bus", "---", "Lock FX Bus",
                           "Lock All FX Busses", "---", "Load FX Bus...", "Save FX Bus..." }, "rack background");
            s.save ("pages_fx_rack_menu");
            s.choose ("Copy FX Bus");
        }
    }
    // Bypass button in the list (Alt-click = all).
    if (auto* bp = s.find<fxui::BypassButton> ([] (fxui::BypassButton& b) { return fxui::panelVisible (b); }))
    {
        s.click (*bp);
        s.check (rackOf (s, 0).modules[0].bypass, "list bypass");
        s.play (1);    // the list rebuilds: find the button again
        bp = s.find<fxui::BypassButton> ([] (fxui::BypassButton& b) { return fxui::panelVisible (b); });
        s.click (*bp);
        s.check (! rackOf (s, 0).modules[0].bypass, "list bypass off");
    }
    // BUS 1 tab + Paste FX Bus.
    s.click (*s.button ("BUS 1"));
    {
        auto vps = viewports (*fxPage);
        auto* content = vps[1]->getViewedComponent();
        vps[1]->setViewPosition (0, 0);
        s.click (*content, { 300, 300 }, true);
        s.choose ("Paste FX Bus");
        s.check (rackOf (s, 1).modules.size() == 5, "pasted into BUS 1");
        s.play (2);
        vps[1]->setViewPosition (0, 100000);
        s.pump (20);
        s.click (*content, { 300, content->getHeight() - 10 }, true);
        s.choose ("Clear FX Bus");
        s.check (rackOf (s, 1).modules.empty(), "BUS 1 cleared");
    }
    // Rack presets.
    if (auto* pb = s.findByTooltip ("Rack presets"))
    {
        s.click (*pb);
        itemsAre (s, { "Factory", "User" }, "rack preset");
        s.check (s.menuHas ("Factory/Reverb/Big Hall"), "factory rack presets");
        s.save ("pages_fx_rack_presets");
        s.choose ("Factory/Reverb/Plate Split");
        s.check (rackOf (s, 1).modules.size() == 1 && rackOf (s, 1).modules[0].bands[1].size() == 1 && rackOf (s, 1).presetName == "Plate Split", "rack preset loaded into BUS 1");
        s.play (4);
        s.save ("pages_fx_bus1");
    }
    s.click (*s.button ("MAIN"));
    // Reverb type menu.
    if (auto* rt = s.find<ChoiceDisplay> ([] (ChoiceDisplay& c) { return fxui::panelVisible (c) && c.getText && c.getText() == "PLATE"; }))
    {
        s.click (*rt);
        if (s.menuItems().contains ("Vintage"))
        {
            s.choose ("Vintage");
            s.play (2);
            s.check (s.findByTooltip ("Diffusion Damping") != nullptr, "Vintage shows DIFF B");
        }
        else s.closeMenu();
    }
    s.play (6);
    s.save ("pages_fx_scrolled_top");
    {
        auto vps = viewports (*fxPage);
        vps[1]->setViewPosition (0, 10000);
        s.pump (20);
        s.save ("pages_fx_scrolled_bottom");
        vps[1]->setViewPosition (0, 0);
    }

    // MIX again: bus/main FX lists.
    s.click (*s.button ("MIX"));
    s.play (6);
    s.save ("pages_mix_fx");

    // =======================================================================
    // MATRIX
    // =======================================================================
    s.click (*s.button ("MATRIX"));
    auto* mx = s.panel.pageComponent (TincturePanel::Matrix);
    auto* table = viewports (*mx)[0]->getViewedComponent();
    auto cellX = [] (int c)
    {
        const int w[] { 34, 150, 44, 200, 40, 260, 84, 150, 40, 44, 150, 30, 30 };
        int x = 4;
        for (int i = 0; i < c; ++i) x += w[i] + 6;
        return x + w[c] / 2;
    };
    const int rowsBefore = (int) s.proc.patch().matrix.rows.size();
    const int free = rowsBefore;     // first empty row
    const int y0 = free * 30 + 15;
    s.click (*table, { cellX (1), y0 });
    s.check (s.menuHas ("LFOs/LFO 1"), "source menu");
    itemsAre (s, { "Envelopes", "LFOs", "Note", "Oscillators", "Macros", "Note Expression", "Filters", "Mod Wheel", "Aftertouch", "Poly Aftertch", "Pitch Bend" }, "source");
    s.save ("pages_matrix_source_menu");
    s.choose ("LFOs/LFO 3");
    s.check ((int) s.proc.patch().matrix.rows.size() == rowsBefore + 1 && s.proc.patch().matrix.rows.back().source == ModSource::Lfo3, "source picked creates row");
    s.click (*table, { cellX (5), y0 });
    s.check (s.menuOpen(), "destination menu");
    s.choose ("OSC A/A WT Pos");
    s.check (s.proc.patch().matrix.rows.back().dest == ids().osc[0].wtpos, "destination set");
    s.click (*table, { cellX (7), y0 });
    s.check (s.menuItems()[0] == "---", "aux menu starts with ---");
    s.choose ("Mod Wheel");
    s.check (s.proc.patch().matrix.rows.back().aux == ModSource::ModWheel, "aux set");
    // AMOUNT drag.
    s.drag (*table, { cellX (3), y0 }, { cellX (3) + 55, y0 });
    s.check (s.proc.patch().matrix.rows.back().amount > 0.3f, "amount drag");
    s.click (*table, { cellX (4), y0 });
    s.check (s.proc.patch().matrix.rows.back().bipolar, "POL toggles");
    // Matrix menu.
    auto* mbtn = s.findByTooltip ("Matrix menu");
    s.click (*mbtn);
    itemsAre (s, { "Sort by Source", "Sort by Destination", "Lock Matrix (Keep Assignments on Preset Change)",
                   "Create Vibrato (Unused LFO->Pitch via Wheel)", "Create Velo->Amp Assignment", "Apply and Delete Macros" }, "matrix");
    s.save ("pages_matrix_menu");
    s.choose ("Create Vibrato (Unused LFO->Pitch via Wheel)");
    s.check (s.proc.patch().matrix.rows.back().dest == ids().global.tuning && s.proc.patch().matrix.rows.back().aux == ModSource::ModWheel, "vibrato row");
    s.click (*mbtn);
    s.choose ("Create Velo->Amp Assignment");
    s.check (s.proc.patch().matrix.rows.back().source == ModSource::Velocity, "velo->amp row");
    // A macro row, then Apply and Delete Macros.
    s.proc.editPatch ("test", [] (Patch& p) { p.matrix.assign (ModSource::Macro1, ids().osc[0].level, 0.5f); });
    s.proc.setWithUndo (ids().macro[0], 1.0f);
    s.click (*mbtn);
    s.choose ("Apply and Delete Macros");
    s.check (s.proc.patch().matrix.rowsForSource (ModSource::Macro1).empty(), "macro rows deleted");
    s.click (*mbtn);
    s.choose ("Sort by Source");
    {
        const auto& rows = s.proc.patch().matrix.rows;
        bool sorted = true;
        for (size_t i = 1; i < rows.size(); ++i) sorted &= (int) rows[i - 1].source <= (int) rows[i].source || rows[i].source == ModSource::None;
        s.check (sorted, "sorted by source");
    }
    // Row drag: move the last row to the top.
    {
        const int n = (int) s.proc.patch().matrix.rows.size();
        const auto last = s.proc.patch().matrix.rows.back();
        s.drag (*table, { cellX (0), (n - 1) * 30 + 15 }, { cellX (0), 4 });
        s.check (s.proc.patch().matrix.rows.front() == last, "row drag reorders");
    }
    // CRV drag and double-click -> curve editor.
    s.drag (*table, { cellX (2), 15 }, { cellX (2), -25 });
    s.check (s.proc.patch().matrix.rows[0].curve > 0.6f, "CRV drag bends");
    s.click (*table, { cellX (2), 15 }, true);
    s.check (s.menuHas ("Editable Curve"), "CRV menu");
    s.closeMenu();
    s.noteOn (64);
    s.play (30);
    s.save ("pages_matrix");
    s.doubleClick (*table, { cellX (2), 15 });
    s.check (s.proc.patch().matrix.rows[0].customCurve, "double-click makes the curve editable");
    if (auto* ce = findIn<fxui::CurveEditor> (*mx))
    {
        s.doubleClick (*ce, { ce->getWidth() / 3, ce->getHeight() / 4 });
        s.check (s.proc.patch().matrix.rows[0].srcCurve.points.size() == 3, "curve point added");
    }
    s.save ("pages_matrix_curve");
    if (auto* cl = s.button ("CLOSE")) s.click (*cl);
    // Remove (x).
    {
        const int n = (int) s.proc.patch().matrix.rows.size();
        s.click (*table, { cellX (12), 15 });
        s.check ((int) s.proc.patch().matrix.rows.size() == n - 1, "x removes row");
    }

    // =======================================================================
    // GLOBAL
    // =======================================================================
    s.click (*s.button ("GLOBAL"));
    s.save ("pages_global");
    if (auto* q = s.findByTooltip ("Oversampling"))
    {
        s.click (*q);
        itemsAre (s, { "Good", "High", "Ultra" }, "quality");
        s.save ("pages_global_quality_menu");
        s.choose ("Ultra");
        s.check (s.proc.quality() == 4, "Ultra -> 4x");
        s.click (*q);
        s.choose ("High");
        s.check (s.proc.quality() == 2, "High -> 2x");
    }
    else s.check (false, "quality dropdown");
    if (auto* t = s.findByTooltip ("Tuning files"))
    {
        s.click (*t);
        itemsAre (s, { "Load Tuning...", "Clear Tuning", "---", "Enable MTS-ESP", "MTS-ESP Note-On Only" }, "TUN FILE");
        s.closeMenu();
    }
    if (auto* st = choiceFor (s, ids().global.voiceSteal))
    {
        s.click (*st);
        s.check (s.menuOpen() && s.menuItems().size() == (int) params()[ids().global.voiceSteal].choices.size(), "steal menu");
        s.closeMenu();
    }
    {
        const bool before = s.proc.settings.doubleClickResets();
        if (auto* b = s.button (before ? "TYPE VALUE" : "RESET"))
        {
            s.click (*b);
            s.check (s.proc.settings.doubleClickResets() != before, "double-click preference");
            s.click (*s.button (before ? "RESET" : "TYPE VALUE"));
        }
    }

    // =======================================================================
    // Preset browser
    // =======================================================================
    s.click (*s.button ("BROWSE"));
    s.pump (30);
    s.save ("pages_browser");
    if (auto* ed = s.find<juce::TextEditor> ([] (juce::TextEditor& t) { return fxui::panelVisible (t); }))
    {
        ed->setText ("a", true);
        s.pump (20);
        s.save ("pages_browser_search");
        ed->setText ({}, true);
    }
    juce::Component* browserComp = nullptr;
    if (auto* cog = s.button ("PRESETS"))
    {
        browserComp = cog->getParentComponent();
        s.click (*cog);
        itemsAre (s, { "Show Preset in Finder", "Load Random Preset", "Hybridize", "Hybridize Favoring Selected Preset", "Delete Preset(s)",
                       "Rename / Move Preset", "---", "Auto-Play Previews", "Preview Fallback Clip", "---", "Rescan Database", "Erase and Rebuild Database" }, "presets");
        s.save ("pages_browser_menu");
        s.choose ("Rescan Database");
    }
    {
        juce::Component* table2 = nullptr;
        if (browserComp != nullptr) for (auto* c : browserComp->getChildren()) if (auto* vp = dynamic_cast<juce::Viewport*> (c)) table2 = vp->getViewedComponent();
        if (table2 != nullptr && fxui::panelVisible (*table2))
        {
            const auto before = s.proc.presetName();
            s.click (*table2, { 60, 26 + 13 });
            s.check (s.proc.presetName() != before || s.proc.presetName().isNotEmpty(), "click loads preset");
            s.click (*table2, { 60, 13 }, true);
            itemsAre (s, { "Show in Finder", "Delete", "Rename / Move" }, "preset row");
            s.closeMenu();
            s.save ("pages_browser_loaded");
        }
        else s.check (false, "browser table");
    }
    s.click (*s.button ("CLOSE"));
    s.check (browserComp != nullptr && ! browserComp->isVisible(), "browser closed");
    s.noteOff (60);
    s.noteOff (64);
}

TINCTURE_SHOT ("pages", pagesShot);
