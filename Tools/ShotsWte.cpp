// ShotsWte.cpp — the Wavetable Editor (spec E §3–5): brushes, FFT drawing,
// every menu through real clicks, formula field, import/export, undo.
#include "Shots.h"
#include "UI/WavetableEditor.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace
{
using namespace tincture;
using tincture::ui::WavetableEditor;
namespace we = tincture::wtedit;

float frameDiff (const we::Frame& a, const we::Frame& b)
{
    float d = 0.0f;
    for (size_t i = 0; i < a.size(); ++i) d = std::max (d, std::abs (a[i] - b[i]));
    return d;
}

juce::StringArray tableLabels (we::WaveMenu m, int bin)
{
    juce::StringArray out;
    const auto list = we::waveMenu (m);
    const char* lastSub = nullptr;
    for (int i = 0; i < list.count; ++i)
    {
        const auto& op = list.items[i];
        if (op.kind == we::WaveOp::Separator) { out.add ("---"); continue; }
        if (op.submenu != nullptr)
        {
            if (lastSub == nullptr || juce::String (lastSub) != op.submenu) out.add (op.submenu);
            lastSub = op.submenu;
            continue;
        }
        juce::String l (juce::CharPointer_UTF8 (we::menuLabel (op, bin).c_str()));
        out.add (op.kind == we::WaveOp::Title ? "#" + l : l);
    }
    return out;
}

// Menu rows may carry header markers or ticks differently; compare loosely.
bool sameItems (const juce::StringArray& shown, const juce::StringArray& want)
{
    if (shown.size() != want.size()) return false;
    for (int i = 0; i < want.size(); ++i)
        if (! shown[i].contains (want[i].trimCharactersAtStart ("#"))) return false;
    return true;
}
} // namespace

static void wteShot (Shot& s)
{
    s.proc.initPatch();
    s.pump (50);
    bool closed = false;
    auto comp = tincture::ui::createWavetableEditor (s.panel, 0, [&closed] { closed = true; });
    auto* ed = dynamic_cast<WavetableEditor*> (comp.get());
    s.check (ed != nullptr, "editor created");
    if (ed == nullptr) return;
    s.panel.addAndMakeVisible (*comp);
    comp->setBounds (0, tincture::ui::kHeaderH, tincture::ui::kPageW, tincture::ui::kModuleRowH);
    s.pump (60);
    s.save ("wte_main");

    auto table = [&] { return s.proc.patch().wavetable[0].table; };
    const auto t0 = table();
    s.check (ed->set.size() >= 1, "frames loaded from patch");

    // ---- Brushes: real drags on the waveform ------------------------------
    auto& wave = ed->waveView;
    const int W = wave.getWidth(), H = wave.getHeight();
    for (int b : { (int) we::Brush::FlatLine, (int) we::Brush::Sine, (int) we::Brush::SlopeUp, (int) we::Brush::HalfSinePeak,
                   (int) we::Brush::InterpolateCurved, (int) we::Brush::Nudge, (int) we::Brush::Noise })
    {
        s.click (*ed->brushButtons[(size_t) b]);
        s.check ((int) ed->brush == b, juce::String ("brush selected: ") + we::brushName ((we::Brush) b));
        const auto before = ed->current();
        const auto tb = table();
        if (b == (int) we::Brush::Nudge || b == (int) we::Brush::Noise)
            s.drag (wave, { W / 2, H / 2 }, { W / 2 + 4, H / 5 }, 6);
        else
            s.drag (wave, { W / 10, H / 4 }, { W / 2, H / 3 }, 10);
        s.check (frameDiff (before, ed->current()) > 1.0e-3f, juce::String ("brush changed the frame: ") + we::brushName ((we::Brush) b));
        s.check (table() != tb, juce::String ("brush committed to the patch: ") + we::brushName ((we::Brush) b));
    }
    s.check (s.proc.undoLabel().contains ("Draw Wavetable"), "undo label for drawing");
    // Mirror + grid off (freehand).
    s.click (*ed->brushButtons.back());
    s.check (ed->mirror, "mirror on");
    s.click (*ed->brushButtons[0]);
    s.click (ed->gridXStep, { ed->gridXStep.valueArea().getX() + 6, 12 });       // "<" steps 8 -> 6
    s.check (ed->gridX == 6, "grid X stepper <");
    s.click (ed->gridXStep, ed->gridXStep.valueArea().getCentre());
    s.check (s.menuOpen() && s.menuItems().contains ("OFF"), "grid X menu");
    s.choose ("OFF");
    s.check (ed->gridX == 0, "grid X off");
    {
        const auto before = ed->current();
        s.drag (wave, { W / 8, H / 5 }, { W / 3, H * 4 / 5 }, 12);
        s.check (frameDiff (before, ed->current()) > 1.0e-3f, "freehand mirrored draw");
    }
    s.save ("wte_drawn");
    s.click (ed->gridXStep, ed->gridXStep.valueArea().getCentre());
    s.choose ("8");
    s.click (*ed->brushButtons.back());

    // ---- FFT area: draw magnitudes, menus ----------------------------------
    auto& fft = ed->fftView;
    {
        const auto ma = fft.magArea();
        const int x = (int) (fft.binX (5) + 1.0f);
        const float before = we::harmonicMag (ed->current(), 5);
        s.drag (fft, { x, (int) ma.getY() + 8 }, { x, (int) ma.getY() + 9 }, 2);
        s.check (std::abs (we::harmonicMag (ed->current(), 5) - before) > 1.0e-3f, "FFT magnitude drawn at bin 5");
        s.check (s.proc.undoLabel().contains ("FFT"), "FFT undo label");
    }
    {
        const int bin = 12;
        const auto ma = fft.magArea();
        s.click (fft, { (int) (fft.binX (bin) + 1.0f), (int) ma.getCentreY() }, true);
        s.check (s.menuOpen(), "FFT mags menu open");
        const auto items = s.menuItems();
        s.check (sameItems (items, tableLabels (we::WaveMenu::FftMags, bin)), "FFT Mags items match the table: " + items.joinIntoString (" | "));
        s.check (items[0].contains ("FFT Mags"), "FFT Mags title");
        s.save ("wte_menu_fft");
        s.choose ("Generate Saw");
        s.check (we::harmonicMag (ed->current(), 40) > 1.0e-3f, "generate saw fills harmonics");
        s.click (fft, { (int) (fft.binX (bin) + 1.0f), (int) ma.getCentreY() }, true);
        s.choose ("Clear HF (Bin 12 to End)");
        float hf = 0.0f;
        for (int h = 13; h < 200; ++h) hf = std::max (hf, we::harmonicMag (ed->current(), h));
        s.check (hf < 1.0e-3f, "Clear HF cleared bins above 12");
        s.check (we::harmonicMag (ed->current(), 3) > 1.0e-3f, "Clear HF kept low bins");
        // Toggle item.
        s.click (fft, { (int) (fft.binX (bin) + 1.0f), (int) ma.getCentreY() }, true);
        s.choose ("Draw Odd Harmonics Only");
        s.check (ed->fftOpts.on (we::FftDrawOptions::OddOnly), "odd-only toggle on");
        s.click (fft, { (int) (fft.binX (bin) + 1.0f), (int) ma.getCentreY() }, true);
        s.choose ("Draw Odd Harmonics Only");
        s.check (! ed->fftOpts.on (we::FftDrawOptions::OddOnly), "odd-only toggle off");
        // Phase strip menu.
        const auto pa = fft.phaseArea();
        s.click (fft, { (int) (fft.binX (bin) + 1.0f), (int) pa.getCentreY() }, true);
        s.check (s.menuItems().size() > 0 && s.menuItems()[0].contains ("FFT Phases"), "FFT Phases menu");
        s.closeMenu();
    }
    // Zoom stepper.
    s.click (ed->zoomStep, { ed->zoomStep.valueArea().getRight() - 6, 12 });
    s.check (ed->zoom == 2, "zoom 2x");
    s.wheel (fft, -1.0f, { 300, 30 });
    s.check (ed->fftFirst > 0, "FFT scrolls when zoomed");
    s.save ("wte_zoom");
    s.click (ed->zoomStep, { ed->zoomStep.valueArea().getX() + 6, 12 });

    // ---- REMOVE > Insert, SORT, MORPH --------------------------------------
    s.click (ed->removeBtn);
    s.check (sameItems (s.menuItems(), tableLabels (we::WaveMenu::Remove, 1)), "REMOVE items: " + s.menuItems().joinIntoString (" | "));
    s.check (s.menuHas ("Reduce to/Every 4th Frame"), "Reduce to submenu");
    s.save ("wte_menu_remove");
    s.choose ("Init All (Default)");
    s.check (ed->set.size() == 1 && ! ed->set.morphed(), "Init All: one frame");
    const int n0 = ed->set.size();
    s.click (ed->removeBtn);
    s.choose ("Insert (at current index)");
    s.check (ed->set.size() == n0 + 1, "Insert added a frame");
    s.click (ed->posPlus);
    s.check (ed->set.size() == n0 + 2, "+ inserted a frame");
    // Make frame 3 different, then sort.
    ed->formula.setText ("x", juce::dontSendNotification);
    s.click (ed->formula);
    ed->formula.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    s.pump (20);
    s.check (s.proc.undoLabel().contains ("Formula"), "formula applied via Enter");
    s.click (ed->sortBtn);
    s.check (sameItems (s.menuItems(), tableLabels (we::WaveMenu::Sort, 1)), "SORT items");
    {
        const auto first = ed->set.frames.front();
        s.choose ("Reverse Entire Table Order");
        s.check (frameDiff (first, ed->set.frames.back()) < 1.0e-6f, "Reverse moved frame 1 to the end");
    }
    // Thumbnails: click, shift-click, drag reorder.
    auto& th = ed->thumbs;
    s.click (th, th.thumbBounds (0).getCentre());
    s.check (ed->sel.current == 0, "thumb click selects");
    {
        const auto mods = juce::ModifierKeys::shiftModifier;
        juce::ignoreUnused (mods);
        s.drag (th, th.thumbBounds (2).getCentre(), th.thumbBounds (2).getCentre(), 1, true);   // shift-click
        s.check (ed->sel.count() == 3, "shift-click extends the selection");
        s.save ("wte_selection");
        const auto f0 = ed->set.frames[0];
        s.click (th, th.thumbBounds (0).getCentre());
        s.drag (th, th.thumbBounds (0).getCentre(), { th.thumbBounds (2).getRight() + 1, 20 }, 8);
        s.check (frameDiff (f0, ed->set.frames[2]) < 1.0e-6f, "drag moved frame 1 to position 3");
    }
    // COPY / PASTE.
    s.click (ed->copyBtn);
    const int nb = ed->set.size();
    s.click (th, th.thumbBounds (nb - 1).getCentre());
    s.click (ed->pasteBtn);
    s.check (ed->set.size() == nb, "paste of 1 frame at the last frame overwrites");

    s.click (ed->morphBtn);
    s.check (sameItems (s.menuItems(), tableLabels (we::WaveMenu::Morph, 1)), "MORPH items");
    s.save ("wte_menu_morph");
    s.choose ("Morph - Spectral");
    s.check (ed->set.size() == 256 && ed->set.morphed(), "spectral morph to 256 frames");
    s.check (table() != nullptr && table()->numFrames() == 256 && table()->morph() == Wavetable::Morph::Spectral, "patch table morphed");
    s.save ("wte_morphed");
    s.click (ed->morphBtn);
    s.choose ("Remove Morph Tables");
    s.check (! ed->set.morphed(), "morph removed");

    // ---- PROCESS menus ------------------------------------------------------
    s.click (ed->allBtn);
    s.check (sameItems (s.menuItems(), tableLabels (we::WaveMenu::ProcessAll, 1)), "PROCESS ALL items");
    s.save ("wte_menu_process_all");
    s.choose ("Normalize Each (Gained Separately)");
    {
        bool ok = true;
        for (auto& f : ed->set.frames) if (we::peakOf (f) > 1.0e-3f && std::abs (we::peakOf (f) - 1.0f) > 1.0e-3f) ok = false;
        s.check (ok, "Normalize Each: every frame peaks at 1");
    }
    s.click (ed->singleBtn);
    s.check (sameItems (s.menuItems(), tableLabels (we::WaveMenu::ProcessSingle, 1)), "PROCESS SINGLE items");
    {
        const auto before = ed->current();
        s.choose ("Flip Vertical (Polarity Invert)");
        s.check (std::abs (before[100] + ed->current()[100]) < 1.0e-5f, "Flip Vertical inverts");
    }
    s.click (ed->singleBtn);
    s.choose ("Send to Noise Oscillator");
    s.check (s.proc.patch().noise.sample != nullptr && s.proc.patch().noise.sample->frames() == 2048, "frame sent to the noise osc");

    // ---- Formula field + presets -------------------------------------------
    {
        const auto before = table();
        const float v0 = ed->current()[512];
        ed->formula.setText ("sin(x*pi)*0.5", juce::dontSendNotification);
        ed->formula.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
        s.pump (20);
        s.check (std::abs (ed->current()[512] - 0.5f * std::sin (-0.5f * kPi)) < 0.02f, "formula wrote the frame");
        s.check (table() != before, "formula committed");
        s.proc.undo();
        s.pump (60);
        s.check (table() != nullptr && table()->numFrames() == before->numFrames(), "undo restores the table");
        s.check (std::abs (ed->current()[512] - v0) < 1.0e-3f, "editor reloaded the old frame after undo");
        s.proc.redo();
        s.pump (60);
    }
    ed->formula.setText ("sin(x*foo", juce::dontSendNotification);
    ed->formula.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    s.pump (20);
    s.check (ed->messageIsError && ed->message.startsWith ("Error"), "formula error shown: " + ed->message);
    s.save ("wte_formula_error");
    ed->formula.setText ("F1", juce::dontSendNotification);
    ed->formula.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    s.pump (20);
    s.check (ed->fieldOverride.samplesPerCycle > 0 && ed->message.contains ("split at"), "note name sets the import frame size: " + ed->message);
    ed->formula.setText ("q<z*64", juce::dontSendNotification);
    ed->formula.keyPressed (juce::KeyPress (juce::KeyPress::returnKey));
    s.pump (20);
    s.check (ed->set.size() == 256, "multi formula generated 256 frames");
    s.click (ed->formulaBtn);
    {
        const auto items = s.menuItems();
        s.check (items.size() == 6 && items[0].contains ("Singles") && items[5].contains ("Save Formula"), "formula presets menu: " + items.joinIntoString (" | "));
        s.save ("wte_menu_formula");
        const auto& singles = we::factorySingles();
        if (! singles.empty())
        {
            const auto before = table();
            s.choose ("Singles/" + juce::String (singles.front().name));
            s.check (table() != before, "formula preset applied");
            s.check (ed->formula.getText() == juce::String (singles.front().formula), "preset written to the field");
        }
    }

    // ---- IMPORT / EXPORT ------------------------------------------------------
    s.click (ed->importBtn);
    {
        juce::StringArray want;
        for (auto& i : we::importMenu()) want.add (juce::CharPointer_UTF8 (i.label));
        s.check (sameItems (s.menuItems(), want), "IMPORT items");
        s.save ("wte_menu_import");
        s.closeMenu();
    }
    s.click (ed->exportBtn);
    {
        juce::StringArray want;
        for (auto& i : we::exportMenu()) want.add (juce::CharPointer_UTF8 (i.label));
        s.check (sameItems (s.menuItems(), want), "EXPORT items");
        s.closeMenu();
    }
    auto tmp = s.dir.getChildFile ("wte_io");
    tmp.deleteRecursively();
    tmp.createDirectory();
    {
        // A 110.25 Hz saw-ish tone, 1 s.
        std::vector<float> x (44100);
        for (size_t i = 0; i < x.size(); ++i) { const float ph = std::fmod ((float) i / 400.0f, 1.0f); x[i] = 0.6f * (2.0f * ph - 1.0f); }
        const auto wav = tincture::ui::makeWav (x, 16, false);
        const auto in = tmp.getChildFile ("tone.wav");
        in.replaceWithData (wav.getData(), wav.getSize());
        ed->formula.clear();
        ed->applyFormulaText ({});
        s.check (ed->importFile (in, we::ImportMethod::FixedFrameSize, false), "import audio");
        s.check (ed->set.size() > 1 && table()->name == "tone", "imported frames: " + juce::String (ed->set.size()));
        s.check (ed->importFile (in, we::ImportMethod::Fft1024, false), "FFT import");
        const int n = ed->set.size();
        s.check (ed->importFile (in, we::ImportMethod::FixedFrameSize, true) && ed->set.size() == n, "single-cycle import replaces the selection");
        s.save ("wte_imported");

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        for (int id = 0; id < 5; ++id)
        {
            const auto f = id == 3 ? tmp.getChildFile ("cycles") : tmp.getChildFile ("out" + juce::String (id) + (id == 0 ? ".256" : ".wav"));
            s.check (ed->exportTo (f, id), "export " + juce::String (id));
            if (id == 1 || id == 2 || id == 4)
            {
                std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
                const int want = (id == 4 ? ed->sel.count() : ed->set.size()) * 2048;
                s.check (r != nullptr && (int) r->lengthInSamples == want, "export " + juce::String (id) + " length");
            }
        }
        s.check (tmp.getChildFile ("cycles").getNumberOfChildFiles (juce::File::findFiles) == ed->set.size(), "single-cycle export files");
        s.check (tmp.getChildFile ("out0.256").getSize() == ed->set.size() * 256, ".256 size");
        // Exported selection carries the clm chunk and re-imports at 2048 per frame.
        juce::MemoryBlock mb;
        tmp.getChildFile ("out4.wav").loadFileAsData (mb);
        s.check (mb.getSize() > 16 && std::memcmp (static_cast<const char*> (mb.getData()) + 12, "clm ", 4) == 0, "clm chunk written");
    }

    // ---- Name menu ------------------------------------------------------------
    s.click (ed->nameBox);
    {
        const auto items = s.menuItems();
        s.check (items.contains ("Analog") && items.contains ("Save Table...") && items.contains ("---"), "name menu: " + items.joinIntoString (" | "));
        s.save ("wte_menu_name");
        const auto names = FactoryTables::names ("Analog");
        if (! names.empty())
        {
            s.choose ("Analog/" + juce::String (names.front()));
            s.check (table() != nullptr && table()->factory && table()->name == names.front(), "factory table loaded");
            s.check (ed->set.name == names.front() && ! ed->set.modified, "editor shows the factory table");
        }
    }
    s.click (ed->nextTable);
    s.check (table() != nullptr && table()->factory, "> steps factory tables");
    s.save ("wte_factory");

    // Close.
    s.click (ed->closeBtn);
    s.check (closed, "X closes");
    juce::ignoreUnused (t0);
    s.panel.removeChildComponent (comp.get());
    comp.reset();
    s.pump (20);
}

TINCTURE_SHOT ("wte", wteShot);
