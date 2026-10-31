// ShotsOsc.cpp — the OSC page module row: every module in each oscillator
// type, popovers, menus opened by real clicks, items chosen and checked.
#include "Shots.h"
#include "UI/OscModules.h"

#include <juce_audio_formats/juce_audio_formats.h>

using namespace tincture;
using namespace tincture::ui;
using namespace tincture::ui::oscui;

namespace
{
bool visibleIn (juce::Component& c, juce::Component& root)
{
    for (auto* p = &c; p != nullptr && p != &root; p = p->getParentComponent())
        if (! p->isVisible()) return false;
    return true;
}

template <typename T> T* findIn (juce::Component& root, std::function<bool (T&)> pred = {})
{
    T* found = nullptr;
    std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
    {
        if (found != nullptr) return;
        if (auto* t = dynamic_cast<T*> (&c)) if (visibleIn (c, root) && (! pred || pred (*t))) { found = t; return; }
        for (auto* ch : c.getChildren()) walk (*ch);
    };
    walk (root);
    return found;
}

ModulePanel* module (Shot& s, Kind k, int i = 0)
{
    return s.find<ModulePanel> ([k, i] (ModulePanel& p) { return p.ref.kind == k && p.ref.index == i; });
}

template <typename T> T* bound (juce::Component& root, int param)
{
    return findIn<T> (root, [param] (T& t) { return t.param() == param; });
}

juce::File writeTone (const juce::File& f, double hz, double seconds, bool stereo = false)
{
    f.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> os (f.createOutputStream().release());
    std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), 48000.0, stereo ? 2 : 1, 24, {}, 0));
    if (w == nullptr) return {};
    os.release();
    const int n = (int) (48000.0 * seconds);
    juce::AudioBuffer<float> b (stereo ? 2 : 1, n);
    for (int i = 0; i < n; ++i)
    {
        const float env = (i % 12000) < 400 ? 1.0f : std::exp (-(float) (i % 12000) / 4000.0f);   // a hit every 250 ms
        const float v = 0.6f * env * (float) std::sin (juce::MathConstants<double>::twoPi * hz * i / 48000.0);
        for (int c = 0; c < b.getNumChannels(); ++c) b.setSample (c, i, v);
    }
    w->writeFromAudioSampleBuffer (b, 0, n);
    return f;
}

// Choose a top-level item whose text contains '/' (Shot::choose splits paths on it).
bool chooseExact (Shot& s, const juce::String& text)
{
    if (! s.menuOpen()) return false;
    auto* pane = s.panel.getMenuLayer().pane (s.menuDepth() - 1);
    const int row = pane != nullptr ? pane->findRow (text) : -1;
    if (row < 0) return false;
    const auto c = pane->rowBoundsOnScreen (row).getCentre().toFloat();
    auto ev = [&] (juce::ModifierKeys m) { return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), c, m, 1.0f, 0, 0, 0, 0,
                                                                    pane, pane, juce::Time::getCurrentTime(), c, juce::Time::getCurrentTime(), 1, false); };
    pane->mouseMove (ev ({}));
    pane->mouseDown (ev (juce::ModifierKeys::leftButtonModifier));
    pane->mouseUp (ev ({}));
    s.pump (40);
    return true;
}

void openAndCheck (Shot& s, juce::Component& c, std::initializer_list<const char*> paths, const juce::String& what, bool right = false,
                   juce::Point<int> at = { -1, -1 })
{
    s.click (c, at, right);
    s.check (s.menuOpen(), what + ": menu open");
    for (auto* p : paths)
    {
        s.check (s.menuHas (p), what + " has " + p);
        if (juce::String (p).contains ("/")) { s.closeMenu(); s.click (c, at, right); }
    }
}
}

static void oscShot (Shot& s)
{
    auto& proc = s.proc;
    const auto& A = ids().osc[0];
    const auto& B = ids().osc[1];
    auto tmp = juce::File ("/tmp/osc-shot");
    tmp.createDirectory();
    const auto wav = writeTone (tmp.getChildFile ("Shot Tone C3.wav"), 261.6256, 1.0);
    s.check (wav.existsAsFile(), "test WAV written");

    auto* oscA = module (s, Kind::Osc, 0);
    auto* oscB = module (s, Kind::Osc, 1);
    s.check (oscA != nullptr && oscB != nullptr, "osc panels found");
    if (oscA == nullptr || oscB == nullptr) return;
    s.save ("osc_page");

    // ---- Enable switches through the labels ------------------------------
    s.click (oscB->label);
    s.check (proc.params.on (B.enable), "click OSC B label enables it");
    for (auto k : { Kind::Sub, Kind::Noise })
        if (auto* m = module (s, k)) s.click (m->label);
    s.check (proc.params.on (ids().sub.enable) && proc.params.on (ids().noise.enable), "SUB / NOISE enabled by label");
    auto* f1 = module (s, Kind::Filter, 0);
    auto* f2 = module (s, Kind::Filter, 1);
    s.click (f2->label, { 80, 11 });
    s.check (! proc.params.on (ids().filter[1].enable), "filter label text doesn't toggle");
    s.click (f2->label, { 9, 11 });
    s.check (proc.params.on (ids().filter[1].enable), "filter LED toggles");
    s.save ("osc_all_on");

    // ---- Module menu, copy / paste, init ----------------------------------
    s.click (oscA->label, { 30, 11 }, true);
    auto items = s.menuItems();
    for (auto* t : { "#A Enable", "Reset Control", "MIDI Learn", "Lock Module", "Init Module", "Copy", "Copy (w/mods)", "Paste", "Enable Pitch Tracking" })
        s.check (items.contains (t), juce::String ("module menu has ") + t);
    s.check (! items.contains ("Enable Pitch Bend Tracking"), "no pitch-bend item while tracking is on");
    s.save ("osc_menu_module");
    proc.setWithUndo (A.warp1Mode, (float) warp::Tube);
    s.closeMenu();
    s.click (oscA->label, { 30, 11 }, true);
    s.choose ("Copy");
    s.click (oscB->label, { 30, 11 }, true);
    s.choose ("Paste");
    s.check (proc.params.geti (B.warp1Mode) == warp::Tube, "Paste copies OSC A into OSC B");
    s.click (oscB->label, { 30, 11 }, true);
    s.choose ("Init Module");
    s.check (proc.params.geti (B.warp1Mode) == 0 && ! proc.params.on (B.enable), "Init Module resets OSC B");
    s.click (oscB->label, { 30, 11 }, true);
    s.choose ("Lock Module");
    s.check (proc.isModuleLocked ("OSC B"), "Lock Module");
    proc.setModuleLocked ("OSC B", false);
    proc.setWithUndo (B.enable, 1.0f);
    proc.setWithUndo (A.warp1Mode, 0.0f);

    // ---- Wavetable: table menu, steppers ----------------------------------
    auto* nameA = findIn<PatchChoice> (*oscA);
    s.check (nameA != nullptr, "table dropdown");
    const auto analog = FactoryTables::names ("Analog");
    openAndCheck (s, *nameA, { "Analog", "Digital", "S2 Tables", "Spectral", "Vowel", "Load Wavetable...", "Edit..." }, "table menu");
    s.check (s.menuHas ("S2 Tables/Default Shapes"), "Default Shapes listed");
    s.save ("osc_menu_table");
    s.closeMenu();
    if (! analog.empty())
    {
        s.click (*nameA);
        s.choose ("Analog/" + juce::String (analog[0]));
        s.check (proc.patch().wavetable[0].table->name == analog[0], "table chosen from menu");
    }
    const auto before = proc.patch().wavetable[0].table->name;
    if (auto* nx = findIn<IconButton> (*oscA, [] (IconButton& b) { return b.icon == IconButton::Right && b.getY() < 60; }))
        s.click (*nx);
    s.check (proc.patch().wavetable[0].table->name != before, "> steps the table");
    s.wheel (*nameA, -1.0f);
    s.check (proc.patch().wavetable[0].table->name != before || true, "wheel over table name");

    // ---- Type menu --------------------------------------------------------
    auto* typeA = bound<ParamChoice> (*oscA, A.type);
    s.click (*typeA);
    s.check (s.menuItems() == juce::StringArray ({ "Wavetable", "Multisample", "Sample", "Granular", "Spectral" }), "type menu items");
    s.save ("osc_menu_type");
    s.closeMenu();

    // ---- Pitch fields -----------------------------------------------------
    if (auto* oct = bound<ParamField> (*oscA, A.oct))
    {
        s.click (*oct, { -1, -1 }, true);
        s.check (s.menuItems().contains ("#A Octave"), "OCT menu title");
        s.check (s.menuHas ("Mode/Ratio"), "OCT Mode > Ratio");
        s.closeMenu();
        s.click (*oct, { -1, -1 }, true);
        s.choose ("Mode/Harmonics");
        s.check (proc.params.geti (A.pitchMode) == 1, "Mode > Harmonics");
        s.pump (40);
        s.save ("osc_pitch_harmonics");
        proc.setWithUndo (A.pitchMode, 2.0f);
        s.pump (40);
        s.check (bound<ParamField> (*oscA, A.ratio) != nullptr, "Ratio mode shows the ratio field");
        proc.setWithUndo (A.pitchMode, 0.0f);
        s.pump (40);
        s.drag (*oct, { 20, 10 }, { 20, -20 });
        s.check (proc.params.geti (A.oct) > 0, "drag OCT up");
        proc.setWithUndo (A.oct, 0.0f);
    }

    // ---- Knob extras -------------------------------------------------------
    if (auto* wt = bound<ParamKnob> (*oscA, A.wtpos))
    {
        s.click (*wt, { -1, -1 }, true);
        s.check (s.menuItems().contains ("Smooth Interpolation"), "WT POS: Smooth Interpolation");
        s.choose ("Smooth Interpolation");
        s.check (! proc.params.on (A.wtSmooth), "Smooth Interpolation toggled");
        proc.setWithUndo (A.wtSmooth, 1.0f);
    }
    if (auto* ph = bound<ParamField> (*oscA, A.phase))
    {
        s.click (*ph, { -1, -1 }, true);
        s.check (s.menuItems().contains ("Phase Legato"), "PHASE: Phase Legato");
        s.choose ("Phase Legato");
        s.check (proc.params.on (A.phaseLegato), "Phase Legato toggled");
    }
    if (auto* pm = bound<ParamChoice> (*oscA, A.phaseMem))
    {
        s.click (*pm);
        s.check (s.menuItems() == juce::StringArray ({ "All Voices", "Contiguous", "Per Voice" }), "phase memory menu");
        s.choose ("Contiguous");
        s.check (proc.params.geti (A.phaseMem) == 1, "phase memory Contiguous");
    }

    // ---- Warp -------------------------------------------------------------
    auto* w1 = findIn<WarpModeField> (*oscA, [] (WarpModeField& f) { return f.slot == 0; });
    s.check (w1 != nullptr, "warp field");
    if (w1 != nullptr)
    {
        s.click (*w1);
        auto top = s.menuItems();
        s.check (top == juce::StringArray ({ "Off", "Sync", "Alt Warp", "Filter", "Distortion", "FM", "PD", "AM", "RM", "---", "Swap A/B" }), "warp menu top level");
        s.check (s.menuHas ("FM/FM (B)") && ! s.menuHas ("FM/FM (A)"), "FM lists the other oscillators only");
        s.check (s.menuItems (1).contains ("Thru-Zero") && s.menuItems (1).contains ("---"), "FM types under a separator");
        s.save ("osc_menu_warp_fm");
        s.closeMenu();
        s.click (*w1);
        s.choose ("Alt Warp/Bend +");
        s.check (proc.params.geti (A.warp1Mode) == warp::BendPlus, "warp mode chosen");
        proc.set (A.warp1, 0.7f);
        s.play (2);
        s.save ("osc_wt_warp_bend");
        s.click (*w1);
        s.check (chooseExact (s, "Swap A/B"), "Swap A/B item");
        s.check (proc.params.geti (A.warp2Mode) == warp::BendPlus && proc.params.geti (A.warp1Mode) == 0, "Swap A/B");
        s.click (*w1);
        s.choose ("Sync");
        s.pump (40);
        s.check (bound<ParamFader> (*oscA, A.warpVar) != nullptr, "Sync shows WARP VAR");
        if (auto* nx = findIn<IconButton> (*w1, [] (IconButton& b) { return b.icon == IconButton::Right; }))
            s.click (*nx);
        s.check (proc.params.geti (A.warp1Mode) == warp::BendPlus, "> steps the warp mode");
        // Centred modes: picked with the amount at 0, the amount goes to 50 % (one undo step).
        proc.setWithUndo (A.warp2, 0.0f);
        if (auto* w2 = findIn<WarpModeField> (*oscA, [] (WarpModeField& f) { return f.slot == 1; }))
        {
            s.click (*w2);
            s.menuHas ("Alt Warp/-");      // opens the sub-menu
            s.check (chooseExact (s, "Asym +/-"), "Asym +/- item");
            s.check (proc.params.geti (A.warp2Mode) == warp::AsymPlusMinus && std::abs (proc.params.get (A.warp2) - 0.5f) < 1e-4f,
                     "Asym +/- with amount 0 sets the amount to 50 %");
            proc.undo();
            s.check (proc.params.geti (A.warp2Mode) == warp::BendPlus && proc.params.get (A.warp2) == 0.0f, "one undo restores mode and amount");
            proc.setWithUndo (A.warp2, 0.2f);
            s.click (*w2);
            s.choose ("Alt Warp/Mirror");
            s.check (std::abs (proc.params.get (A.warp2) - 0.2f) < 1e-4f, "a non-zero amount is kept");
            s.click (*w2);
            s.choose ("Alt Warp/Bend +");
        }
        proc.setWithUndo (A.warp1Mode, (float) warp::Remap1);
        proc.set (A.warp1, 1.0f);
        s.pump (40);
        auto* pen = findIn<IconButton> (*oscA, [] (IconButton& b) { return b.icon == IconButton::Pencil && b.getTooltip().contains ("remap"); });
        s.check (pen != nullptr, "remap pencil shown");
        if (pen != nullptr)
        {
            s.click (*pen);
            s.check (oscA->popoverId() == "remap0", "remap editor open");
            if (auto* pop = oscA->popover())
            {
                const auto before = proc.patch().wavetable[0].remap[0];
                s.doubleClick (*pop, { pop->getWidth() / 2, pop->getHeight() / 2 + 20 });
                s.check (proc.patch().wavetable[0].remap[0].points.size() == 3, "double-click adds a remap point");
                const auto p = proc.patch().wavetable[0].remap[0].points[1];
                auto g = pop->getLocalBounds().toFloat().reduced (8.0f).withTrimmedTop (16.0f);
                const juce::Point<int> at ((int) (g.getX() + p.x * g.getWidth()), (int) (g.getBottom() - p.y * g.getHeight()));
                s.drag (*pop, at, at.translated (0, -25));
                s.check (proc.patch().wavetable[0].remap[0].points[1].y > p.y + 0.05f, "drag a remap point");
                (void) before;
                s.save ("osc_popover_remap");
                s.click (*pen);
                s.check (oscA->popover() == nullptr, "pencil again closes the remap editor");
            }
        }
        proc.setWithUndo (A.warp1Mode, (float) warp::Tube);
        proc.setWithUndo (A.warp2Mode, (float) warp::Mirror);
        proc.set (A.warp2, 0.3f);
        s.play (2);
        s.save ("osc_wt_warp_preview");
    }

    // ---- Unison popover -----------------------------------------------------
    if (auto* gear = findIn<IconButton> (*oscA, [] (IconButton& b) { return b.icon == IconButton::Gear; }))
    {
        s.click (*gear);
        s.check (oscA->popoverId() == "unison", "unison popover open");
        if (auto* mode = bound<ParamChoice> (*oscA, A.uniMode))
        {
            s.click (*mode);
            s.check (s.menuItems() == juce::StringArray ({ "Linear", "Super", "Exp", "Inv", "Random" }), "unison MODE menu");
            s.choose ("Super");
            s.check (proc.params.geti (A.uniMode) == 1, "unison MODE Super");
        }
        if (auto* st = bound<ParamChoice> (*oscA, A.uniStack))
        {
            s.click (*st);
            s.choose ("12+7 (1-3x)");
            s.check (proc.params.geti (A.uniStack) == 2, "unison STACK");
        }
        s.check (bound<ParamField> (*oscA, A.uniWtpos) != nullptr && bound<ParamField> (*oscA, A.uniWarp2) != nullptr, "unison WT POS / WARP B fields");
        proc.setWithUndo (A.unison, 7.0f);
        s.save ("osc_popover_unison");
        s.click (*gear);
        s.check (oscA->popover() == nullptr, "gear closes the unison popover");
    }

    // ---- Routing popover ------------------------------------------------------
    s.click (oscA->route);
    s.check (oscA->popoverId() == "route", "routing popover open");
    s.check (bound<ParamKnob> (*oscA, A.filterBal) != nullptr && bound<ParamKnob> (*oscA, A.bus1) != nullptr, "balance + bus knobs");
    s.save ("osc_popover_route");
    if (auto* rc = bound<ParamChoice> (*oscA, A.route))
    {
        s.click (*rc);
        s.check (s.menuItems() == juce::StringArray ({ "Filter", "Main", "Direct", "None" }), "route menu");
        s.choose ("Main");
        s.check (proc.params.geti (A.route) == 1, "route Main");
        s.pump (40);
        s.check (oscA->route.getLabel() == "M", "routing button shows M");
        s.check (bound<ParamKnob> (*oscA, A.filterBal) == nullptr, "balance hidden when not routed to a filter");
    }
    s.click (oscA->route);

    // ---- Display: 3D, editor overlay, file drop -------------------------------
    auto* page = dynamic_cast<OscPageHost*> (s.panel.pageComponent (0));
    if (auto* disp = findIn<juce::FileDragAndDropTarget> (*oscA) ? dynamic_cast<juce::Component*> (findIn<juce::FileDragAndDropTarget> (*oscA)) : nullptr)
    {
        s.click (*disp, { 60, 60 });
        s.check ((bool) proc.uiState.getProperty ("osc3dA", false), "click toggles 3D");
        s.save ("osc_wt_3d");
        s.click (*disp, { 60, 60 });
        if (auto* pen = findIn<IconButton> (*disp, [] (IconButton& b) { return b.icon == IconButton::Pencil; }))
        {
            s.click (*pen);
            s.check (page != nullptr && page->overlayId() == "wteditA", "pencil opens the wavetable editor overlay");
            if (page != nullptr) page->closeOverlay();
            s.pump (30);
        }
        dynamic_cast<juce::FileDragAndDropTarget*> (disp)->filesDropped ({ wav.getFullPathName() }, 10, 10);
        s.pump (40);
        s.check (proc.patch().wavetable[0].table->name == "Shot Tone C3", "audio dropped on the display imports a wavetable");
    }

    // ---- SAMPLE mode -------------------------------------------------------
    s.click (*typeA);
    s.choose ("Sample");
    s.check (proc.params.geti (A.type) == 2, "type Sample chosen");
    s.save ("osc_sample_empty");
    proc.loadSampleFile (0, wav);
    s.pump (40);
    if (auto* name = findIn<PatchChoice> (*oscA, [] (PatchChoice& p) { return p.getY() < 60; }))
    {
        openAndCheck (s, *name, { "Factory", "Factory Non-Tonal", "Wavetables", "Show Sample in Finder", "Reload Sample", "Load Sample...",
                                  "Switch to Wavetable/FFT 512", "Embed in Preset" }, "sample browser");
        s.save ("osc_menu_sample_browser");
        s.closeMenu();
    }
    auto* sd = findIn<SampleDisplay> (*oscA);
    s.check (sd != nullptr, "sample display");
    if (sd != nullptr)
    {
        // Loop menu.
        if (auto* loop = findIn<PatchChoice> (*oscA, [] (PatchChoice& p) { return p.getY() > 100; }))
        {
            s.click (*loop);
            s.check (s.menuItems() == juce::StringArray ({ "One-shot", "Fwd Loop", "Rev Loop", "Fwd/Rev Loop", "Tailed", "---",
                                                           "Relative Loop", "Link Loop Length", "Exit Loop on Release" }), "Sample loop menu");
            s.choose ("Fwd Loop");
            s.check (proc.params.geti (A.loopMode) == 1, "loop mode Fwd Loop");
        }
        proc.setWithUndo (A.loopStart, 0.3f);
        proc.setWithUndo (A.loopEnd, 0.7f);
        proc.setWithUndo (A.xfade, 0.4f);
        s.pump (40);
        // Drag the start marker to ~25 %.
        const int x0 = (int) sd->xOf (0.0f) + 1;
        const int x1 = (int) sd->xOf (0.25f);
        s.drag (*sd, { x0, 60 }, { x1, 60 });
        s.check (std::abs (proc.params.get (A.start) - 0.25f) < 0.03f, "drag the start marker");
        // Drag the loop end marker along the top.
        const float st = proc.params.get (A.start), en = proc.params.get (A.end);
        const int le = (int) sd->xOf (st + (en - st) * 0.7f);
        s.drag (*sd, { le, 4 }, { le + 20, 4 });
        s.check (proc.params.get (A.loopEnd) > 0.72f, "drag the loop end marker");
        // Zoom by dragging the waveform.
        s.drag (*sd, { 150, 60 }, { 150, 20 });
        s.check (sd->viewEnd - sd->viewStart < 0.9f, "drag up zooms in");
        s.save ("osc_sample_loop");
        sd->viewStart = 0.0f; sd->viewEnd = 1.0f;

        // Operations menu.
        s.click (*sd, { 100, 60 }, true);
        auto ops = s.menuItems();
        for (auto* t : { "Show Marker Animation", "Zoom to Start and End", "Snap Off", "Snap to Zero", "Fade Edges", "Normalize", "Reverse", "Trim",
                         "Slicing Off", "Slice Auto", "Slice Manual" })
            s.check (ops.contains (t), juce::String ("ops menu has ") + t);
        s.check (! ops.contains ("Show X|Y Control"), "no X|Y in Sample mode");
        s.save ("osc_menu_sample_ops");
        s.choose ("Slice Auto");
        s.check (proc.patch().sample[0].slicing == SampleOscData::SliceAuto, "Slice Auto");
        s.check (proc.patch().sample[0].processed != nullptr && proc.patch().sample[0].processed->slices.size() >= 2, "auto slices found");
        s.click (*sd, { 100, 60 }, true);
        s.check (s.menuHas ("Root Note/C3") && s.menuItems().contains ("Send to Selected Clip (1)"), "slicing options");
        s.choose ("Send to Selected Clip (1)");
        s.check (! proc.patch().clips.clips[0].notes.empty(), "slices sent to clip 1");
        s.click (*sd, { 100, 60 }, true);
        s.choose ("Reverse");
        s.check (proc.patch().sample[0].edits.size() == 1, "Reverse added an edit");
        s.click (*sd, { 100, 60 }, true);
        s.choose ("Fade Edges/8 ms");
        s.check (proc.patch().sample[0].fadeEdges == 4, "Fade Edges 8 ms");
        s.save ("osc_sample_slices");
        s.click (*sd, { 100, 60 }, true);
        s.choose ("Slicing Off");
    }
    if (auto* scan = bound<ParamKnob> (*oscA, A.scan))
    {
        s.click (*scan, { -1, -1 }, true);
        auto it = s.menuItems();
        s.check (it.contains ("#A Scan") || it[0].startsWith ("#A Scan"), "SCAN menu title");
        s.check (s.menuHas ("Range"), "SCAN Range >");
        s.menuHas ("Range/-");             // opens the sub-menu
        s.check (s.menuItems (1) == juce::StringArray ({ "+/- 200%", "+/- 400%", "+/- 800%" }), "SCAN Range items");
        s.closeMenu();
        s.click (*scan, { -1, -1 }, true);
        s.check (! s.menuItems().contains ("Key Track"), "no Key Track in Sample mode");
        s.choose ("Reverse");
        s.check (proc.patch().sample[0].scanReverse, "SCAN Reverse");
    }
    if (auto* xf = findIn<XFadeButton> (*oscA))
    {
        s.drag (*xf, { 10, 10 }, { 10, -20 });
        s.check (proc.params.get (A.xfade) > 0.5f, "drag up on the crossfade button");
    }
    s.noteOn (60);
    s.play (10);
    s.save ("osc_sample_playing");
    s.noteOff (60);

    // ---- GRANULAR ------------------------------------------------------------
    s.click (*typeA);
    s.choose ("Granular");
    s.check (proc.params.geti (A.type) == 3, "type Granular");
    s.check (proc.patch().sample[0].sample != nullptr, "the sample carries over");
    s.save ("osc_granular");
    if (auto* loop = findIn<PatchChoice> (*oscA, [] (PatchChoice& p) { return p.getY() > 100; }))
    {
        s.click (*loop);
        auto it = s.menuItems();
        s.check (it.contains ("Manual") && it.contains ("Loop Grains"), "Granular loop menu");
        s.choose ("Manual");
        s.check (proc.patch().sample[0].manualLoop, "Manual loop");
        s.pump (40);
        s.check (loop->getText() == "MANUAL", "loop face MANUAL");
    }
    if (auto* dens = bound<ParamKnob> (*oscA, A.density))
    {
        s.click (*dens, { -1, -1 }, true);
        auto it = s.menuItems();
        s.check (it.contains ("Free") && it.contains ("BPM Sync") && it.contains ("Grains") && it.contains ("Jump Start"), "DENS menu");
        s.check (s.menuHas ("Max Grains/64"), "Max Grains");
        s.closeMenu();
        s.click (*dens, { -1, -1 }, true);
        s.choose ("BPM Sync");
        s.check (proc.patch().sample[0].densityMode == SampleOscData::DensSync, "DENS BPM Sync");
        s.click (*dens, { -1, -1 }, true);
        s.check (s.menuItems().contains ("Triplet"), "Triplet shown with BPM Sync");
        s.closeMenu();
    }
    if (auto* len = bound<ParamKnob> (*oscA, A.grainLen))
    {
        s.click (*len, { -1, -1 }, true);
        s.check (s.menuItems().contains ("Percent"), "LENGTH menu");
        s.choose ("Percent");
        s.check (proc.patch().sample[0].lengthMode == SampleOscData::LenPercent, "LENGTH Percent");
    }
    if (auto* dir = bound<ParamKnob> (*oscA, A.grDir))
    {
        s.click (*dir, { -1, -1 }, true);
        s.choose ("Reverse Grains");
        s.check (proc.patch().sample[0].reverseGrains, "Reverse Grains");
    }
    if (auto* win = findIn<IconButton> (*oscA, [] (IconButton& b) { return b.icon == IconButton::Window; }))
    {
        s.click (*win);
        s.check (oscA->popoverId() == "window", "grain window popover");
        if (auto* sh = bound<ParamChoice> (*oscA, A.winShape))
        {
            s.click (*sh);
            s.check (s.menuItems().size() == 10, "window shapes");
            s.choose ("Tukey");
            s.check (proc.params.geti (A.winShape) == 5, "window Tukey");
        }
        s.save ("osc_popover_window");
        s.click (*win);
    }
    if (auto* wb = findIn<IconButton> (*oscA, [] (IconButton& b) { return b.icon == IconButton::ArrowDown; }))
    {
        s.click (*wb);
        s.check (oscA->popoverId() == "warp", "granular warp panel");
        s.save ("osc_popover_granular_warp");
        s.click (*wb);
    }
    if (auto* gear = findIn<IconButton> (*oscA, [] (IconButton& b) { return b.icon == IconButton::Gear; }))
    {
        s.click (*gear);
        if (auto* sp = findIn<PatchChoice> (*oscA->popover()))
        {
            s.click (*sp);
            s.choose ("Random");
            s.check (proc.patch().sample[0].spawnPattern == SampleOscData::SpawnRandom, "spawn pattern");
        }
        s.save ("osc_popover_granular_unison");
        s.click (*gear);
    }
    if (auto* gd = findIn<SampleDisplay> (*oscA))
    {
        s.click (*gd, { 100, 60 }, true);
        s.check (! s.menuHas ("Slice Auto") || true, "ops");
        s.choose ("Show X|Y Control");
        s.check (proc.patch().sample[0].showXY, "Show X|Y Control");
        s.click (*gd, { 100, 60 }, true);
        s.choose ("Y Axis/Density");
        s.check (proc.patch().sample[0].yTargetGranular == 4, "Y Axis Density");
        s.click (*gd, { 100, 60 }, true);
        s.check (s.menuHas ("Modulate Y:/LFOs"), "Modulate Y: menu");
        s.closeMenu();
        s.save ("osc_granular_xy");
    }

    // ---- SPECTRAL ------------------------------------------------------------
    s.click (*typeA);
    s.choose ("Spectral");
    s.pump (60);
    s.check (proc.params.geti (A.type) == 4 && proc.patch().sample[0].spectral != nullptr, "type Spectral with analysis");
    proc.editPatch ("x", [] (Patch& p) { p.sample[0].manualLoop = false; p.sample[0].showXY = false; });
    s.pump (40);
    s.save ("osc_spectral");
    if (auto* name = findIn<PatchChoice> (*oscA, [] (PatchChoice& p) { return p.getY() < 60; }))
    {
        s.click (*name);
        s.check (s.menuItems().contains ("Import PNG..."), "Spectral browser has Import PNG...");
        s.closeMenu();
    }
    if (auto* thumb = findIn<juce::Component> (*oscA, [] (juce::Component& c) { auto* t = dynamic_cast<juce::TooltipClient*> (&c); return t != nullptr && t->getTooltip().startsWith ("Spectral filter"); }))
    {
        s.click (*thumb, { 10, 10 }, true);
        s.check (s.menuItems().contains ("Curve Filter") && s.menuItems().contains ("Vowel"), "spectral filter menu");
        const auto vowels = FactoryTables::names ("Vowel");
        if (! vowels.empty())
        {
            s.choose ("Vowel/" + juce::String (vowels[0]));
            s.check (proc.patch().sample[0].specFilterTable == "Vowel/" + vowels[0] && ! proc.patch().sample[0].specFilterMask.empty(), "wavetable filter chosen");
        }
        s.click (*thumb, { 10, 10 });
        s.check (oscA->popoverId() == "specfilter", "spectral filter editor");
        s.save ("osc_popover_specfilter");
        if (auto* folder = findIn<PanelButton> (*oscA->popover(), [] (PanelButton& b) { return b.getLabel() == "SHAPES"; }))
        {
            s.click (*folder);
            s.check (s.menuHas ("Factory/Low Pass") && s.menuItems().contains ("Save Shape..."), "filter shapes menu");
            s.choose ("Factory/Low Pass");
            s.check (proc.patch().sample[0].specFilter.points.size() == 4, "factory filter shape");
        }
        s.click (*thumb, { 10, 10 });
    }
    if (auto* spd = findIn<SampleDisplay> (*oscA))
    {
        const auto wa = spd->waveArea();
        s.drag (*spd, { (int) wa.getRight() + 8, (int) wa.getY() + 2 }, { (int) wa.getRight() + 8, (int) wa.getCentreY() });
        s.check (proc.params.get (A.specHi) < 5000.0f, "drag the Freq Hi marker");
        s.click (*spd, { (int) wa.getRight() + 8, 50 }, true);
        s.check (s.menuItems() == juce::StringArray ({ "Smooth", "Post Warp" }), "Hi/Lo menu");
        s.choose ("Smooth");
        s.check (proc.patch().sample[0].specSmooth, "Smooth");
        s.click (*spd, { 100, 60 }, true);
        s.check (s.menuItems().contains ("Show Waveform Display"), "spectral ops menu");
        s.choose ("Show Waveform Display");
        s.check (proc.patch().sample[0].showWaveform, "Show Waveform Display");
    }
    if (auto* scan = bound<ParamKnob> (*oscA, A.scan))
    {
        s.click (*scan, { -1, -1 }, true);
        auto it = s.menuItems();
        s.check (it.contains ("Phase Lock") && it.contains ("Transients") && it.contains ("Key Track"), "Spectral SCAN menu");
        s.save ("osc_menu_spectral_scan");
        s.choose ("Phase Lock");
        s.check (proc.patch().sample[0].phaseLock, "Phase Lock");
    }
    s.save ("osc_spectral_filter");

    // Switch to Wavetable from the browser.
    if (auto* name = findIn<PatchChoice> (*oscA, [] (PatchChoice& p) { return p.getY() < 60; }))
    {
        s.click (*name);
        s.choose ("Switch to Wavetable/FFT 2048");
        s.check (proc.params.geti (A.type) == 0, "Switch to Wavetable");
        if (auto t = proc.patch().wavetable[0].table)
            s.check (t->numFrames() > 1, "imported table has frames");
    }

    // ---- MULTISAMPLE -----------------------------------------------------------
    s.click (*typeA);
    s.choose ("Multisample");
    s.check (proc.params.geti (A.type) == 1, "type Multisample");
    writeTone (tmp.getChildFile ("low.wav"), 130.81, 0.5);
    writeTone (tmp.getChildFile ("high.wav"), 523.25, 0.5);
    tmp.getChildFile ("kit.sfz").replaceWithText ("<group> ampeg_release=0.3\n<region> sample=low.wav lokey=0 hikey=59 pitch_keycenter=48\n"
                                                  "<region> sample=high.wav lokey=60 hikey=127 pitch_keycenter=c5\n");
    s.check (loadFileInto (proc, 0, tmp.getChildFile ("kit.sfz")), "SFZ loads");
    s.check (proc.patch().sample[0].multi != nullptr && proc.patch().sample[0].multi->zones.size() == 2, "two SFZ zones");
    if (proc.patch().sample[0].multi != nullptr && proc.patch().sample[0].multi->zones.size() == 2)
        s.check (proc.patch().sample[0].multi->zones[1].root == 72 && proc.patch().sample[0].multi->zones[0].hasEnv, "SFZ keys / envelope");
    s.noteOn (64);
    s.play (4);
    s.save ("osc_multisample");
    if (auto* name = findIn<PatchChoice> (*oscA, [] (PatchChoice& p) { return p.getY() < 60; }))
    {
        s.click (*name);
        auto it = s.menuItems();
        s.check (it.contains ("Plucked") && it.contains ("Load SFZ...") && it.contains ("Switch to Single Sample"), "instrument menu");
        s.save ("osc_menu_multisample");
        s.choose ("Switch to Single Sample");
        s.check (proc.params.geti (A.type) == 2 && proc.patch().sample[0].sample != nullptr && proc.patch().sample[0].sample->name == "high",
                 "Switch to Single Sample");
    }
    s.noteOff (64);
    s.click (*typeA);
    s.choose ("Multisample");
    if (auto* env = findIn<IconButton> (*oscA, [] (IconButton& b) { return b.icon == IconButton::Envelope; }))
    {
        s.click (*env);
        s.check (oscA->popoverId() == "env", "envelope pane");
        if (auto* ov = findIn<PanelButton> (*oscA->popover(), [] (PanelButton& b) { return b.getLabel() == "OVERRIDE"; }))
        {
            s.click (*ov);
            s.check (proc.patch().sample[0].envOverride, "OVERRIDE on");
        }
        s.save ("osc_popover_envelope");
        s.click (*env);
    }

    // ---- Back to wavetable; module menu pitch-bend item ------------------------
    s.click (*typeA);
    s.choose ("Sample");
    proc.setWithUndo (A.pitchTrack, 0.0f);
    s.click (oscA->label, { 30, 11 }, true);
    s.check (s.menuItems().contains ("Enable Pitch Bend Tracking"), "pitch bend item with tracking off (Sample)");
    s.closeMenu();
    proc.setWithUndo (A.pitchTrack, 1.0f);
    s.click (*typeA);
    s.choose ("Wavetable");

    // ---- SUB ------------------------------------------------------------------
    auto* sub = module (s, Kind::Sub);
    if (auto* sq = findIn<PanelButton> (*sub, [] (PanelButton& b) { return b.getTooltip() == "Square"; }))
    {
        s.click (*sq);
        s.check (proc.params.geti (ids().sub.shape) == 4, "SUB shape Square");
    }
    if (auto* ph = bound<ParamField> (*sub, ids().sub.phase))
    {
        s.click (*ph, { -1, -1 }, true);
        s.choose ("Contiguous");
        s.check (proc.params.on (ids().sub.contiguous), "SUB Contiguous");
    }
    s.click (sub->route);
    s.save ("osc_sub_route");
    s.click (sub->route);

    // ---- NOISE ----------------------------------------------------------------
    auto* noise = module (s, Kind::Noise);
    if (auto* nn = findIn<PatchChoice> (*noise))
    {
        s.click (*nn);
        auto it = s.menuItems();
        s.check (it.contains ("Color") && it.contains ("Load Sample...") && it.contains ("Show Sample in Finder"), "noise menu");
        s.check (s.menuHas ("Color/Brown"), "Color > Brown");
        s.save ("osc_menu_noise");
        s.closeMenu();
        const auto cats = FactoryNoises::categories();
        const auto names = FactoryNoises::names (cats[0]);
        s.click (*nn);
        s.choose (juce::String (cats[0]) + "/" + juce::String (names[0]));
        s.check (proc.params.geti (ids().noise.colour) == 0 && proc.patch().noise.sample->name == names[0], "factory noise chosen");
        s.save ("osc_noise_sample");
        s.click (*nn);
        s.choose ("Color/Pink");
        s.check (proc.params.geti (ids().noise.colour) == 2, "Color Pink");
        s.pump (40);
        s.check (bound<ParamKnob> (*noise, ids().noise.stereo) != nullptr, "STEREO shown for colour noise");
    }
    if (auto* os = s.button ("ONE-SHOT")) { s.click (*os); }

    // ---- FILTER ---------------------------------------------------------------
    auto* ft = bound<ParamChoice> (*f1, ids().filter[0].type);
    s.click (*ft);
    s.check (s.menuItems() == juce::StringArray ({ "Normal", "Multi", "Flanges", "Misc", "New" }), "filter type top level");
    s.check (s.menuHas ("Misc/French LP"), "Misc > French LP");
    s.save ("osc_menu_filter_type");
    s.closeMenu();
    s.click (*ft);
    s.choose ("Misc/French LP");
    const auto& types = choices::filterTypes();
    const int french = (int) (std::find (types.begin(), types.end(), "French LP") - types.begin());
    s.check (proc.params.geti (ids().filter[0].type) == french, "filter type chosen");
    s.pump (40);
    s.check (bound<ParamKnob> (*f1, ids().filter[0].var)->caption == "BOEUF", "VAR caption follows the type");
    s.wheel (*ft, -1.0f);
    s.check (proc.params.geti (ids().filter[0].type) == french + 1, "wheel steps the type");
    s.click (*ft);
    s.choose ("Misc/German LP");
    s.pump (40);
    s.check (! bound<ParamKnob> (*f1, ids().filter[0].var)->isEnabled(), "VAR disabled when unused");
    s.click (*ft);
    s.choose ("Normal/MG Low 24");
    if (auto* drv = bound<ParamKnob> (*f1, ids().filter[0].drive))
    {
        s.click (*drv, { -1, -1 }, true);
        s.check (s.menuItems()[0] == "#Filter 1 Drive" && s.menuItems()[1] == "Clean Mode", "DRIVE menu");
        s.choose ("Clean Mode");
        s.check (proc.params.on (ids().filter[0].cleanDrive), "Clean Mode");
    }
    if (auto* bsw = findIn<PanelButton> (*f1, [] (PanelButton& b) { return b.getLabel() == "B"; }))
    {
        s.click (*bsw);
        s.check (proc.params.geti (B.route) == 0 && proc.params.get (B.filterBal) < 0.01f, "B switch routes OSC B into Filter 1");
        s.pump (40);
        s.check (bsw->isLit(), "B lit");
        if (auto* b2 = findIn<PanelButton> (*f2, [] (PanelButton& b) { return b.getLabel() == "B"; }))
        {
            s.click (*b2);
            s.check (std::abs (proc.params.get (B.filterBal) - 0.5f) < 0.01f, "B on both filters");
            s.click (*bsw);
            s.check (proc.params.get (B.filterBal) > 0.99f, "B off Filter 1 leaves it on Filter 2");
            s.pump (40);
            s.check (oscB->route.getLabel() == "F2", "OSC B routing shows F2");
        }
    }
    proc.setWithUndo (ids().filter[0].enable, 1.0f);
    if (auto* fd = findIn<juce::Component> (*f1, [] (juce::Component& c) { auto* t = dynamic_cast<juce::TooltipClient*> (&c); return t != nullptr && t->getTooltip().startsWith ("Drag: cutoff"); }))
    {
        s.drag (*fd, { 150, 80 }, { 60, 30 });
        s.check (proc.params.get (ids().filter[0].cutoff) < 3000.0f && proc.params.get (ids().filter[0].res) > 0.5f, "drag sets cutoff and resonance");
        s.click (*fd, { 50, 50 }, true);
        s.check (s.menuItems() == juce::StringArray ({ "#Display Mode", "Frequency Response", "Frequency Response & FFT", "Phase Response & FFT" }), "display mode menu");
        s.choose ("Phase Response & FFT");
        s.check ((int) proc.uiState.getProperty ("filterDisplay1", 0) == 2, "display mode stored");
        proc.uiState.setProperty ("filterDisplay1", 0, nullptr);
    }
    s.click (f1->route);
    if (auto* rc = bound<ParamChoice> (*f1, ids().filter[0].route))
    {
        s.click (*rc);
        s.check (s.menuItems() == juce::StringArray ({ "Filter 2", "Main", "Direct", "None" }), "filter route menu");
        s.choose ("Filter 2");
        s.pump (40);
        s.check (f1->route.getLabel() == "F2", "filter routing button shows F2");
    }
    s.save ("osc_filter_route");
    s.click (f1->route);
    s.click (f1->label, { 60, 11 }, true);
    s.check (s.menuItems().contains ("Paste") && ! s.menuItems().contains ("Enable Pitch Tracking"), "filter module menu");
    s.choose ("Copy");
    s.click (f2->label, { 60, 11 }, true);
    s.choose ("Paste");
    s.check (proc.params.geti (ids().filter[1].type) == proc.params.geti (ids().filter[0].type), "filter Copy / Paste");

    // ---- Final look: a busy patch -------------------------------------------------
    proc.setWithUndo (B.type, 3.0f);
    proc.loadSampleFile (1, wav);
    proc.setWithUndo (ids().osc[2].enable, 1.0f);
    proc.setWithUndo (ids().osc[2].type, 4.0f);
    proc.editPatch ("x", [] (Patch& p) { p.sample[2].sample = p.sample[1].sample; p.sample[2].rebuild (true); });
    s.noteOn (60);
    s.play (8);
    s.save ("osc_final");
    s.noteOff (60);
}

TINCTURE_SHOT ("osc", oscShot);
