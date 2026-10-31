// FxModules.h — the FX module editors (one panel per module in the FX rack
// view) and the FX edit operations shared by the FX and MIX pages (add,
// remove, duplicate, move, bypass, module/add menus, rack and module presets).
// Also a small breakpoint-curve editor shared with the MATRIX page (CRV
// curves) and the Distortion X-Shaper.
//
// A module's controls are the slots of its FX unit (ids().fx[u].p[k], all
// stored 0..1) plus the unit's MIX; FxTypes.h says what each slot means.
#pragma once

#include "Pages.h"
#include "../PluginProcessor.h"
#include "../Engine/Fx/FxEngine.h"
#include "../Model/Curve.h"

#include <array>
#include <map>

namespace tincture::ui::fxui
{
// ---------------------------------------------------------------------------
// Data helpers
// ---------------------------------------------------------------------------
juce::Colour typeColour (FxType t);
// Visible in the panel (every parent visible); works without a desktop window.
inline bool panelVisible (const juce::Component& c)
{
    for (auto* p = &c; p != nullptr; p = p->getParentComponent())
    {
        if (dynamic_cast<const PanelContext*> (p) != nullptr) return true;   // the panel: its host decides
        if (! p->isVisible()) return false;
    }
    return true;
}
std::array<float, kFxParams> unitValues (const TinctureProcessor&, int unit);
void writeUnit (ParamValues& v, int unit, const std::array<float, kFxParams>& p, float mix);
// Defaults for a new module: the user's "Save as Default Preset" if any, else the factory defaults.
std::array<float, kFxParams> defaultsFor (FxType t, float& mixOut);
// A string that changes whenever anything shown about the racks changes
// (modules, order, units, bypass, preset names, locks). rack -1 = all.
juce::String layoutSignature (const FxLayout&, int rack = -1);
juce::String listName (const FxList&);         // "MAIN", "BUS 1 > LOWS" ...
// The modulation destination name for a parameter, resolving FX unit slots
// to their module ("REVERB Size"); other parameters keep their own name.
juce::String paramDisplayName (const TinctureProcessor&, int param);

// Folders (created on demand) under Settings::userBanks().
juce::File rackPresetFolder();
juce::File fxPresetFolder (FxType);
juce::File fxDefaultFile (FxType);

// ---------------------------------------------------------------------------
// Edit operations (each one undo step)
// ---------------------------------------------------------------------------
int addModule (TinctureProcessor&, const FxList& where, FxType t, int at = -1);   // unit or -1
void removeModule (TinctureProcessor&, const FxPath&);
bool duplicateModule (TinctureProcessor&, const FxPath& from, const FxList& to, int at);
bool moveModule (TinctureProcessor&, const FxPath& from, const FxList& to, int at);
// Plain click: toggle this module. allInRack (Alt-click): every module of the rack follows.
void toggleBypass (TinctureProcessor&, const FxPath&, bool allInRack);
void toggleBandBypass (TinctureProcessor&, int rack, int splitter, int band);
void loadRackPreset (TinctureProcessor&, int rack, const Node& n, const juce::String& name);
Node saveRackNode (const TinctureProcessor&, int rack, const juce::String& name);
bool writeNode (const juce::File&, const Node&);
bool readNode (const juce::File&, Node&);
void applyFxPreset (TinctureProcessor&, int unit, const Node&, const juce::String& label);
Node fxPresetNode (const TinctureProcessor&, FxType, int unit);

// The "+ FX" list (16 items, verbatim). Splitters are greyed inside bands.
Menu addMenu (TinctureProcessor&, const FxList& where, int at = -1);
// The module menu (coloured ▼): Factory > / User > · Save FX Preset... · Save
// as Default Preset · Duplicate FX Module · Remove FX Module. `nameAnchor`
// gets the name box for Save FX Preset...
Menu moduleMenu (PanelContext&, const FxPath&, juce::Component& nameAnchor);

// ---------------------------------------------------------------------------
// Widgets
// ---------------------------------------------------------------------------
// The bypass button (a power symbol): grey, red when bypassed.
class BypassButton final : public juce::Component, public juce::SettableTooltipClient
{
public:
    BypassButton() { setTooltip ("Bypass (Alt-click: all FX of this rack)"); }
    bool bypassed = false;
    std::function<void (const juce::ModifierKeys&)> onClick;
    void set (bool b) { if (b != bypassed) { bypassed = b; repaint(); } }
    void paint (juce::Graphics&) override;
    void mouseUp (const juce::MouseEvent& e) override
    {
        if (getLocalBounds().contains (e.getPosition()) && onClick) onClick (e.mods);
    }
};

// A breakpoint curve editor (x = input, y = output). Double-click adds or
// removes a point, drag points, drag a segment vertically to bend it,
// Alt-drag bends every segment. `set (curve, commit)` is called during the
// drag with commit = false and on mouse-up with commit = true.
class CurveEditor final : public juce::Component
{
public:
    std::function<Curve()> get;
    std::function<void (const Curve&, bool commit)> set;
    juce::Colour colour = colours::brand;
    bool symmetricGuide = false;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    juce::Rectangle<float> plot() const { return getLocalBounds().toFloat().reduced (10.0f); }
    juce::Point<float> toScreen (float x, float y) const;

private:
    Curve work;
    int dragPoint = -1, dragSegment = -1;
    float startBend = 0.0f;
    std::vector<float> startBends;
    bool dragging = false;
};

// ---------------------------------------------------------------------------
// Module editors
// ---------------------------------------------------------------------------
// What the hosting page offers to its module editors.
struct FxHost
{
    virtual ~FxHost() = default;
    virtual void openCurveEditor (const juce::String& title, juce::Colour, std::function<Curve()> get,
                                  std::function<void (const Curve&, bool commit)> set) = 0;
    virtual int selectedBand (int unit) const = 0;
    virtual void selectBand (int unit, int band) = 0;
    // Dragging a module by its title strip (page coordinates).
    virtual void moduleDragged (const FxPath&, juce::Point<int> pagePos, bool finished, bool alt) = 0;
};

constexpr int kModuleH = 132;

class FxModuleEditor : public juce::Component, public Refreshable
{
public:
    ~FxModuleEditor() override = default;
    virtual const FxPath& path() const = 0;
    virtual int unit() const = 0;
};

std::unique_ptr<FxModuleEditor> createModuleEditor (PanelContext&, FxHost&, const FxPath&, const FxModuleSlot&);
} // namespace tincture::ui::fxui
