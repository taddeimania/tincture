// OscModules.h — the OSC page's module row: SUB, OSC A/B/C, NOISE, FILTER 1/2.
//
// createModuleRow() builds the whole row (1400 x kModuleRowH). The pieces
// below are shared between the OscModules*.cpp files:
//   OscModules.cpp        the row, module identity, the module (label) menu
//   OscModulesCommon.cpp  labels, routing button + popover, popovers, pitch
//                         row, warp section + menu, remap editor, unison pane
//   OscModulesOsc.cpp     OSC A/B/C: header, wavetable body, sample-mode bodies
//   OscModulesSample.cpp  sample/spectral/multisample displays and their menus
//   OscModulesOther.cpp   SUB, NOISE, FILTER panels
#pragma once

#include "Pages.h"
#include "../PluginProcessor.h"

namespace tincture::ui
{
std::unique_ptr<juce::Component> createModuleRow (PanelContext&, OscPageHost&);

namespace oscui
{
inline juce::Colour filterAccent() { return juce::Colour (0xff7fb8ff); }
inline juce::Colour loopBlue()     { return juce::Colour (0xff5cc8f2); }

// ---------------------------------------------------------------------------
// Which module a control belongs to.
// ---------------------------------------------------------------------------
enum class Kind { Osc, Sub, Noise, Filter };

struct ModuleRef
{
    Kind kind = Kind::Osc;
    int index = 0;                          // osc 0..2, filter 0..1

    juce::String group() const;             // ParamInfo::group: "OSC A", "SUB", "NOISE", "FILTER 1"
    juce::String title() const;             // header label: "OSC A", "SUB", "NOISE", "FILTER 1"
    juce::String letter() const;            // "A", "Sub", "Noise", "Filter 1" (menu titles)
    int enableParam() const;
    int routeParam() const;
    int balanceParam() const;               // -1 for filters
    int bus1Param() const;
    int bus2Param() const;
    ModSource source() const;
    juce::Colour accent() const;
    bool isOsc() const noexcept { return kind == Kind::Osc; }
};

// The module (label) right-click menu, spec A 2.5.
Menu moduleMenu (PanelContext&, ModuleRef);

// Short routing code shown on a module's routing button: F1 F2 F1/2 M D -.
juce::String routeCode (const TinctureProcessor&, ModuleRef);

// ---------------------------------------------------------------------------
// A small square button with a drawn icon.
// ---------------------------------------------------------------------------
class IconButton : public PanelButton
{
public:
    enum Icon { Gear, Pencil, Close, Window, Envelope, Keys, XFade, Arrows, ArrowDown, ArrowUp, Undo, Redo, Left, Right };
    explicit IconButton (Icon i) : icon (i) {}
    Icon icon;
    juce::Colour iconColour = colours::ink;
    void paint (juce::Graphics&) override;
};
void drawIcon (juce::Graphics&, IconButton::Icon, juce::Rectangle<float>, juce::Colour);

// ---------------------------------------------------------------------------
// The power LED + module name at the left of a header. Oscillators (and SUB /
// NOISE): click anywhere toggles the module. Filters: only the LED does.
// Right-click: module menu. Drag: the module as a modulation source.
// ---------------------------------------------------------------------------
class ModuleLabel : public juce::Component, public juce::SettableTooltipClient, public Refreshable
{
public:
    ModuleLabel (PanelContext&, ModuleRef);
    ~ModuleLabel() override;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    bool ledOnly = false;                   // filters
    juce::Rectangle<int> ledArea() const { return { 0, 0, 18, getHeight() }; }

private:
    PanelContext& ctx;
    ModuleRef ref;
    bool lit = false, dragStarted = false, downOnLed = false;
};

// ---------------------------------------------------------------------------
// Routing: the header button and the popover it opens.
// ---------------------------------------------------------------------------
class RouteButton : public PanelButton, public Refreshable
{
public:
    RouteButton (PanelContext&, ModuleRef);
    ~RouteButton() override;
    void refresh() override;
    std::function<void()> onOpen;
private:
    PanelContext& ctx;
    ModuleRef ref;
};

// A pane drawn over part of a module (unison settings, routing, warp remap,
// grain window...). Black LCD-style box, title at the top left, X at the top right.
class Popover : public juce::Component
{
public:
    explicit Popover (juce::String title);
    std::function<void()> onClose;          // set by the owner
    void paint (juce::Graphics&) override;
    void resized() override;
    virtual void layout (juce::Rectangle<int>) {}
    juce::String title;
protected:
    IconButton closeBtn { IconButton::Close };
};

std::unique_ptr<Popover> createRoutingPopover (PanelContext&, ModuleRef);

// ---------------------------------------------------------------------------
// A module panel: outlined box, header label + routing button, dimmed when
// the module is off, and one popover at a time over part of it.
// ---------------------------------------------------------------------------
class ModulePanel : public juce::Component, public Refreshable
{
public:
    ModulePanel (PanelContext&, ModuleRef);
    ~ModulePanel() override;
    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void refresh() override;                // dimming; derived classes call it

    // Toggle a popover named `id` over `area` (a second call with the same id closes it).
    void togglePopover (const juce::String& id, juce::Rectangle<int> area, std::function<std::unique_ptr<Popover>()> make);
    void closePopover();
    juce::String popoverId() const { return popId; }
    Popover* popover() const { return pop.get(); }

    PanelContext& ctx;
    ModuleRef ref;
    ModuleLabel label;
    RouteButton route;

protected:
    std::unique_ptr<Popover> pop;
    juce::String popId;
    bool dimmed = false;
};

// ---------------------------------------------------------------------------
// OCT / SEM / FIN / CRS (OCT/SEM: Mode > Semitones / Harmonics / Ratio).
// ---------------------------------------------------------------------------
class PitchRow : public juce::Component, public Refreshable
{
public:
    PitchRow (PanelContext&, int osc);
    ~PitchRow() override;
    void resized() override;
    void refresh() override;
private:
    PanelContext& ctx;
    int osc;
    int shownMode = -1;
    std::unique_ptr<ParamField> oct, sem, fin, crs;
};

// ---------------------------------------------------------------------------
// Warp: the two slots (knob, mode field with < >), WARP VAR fader for Sync,
// remap pencils for Remap 1/2/4.
// ---------------------------------------------------------------------------
Menu warpMenu (PanelContext&, int osc, int slot);
juce::String warpModeShort (int mode);      // field text ("OFF", "SYNC", "FM (B)"...)

class WarpModeField : public juce::Component, public Refreshable, public juce::SettableTooltipClient
{
public:
    WarpModeField (PanelContext&, int osc, int slot);
    ~WarpModeField() override;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void step (int delta);
    PanelContext& ctx;
    int osc, slot;
private:
    IconButton prev { IconButton::Left }, next { IconButton::Right };
    int shown = -1;
};

class WarpSection : public juce::Component, public Refreshable
{
public:
    WarpSection (PanelContext&, int osc, bool compact = false);
    ~WarpSection() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh() override;
    std::function<void (int slot)> onEditRemap;
private:
    PanelContext& ctx;
    int osc;
    bool compact;
    std::unique_ptr<ParamKnob> k1, k2;
    std::unique_ptr<WarpModeField> m1, m2;
    std::unique_ptr<ParamFader> var;
    IconButton pen1 { IconButton::Pencil }, pen2 { IconButton::Pencil };
    int shownModes = -1;
};

std::unique_ptr<Popover> createRemapEditor (PanelContext&, int osc, int slot);
// type: choices::oscTypes() index.
std::unique_ptr<Popover> createUnisonPopover (PanelContext&, int osc, int type);
std::unique_ptr<Popover> createWindowPopover (PanelContext&, int osc);

// An LCD dropdown showing a value from patch data (not a parameter).
class PatchChoice : public ChoiceDisplay, public Refreshable
{
public:
    PatchChoice (PanelContext&);
    ~PatchChoice() override;
    std::function<Menu()> buildMenu;
    std::function<void (int delta)> onStep;
    void refresh() override { ChoiceDisplay::refresh(); }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
private:
    PanelContext& ctx;
};

// An LCD number on patch data you drag (VEL TRACK, φ RAND in Multisample).
class PatchField : public juce::Component, public Refreshable, public juce::SettableTooltipClient
{
public:
    PatchField (PanelContext&, juce::String caption);
    ~PatchField() override;
    std::function<float()> get;             // 0..1
    std::function<void (float, bool commit)> set;
    std::function<juce::String (float)> format;
    juce::String caption;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
private:
    PanelContext& ctx;
    float startV = 0.0f, shown = -1.0f;
    bool dragging = false;
};

// ---------------------------------------------------------------------------
// Sample-mode pieces (OscModulesSample.cpp).
// ---------------------------------------------------------------------------
// The sample / instrument browser menu for an oscillator in a sample mode.
Menu sampleBrowserMenu (PanelContext&, int osc, int type, juce::Component& anchor);
void stepSample (PanelContext&, int osc, int delta);
juce::String sampleName (const TinctureProcessor&, int osc, int type);
// Right-click operations menu of the sample / granular / spectral display.
Menu sampleOperationsMenu (PanelContext&, int osc, int type);
Menu loopMenu (PanelContext&, int osc, int type);
juce::String loopFace (const TinctureProcessor&, int osc);
// The extras of the SCAN / DENS / LENGTH / DIR knob menus.
void addScanItems (Menu&, PanelContext&, int osc, int type);
void addDensityItems (Menu&, PanelContext&, int osc);
void addLengthItems (Menu&, PanelContext&, int osc);
void addDirItems (Menu&, PanelContext&, int osc);
// Loads a file into an oscillator for its current type (wavetable: import).
bool loadFileInto (TinctureProcessor&, int osc, const juce::File&);
bool isAudioFile (const juce::String& path);

// Waveform / spectrogram with markers, loop, slices, playheads, X|Y dot.
class SampleDisplay : public juce::Component, public Refreshable, public juce::FileDragAndDropTarget,
                      public juce::SettableTooltipClient
{
public:
    SampleDisplay (PanelContext&, int osc, int type);
    ~SampleDisplay() override;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;

    // View range (fractions of the sample).
    float viewStart = 0.0f, viewEnd = 1.0f;
    juce::Rectangle<float> waveArea() const;
    float xOf (float frac) const;
    float fracAt (float x) const;

    enum class Grab { None, Start, End, LoopStart, LoopEnd, LoopBar, Threshold, Slice, HiFreq, LoFreq, View };
    Grab hitTestGrab (juce::Point<float>) const;

private:
    void rebuildImage();
    PanelContext& ctx;
    int osc, type;
    juce::Image image;                       // waveform or spectrogram of the view
    const void* imageKey = nullptr;
    float imageV0 = -1.0f, imageV1 = -1.0f;
    juce::Rectangle<int> imageSize;
    Grab grab = Grab::None, hover = Grab::None;
    int grabSlice = -1, hoverSlice = -1;
    float grabFrac0 = 0.0f, grabA = 0.0f, grabB = 0.0f, view0 = 0.0f, view1 = 1.0f;
    bool hovering = false;
    juce::uint32 stateKey = 0;
    std::vector<float> shownPlayheads;
};

// Multisample zone map (click: waveform of the last zone).
class ZoneMapDisplay : public juce::Component, public Refreshable, public juce::FileDragAndDropTarget
{
public:
    ZoneMapDisplay (PanelContext&, int osc);
    ~ZoneMapDisplay() override;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void filesDropped (const juce::StringArray&, int, int) override;
    bool showWave = false;
private:
    PanelContext& ctx;
    int osc;
    int shownZone = -2, lastZone = -1;
    const void* shownMulti = nullptr;
};

// The loop crossfade button: drag up to set the amount.
class XFadeButton : public juce::Component, public Refreshable, public juce::SettableTooltipClient
{
public:
    XFadeButton (PanelContext&, int osc);
    ~XFadeButton() override;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
private:
    PanelContext& ctx;
    int osc;
    float startV = 0.0f, shown = -1.0f;
    bool dragging = false;
};

// Panel factories.
std::unique_ptr<juce::Component> createOscPanel (PanelContext&, OscPageHost&, int osc);
std::unique_ptr<juce::Component> createSubPanel (PanelContext&);
std::unique_ptr<juce::Component> createNoisePanel (PanelContext&);
std::unique_ptr<juce::Component> createFilterPanel (PanelContext&, int filter);

// A self-contained SFZ reader (regions with sample/lokey/hikey/key/
// pitch_keycenter/lovel/hivel/tune/volume/loop_mode, <group> defaults).
std::shared_ptr<const MultiSample> loadSfz (const juce::File&);
} // namespace oscui
} // namespace tincture::ui
