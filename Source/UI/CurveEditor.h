// CurveEditor.h — a reusable breakpoint-curve editor (tincture::Curve) for the
// LFO graph, the VELO / NOTE graphs and the matrix curve editor.
//
// Mouse (spec D §2.6, §7):
//   double-click            add a point, or remove the point under the cursor
//   drag a point            move it (Alt: snapped to the grid; Cmd/Ctrl: relative
//                           multi-point move, nearer points move more)
//   drag a curve handle     bend that segment (Alt: bend all segments)
//   drag on the background  marquee-select points (Point tool)
//   Flat / Ramp tools       click or drag across grid cells to draw steps / ramps
//   Shift-click / drag      draw steps with the "Shift-Key Function" tool
//   Shift-Cmd-click a point onSetLoopback (ENVELOPE mode loopback)
//   right-click             pointMenu (point, handle or empty space)
//   drop a mod source       X / Y drop boxes beside the nearest point -> onPointDrop
//
// The editor never stores the curve: it reads it through getCurve() and
// reports edits through onEdit (continuous, no undo) and onCommit (once at
// the end of a gesture, with an undo label).
#pragma once

#include "Menu.h"
#include "Widgets.h"
#include "../Model/Curve.h"
#include "../Engine/ModMatrix.h"

#include <set>

namespace tincture::ui
{
class CurveEditor : public juce::Component, public juce::DragAndDropTarget
{
public:
    enum class Tool { Point = 0, Flat, RampUp, RampDown };

    CurveEditor();

    // Data.
    std::function<Curve()> getCurve;
    std::function<void (const Curve&)> onEdit;
    std::function<void (const juce::String& undoLabel)> onCommit;

    // Look and behaviour.
    juce::Colour colour = colours::lcd;
    int gridX = 8, gridY = 8;
    bool showGrid = true;
    bool editable = true;
    bool fill = true;
    Tool tool = Tool::Point;
    Tool shiftTool = Tool::Flat;
    juce::String overlayText;               // drawn centred (e.g. "CHAOS: LORENZ")
    float padding = 5.0f;

    // Live display.
    float playhead = -1.0f;                 // x 0..1 (or < 0 for none)
    int loopback = -1;                      // point index, -1 none
    std::vector<float> trace;               // optional values 0..1 drawn as a scope (chaos / S&H)

    // Point modulation bars (§2.8).
    struct ModBar { int point; float dx, dy, dc; juce::Colour colour; };
    std::vector<ModBar> modBars;

    // Hooks.
    std::function<Menu (int point, bool handle)> pointMenu;
    std::function<void (int point)> onSetLoopback;
    std::function<void (int point, int axis /* 0 = X, 1 = Y */, ModSource)> onPointDrop;

    // Selection (indices into the curve's points).
    std::set<int> selection;

    // Call when the data or live values may have changed; repaints only on change.
    void update();

    // Geometry (public for tests / the shots).
    juce::Rectangle<float> plotArea() const;
    juce::Point<float> toScreen (float x, float y) const;
    juce::Point<float> fromScreen (juce::Point<float>) const;   // clamped 0..1
    juce::Point<float> pointPos (int i) const;                  // screen position of point i
    juce::Point<float> handlePos (int i) const;                 // handle of the segment ending at i (i >= 1)
    int hitPoint (juce::Point<float>) const;
    int hitHandle (juce::Point<float>) const;

    // Edits used by the tools and menus (operate on and return a copy).
    static Curve drawCell (const Curve& c, float x0, float x1, float y, Tool t);
    static Curve flipVertical (const Curve& c, const std::set<int>& sel);
    static Curve flipHorizontal (const Curve& c, const std::set<int>& sel);

    void paint (juce::Graphics&) override;
    void resized() override { pathValid = false; }
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;

    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragMove (const SourceDetails&) override;
    void itemDragExit (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    enum class Gesture { None, Point, Handle, Marquee, Draw };
    void edit (const Curve& c);
    void drawAt (juce::Point<float> p, Tool t);
    juce::Rectangle<float> dropBox (int point, int axis) const;
    void rebuildPath (const Curve& c);

    Curve cur;                  // last curve read (for painting / hit tests)
    Curve start;                // at the start of a drag
    Gesture gesture = Gesture::None;
    int dragIndex = -1;
    bool relative = false, bendAll = false, changed = false;
    juce::Point<float> downPos, marqueeEnd;
    std::set<int> dragSet;
    int lastCell = -1;
    float lastY = -1.0f;
    Tool drawTool = Tool::Flat;
    int hoverPoint = -1, hoverHandle = -1;
    int dropPoint = -1, dropAxis = -1;

    // Paint cache.
    juce::Path curvePath, fillPath;
    bool pathValid = false;
    Curve pathCurve;
    float shownPlayhead = -2.0f;
    int shownLoop = -2;
    size_t shownTrace = 0;
};

// Snap v (0..1) to `divisions` steps.
inline float snapTo (float v, int divisions) { return divisions > 0 ? std::round (v * (float) divisions) / (float) divisions : v; }
} // namespace tincture::ui
