#include "Widgets.h"
#include "../Engine/Core.h"

#include <atomic>

namespace tincture::ui
{
using namespace juce;

// ---------------------------------------------------------------------------
// Type
// ---------------------------------------------------------------------------
Font panelFont (float height, bool bold)
{
   #if JUCE_MAC
    Font f (FontOptions ("Helvetica Neue", height, bold ? Font::bold : Font::plain));
   #else
    Font f (FontOptions (height, bold ? Font::bold : Font::plain));
   #endif
    f.setHorizontalScale (0.86f);
    return f;
}

void drawText (Graphics& g, const String& text, Rectangle<float> r, float height, Colour colour, bool bold, Justification j)
{
    g.setColour (colour);
    g.setFont (panelFont (height, bold));
    g.drawText (text, r, j, false);
}

// ---------------------------------------------------------------------------
// Panel furniture
// ---------------------------------------------------------------------------
void drawLed (Graphics& g, Point<float> c, float r, Colour colour, bool lit, float level)
{
    const auto body = Rectangle<float> (r * 2.0f, r * 2.0f).withCentre (c);
    if (! lit)
    {
        g.setColour (colours::socket);
        g.fillEllipse (body);
        g.setColour (colours::panelEdge);
        g.drawEllipse (body, 1.0f);
        return;
    }
    g.setColour (level > 0.0f ? colour.brighter (0.45f * level) : colour.darker (0.08f));
    g.fillEllipse (body);
    g.setColour (colour.darker (0.5f));
    g.drawEllipse (body, 1.0f);
}

void drawSection (Graphics& g, Rectangle<float> r, const String& title, Colour tabColour)
{
    g.setColour (colours::ink.withAlpha (0.85f));
    g.drawRect (r, 1.5f);
    if (title.isEmpty())
        return;
    const auto f = panelFont (12.0f, true);
    const float w = GlyphArrangement::getStringWidth (f, title) + 12.0f;
    const auto tab = Rectangle<float> (r.getX() + 14.0f, r.getY() - 8.0f, w, 16.0f);
    g.setColour (tabColour);
    g.fillRect (tab);
    g.setColour (colours::panel);
    g.setFont (f);
    g.drawText (title, tab, Justification::centred, false);
}

void drawScrewSlot (Graphics& g, Point<float> c)
{
    const auto r = Rectangle<float> (64.0f, 34.0f).withCentre (c);
    g.setColour (colours::cream);
    g.fillRoundedRectangle (r, 17.0f);
    g.setColour (colours::creamShade);
    g.drawRoundedRectangle (r, 17.0f, 1.2f);
}

void drawLcdBox (Graphics& g, Rectangle<float> r)
{
    g.setColour (colours::black);
    g.fillRect (r);
    g.setColour (colours::inkDim);
    g.drawRect (r, 1.0f);
}

namespace
{
std::atomic<int> gScale { ScaleLog };
float melOf (float hz) { return 2595.0f * std::log10 (1.0f + hz / 700.0f); }
float hzOfMel (float m) { return 700.0f * (std::pow (10.0f, m / 2595.0f) - 1.0f); }
}

void setFrequencyScale (int s) { gScale.store (jlimit (0, NumScales - 1, s)); }
int  frequencyScale() { return gScale.load(); }
const char* frequencyScaleName (int s)
{
    static const char* n[NumScales] { "LOGARITHMIC", "MEL", "LINEAR", "PIANO ROLL" };
    return n[jlimit (0, NumScales - 1, s)];
}

float freqToNorm (float hz)
{
    hz = jlimit (20.0f, 20000.0f, hz);
    switch (gScale.load (std::memory_order_relaxed))
    {
        case ScaleMel:    return (melOf (hz) - melOf (20.0f)) / (melOf (20000.0f) - melOf (20.0f));
        case ScaleLinear: return (hz - 20.0f) / (20000.0f - 20.0f);
        default:          return std::log (hz / 20.0f) / std::log (1000.0f);
    }
}
float normToFreq (float t)
{
    t = jlimit (0.0f, 1.0f, t);
    switch (gScale.load (std::memory_order_relaxed))
    {
        case ScaleMel:    return hzOfMel (melOf (20.0f) + t * (melOf (20000.0f) - melOf (20.0f)));
        case ScaleLinear: return 20.0f + t * (20000.0f - 20.0f);
        default:          return 20.0f * std::pow (1000.0f, t);
    }
}

String noteName (float hz)
{
    static const char* names[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    if (hz <= 0.0f) return {};
    const int midi = roundToInt (69.0f + 12.0f * std::log2 (hz / 440.0f));
    return String (names[((midi % 12) + 12) % 12]) + String (midi / 12 - 1);
}

void drawPianoRoll (Graphics& g, Rectangle<float> r)
{
    g.setColour (colours::black);
    g.fillRect (r);
    // MIDI 16 (20.6 Hz) .. 135 (19.9 kHz).
    for (int m = 16; m <= 135; ++m)
    {
        const float f0 = 440.0f * std::pow (2.0f, ((float) m - 69.5f) / 12.0f);
        const float f1 = 440.0f * std::pow (2.0f, ((float) m - 68.5f) / 12.0f);
        const float x0 = r.getX() + freqToNorm (f0) * r.getWidth(), x1 = r.getX() + freqToNorm (f1) * r.getWidth();
        const int pc = m % 12;
        const bool black = pc == 1 || pc == 3 || pc == 6 || pc == 8 || pc == 10;
        g.setColour (black ? colours::panelEdge : colours::cream.withAlpha (pc == 0 ? 0.95f : 0.75f));
        g.fillRect (Rectangle<float> (x0, r.getY() + (black ? 0.0f : 1.0f), jmax (0.5f, x1 - x0 - 0.5f), r.getHeight() - (black ? 4.0f : 1.0f)));
        if (pc == 0 && x1 - x0 > 1.5f)
            drawText (g, "C" + String (m / 12 - 1), { x0 - 8.0f, r.getBottom() - 10.0f, 22.0f, 9.0f }, 7.0f, colours::black, false);
    }
}

void drawFrequencyGrid (Graphics& g, Rectangle<float> r, bool labels, float dbTop, float dbBottom, float dbStep)
{
    g.setColour (colours::grid);
    const bool linear = frequencyScale() == ScaleLinear;
    std::vector<float> marks;
    if (linear) for (float hz = 1000.0f; hz < 20000.0f; hz += 1000.0f) marks.push_back (hz);
    else marks = { 20, 30, 40, 50, 60, 70, 80, 90, 100, 200, 300, 400, 500, 600, 700, 800, 900, 1000, 2000, 3000, 4000, 5000, 6000, 7000, 8000, 9000, 10000, 20000 };
    for (float hz : marks)
    {
        const float x = r.getX() + freqToNorm (hz) * r.getWidth();
        const bool major = linear ? (int) hz % 5000 == 0 : (tincture::same (hz, 100.0f) || tincture::same (hz, 1000.0f) || tincture::same (hz, 10000.0f));
        g.setColour (major ? colours::grid.brighter (0.4f) : colours::grid);
        g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
    }
    if (dbStep > 0.0f && dbTop > dbBottom)
    {
        for (float db = std::ceil (dbBottom / dbStep) * dbStep; db <= dbTop; db += dbStep)
        {
            const float y = r.getY() + (dbTop - db) / (dbTop - dbBottom) * r.getHeight();
            g.setColour (tincture::same (db, 0.0f) ? colours::grid.brighter (0.6f) : colours::grid);
            g.drawHorizontalLine ((int) y, r.getX(), r.getRight());
            if (labels)
                drawText (g, String ((int) db), { r.getRight() - 34.0f, y - 7.0f, 30.0f, 14.0f }, 10.0f, colours::inkDim, false, Justification::centredRight);
        }
    }
    if (labels)
    {
        const std::vector<float> lab = linear ? std::vector<float> { 1000, 2000, 5000, 10000, 15000 }
                                              : std::vector<float> { 50, 100, 200, 500, 1000, 2000, 5000, 10000 };
        for (float hz : lab)
        {
            const float x = r.getX() + freqToNorm (hz) * r.getWidth();
            const String t = hz >= 1000 ? String ((int) (hz / 1000)) + "k" : String ((int) hz);
            drawText (g, t, { x - 20.0f, r.getBottom() - 14.0f, 40.0f, 12.0f }, 10.0f, colours::inkDim, false);
        }
    }
}

// ---------------------------------------------------------------------------
// Look and feel
// ---------------------------------------------------------------------------
PanelLookAndFeel::PanelLookAndFeel()
{
    setColour (PopupMenu::backgroundColourId, colours::panel);
    setColour (PopupMenu::textColourId, colours::ink);
    setColour (PopupMenu::headerTextColourId, colours::brand);
    setColour (PopupMenu::highlightedBackgroundColourId, colours::cream);
    setColour (PopupMenu::highlightedTextColourId, colours::black);
    setColour (BubbleComponent::backgroundColourId, colours::black);
    setColour (BubbleComponent::outlineColourId, colours::ink);
    setColour (TooltipWindow::backgroundColourId, colours::black);
    setColour (TooltipWindow::textColourId, colours::ink);
    setColour (TooltipWindow::outlineColourId, colours::inkDim);
    setColour (AlertWindow::backgroundColourId, colours::panel);
    setColour (AlertWindow::textColourId, colours::ink);
    setColour (AlertWindow::outlineColourId, colours::ink);
    setColour (TextEditor::backgroundColourId, colours::black);
    setColour (TextEditor::textColourId, colours::lcd);
    setColour (TextEditor::outlineColourId, colours::inkDim);
    setColour (TextEditor::focusedOutlineColourId, colours::ink);
    setColour (TextEditor::highlightColourId, colours::lcdDim);
    setColour (TextButton::buttonColourId, colours::panel);
    setColour (TextButton::textColourOffId, colours::ink);
    setColour (Slider::textBoxTextColourId, colours::ink);
    setColour (Label::textColourId, colours::ink);
    setColour (FileChooserDialogBox::titleTextColourId, colours::ink);
    setColour (ListBox::backgroundColourId, colours::black);
    setColour (ListBox::textColourId, colours::ink);
    setColour (DirectoryContentsDisplayComponent::textColourId, colours::ink);
    setColour (DirectoryContentsDisplayComponent::highlightColourId, colours::lcdDim);
    setColour (ComboBox::backgroundColourId, colours::black);
    setColour (ComboBox::textColourId, colours::lcd);
    setColour (ComboBox::outlineColourId, colours::inkDim);
    setColour (ScrollBar::thumbColourId, colours::inkDim);
}

void PanelLookAndFeel::drawRotarySlider (Graphics& g, int x, int y, int w, int h, float pos, float a0, float a1, Slider& s)
{
    const auto* k = dynamic_cast<Knob*> (&s);
    const bool bip = k != nullptr && k->bipolar;
    const Colour arcColour = (k != nullptr && ! k->accent.isTransparent()) ? k->accent : colours::ink;

    const auto bounds = Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const float r = jmin (bounds.getWidth(), bounds.getHeight()) * 0.5f - 4.0f;
    const auto  c = bounds.getCentre();

    Path arc;
    arc.addCentredArc (c.x, c.y, r, r, 0.0f, a0, a1, true);
    g.setColour (arcColour);
    g.strokePath (arc, PathStrokeType (jmax (1.5f, r * 0.06f), PathStrokeType::curved, PathStrokeType::butt));
    if (bip)
    {
        const float am = (a0 + a1) * 0.5f;
        g.drawLine (Line<float> (c.getPointOnCircumference (r - 1.0f, am), c.getPointOnCircumference (r + 4.0f, am)), 2.0f);
    }

    const float cr = r * 0.74f;
    Path cap;
    for (int i = 0; i < 7; ++i)
    {
        const auto p = c.getPointOnCircumference (cr, MathConstants<float>::twoPi * (float) i / 7.0f + 0.2f);
        if (i == 0) cap.startNewSubPath (p); else cap.lineTo (p);
    }
    cap.closeSubPath();
    cap = cap.createPathWithRoundedCorners (cr * 0.18f);
    g.setColour (colours::creamShade);
    g.fillEllipse (Rectangle<float> (cr * 2.12f, cr * 2.12f).withCentre (c));
    g.setColour (s.isEnabled() ? colours::cream : colours::creamShade);
    g.fillPath (cap);
    g.setColour (colours::creamShade.darker (0.25f));
    g.strokePath (cap, PathStrokeType (1.0f));

    const float ang = a0 + pos * (a1 - a0);
    g.setColour (colours::black);
    g.drawLine (Line<float> (c.getPointOnCircumference (cr * 0.12f, ang), c.getPointOnCircumference (cr * 0.86f, ang)), jmax (2.0f, r * 0.08f));
}

void PanelLookAndFeel::drawLinearSlider (Graphics& g, int x, int y, int w, int h, float pos, float, float, Slider::SliderStyle style, Slider& s)
{
    const auto* f = dynamic_cast<Fader*> (&s);
    const bool vertical = style == Slider::LinearVertical;
    const auto r = Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
    const Colour fill = f != nullptr ? f->fill : colours::lcd;
    const bool bip = f != nullptr && f->bipolar;
    const float scale = f != nullptr ? f->fillScale : 1.0f;

    if (vertical)
    {
        const auto slot = Rectangle<float> (6.0f, r.getHeight()).withCentre (r.getCentre());
        g.setColour (colours::black);
        g.fillRoundedRectangle (slot, 3.0f);
        g.setColour (colours::inkDim);
        g.drawRoundedRectangle (slot, 3.0f, 1.0f);
        const float mid = bip ? r.getCentreY() : r.getBottom();
        const float fy = mid + (pos - mid) * scale;
        const auto fillR = Rectangle<float> (slot.getX() + 1.0f, jmin (mid, fy), slot.getWidth() - 2.0f, std::abs (mid - fy));
        g.setColour (fill);
        g.fillRect (fillR);
        const auto handle = Rectangle<float> (22.0f, 12.0f).withCentre ({ r.getCentreX(), pos });
        g.setColour (colours::cream);
        g.fillRoundedRectangle (handle, 2.0f);
        g.setColour (colours::creamShade.darker (0.3f));
        g.drawRoundedRectangle (handle, 2.0f, 1.0f);
        g.setColour (colours::black);
        g.drawHorizontalLine ((int) pos, handle.getX() + 4.0f, handle.getRight() - 4.0f);
    }
    else
    {
        const auto slot = Rectangle<float> (r.getWidth(), 6.0f).withCentre (r.getCentre());
        g.setColour (colours::black);
        g.fillRoundedRectangle (slot, 3.0f);
        g.setColour (colours::inkDim);
        g.drawRoundedRectangle (slot, 3.0f, 1.0f);
        const float mid = bip ? r.getCentreX() : r.getX();
        const float fx = mid + (pos - mid) * scale;
        g.setColour (fill);
        g.fillRect (Rectangle<float> (jmin (mid, fx), slot.getY() + 1.0f, std::abs (fx - mid), slot.getHeight() - 2.0f));
        const auto handle = Rectangle<float> (12.0f, 22.0f).withCentre ({ pos, r.getCentreY() });
        g.setColour (colours::cream);
        g.fillRoundedRectangle (handle, 2.0f);
        g.setColour (colours::creamShade.darker (0.3f));
        g.drawRoundedRectangle (handle, 2.0f, 1.0f);
        g.setColour (colours::black);
        g.drawVerticalLine ((int) pos, handle.getY() + 4.0f, handle.getBottom() - 4.0f);
    }
}

// ---------------------------------------------------------------------------
// Knob
// ---------------------------------------------------------------------------
Knob::Knob()
{
    setSliderStyle (Slider::RotaryHorizontalVerticalDrag);
    setTextBoxStyle (Slider::NoTextBox, true, 0, 0);
    setRotaryParameters (MathConstants<float>::pi * 1.25f, MathConstants<float>::pi * 2.75f, true);
    setPopupDisplayEnabled (true, true, nullptr, 900);
    setMouseDragSensitivity (220);
    setScrollWheelEnabled (true);
}

double Knob::snapValue (double attempted, DragMode)
{
    if (bipolar)
    {
        const double mid = (getMinimum() + getMaximum()) * 0.5;
        if (std::abs (attempted - mid) < 0.02 * (getMaximum() - getMinimum()))
            return mid;
    }
    return attempted;
}

void Knob::paint (Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    const float capH = caption.isNotEmpty() ? (big ? 30.0f : (small ? 14.0f : 18.0f)) : 0.0f;
    const auto dial = area.withTrimmedBottom (capH);
    const float side = jmin (dial.getWidth(), dial.getHeight());
    const auto sq = Rectangle<float> (side, side).withCentre (dial.getCentre());
    getLookAndFeel().drawRotarySlider (g, (int) sq.getX(), (int) sq.getY(), (int) sq.getWidth(), (int) sq.getHeight(),
                                       (float) valueToProportionOfLength (getValue()),
                                       getRotaryParameters().startAngleRadians, getRotaryParameters().endAngleRadians, *this);
    if (capH > 0.0f)
        drawText (g, caption, area.removeFromBottom (capH), big ? 22.0f : (small ? 11.0f : 13.0f), isEnabled() ? colours::ink : colours::inkDim, true);
}

// ---------------------------------------------------------------------------
// Fader
// ---------------------------------------------------------------------------
Fader::Fader (bool vertical)
{
    setSliderStyle (vertical ? Slider::LinearVertical : Slider::LinearHorizontal);
    setTextBoxStyle (Slider::NoTextBox, true, 0, 0);
    setPopupDisplayEnabled (true, true, nullptr, 900);
    setScrollWheelEnabled (true);
}

double Fader::snapValue (double attempted, DragMode)
{
    if (bipolar)
    {
        const double mid = (getMinimum() + getMaximum()) * 0.5;
        if (std::abs (attempted - mid) < 0.015 * (getMaximum() - getMinimum()))
            return mid;
    }
    return attempted;
}

void Fader::paint (Graphics& g)
{
    auto area = getLocalBounds().toFloat();
    const bool vertical = getSliderStyle() == Slider::LinearVertical;
    const float capH = caption.isNotEmpty() ? 16.0f : 0.0f;
    auto slot = area.withTrimmedBottom (capH);
    if (vertical) slot = slot.reduced (0.0f, 8.0f); else slot = slot.reduced (8.0f, 0.0f);
    const float pos = (float) valueToProportionOfLength (getValue());
    const float p = vertical ? slot.getBottom() - pos * slot.getHeight() : slot.getX() + pos * slot.getWidth();
    getLookAndFeel().drawLinearSlider (g, (int) slot.getX(), (int) slot.getY(), (int) slot.getWidth(), (int) slot.getHeight(),
                                       p, 0.0f, 0.0f, getSliderStyle(), *this);
    if (capH > 0.0f)
        drawText (g, caption, area.removeFromBottom (capH), 11.0f, isEnabled() ? colours::ink : colours::inkDim, true);
}

// ---------------------------------------------------------------------------
// Readout
// ---------------------------------------------------------------------------
Readout::Readout()
{
    setSliderStyle (Slider::LinearBarVertical);
    setTextBoxStyle (Slider::NoTextBox, true, 0, 0);
    setPopupDisplayEnabled (false, false, nullptr);
    setMouseDragSensitivity (160);
    setScrollWheelEnabled (true);
    setVelocityBasedMode (true);
    setVelocityModeParameters (0.6, 1, 0.08, false);
}

void Readout::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    if (caption.isNotEmpty())
    {
        if (captionLeft)
            drawText (g, caption, r.removeFromLeft (jmin (r.getWidth() * 0.45f, 70.0f)), 11.0f, colours::ink, true, Justification::centredLeft);
        else
            drawText (g, caption, r.removeFromBottom (14.0f), 11.0f, isEnabled() ? colours::ink : colours::inkDim, true);
    }
    r = r.reduced (1.0f);
    drawLcdBox (g, r);
    drawText (g, getTextFromValue (getValue()), r.reduced (4.0f, 0.0f), fontH, isEnabled() ? colours::lcd : colours::lcdDim, true);
}

// ---------------------------------------------------------------------------
// PanelButton
// ---------------------------------------------------------------------------
PanelButton::PanelButton (String l) : label (std::move (l)) {}

void PanelButton::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    const bool on = lit != pressed;
    g.setColour (on ? litColour : colours::black);
    g.fillRect (r);
    g.setColour (dimmed ? colours::inkDim : colours::ink);
    g.drawRect (r, 1.5f);
    drawText (g, label, r, fontH, on ? colours::black : (dimmed ? colours::inkDim : colours::ink), true);
}

void PanelButton::mouseDown (const MouseEvent&)
{
    pressed = true;
    repaint();
    if (onPress) onPress();
}

void PanelButton::mouseUp (const MouseEvent& e)
{
    pressed = false;
    repaint();
    if (onRelease) onRelease();
    if (getLocalBounds().contains (e.getPosition()) && onClick)
        onClick (e.mods);
}

// ---------------------------------------------------------------------------
// ChoiceDisplay
// ---------------------------------------------------------------------------
void ChoiceDisplay::refresh()
{
    const auto t = getText ? getText() : String();
    if (t != shown) { shown = t; repaint(); }
}

void ChoiceDisplay::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    if (caption.isNotEmpty())
    {
        if (captionLeft)
            drawText (g, caption, r.removeFromLeft (jmin (r.getWidth() * 0.45f, 80.0f)), 11.0f, colours::ink, true, Justification::centredLeft);
        else
            drawText (g, caption, r.removeFromBottom (14.0f), 11.0f, colours::ink, true);
    }
    r = r.reduced (1.0f);
    drawLcdBox (g, r);
    const float ax = r.getRight() - 10.0f, ay = r.getCentreY();
    Path tri;
    tri.addTriangle (ax - 4.0f, ay - 2.0f, ax + 4.0f, ay - 2.0f, ax, ay + 3.0f);
    g.setColour (colours::inkDim);
    g.fillPath (tri);
    shown = getText ? getText() : String();
    drawText (g, shown, r.withTrimmedRight (16.0f).reduced (5.0f, 0.0f), fontH, colours::lcd, true, Justification::centredLeft);
}

// ---------------------------------------------------------------------------
// Meters
// ---------------------------------------------------------------------------
namespace { constexpr int kSegments = 20; }

void Meter::push (float peak)
{
    level = std::max (peak, level * 0.85f);
    if (peak >= hold) { hold = peak; holdFrames = 30; }
    else if (--holdFrames < 0) hold *= 0.92f;
    auto segs = [this] (float v)
    {
        if (v <= 0.0f) return 0;
        const float db = 20.0f * std::log10 (v);
        return jlimit (0, kSegments, (int) std::ceil ((db - floorDb_) / -floorDb_ * kSegments));
    };
    const int s = segs (level), h = segs (hold);
    if (s != shownSegs || h != shownHold) { shownSegs = s; shownHold = h; repaint(); }
}

void Meter::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat();
    const float segH = r.getHeight() / (float) kSegments;
    for (int i = 0; i < kSegments; ++i)
    {
        const auto seg = Rectangle<float> (r.getX(), r.getBottom() - (float) (i + 1) * segH, r.getWidth(), segH).reduced (0.0f, 1.0f);
        const Colour c = i >= kSegments - 1 ? colours::ledRed : i >= kSegments - 5 ? colours::ledYellow : colours::ledGreen;
        const bool lit = i < shownSegs || i == shownHold - 1;
        g.setColour (lit ? c : colours::socket);
        g.fillRect (seg);
    }
}

void BarMeter::paint (Graphics& g)
{
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (colours::black);
    g.fillRect (r);
    g.setColour (colours::inkDim);
    g.drawRect (r, 1.0f);
    // Unipolar: fill from the end nearest zero (a gain-reduction meter fills from the right).
    const float zero = bipolar ? r.getX() + (0.0f - rangeMin) / (rangeMax - rangeMin) * r.getWidth() : (rangeMax <= 0.0f ? r.getRight() : r.getX());
    const float x = r.getX() + (value - rangeMin) / (rangeMax - rangeMin) * r.getWidth();
    g.setColour (colour);
    g.fillRect (Rectangle<float> (jmin (zero, x), r.getY() + 2.0f, std::abs (x - zero), r.getHeight() - 4.0f));
    if (bipolar)
    {
        g.setColour (colours::inkDim);
        g.drawVerticalLine ((int) zero, r.getY(), r.getBottom());
    }
}
} // namespace tincture::ui
