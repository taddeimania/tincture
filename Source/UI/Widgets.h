// Widgets.h — Tincture panel controls, in the same flat hardware language as
// Planish, Burnish, Lacquer, Mottle, Posy and the rest: charcoal panel, cream knob
// caps, outlined sections with inverse tabs, flat LEDs and amber LCD
// readouts.
#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace tincture::ui
{
// ---------------------------------------------------------------------------
// Palette
// ---------------------------------------------------------------------------
namespace colours
{
    inline const juce::Colour panel      { 0xff2a2a2c };
    inline const juce::Colour panelEdge  { 0xff1c1c1e };
    inline const juce::Colour ink        { 0xfff3f0e6 };
    inline const juce::Colour inkDim     { 0xff9a978e };
    inline const juce::Colour inkFaint   { 0xff5a5852 };
    inline const juce::Colour cream      { 0xfff7f1dc };
    inline const juce::Colour creamShade { 0xffd9d1b4 };
    inline const juce::Colour black      { 0xff111112 };
    inline const juce::Colour socket     { 0xff3a3a3d };   // an unlit LED
    inline const juce::Colour brand      { 0xff8fd6b0 };   // Tincture mint
    inline const juce::Colour sidechain  { 0xffb07cff };   // the sidechain, everywhere it is drawn
    inline const juce::Colour lcd        { 0xffffb238 };   // LCD text
    inline const juce::Colour lcdDim     { 0xff8a6a2a };
    inline const juce::Colour ledRed     { 0xffff2b3d };
    inline const juce::Colour ledGreen   { 0xff3fe060 };
    inline const juce::Colour ledYellow  { 0xffffd21f };
    inline const juce::Colour grid       { 0xff3c3c40 };
    inline const juce::Colour spectrum   { 0xff6f6c62 };
    inline const juce::Colour spectrumFill { 0x30f3f0e6 };

    // The three band colours of the multiband modules.
    inline juce::Colour band (int b)
    {
        const juce::Colour c[] { juce::Colour (0xff5cc8f2), juce::Colour (0xffffae42), juce::Colour (0xffff7088) };
        return c[juce::jlimit (0, 2, b)];
    }
    // Twelve node colours for EQ bands.
    inline juce::Colour node (int b)
    {
        const juce::Colour c[] { juce::Colour (0xff5cc8f2), juce::Colour (0xffffae42), juce::Colour (0xffff7088), juce::Colour (0xffa98dff),
                                 juce::Colour (0xff4ccaa0), juce::Colour (0xfff2d35c), juce::Colour (0xffe08a5c), juce::Colour (0xff9ad75c),
                                 juce::Colour (0xff7fb8ff), juce::Colour (0xffff9fd0), juce::Colour (0xffc9b28a), juce::Colour (0xff6fe0e0) };
        return c[juce::jlimit (0, 11, b)];
    }
}

juce::Font panelFont (float height, bool bold = true);

template <typename... C> void showAll (bool on, C&... c) { (c.setVisible (on), ...); }
template <typename... C> void enableAll (bool on, C&... c) { (c.setEnabled (on), ...); }
void drawText (juce::Graphics&, const juce::String&, juce::Rectangle<float>, float height,
               juce::Colour = colours::ink, bool bold = true,
               juce::Justification = juce::Justification::centred);

// A flat LED. Unlit is a dark socket; `level` 0..1 brightens a lit one.
void drawLed (juce::Graphics&, juce::Point<float> centre, float radius, juce::Colour colour,
              bool lit, float level = 0.0f);

// An outlined panel section with its name in an inverse tab.
void drawSection (juce::Graphics&, juce::Rectangle<float>, const juce::String& title, juce::Colour tab = colours::ink);

// Mounting-screw slot, as on Dicey's corners.
void drawScrewSlot (juce::Graphics&, juce::Point<float> centre);

// A black LCD box with a dim outline.
void drawLcdBox (juce::Graphics&, juce::Rectangle<float>);

// Frequency axis helpers (20 Hz .. 20 kHz) on the chosen scale: every
// spectrum and curve in the panel goes through these two.
enum FrequencyScale { ScaleLog = 0, ScaleMel, ScaleLinear, ScalePianoRoll, NumScales };
void setFrequencyScale (int scale);
int  frequencyScale();
const char* frequencyScaleName (int scale);
float freqToNorm (float hz);
float normToFreq (float t);
// A strip of piano keys under a frequency axis (the PIANO ROLL scale).
void drawPianoRoll (juce::Graphics&, juce::Rectangle<float>);
juce::String noteName (float hz);
void drawFrequencyGrid (juce::Graphics&, juce::Rectangle<float>, bool labels, float dbTop = 0.0f, float dbBottom = 0.0f, float dbStep = 0.0f);

// ---------------------------------------------------------------------------
// Look and feel
// ---------------------------------------------------------------------------
class PanelLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    PanelLookAndFeel();
    void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos,
                           float startAngle, float endAngle, juce::Slider&) override;
    void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;
    juce::Font getSliderPopupFont (juce::Slider&) override { return panelFont (15.0f); }
    int getSliderPopupPlacement (juce::Slider&) override { return juce::BubbleComponent::above; }
    juce::Font getPopupMenuFont() override { return panelFont (15.0f, false); }
    juce::Font getAlertWindowMessageFont() override { return panelFont (15.0f, false); }
    juce::Font getTextButtonFont (juce::TextButton&, int) override { return panelFont (13.0f); }
    juce::Font getLabelFont (juce::Label&) override { return panelFont (14.0f); }
};

// ---------------------------------------------------------------------------
// Knob: a cream cap with a pointer line inside a scale arc, caption below.
// The value shows in a bubble while you drag.
// ---------------------------------------------------------------------------
class Knob : public juce::Slider
{
public:
    Knob();
    juce::String caption;
    bool bipolar = false;
    bool small = false;
    bool big = false;
    juce::Colour accent = juce::Colours::transparentBlack;   // tints the scale arc
    std::function<juce::String (double)> format;

    void paint (juce::Graphics&) override;
    juce::String getTextFromValue (double v) override { return format ? format (v) : Slider::getTextFromValue (v); }

private:
    double snapValue (double attempted, DragMode) override;
};

// ---------------------------------------------------------------------------
// Fader: a vertical (or horizontal) slider with a cream handle on a slot.
// ---------------------------------------------------------------------------
class Fader : public juce::Slider
{
public:
    explicit Fader (bool vertical = true);
    juce::String caption;
    bool bipolar = false;
    juce::Colour fill = colours::lcd;
    float fillScale = 1.0f;   // shows a reduced fill when an AMOUNT scales the value
    std::function<juce::String (double)> format;
    void paint (juce::Graphics&) override;
    juce::String getTextFromValue (double v) override { return format ? format (v) : Slider::getTextFromValue (v); }
private:
    double snapValue (double attempted, DragMode) override;
};

// ---------------------------------------------------------------------------
// Readout: an LCD value you drag to scrub. A Slider underneath, so host
// parameter attachments work.
// ---------------------------------------------------------------------------
class Readout : public juce::Slider
{
public:
    Readout();
    juce::String caption;
    float fontH = 15.0f;
    bool captionLeft = false;
    std::function<juce::String (double)> format;
    void paint (juce::Graphics&) override;
    juce::String getTextFromValue (double v) override { return format ? format (v) : Slider::getTextFromValue (v); }
};

// ---------------------------------------------------------------------------
// Rectangular button. Lit = inverse (cream face, dark text).
// ---------------------------------------------------------------------------
class PanelButton : public juce::Component, public juce::SettableTooltipClient
{
public:
    explicit PanelButton (juce::String label = {});

    std::function<void (const juce::ModifierKeys&)> onClick;
    std::function<void()> onPress;
    std::function<void()> onRelease;

    void setLabel (const juce::String& t) { if (t != label) { label = t; repaint(); } }
    void setLit (bool l) { if (l != lit) { lit = l; repaint(); } }
    void setFontHeight (float h) { fontH = h; repaint(); }
    void setLitColour (juce::Colour c) { litColour = c; repaint(); }
    void setDimmed (bool d) { if (d != dimmed) { dimmed = d; repaint(); } }
    bool isLit() const noexcept { return lit; }
    const juce::String& getLabel() const { return label; }

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;

private:
    juce::String label;
    bool lit = false, pressed = false, dimmed = false;
    float fontH = 12.0f;
    juce::Colour litColour = colours::cream;
};

// ---------------------------------------------------------------------------
// LCD readout that opens a menu when clicked.
// ---------------------------------------------------------------------------
class ChoiceDisplay : public juce::Component, public juce::SettableTooltipClient
{
public:
    std::function<juce::String()> getText;
    std::function<void()> onOpen;
    juce::String caption;
    float fontH = 15.0f;
    bool captionLeft = false;

    void paint (juce::Graphics&) override;
    void mouseDown (const juce::MouseEvent&) override { if (onOpen) onOpen(); }
    void refresh();

private:
    juce::String shown;
};

// ---------------------------------------------------------------------------
// Segmented LED meter with a falling peak, vertical.
// ---------------------------------------------------------------------------
class Meter final : public juce::Component
{
public:
    void push (float linear);          // from the panel timer
    void setRangeDb (float floorDb) { floorDb_ = floorDb; }
    void paint (juce::Graphics&) override;

private:
    float level = 0.0f, hold = 0.0f, floorDb_ = -60.0f;
    int holdFrames = 0, shownSegs = -1, shownHold = -1;
};

// A small horizontal bar meter (gain reduction, difference).
class BarMeter final : public juce::Component
{
public:
    float value = 0.0f, rangeMin = -12.0f, rangeMax = 12.0f;   // dB
    bool bipolar = true;
    juce::Colour colour = colours::lcd;
    void set (float v) { const float c = juce::jlimit (rangeMin, rangeMax, v); if (std::abs (c - value) > 0.02f) { value = c; repaint(); } }
    void paint (juce::Graphics&) override;
};
} // namespace tincture::ui
