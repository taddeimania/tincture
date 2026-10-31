// SeqWidgets.h — small pieces shared by the bottom strip, the ARP editor and
// the CLIP editor: LCD step fields, checkboxes, patch-bound knobs, the
// 12-key launchers, the power button, the MIDI OUT box, bank files, and the
// "edit the patch during a drag" helper.
#pragma once

#include "Pages.h"
#include "../PluginProcessor.h"
#include "../Engine/NoteProcessor.h"

namespace tincture::ui::seq
{
inline const juce::Colour clipColour { 0xff4fd1a8 };    // CLIP: teal green
inline const juce::Colour arpColour  { 0xffc49bff };    // ARP: purple
inline const juce::Colour orange     { 0xffffa040 };    // last added / selected notes
inline const juce::Colour takeRed    { 0xffff5a5a };    // recorded, not committed

bool altHeld();
juce::String noteName (int note);                       // 60 = "C3"
int parseNoteName (const juce::String&);                // "C#3" -> 61, -1 if not a note

// Patch edits from sequencer controls. While a drag runs (beginDrag/endDrag)
// edits skip the history and one entry is committed when it ends.
void edit (PanelContext&, const juce::String& label, const std::function<void (Patch&)>& fn);
void beginDrag();
void endDrag (PanelContext&);

// "1. 0. 0" (bars . beats . 16ths) and the step for a drag on that segment.
juce::String formatLength (double beats);
double lengthStepFor (float downFrac);
bool parseLength (const juce::String&, double& beats);
// A single-choice list as a menu.
Menu listMenu (const std::vector<std::string>& items, int current, std::function<void (int)> onPick);
// The knob context menu for sequencer settings that are patch data (not
// host parameters): title, extra items, the modulation items (shown
// disabled: these settings aren't modulation destinations), Reset Control.
Menu settingMenu (const juce::String& title, const std::function<void (Menu&)>& extra, std::function<void()> onReset);

// Draws a filled triangle pointing left/right/up/down.
void drawArrow (juce::Graphics&, juce::Rectangle<float> r, int direction, juce::Colour);   // 0 left 1 right 2 up 3 down

// ---------------------------------------------------------------------------
// An LCD field. Drag up/down or wheel steps it, click opens its menu, < >
// arrows step, right-click opens the context menu, double-click types.
// ---------------------------------------------------------------------------
class Field : public juce::Component, public Refreshable, public juce::SettableTooltipClient
{
public:
    explicit Field (PanelContext&, const juce::String& caption = {}, int captionW = 0);
    ~Field() override;

    PanelContext& ctx;
    juce::String caption;
    int captionW = 0;                 // caption drawn left in this width
    bool arrows = false;              // < > inside the box
    bool dropArrow = false;           // a small triangle: click opens the menu
    bool centred = true;
    float fontH = 12.5f;
    float pixelsPerStep = 8.0f;
    juce::Colour textColour = colours::lcd;
    std::function<juce::String()> text;
    std::function<bool()> active;     // false: dimmed (still usable)
    std::function<void (int)> onStep;
    std::function<Menu()> menu;
    std::function<Menu()> contextMenu;
    std::function<void()> onDoubleClick;
    std::function<void (juce::Point<int>, const juce::ModifierKeys&)> onPlainClick;   // instead of the menu

    float downFrac = 0.5f;            // where in the box the last press was (0..1), for segmented fields

    juce::Rectangle<float> box() const;
    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    juce::String shown;
    bool shownActive = true, dragging = false, moved = false, arrowHit = false, popup = false;
    int stepsDone = 0;
};

// ---------------------------------------------------------------------------
// A checkbox (or, with `button`, a lit panel button) bound to patch data.
// ---------------------------------------------------------------------------
class Check : public juce::Component, public Refreshable, public juce::SettableTooltipClient
{
public:
    Check (PanelContext&, const juce::String& label, bool button = false);
    ~Check() override;
    PanelContext& ctx;
    juce::String label;
    bool button;
    float fontH = 11.5f;
    juce::Colour accent = colours::cream;
    std::function<bool()> ticked;
    std::function<bool()> active;
    std::function<void()> onToggle;
    std::function<Menu()> contextMenu;

    void refresh() override;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    int shown = -1;
};

// ---------------------------------------------------------------------------
// A small knob over patch data (normalised 0..1) with its value in an LCD
// box below. Double-click the box to type.
// ---------------------------------------------------------------------------
class SeqKnob : public juce::Component, public Refreshable
{
public:
    SeqKnob (PanelContext&, const juce::String& caption);
    ~SeqKnob() override;
    PanelContext& ctx;
    std::function<float()> get;
    std::function<void (float)> set;
    std::function<juce::String (float)> format;
    std::function<bool (const juce::String&, float&)> parse;   // text -> norm
    std::function<Menu()> contextMenu;
    std::function<bool()> active;
    float defaultNorm = 0.0f;
    void setAccent (juce::Colour c) { knob.accent = c; knob.repaint(); }
    void setBipolar (bool b) { knob.bipolar = b; }

    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    void mouseDoubleClick (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;

    struct Inner : Knob
    {
        SeqKnob* owner = nullptr;
        void mouseDown (const juce::MouseEvent&) override;
    };
    Inner knob;

private:
    bool syncing = false;
    juce::String shownText;
    float shownNorm = -1.0f;
    bool shownActive = true;
};

// ---------------------------------------------------------------------------
// One octave of launcher keys (CLIP / ARP tabs).
// ---------------------------------------------------------------------------
class MiniKeys : public juce::Component, public juce::SettableTooltipClient
{
public:
    juce::Colour accent = clipColour;
    std::function<int (int)> state;          // 0 idle, 1 playing, 2 pending, 3 selected (outline)
    std::function<bool (int)> hasContent;
    std::function<void (int)> onKey;
    void update();                           // repaint if a key's state changed
    int keyAt (juce::Point<float>) const;
    juce::Rectangle<float> keyBounds (int k) const;
    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;

private:
    int shown[12] { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
};

// The CLIP / ARP power button (a parameter toggle with a power glyph).
class PowerButton : public ParamToggle
{
public:
    PowerButton (PanelContext& c, int param, juce::Colour accentColour);
    void paint (juce::Graphics&) override;
    juce::Colour accent;
};

// MIDI OUT: the dot display of notes sent to the engine and the MIDI OUT choice.
class MidiOutBox : public juce::Component, public Refreshable
{
public:
    explicit MidiOutBox (PanelContext&);
    ~MidiOutBox() override;
    void refresh() override;
    void resized() override;
    void paint (juce::Graphics&) override;
    ParamChoice choice;

private:
    PanelContext& ctx;
    uint32_t shownCount = 0xffffffff;
    int idleFrames = 0;
};

// "Lock Module" / "MIDI Input Trigger Octave >" (the CLIP and ARP tab menus).
Menu tabMenu (PanelContext&, bool clip);
// Header rows of the editors: title tab + close button.
void drawEditorBackground (juce::Graphics&, juce::Rectangle<int> bounds, const juce::String& title, juce::Colour accent);

// ---------------------------------------------------------------------------
// Bank files (user folder beside the presets).
// ---------------------------------------------------------------------------
juce::File userFolder (const juce::String& kind);      // "Arp Banks", "Clip Banks", "Arp Patterns", "Clips"
juce::Array<juce::File> userFiles (const juce::String& kind);
bool saveNode (const juce::File&, const Node&);
bool loadNode (const juce::File&, Node&);
juce::File userFileFor (const juce::String& kind, const juce::String& name);

// Modified marks (`MyBank*`): the content signature at load/save time.
void setBaseline (const void* owner, const juce::String& kind, const std::string& name, std::vector<uint8_t> signature);
bool isModified (const void* owner, const juce::String& kind, const std::string& name, const std::vector<uint8_t>& signature,
                 const std::vector<uint8_t>& initSignature);
} // namespace tincture::ui::seq
