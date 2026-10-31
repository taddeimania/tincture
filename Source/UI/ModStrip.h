// ModStrip.h — the OSC page's modulation strip (spec D §0-§8): MACROS, ENV 1-4,
// LFO 1-10 (+ the pop-out LFO editor), VELO / NOTE, VOICING and PORTAMENTO.
// The classes are declared here so the shot (Tools/ShotsMod.cpp) can reach
// the real components; the panel itself only uses createModStrip().
#pragma once

#include "CurveEditor.h"
#include "Pages.h"
#include "../PluginProcessor.h"

#include <deque>

namespace tincture::ui
{
// ---------------------------------------------------------------------------
// Helpers shared by the strip's parts.
// ---------------------------------------------------------------------------
juce::String beatsText (float beats);                       // "1/4", "1/8 D", "1/16 T", "2 bars", "0.37 bt"
juce::String sourceTooltip (TinctureProcessor&, ModSource); // "LFO 2 Source / ---- / Destinations: ..."
Menu sourceTabMenu (PanelContext&, ModSource);              // §1.6: Bypass: X ..., Bypass All Destinations, ---, Remove All Destinations

// A PanelButton with a drawn icon instead of (or beside) text.
class IconButton : public PanelButton
{
public:
    explicit IconButton (const juce::String& text = {}) : PanelButton (text) {}
    std::function<void (juce::Graphics&, juce::Rectangle<float>, juce::Colour)> icon;
    void paint (juce::Graphics&) override;
};

// A mod source tile: ENV n, LFO n, VELO, NOTE or a macro number. Drag it onto
// a control (ModSourceTab does the drag); shows its destination count; the
// right-click menu is §1.6 (plus extras); it can also take tile drops (copy
// LFO with Alt, swap macros).
class SourceTile : public ModSourceTab, public Refreshable, public juce::DragAndDropTarget
{
public:
    SourceTile (PanelContext&, ModSource, const juce::String& label);
    ~SourceTile() override;

    std::function<void (Menu&)> menuExtras;                  // items above the §1.6 items
    std::function<void (ModSource dropped, bool alt)> onTileDrop;
    bool numberTile = false;                                 // a macro's small number tile
    juce::String label;

    int destinations() const noexcept { return count; }
    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override { hover = true; repaint(); }
    void mouseExit (const juce::MouseEvent&) override { hover = false; repaint(); }

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails&) override;
    Menu buildMenu();

private:
    PanelContext& c;
    int count = -1;
    bool hover = false, dropHover = false;
};

// ---------------------------------------------------------------------------
// MACROS
// ---------------------------------------------------------------------------
juce::String macroName (const Patch&, int macro);           // the name, or "MACRO n"

class MacroPanel : public juce::Component, public Refreshable
{
public:
    explicit MacroPanel (PanelContext&);
    ~MacroPanel() override;
    void resized() override;
    void refresh() override;
    void rename (int macro);                                  // "Rename..." (knob and tile menus)
    std::unique_ptr<ParamKnob> knobs[kNumMacros];
    std::unique_ptr<SourceTile> tiles[kNumMacros];
private:
    PanelContext& ctx;
};

// ---------------------------------------------------------------------------
// ENV 1-4
// ---------------------------------------------------------------------------
class EnvGraph : public juce::Component, public Refreshable
{
public:
    EnvGraph (PanelContext&);
    ~EnvGraph() override;
    int env = 0;
    bool locked = true;
    float zoomSeconds = 4.0f;          // unlocked view length
    bool gridBeats = false;

    EnvSettings settingsOf (int e) const;
    float viewLength() const;
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (5.0f, 4.0f).withTrimmedBottom (12.0f); }
    juce::Point<float> toScreen (float t, float level) const;
    // Main points (start, peak, hold end, decay end, release end) and handles.
    std::array<juce::Point<float>, 5> points() const;
    juce::Point<float> handle (int seg) const;   // 0 attack, 1 decay, 2 release
    Menu graphMenu();

    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    PanelContext& ctx;
    int dragPoint = -1, dragHandle = -1;
    float dragView = 1.0f, startValue = 0.0f;
    std::vector<int> gestureParams;
    juce::String shownKey;
    float shownLive = -1.0f;
    void beginParams (std::initializer_list<int>);
    float toSeconds (float plain) const;      // param value -> seconds (BPM mode: beats)
    float fromSeconds (float s) const;
};

class EnvZoomStrip : public juce::Component
{
public:
    explicit EnvZoomStrip (EnvGraph& g) : graph (g) {}
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    juce::Rectangle<float> lockBox() const, upBox() const, zoomBox() const, downBox() const;
private:
    EnvGraph& graph;
    float startZoom = 1.0f;
    bool zoomDrag = false;
};

class EnvPanel : public juce::Component, public Refreshable
{
public:
    EnvPanel (PanelContext&);
    ~EnvPanel() override;
    void select (int e);
    int selected() const noexcept { return sel; }
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh() override;
    void setBpm (bool on);              // converts the times (MS -> BPM snaps to note values)

    std::unique_ptr<SourceTile> tabs[kNumEnvs];
    EnvGraph graph;
    EnvZoomStrip zoom { graph };
    std::unique_ptr<ParamField> values[5];
    std::unique_ptr<ParamKnob> knobs[5];
    PanelButton bpmBtn { "BPM" }, msBtn { "MS" };
    std::unique_ptr<ParamToggle> legInv;

private:
    PanelContext& ctx;
    int sel = 0;
    bool shownBpm = false;
    void applyFormats();
};

// ---------------------------------------------------------------------------
// LFO 1-10
// ---------------------------------------------------------------------------
// Binds a CurveEditor to LFO `lfo` (data, point menu, drops, loopback) and
// refreshes its live display; used by the panel graph and the pop-out editor.
void bindLfoGraph (CurveEditor&, PanelContext&, const int* lfo);
void refreshLfoGraph (CurveEditor&, PanelContext&, int lfo, std::deque<float>* history);
Menu lfoShapeMenu (PanelContext&, int lfo, juce::Component* saveAnchor);
void stepLfoShape (PanelContext&, int lfo, int delta);
void lfoRateExtras (PanelContext&, int lfo, Menu&);         // Swing (BPM) / 10x (HZ)

class LfoGridField;   // ≡ / ||| steppers

class LfoPanel : public juce::Component, public Refreshable
{
public:
    LfoPanel (PanelContext&, OscPageHost&);
    ~LfoPanel() override;
    void select (int l);
    int selected() const noexcept { return sel; }
    int visibleTabs() const;
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh() override;
    void openEditor();
    void setTool (CurveEditor::Tool);

    std::unique_ptr<SourceTile> tabs[kNumLfos];
    std::unique_ptr<ParamChoice> type, direction;
    PanelButton modeBtn[3] { PanelButton ("FREE"), PanelButton ("RETRIG"), PanelButton ("ENVELOPE") };
    std::unique_ptr<ParamToggle> mono, host, trip, dot;
    CurveEditor graph;
    IconButton popout, toolBtn[4];
    ChoiceDisplay shape;
    PanelButton shapePrev { "<" }, shapeNext { ">" };
    std::unique_ptr<ParamField> gridY, gridX, rateField;
    PanelButton bpmBtn { "BPM" }, hzBtn { "HZ" };
    std::unique_ptr<ParamKnob> rate, rise, delay, smooth, phase;

private:
    PanelContext& ctx;
    OscPageHost& host_;
    int sel = 0;
    int shownTabs = -1;
    bool shownBpm = true;
    std::deque<float> history;
    void rebind();
};

// The large pop-out LFO editor (§2.7), shown over the module row and the strip.
class LfoEditorOverlay : public juce::Component, public Refreshable
{
public:
    LfoEditorOverlay (PanelContext&, int lfo, std::function<void()> onClose);
    ~LfoEditorOverlay() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh() override;
    void setTool (CurveEditor::Tool);

    int lfo;
    CurveEditor graph;
    IconButton close;
    ChoiceDisplay shape;
    PanelButton shapePrev { "<" }, shapeNext { ">" };
    IconButton toolBtn[4];
    std::unique_ptr<ParamChoice> direction;
    std::unique_ptr<ParamField> gridY, gridX;

private:
    PanelContext& ctx;
    std::deque<float> history;
};

// ---------------------------------------------------------------------------
// VELO / NOTE
// ---------------------------------------------------------------------------
class VeloNotePanel : public juce::Component, public Refreshable
{
public:
    explicit VeloNotePanel (PanelContext&);
    ~VeloNotePanel() override;
    void select (int which);              // 0 VELO, 1 NOTE
    int selected() const noexcept { return sel; }
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh() override;

    std::unique_ptr<SourceTile> tabs[2];
    CurveEditor graph;
private:
    PanelContext& ctx;
    int sel = 0;
};

// ---------------------------------------------------------------------------
// VOICING and PORTAMENTO
// ---------------------------------------------------------------------------
class PolyField : public ParamField
{
public:
    PolyField (PanelContext& c, int p) : ParamField (c, p, "POLY"), ctx2 (c) {}
    void mouseDown (const juce::MouseEvent&) override;
    Menu polyMenu();
private:
    PanelContext& ctx2;
};

class PortaCurve : public juce::Component, public Bound, public juce::SettableTooltipClient
{
public:
    PortaCurve (PanelContext&, int param);
    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
private:
    float startNorm = 0.0f;
    bool dragging = false;
};

class VoicingPanel : public juce::Component, public Refreshable
{
public:
    explicit VoicingPanel (PanelContext&);
    ~VoicingPanel() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void refresh() override;
    juce::String counterText() const;

    std::unique_ptr<ParamToggle> mono, legato, always, scaled;
    std::unique_ptr<PolyField> poly;
    IconButton polyUp, polyDown;
    std::unique_ptr<ParamKnob> porta;
    std::unique_ptr<PortaCurve> curve;
    juce::Rectangle<int> counterBox, portaBox;
private:
    PanelContext& ctx;
    juce::String shownCounter;
};

// ---------------------------------------------------------------------------
class ModStrip final : public juce::Component
{
public:
    ModStrip (PanelContext&, OscPageHost&);
    void paint (juce::Graphics&) override;
    void resized() override;

    MacroPanel macros;
    EnvPanel envs;
    LfoPanel lfos;
    VeloNotePanel veloNote;
    VoicingPanel voicing;

    // Section frames (strip coordinates).
    static juce::Rectangle<int> macroArea()   { return { 6, 12, 150, 222 }; }
    static juce::Rectangle<int> envArea()     { return { 162, 12, 352, 222 }; }
    static juce::Rectangle<int> lfoArea()     { return { 520, 12, 548, 222 }; }
    static juce::Rectangle<int> veloArea()    { return { 1074, 12, 132, 222 }; }
    static juce::Rectangle<int> voicingArea() { return { 1212, 12, 182, 222 }; }
};
} // namespace tincture::ui
