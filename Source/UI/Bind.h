// Bind.h — panel widgets bound to Tincture parameters, and the services
// every page shares (refresh, context menus, modulation drag and drop, the
// value-entry box, mod halos).
//
// Every control that shows a parameter should be one of these:
//   ParamKnob      a knob (with modulation rings) — drag, wheel, Shift fine,
//                  Cmd/Ctrl-click reset, double-click types a value, right-click menu
//   ParamFader     a vertical or horizontal fader, same behaviour
//   ParamField     an LCD number you drag (UNISON, OCT, START...)
//   ParamToggle    a lit button for a Bool parameter
//   ParamChoice    an LCD dropdown for a Choice parameter (+ optional < > steppers)
// They all refresh themselves from the processor on the panel timer, take
// mod-source drops (drag an ENV/LFO/macro tab onto them), and open the
// standard control menu on right-click (see controlMenu()).
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "Menu.h"
#include <map>
#include "Widgets.h"
#include "../Engine/ModMatrix.h"

class TinctureProcessor;

namespace tincture::ui
{
// ---------------------------------------------------------------------------
// Shared panel services. One per editor; widgets find it via their parent.
// ---------------------------------------------------------------------------
class Refreshable
{
public:
    virtual ~Refreshable() = default;
    virtual void refresh() = 0;            // ~30 Hz, message thread: pull state, repaint if changed
};

class PanelContext
{
public:
    explicit PanelContext (TinctureProcessor& p) : proc (p) {}
    TinctureProcessor& proc;

    void add (Refreshable* r)    { items.push_back (r); }
    void remove (Refreshable* r) { items.erase (std::remove (items.begin(), items.end(), r), items.end()); }
    void refreshAll()            { for (size_t i = 0; i < items.size(); ++i) items[i]->refresh(); }

    // Modulation colours (one per source family).
    static juce::Colour sourceColour (ModSource s);

    // Halo slots: a modulated control asks for its live value to be published.
    int haloSlotFor (int param);           // -1 if none free
    void releaseHalo (int param);
    float haloValue (int slot) const;      // normalised, from telemetry

    // A text box over `target` to type a value; `onDone` gets the text.
    std::function<void (juce::Component& target, const juce::String& initial, std::function<void (const juce::String&)> onDone)> editValue;
    // Display name of a parameter (header of its menu, status line, tooltips).
    // Defaults to ParamInfo::name; FX unit parameters are named after their module.
    juce::String paramName (int param) const;
    // Status line text (bottom of the panel): what the hovered control does.
    std::function<void (const juce::String&)> setStatus;

private:
    std::vector<Refreshable*> items;
    std::map<int, int> haloOf;             // param -> slot
};

PanelContext& contextOf (juce::Component& c);   // walks up to the panel

// The standard right-click menu for a parameter control:
//   <title>  /  Mod Source >  Bypass Modulator  Remove Modulator  Remove All Modulators
//   /  Reset Control  MIDI Learn (Clear MIDI Learn)  Lock Parameter
// `extra` (optional) adds items right after the title (e.g. OCT/SEM Mode >).
Menu controlMenu (PanelContext&, int param, const std::function<void (Menu&)>& extra = {});
// The source tree as a menu: Envelopes >, LFOs >, Note >, ... `onPick(source)`.
Menu sourceMenu (const std::function<void (ModSource)>& onPick, ModSource current = ModSource::None);
// The destination tree (every modulatable parameter by module): `onPick(param)`.
Menu destinationMenu (const std::function<void (int)>& onPick, int current = -1);
// A choice parameter as a menu with the current value ticked (sub-menus for
// long or categorised lists when `categories` is given).
Menu choiceMenu (PanelContext&, int param);

// Drag-and-drop description for a modulation source: "modsrc:<index>".
juce::var modSourceDragDescription (ModSource);
ModSource modSourceFromDrag (const juce::var&);

// ---------------------------------------------------------------------------
// Base for bound controls.
// ---------------------------------------------------------------------------
class Bound : public Refreshable
{
public:
    Bound (PanelContext&, int param);
    ~Bound() override;
    int param() const noexcept { return paramId; }
    void setParam (int p);                 // rebind (e.g. a panel showing OSC A/B/C)

protected:
    PanelContext& ctx;
    int paramId;
    float shownNorm = -1.0f;
    juce::String tooltipFor() const;
};

// ---------------------------------------------------------------------------
class ParamKnob : public Knob, public Bound, public juce::DragAndDropTarget
{
public:
    ParamKnob (PanelContext&, int param, const juce::String& caption = {});
    std::function<void (Menu&)> extraMenu;  // items added under the title of the right-click menu
    bool showHalo = true;

    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseEnter (const juce::MouseEvent&) override;
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDragEnter (const SourceDetails&) override { dropHover = true; repaint(); }
    void itemDragExit (const SourceDetails&) override { dropHover = false; repaint(); }
    void itemDropped (const SourceDetails&) override;

private:
    void valueChanged() override;
    void startedDragging() override;
    void stoppedDragging() override;
    bool syncing = false, dropHover = false;
    int haloSlot = -1;
    struct Ring { float from, to; juce::Colour colour; bool bypassed; };
    std::vector<Ring> rings;
};

class ParamFader : public Fader, public Bound, public juce::DragAndDropTarget
{
public:
    ParamFader (PanelContext&, int param, bool vertical = true, const juce::String& caption = {});
    std::function<void (Menu&)> extraMenu;  // items added under the title of the right-click menu
    void refresh() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    void valueChanged() override;
    void startedDragging() override;
    void stoppedDragging() override;
    bool syncing = false;
};

// An LCD value you drag up/down (or scroll); double-click to type.
class ParamField : public juce::Component, public Bound, public juce::SettableTooltipClient, public juce::DragAndDropTarget
{
public:
    ParamField (PanelContext&, int param, const juce::String& caption = {});
    juce::String caption;
    bool captionLeft = true;
    float fontH = 13.0f;
    std::function<juce::String (float plain)> format;   // default: ParamInfo::format
    std::function<void (Menu&)> extraMenu;
    float pixelsPerStep = 6.0f;            // discrete params: drag distance per step

    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool isInterestedInDragSource (const SourceDetails&) override;
    void itemDropped (const SourceDetails&) override;

private:
    float dragStartNorm = 0.0f;
    bool dragging = false;
};

class ParamToggle : public PanelButton, public Bound
{
public:
    ParamToggle (PanelContext&, int param, const juce::String& label);
    void refresh() override;
};

// An LCD with the current choice and a ▼; click opens the choice menu.
class ParamChoice : public ChoiceDisplay, public Bound
{
public:
    ParamChoice (PanelContext&, int param);
    std::function<Menu()> buildMenu;       // default: choiceMenu()
    std::function<juce::String (int)> label;   // default: the choice text
    void refresh() override;
    void mouseDown (const juce::MouseEvent&) override;
    void step (int delta);                 // for < > buttons and the wheel
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
};

// A tab that can be dragged onto controls to create a modulation (ENV 1,
// LFO 3, MACRO 2, VELO, NOTE, OSC A labels...).
class ModSourceTab : public juce::Component, public juce::SettableTooltipClient
{
public:
    ModSourceTab (PanelContext&, ModSource, const juce::String& label);
    std::function<void()> onClick;
    std::function<void (Menu&)> extraMenu;
    bool selected = false;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    ModSource source() const noexcept { return src; }

private:
    PanelContext& ctx;
    ModSource src;
    juce::String text;
    bool dragStarted = false;
};

// Create/adjust a modulation row from a drop (amount defaults per spec §3).
void dropModulation (PanelContext&, ModSource, int param);
} // namespace tincture::ui
