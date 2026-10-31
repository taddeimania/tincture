// WavetableEditor.h — the Wavetable Editor overlay (spec E §3–5), shown over
// the OSC page module row (kPageW x kModuleRowH). Declared here so the shot
// tests (Tools/ShotsWte.cpp) can reach its parts; the panel only uses
// createWavetableEditor() from Pages.h.
//
// Editing works on a local wtedit::FrameSet. Every committed operation (the
// mouse-up after a brush / FFT stroke, a menu op, a formula, an import)
// builds a new immutable Wavetable and sets it on the patch through
// proc.editPatch with an undo label, so the sound follows and undo works.
// When the patch's table changes from outside (undo, preset load, the OSC
// page's table menu) the editor reloads its frames from it.
#pragma once

#include "Pages.h"
#include "../Engine/WaveEdit.h"
#include "../Engine/WaveImport.h"
#include "../Engine/Formula.h"
#include "../Engine/Wavetable.h"

namespace tincture::ui
{
class WavetableEditor;

namespace wte
{
// A small LCD stepper: [<] value [>]; click the value for a menu of choices.
class Stepper final : public juce::Component, public juce::SettableTooltipClient
{
public:
    juce::String caption;
    std::vector<int> values;
    std::function<juce::String (int)> format;
    std::function<int()> get;
    std::function<void (int)> set;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void step (int dir);
    juce::Rectangle<int> valueArea() const;
};

// A brush tool button drawing its shape as a glyph.
class BrushButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    BrushButton (WavetableEditor& e, int brushIndex);   // -1 = Mirror
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    const int index;
private:
    WavetableEditor& ed;
};

// A PanelButton with a small dropdown triangle.
class DropButton final : public PanelButton
{
public:
    using PanelButton::PanelButton;
    void paint (juce::Graphics&) override;
};

class NameDisplay final : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit NameDisplay (WavetableEditor& e) : ed (e) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
private:
    WavetableEditor& ed;
};

class WaveView final : public juce::Component
{
public:
    explicit WaveView (WavetableEditor& e) : ed (e) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
private:
    WavetableEditor& ed;
    juce::Point<float> last;
    bool stroking = false, changed = false;
    Rng rng { 0x2545f491u };
    Rng& rngFor() { return rng; }
    void strokeTo (juce::Point<float> p, bool first);
    float toX01 (float px) const;
    float toY (float py) const;      // -1..1
};

class FftView final : public juce::Component
{
public:
    explicit FftView (WavetableEditor& e) : ed (e) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

    juce::Rectangle<float> magArea() const;
    juce::Rectangle<float> phaseArea() const;
    juce::Rectangle<float> scrollArea() const;
    int binAt (float x) const;                     // 1..512
    float binX (int bin) const;                    // left edge
private:
    WavetableEditor& ed;
    enum class Drag { None, Mag, Phase, Scroll } drag = Drag::None;
    int lastBin = -1, hoverBin = -1;
    float lastBar = 0.0f;
    bool changed = false;
    wtedit::Spectrum work;
    void paintTo (const juce::MouseEvent&, bool first);
};

class ThumbStrip final : public juce::Component, public juce::FileDragAndDropTarget
{
public:
    explicit ThumbStrip (WavetableEditor& e) : ed (e) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragMove (const juce::StringArray&, int x, int y) override;
    void fileDragExit (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int x, int y) override;

    static constexpr int kThumbW = 46, kGap = 3;
    int count() const;                              // thumbnails shown (key frames when morphed)
    int frameOf (int thumb) const;                  // thumbnail -> frame index
    int thumbOf (int frame) const;                  // frame -> thumbnail (nearest key at or before)
    juce::Rectangle<int> thumbBounds (int thumb) const;
    juce::Rectangle<int> plusBounds() const;
    int thumbAt (juce::Point<int>) const;           // -1 none
    void ensureVisible (int thumb);
private:
    WavetableEditor& ed;
    int scroll = 0;
    int pressThumb = -1, insertAt = -1, dropThumb = -1;
    bool dragging = false;
    int maxScroll() const;
};
} // namespace wte

class WavetableEditor final : public juce::Component, public Refreshable
{
public:
    WavetableEditor (PanelContext&, int osc, std::function<void()> onClose);
    ~WavetableEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void refresh() override;

    // ---- state (tests read these) -------------------------------------
    PanelContext& ctx;
    const int osc;
    wtedit::FrameSet set;
    wtedit::Selection sel;
    wtedit::FftDrawOptions fftOpts;
    wtedit::Brush brush = wtedit::Brush::FlatLine;
    bool mirror = false;
    int gridX = 8, gridY = 8, zoom = 1, fftFirst = 0;   // fftFirst: first visible bin - 1
    wtedit::MorphMode morphMode = wtedit::MorphMode::Crossfade;
    wtedit::ImportOverride fieldOverride;
    juce::String message;                          // formula / import feedback under the field
    bool messageIsError = false;
    uint32_t version = 1;                          // bumps on every frame change (caches)

    wtedit::Frame& current() { sel.clamp (set.size()); return set.frames[(size_t) sel.current]; }
    const wtedit::Spectrum& spectrum();            // of the current frame, cached
    void setSpectrum (const wtedit::Spectrum& s) { spec = s; specVersion = version; }
    void framesChanged();                          // bump version, repaint views
    void selectFrame (int frame, bool extend);
    int visibleBins() const { return wtedit::kFftBins / zoom; }

    // ---- operations ---------------------------------------------------
    void commit (const juce::String& undoLabel);   // frames -> new Wavetable -> patch
    void reloadFromPatch();
    void runWaveOp (wtedit::WaveMenu m, int index, int bin);
    Menu buildWaveMenu (wtedit::WaveMenu m, int bin);
    void applyFormulaText (const juce::String& text);
    bool importFile (const juce::File&, wtedit::ImportMethod, bool singleCycle);
    bool importImageFile (const juce::File&);
    bool replaceFrameFromFile (int frame, const juce::File&);
    bool exportTo (const juce::File& target, int exportId);
    bool saveTableTo (const juce::File&);
    void loadFactory (const juce::String& category, const juce::String& name);
    void stepTable (int dir);

    // ---- menus (built fresh on open) ----------------------------------
    Menu nameMenu();
    Menu importMenu();
    Menu exportMenu();
    Menu formulaMenu();
    void showFormulaSave();

    juce::String oscName() const { return juce::String ("OSC ") + juce::String::charToString ((juce::juce_wchar) ('A' + osc)); }
    juce::Colour accent() const { return colours::brand; }

    // ---- children -----------------------------------------------------
    wte::NameDisplay nameBox { *this };
    PanelButton prevTable { "<" }, nextTable { ">" }, closeBtn { "X" };
    wte::DropButton importBtn { "IMPORT" }, exportBtn { "EXPORT" }, singleBtn { "SINGLE" }, allBtn { "ALL" },
                    morphBtn { "MORPH" }, removeBtn { "REMOVE..." }, sortBtn { "SORT..." }, formulaBtn { "" };
    PanelButton shiftL { "<" }, shiftR { ">" }, shiftU { "^" }, shiftD { "v" };
    PanelButton posMinus { "-" }, posPlus { "+" }, copyBtn { "COPY" }, pasteBtn { "PASTE" };
    wte::Stepper zoomStep, gridXStep, gridYStep;
    std::vector<std::unique_ptr<wte::BrushButton>> brushButtons;   // 12 tools + Mirror (last)
    wte::FftView fftView { *this };
    wte::WaveView waveView { *this };
    wte::ThumbStrip thumbs { *this };
    juce::TextEditor formula;

private:
    std::function<void()> onClose;
    std::shared_ptr<const Wavetable> shown;        // the table our frames came from / went to
    wtedit::Spectrum spec;
    uint32_t specVersion = 0, seed = 0x51ed27u;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::Rectangle<int> posArea, msgArea;

    void layoutBrushes();
    void chooseFile (bool save, bool directory, const juce::String& title, const juce::File& start,
                     const juce::String& patterns, std::function<void (const juce::File&)> then);
    void setMessage (const juce::String& m, bool error) { message = m; messageIsError = error; repaint (msgArea); }
    void sendToNoise (const std::vector<float>& cycle);
    static juce::File formulaFile (bool user, bool multi);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WavetableEditor)
};

// Frames of a table as the editor sees them (logical frames; key frames when morphed).
wtedit::FrameSet frameSetFromTable (const Wavetable* t);
// A mono WAV (44.1 kHz) in memory; bits 16 = PCM, 32 = float. clm: add a Serum-style
// "clm " chunk (2048 samples per frame).
juce::MemoryBlock makeWav (const std::vector<float>& samples, int bits, bool clm);
} // namespace tincture::ui
