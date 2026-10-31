#include "PluginEditor.h"
#include "SerumLibrary.h"
#include "Model/FactoryPresets.h"

#ifndef JucePlugin_VersionString
 #define JucePlugin_VersionString "dev"
#endif

using namespace tincture;
using namespace tincture::ui;
using juce::Rectangle;

namespace
{
const char* kPageTips[] { "Oscillators, filters and modulators", "The mixer: levels, routing and buses",
                          "The FX racks: MAIN, BUS 1, BUS 2", "The modulation matrix", "Preferences, voicing, quality and tuning" };
}

// ===========================================================================
// TincturePanel
// ===========================================================================
TincturePanel::TincturePanel (TinctureProcessor& p) : PanelContext (p)
{
    setLookAndFeel (&lnf);
    setSize (designW, designH);
    setWantsKeyboardFocus (true);

    editValue = [this] (juce::Component& t, const juce::String& init, std::function<void (const juce::String&)> done) { editValueBox (t, init, std::move (done)); };
    setStatus = [this] (const juce::String& s) { status = s; };

    // Header.
    int x = 196;
    for (int i = 0; i < NumPages; ++i)
    {
        addAndMakeVisible (tabs[i]);
        tabs[i].setBounds (x, 14, 74, 34);
        tabs[i].setFontHeight (13.0f);
        tabs[i].setTooltip (kPageTips[i]);
        tabs[i].onClick = [this, i] (const juce::ModifierKeys&) { showPage (i); };
        x += 78;
    }
    x += 12;
    auto addBtn = [this] (PanelButton& b, Rectangle<int> r, const juce::String& tip, std::function<void()> fn)
    {
        addAndMakeVisible (b);
        b.setBounds (r);
        b.setTooltip (tip);
        b.onClick = [fn] (const juce::ModifierKeys&) { fn(); };
    };
    addBtn (saveBtn, { x, 14, 54, 34 }, "Save the preset", [this] { savePresetDialog(); });
    x += 60;
    addAndMakeVisible (presetName);
    presetName.setBounds (x, 14, 300, 34);
    presetName.fontH = 16.0f;
    presetName.getText = [this]
    {
        auto n = proc.presetName();
        return (n.isEmpty() || n == "Init" ? juce::String ("- Init -") : n) + (proc.presetEdited() ? " *" : "");
    };
    presetName.onOpen = [this] { openPresetMenu(); };
    presetName.setTooltip ("Click for presets; < > step through them");
    x += 304;
    addBtn (prevBtn, { x, 14, 30, 34 }, "Previous preset", [this] { proc.stepPreset (-1); });
    addBtn (nextBtn, { x + 32, 14, 30, 34 }, "Next preset", [this] { proc.stepPreset (1); });
    x += 70;
    addBtn (browseBtn, { x, 14, 70, 34 }, "Open the preset browser", [this] { toggleBrowser(); });
    x += 76;
    addBtn (menuBtn, { x, 14, 60, 34 }, "Main menu", [this] { openMainMenu(); });
    x += 66;
    addBtn (undoBtn, { x, 8, 54, 22 }, "Undo", [this] { proc.undo(); });
    addBtn (redoBtn, { x, 32, 54, 22 }, "Redo", [this] { proc.redo(); });
    undoBtn.setFontHeight (10.5f);
    redoBtn.setFontHeight (10.5f);

    masterKnob = std::make_unique<ParamKnob> (*this, ids().global.master, "MAIN");
    masterKnob->small = true;
    addAndMakeVisible (*masterKnob);
    masterKnob->setBounds (1298, 2, 58, 58);
    addAndMakeVisible (meterL);
    addAndMakeVisible (meterR);
    meterL.setBounds (1364, 8, 9, 46);
    meterR.setBounds (1377, 8, 9, 46);

    // Pages.
    pages[Osc] = createOscPage (*this);
    pages[Mix] = createMixPage (*this);
    pages[Fx] = createFxPage (*this);
    pages[Matrix] = createMatrixPage (*this);
    pages[Global] = createGlobalPage (*this);
    for (auto& pg : pages)
    {
        addChildComponent (*pg);
        pg->setBounds (0, kHeaderH, kPageW, kPageH);
    }
    browser = createPresetBrowser (*this, [this] { toggleBrowser(); });
    addChildComponent (*browser);
    browser->setBounds (0, kHeaderH, kPageW, kPageH);

    showPage (juce::jlimit (0, NumPages - 1, (int) proc.uiState.getProperty ("page", 0)));
    startTimerHz (30);
}

TincturePanel::~TincturePanel()
{
    stopTimer();
    for (auto& pg : pages) pg.reset();
    browser.reset();
    setLookAndFeel (nullptr);
}

void TincturePanel::showPage (int p)
{
    page = juce::jlimit (0, NumPages - 1, p);
    if (browser != nullptr && browser->isVisible()) browser->setVisible (false);
    for (int i = 0; i < NumPages; ++i)
    {
        pages[(size_t) i]->setVisible (i == page);
        tabs[i].setLit (i == page);
    }
    proc.uiState.setProperty ("page", page, nullptr);
    browseBtn.setLit (false);
}

void TincturePanel::toggleBrowser()
{
    const bool show = ! browser->isVisible();
    dismissMenus (*this);
    browser->setVisible (show);
    pages[(size_t) page]->setVisible (! show);
    browseBtn.setLit (show);
}

void TincturePanel::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    // Header strip.
    g.setColour (colours::panelEdge);
    g.fillRect (0, 0, designW, kHeaderH - 4);
    drawText (g, "TINCTURE", logoArea().toFloat().removeFromTop (30.0f), 28.0f, colours::brand, true, juce::Justification::centredLeft);
    drawText (g, "wavetable synthesizer", logoArea().toFloat().withTrimmedTop (30.0f), 11.0f, colours::inkDim, false, juce::Justification::centredLeft);
    g.setColour (colours::inkFaint);
    g.fillRect (0, kHeaderH - 4, designW, 1);

    // Voice counter, left of MAIN.
    const auto vr = Rectangle<float> (1232.0f, 10.0f, 62.0f, 40.0f);
    drawText (g, "VOICES", vr.withHeight (14.0f), 10.0f, colours::inkDim, true);
    drawLcdBox (g, vr.withTrimmedTop (16.0f));
    drawText (g, juce::String (juce::jmax (0, unison)) + " / " + juce::String ((int) proc.params.get (ids().global.poly)),
              vr.withTrimmedTop (16.0f), 13.0f, colours::lcd, true);

    // Status line.
    const auto sr = Rectangle<float> (0.0f, (float) (designH - kStatusH), (float) designW, (float) kStatusH);
    g.setColour (colours::panelEdge);
    g.fillRect (sr);
    drawText (g, shownStatus, sr.reduced (12.0f, 0.0f), 11.5f, colours::inkDim, false, juce::Justification::centredLeft);
}

void TincturePanel::resized() { resizeMenuLayer(); }

void TincturePanel::mouseDown (const juce::MouseEvent& e)
{
    if (logoArea().contains (e.getPosition())) openResizeMenu();
}

bool TincturePanel::keyPressed (const juce::KeyPress& k)
{
    const auto mods = k.getModifiers();
    if (mods.isCommandDown() && k.getKeyCode() == 'Z') { if (mods.isShiftDown()) proc.redo(); else proc.undo(); return true; }
    if (mods.isCommandDown() && k.getKeyCode() == 'Y') { proc.redo(); return true; }
    if (k.getKeyCode() == juce::KeyPress::escapeKey && browser->isVisible()) { toggleBrowser(); return true; }
    return false;
}

void TincturePanel::timerCallback()
{
    refreshAll();
    presetName.refresh();
    undoBtn.setDimmed (! proc.canUndo());
    redoBtn.setDimmed (! proc.canRedo());
    undoBtn.setTooltip (proc.canUndo() ? "Undo " + proc.undoLabel() : juce::String ("Nothing to undo"));
    redoBtn.setTooltip (proc.canRedo() ? "Redo " + proc.redoLabel() : juce::String ("Nothing to redo"));

    auto& t = proc.telemetry();
    meterL.push (t.peakL.exchange (0.0f));
    meterR.push (t.peakR.exchange (0.0f));
    const int u = t.activeUnison.load();
    if (u != unison) { unison = u; repaint (1228, 0, 70, kHeaderH); }
    if (proc.midiLearnTarget() >= 0)
        status = "MIDI Learn: move a controller to assign it to " + juce::String (params()[proc.midiLearnTarget()].name);
    if (status != shownStatus) { shownStatus = status; repaint (0, designH - kStatusH, designW, kStatusH); }
}

// ---------------------------------------------------------------------------
// Value entry
// ---------------------------------------------------------------------------
void TincturePanel::editValueBox (juce::Component& target, const juce::String& initial, std::function<void (const juce::String&)> onDone)
{
    valueEditor = std::make_unique<juce::TextEditor>();
    auto& ed = *valueEditor;
    ed.setFont (panelFont (15.0f));
    ed.setJustification (juce::Justification::centred);
    ed.setText (initial, false);
    ed.selectAll();
    auto r = getLocalArea (&target, target.getLocalBounds());
    r = r.withSizeKeepingCentre (juce::jmax (90, r.getWidth()), 26);
    addAndMakeVisible (ed);
    ed.setBounds (r.constrainedWithin (getLocalBounds()));
    valueDone = std::move (onDone);
    auto finish = [this] (bool apply)
    {
        if (valueEditor == nullptr) return;
        const auto text = valueEditor->getText();
        auto done = std::move (valueDone);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<TincturePanel> (this)] { if (safe != nullptr) safe->valueEditor.reset(); });
        valueEditor->setVisible (false);
        if (apply && done) done (text);
    };
    ed.onReturnKey = [finish] { finish (true); };
    ed.onEscapeKey = [finish] { finish (false); };
    ed.onFocusLost = [finish] { finish (true); };
    ed.grabKeyboardFocus();
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------
void TincturePanel::openResizeMenu()
{
    const float cur = getTransform().mat00;   // the panel is only ever scaled uniformly
    Menu m;
    m.add ("Default (100%)", [this] { if (onScale) onScale (1.0f); });
    m.separator();
    for (int pc : { 50, 75, 90, 100, 110, 125, 133, 150, 175, 200 })
    {
        const float s = (float) pc / 100.0f;
        m.add (juce::String (pc) + "%", [this, s] { if (onScale) onScale (s); }, std::abs (cur - s) < 0.01f);
    }
    m.separator();
    m.add ("Set " + juce::String (juce::roundToInt (cur * 100.0f)) + "% as Default", [this, cur] { proc.settings.setDefaultScale (cur); });
    showContextMenu (*this, { logoArea().getX(), logoArea().getBottom() }, m);
}

void TincturePanel::openPresetMenu()
{
    auto& store = proc.presets;
    store.rescan();
    Menu factory, user;
    std::map<juce::String, Menu> byCat;
    const auto& all = store.all();
    for (size_t i = 0; i < all.size(); ++i)
    {
        const auto& e = all[i];
        auto pick = [this, i]
        {
            const auto& en = proc.presets.all()[i];
            if (en.factory)
            {
                Patch p;
                ParamValues v;
                v.resetToDefaults();
                factoryPresets()[(size_t) en.factoryIndex].build (p, v);
                p.info.name = en.name.toStdString();
                p.info.category = en.category.toStdString();
                p.info.author = "Tincture";
                p.info.description = en.description.toStdString();
                const auto bin = encodePreset (p, v).toBinary();
                proc.loadPresetData (juce::MemoryBlock (bin.data(), bin.size()), en.name);
            }
            else proc.loadPreset (en.file);
        };
        const bool current = e.name == proc.presetName();
        if (e.factory) byCat[e.category.isEmpty() ? juce::String ("Other") : e.category].add (e.name, pick, current);
        else user.add ((e.folder.isNotEmpty() ? e.folder + "/" : juce::String()) + e.name, pick, current);
    }
    for (auto& [cat, m] : byCat) factory.sub (cat, m);
    Menu m;
    m.sub ("Factory", factory);
    m.sub ("User", user.empty() ? Menu().add ("(no user presets yet)", {}, false, false) : user);
    m.separator();
    m.add ("Save Preset...", [this] { savePresetDialog(); });
    m.add ("Load Preset...", [this] { loadPresetDialog(); });
    m.add ("Import Serum 2 Preset...", [this] { importSerumDialog(); });
    m.add ("Import Serum 2 Folder...", [this] { importSerumFolderDialog(); });
    m.add ("Open Preset Browser", [this] { toggleBrowser(); });
    showMenu (presetName, m);
}

void TincturePanel::openMainMenu()
{
    Menu m;
    m.add ("About...", [this]
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, "Tincture",
            "Tincture " JucePlugin_VersionString "\nA wavetable synthesizer by Taddei.", "OK", this);
    });
    m.separator();
    m.add ("Init Preset", [this] { proc.initPatch(); });
    m.add ("Init LFOs + LFO Mods", [this]
    {
        proc.editAll ("Init LFOs", [] (Patch& p, ParamValues& v)
        {
            const auto& reg = params();
            for (int l = 0; l < kNumLfos; ++l)
            {
                p.lfo[(size_t) l] = LfoData();
                p.matrix.removeSource (lfoSource (l));
                const std::string g = "LFO " + std::to_string (l + 1);
                for (int i = 0; i < reg.size(); ++i) if (reg[i].group == g) v[i] = reg[i].def;
            }
        });
    });
    m.add ("Init Modulations", [this] { proc.editPatch ("Init Modulations", [] (Patch& p) { p.matrix.initModulations(); }); });
    m.separator();
    m.add ("Load Preset...", [this] { loadPresetDialog(); });
    m.add ("Import Serum 2 Preset...", [this] { importSerumDialog(); });
    m.add ("Import Serum 2 Folder...", [this] { importSerumFolderDialog(); });
    m.add ("Serum Library Folder...", [this] { serumFolderDialog(); });
    m.add ("Revert to Saved", [this] { proc.revertToSaved(); }, false, proc.presetEdited() && proc.currentPresetFile().existsAsFile());
    m.add ("Save as Default Preset", [this] { proc.saveAsDefault(); });
    m.separator();
    m.add ("Open Presets Folder", [] { Settings::userPresets().startAsProcess(); });
    m.add ("Rescan Folders on Disk", [this] { proc.presets.rescan(); });
    showMenu (menuBtn, m, { MenuOptions::Place::Below, {}, 220, false });
}

void TincturePanel::savePresetDialog()
{
    saveWindow = std::make_unique<juce::AlertWindow> ("Save Preset", "Name:", juce::MessageBoxIconType::NoIcon, this);
    const auto cur = proc.presetName();
    saveWindow->addTextEditor ("name", cur == "Init" ? juce::String() : cur);
    saveWindow->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
    saveWindow->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    saveWindow->enterModalState (true, juce::ModalCallbackFunction::create ([this] (int r)
    {
        if (r == 1 && saveWindow != nullptr)
        {
            const auto name = juce::File::createLegalFileName (saveWindow->getTextEditorContents ("name").trim());
            if (name.isNotEmpty())
                proc.savePreset (Settings::userPresets().getChildFile (name + PresetStore::extension));
        }
        saveWindow.reset();
    }), false);
}

void TincturePanel::loadPresetDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Load Preset", Settings::userPresets(), juce::String ("*") + PresetStore::extension + ";*.SerumPreset");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        if (TinctureProcessor::isSerumPreset (f))
        {
            juce::String summary;
            const bool ok = proc.importSerumPreset (f, summary);
            showReport (ok ? "Serum 2 Import" : "Serum 2 Import Failed", summary);
        }
        else proc.loadPreset (f);
    });
}

// Where the Serum dialogs start: the last folder used, else Serum's own presets.
static juce::File serumStartFolder (const Settings& s)
{
    const juce::File last (s.lastSerumImport());
    if (last.isDirectory()) return last;
    for (const auto& r : SerumLibrary::standardRoots())
        if (r.getChildFile ("Presets").isDirectory()) return r.getChildFile ("Presets");
    return juce::File::getSpecialLocation (juce::File::userHomeDirectory);
}

void TincturePanel::importSerumDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a Serum 2 Preset", serumStartFolder (proc.settings), "*.SerumPreset");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles, [this] (const juce::FileChooser& fc)
    {
        const auto f = fc.getResult();
        if (! f.existsAsFile()) return;
        proc.settings.setLastSerumImport (f.getParentDirectory().getFullPathName());
        juce::String summary;
        const bool ok = proc.importSerumPreset (f, summary);
        showReport (ok ? "Serum 2 Import" : "Serum 2 Import Failed", summary);
    });
}

void TincturePanel::importSerumFolderDialog()
{
    chooser = std::make_unique<juce::FileChooser> ("Import a Folder of Serum 2 Presets", serumStartFolder (proc.settings));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (! dir.isDirectory()) return;
        proc.settings.setLastSerumImport (dir.getFullPathName());
        juce::String summary;
        proc.importSerumFolder (dir, summary);
        showReport ("Serum 2 Folder Import", summary);
    });
}

void TincturePanel::serumFolderDialog()
{
    const juce::File current (proc.settings.serumFolder());
    chooser = std::make_unique<juce::FileChooser> ("Choose the Serum Presets Folder (the one holding Tables)",
                                                   current.isDirectory() ? current : juce::File::getSpecialLocation (juce::File::userHomeDirectory));
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories, [this] (const juce::FileChooser& fc)
    {
        const auto dir = fc.getResult();
        if (dir.isDirectory()) proc.settings.setSerumFolder (dir.getFullPathName());
    });
}

void TincturePanel::showReport (const juce::String& title, const juce::String& text)
{
    // Keep the box a sensible height: the first 24 lines.
    auto lines = juce::StringArray::fromLines (text);
    if (lines.size() > 24)
    {
        const int more = lines.size() - 23;
        lines.removeRange (23, lines.size());
        lines.add ("... and " + juce::String (more) + " more");
    }
    juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::NoIcon, title, lines.joinIntoString ("\n"), "OK", this);
}

// ===========================================================================
// TinctureEditor
// ===========================================================================
TinctureEditor::TinctureEditor (TinctureProcessor& p) : AudioProcessorEditor (p), panel (p)
{
    addAndMakeVisible (panel);
    setResizable (true, true);
    const double ratio = (double) TincturePanel::designW / TincturePanel::designH;
    getConstrainer()->setFixedAspectRatio (ratio);
    setResizeLimits (TincturePanel::designW / 2, TincturePanel::designH / 2, TincturePanel::designW * 2, TincturePanel::designH * 2);
    panel.onScale = [this] (float s) { setSize (juce::roundToInt (TincturePanel::designW * s), juce::roundToInt (TincturePanel::designH * s)); };
    const float s = juce::jlimit (0.5f, 2.0f, p.settings.defaultScale());
    setSize (juce::roundToInt (TincturePanel::designW * s), juce::roundToInt (TincturePanel::designH * s));
    if (p.settings.tooltips()) tooltips = std::make_unique<juce::TooltipWindow> (this, 700);
}

void TinctureEditor::paint (juce::Graphics& g) { g.fillAll (colours::panel); }

void TinctureEditor::resized()
{
    const float s = (float) getWidth() / (float) TincturePanel::designW;
    panel.setTransform (juce::AffineTransform::scale (s));
    panel.setBounds (0, 0, TincturePanel::designW, TincturePanel::designH);
}

juce::AudioProcessorEditor* TinctureProcessor::createEditor() { return new TinctureEditor (*this); }
