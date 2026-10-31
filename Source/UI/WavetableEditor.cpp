// WavetableEditor.cpp — the Wavetable Editor overlay (spec E §3–5). See
// WavetableEditor.h for the data flow.
#include "WavetableEditor.h"
#include "../PluginProcessor.h"
#include "../Settings.h"

#include <juce_audio_formats/juce_audio_formats.h>

namespace tincture::ui
{
using namespace juce;
namespace we = tincture::wtedit;

namespace
{
// ---------------------------------------------------------------------------
// Layout (editor-local, 1400 x 392)
// ---------------------------------------------------------------------------
constexpr int kTop = 22, kRowH = 24;                 // header row
constexpr int kBrushX = 16, kBrushW = 96;
constexpr int kMidX = 122, kMidW = 1028;             // FFT + waveform column
constexpr int kRightX = 1160, kRightW = 224;
constexpr int kFftY = 56, kFftH = 94;
constexpr int kWaveY = 158, kWaveH = 138;
constexpr int kThumbY = 304, kThumbH = 42;
constexpr int kBarY = 352;

const Colour kBlue { 0xff5cc8f2 };

struct TableCache
{
    std::weak_ptr<const Wavetable> table;
    we::FrameSet set;
    we::MorphMode mode = we::MorphMode::Crossfade;
};
TableCache& cacheFor (int osc)
{
    static TableCache c[kNumOscs];
    return c[jlimit (0, kNumOscs - 1, osc)];
}

std::vector<we::Frame>& clipboard()
{
    static std::vector<we::Frame> c;
    return c;
}

bool isAudioFile (const File& f)
{
    return f.hasFileExtension ("wav;aif;aiff;flac");
}

void glyph (Graphics& g, Rectangle<float> r, int index, Colour c)
{
    g.setColour (c);
    const auto yOf = [&] (float v) { return r.getCentreY() - v * r.getHeight() * 0.5f; };
    if (index == (int) we::Brush::Nudge)
    {
        const float x = r.getCentreX();
        g.drawArrow ({ x, r.getCentreY(), x, r.getY() }, 1.6f, 5.0f, 5.0f);
        g.drawArrow ({ x, r.getCentreY(), x, r.getBottom() }, 1.6f, 5.0f, 5.0f);
        return;
    }
    Path p;
    const int n = 28;
    Rng rng (7);
    for (int i = 0; i <= n; ++i)
    {
        const float t = (float) i / (float) n;
        float v = 0.0f;
        switch ((we::Brush) index)
        {
            case we::Brush::FlatLine:          v = 0.0f; break;
            case we::Brush::Sine:              v = std::sin (kTwoPi * t); break;
            case we::Brush::SlopeUp:           v = -1.0f + 2.0f * t; break;
            case we::Brush::SlopeDown:         v = 1.0f - 2.0f * t; break;
            case we::Brush::CurveUp:           v = -1.0f + 2.0f * std::sin (kPi * 0.5f * t); break;
            case we::Brush::CurveDown:         v = 1.0f - 2.0f * std::sin (kPi * 0.5f * t); break;
            case we::Brush::HalfSinePeak:      v = -0.8f + 1.6f * std::sin (kPi * t); break;
            case we::Brush::HalfSineValley:    v = 0.8f - 1.6f * std::sin (kPi * t); break;
            case we::Brush::InterpolateLinear: v = -0.7f + 1.4f * t; break;
            case we::Brush::InterpolateCurved: v = -0.7f + 1.4f * (0.5f - 0.5f * std::cos (kPi * t)); break;
            case we::Brush::Noise:             v = (i == 0 || i == n) ? 0.0f : rng.bi() * 0.9f; break;
            case we::Brush::Nudge:
            case we::Brush::NumBrushes:        break;
        }
        const Point<float> pt (r.getX() + t * r.getWidth(), yOf (v));
        if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
    }
    g.strokePath (p, PathStrokeType (1.6f, PathStrokeType::curved, PathStrokeType::rounded));
    if (index == (int) we::Brush::InterpolateLinear || index == (int) we::Brush::InterpolateCurved)
    {
        g.fillEllipse (Rectangle<float> (5.0f, 5.0f).withCentre ({ r.getX(), yOf (-0.7f) }));
        g.fillEllipse (Rectangle<float> (5.0f, 5.0f).withCentre ({ r.getRight(), yOf (0.7f) }));
    }
}

void triangle (Graphics& g, Point<float> c, float w, Colour col)
{
    Path t;
    t.addTriangle (c.x - w * 0.5f, c.y - w * 0.3f, c.x + w * 0.5f, c.y - w * 0.3f, c.x, c.y + w * 0.35f);
    g.setColour (col);
    g.fillPath (t);
}

Path framePath (const float* f, int n, Rectangle<float> r, int points)
{
    Path p;
    points = jmax (2, points);
    for (int i = 0; i <= points; ++i)
    {
        const int idx = jmin (n - 1, i * n / points);
        const float v = jlimit (-1.0f, 1.0f, i == points ? f[0] : f[idx]);
        const Point<float> pt (r.getX() + r.getWidth() * (float) i / (float) points, r.getCentreY() - v * r.getHeight() * 0.5f);
        if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
    }
    return p;
}
} // namespace

// ===========================================================================
// Helpers
// ===========================================================================
we::FrameSet frameSetFromTable (const Wavetable* t)
{
    if (t == nullptr) return we::FrameSet::initSaw();
    we::FrameSet s;
    s.name = t->name;
    s.modified = ! t->factory;
    const int n = jlimit (1, we::kMaxFrames, t->numFrames());
    s.frames.resize ((size_t) n);
    for (int i = 0; i < n; ++i) t->renderFrame (i, s.frames[(size_t) i].data());
    if (t->morph() != Wavetable::Morph::None && t->numSourceFrames() > 1)
        for (int i = 0; i < n; ++i)
            if (t->isSourceFrame (i)) s.keyFrames.push_back (i);
    return s;
}

MemoryBlock makeWav (const std::vector<float>& samples, int bits, bool clm)
{
    MemoryBlock block;
    {
        std::unique_ptr<OutputStream> out = std::make_unique<MemoryOutputStream> (block, false);
        WavAudioFormat wav;
        const auto opts = AudioFormatWriterOptions {}.withSampleRate (44100.0).withNumChannels (1).withBitsPerSample (bits)
                              .withSampleFormat (bits == 32 ? AudioFormatWriterOptions::SampleFormat::floatingPoint
                                                            : AudioFormatWriterOptions::SampleFormat::integral);
        auto writer = wav.createWriterFor (out, opts);
        if (writer == nullptr) return {};
        const float* ch[] { samples.data() };
        writer->writeFromFloatArrays (ch, 1, (int) samples.size());
    }
    if (clm && block.getSize() > 12)
    {
        std::string text = we::makeClmChunk (we::kFrameSize, true);
        const uint32_t len = (uint32_t) text.size();
        if (text.size() % 2) text.push_back ('\0');
        MemoryBlock chunk;
        chunk.append ("clm ", 4);
        const uint8_t sz[4] { (uint8_t) len, (uint8_t) (len >> 8), (uint8_t) (len >> 16), (uint8_t) (len >> 24) };
        chunk.append (sz, 4);
        chunk.append (text.data(), text.size());
        block.insert (chunk.getData(), chunk.getSize(), 12);
        auto* d = static_cast<uint8_t*> (block.getData());
        uint32_t riff = (uint32_t) d[4] | ((uint32_t) d[5] << 8) | ((uint32_t) d[6] << 16) | ((uint32_t) d[7] << 24);
        riff += (uint32_t) chunk.getSize();
        for (int i = 0; i < 4; ++i) d[4 + i] = (uint8_t) (riff >> (8 * i));
    }
    return block;
}

namespace wte
{
// ===========================================================================
// Stepper
// ===========================================================================
Rectangle<int> Stepper::valueArea() const
{
    auto r = getLocalBounds();
    if (caption.isNotEmpty()) r.removeFromLeft (48);
    return r;
}

void Stepper::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat();
    if (caption.isNotEmpty())
        drawText (g, caption, r.removeFromLeft (48.0f), 11.0f, colours::ink, true, Justification::centredLeft);
    r = r.reduced (0.5f);
    drawLcdBox (g, r);
    drawText (g, "<", r.removeFromLeft (16.0f), 12.0f, colours::inkDim, true);
    drawText (g, ">", r.removeFromRight (16.0f), 12.0f, colours::inkDim, true);
    const int v = get ? get() : 0;
    drawText (g, format ? format (v) : String (v), r, 13.0f, colours::lcd, true);
}

void Stepper::step (int dir)
{
    if (values.empty() || ! get || ! set) return;
    const int v = get();
    int idx = 0;
    for (int i = 0; i < (int) values.size(); ++i) if (values[(size_t) i] <= v) idx = i;
    idx = jlimit (0, (int) values.size() - 1, idx + dir);
    set (values[(size_t) idx]);
    repaint();
}

void Stepper::mouseDown (const MouseEvent& e)
{
    const auto r = valueArea();
    if (e.x < r.getX() + 18) { step (-1); return; }
    if (e.x > r.getRight() - 18) { step (1); return; }
    Menu m;
    const int cur = get ? get() : 0;
    for (int v : values)
        m.add (format ? format (v) : String (v), [safe = SafePointer<Stepper> (this), v] { if (safe != nullptr && safe->set) { safe->set (v); safe->repaint(); } }, v == cur);
    MenuOptions o;
    o.matchAnchorWidth = true;
    showMenu (*this, std::move (m), o);
}

void Stepper::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    if (! same (w.deltaY, 0.0f)) step (w.deltaY > 0 ? 1 : -1);
}

// ===========================================================================
// BrushButton / DropButton / NameDisplay
// ===========================================================================
BrushButton::BrushButton (WavetableEditor& e, int i) : index (i), ed (e)
{
    setTooltip (i < 0 ? String ("Mirror: draw point-mirrored about the centre") : String (we::brushName ((we::Brush) i)));
}

void BrushButton::paint (Graphics& g)
{
    const bool lit = index < 0 ? ed.mirror : (int) ed.brush == index;
    const auto r = getLocalBounds().toFloat().reduced (1.0f);
    g.setColour (lit ? colours::cream : colours::black);
    g.fillRect (r);
    g.setColour (colours::ink);
    g.drawRect (r, 1.5f);
    const Colour c = lit ? colours::black : colours::ink;
    if (index < 0) drawText (g, "MIRROR", r, 12.0f, c, true);
    else glyph (g, r.reduced (9.0f, 7.0f), index, c);
}

void BrushButton::mouseDown (const MouseEvent&)
{
    if (index < 0) ed.mirror = ! ed.mirror;
    else ed.brush = (we::Brush) index;
    for (auto& b : ed.brushButtons) b->repaint();
    ed.waveView.repaint();
}

void DropButton::paint (Graphics& g)
{
    PanelButton::paint (g);
    const auto r = getLocalBounds().toFloat();
    const bool on = isLit();
    if (getLabel().isEmpty()) triangle (g, r.getCentre(), 9.0f, on ? colours::black : colours::ink);
    else triangle (g, { r.getRight() - 11.0f, r.getCentreY() }, 7.0f, on ? colours::black : colours::inkDim);
}

void NameDisplay::paint (Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (0.5f);
    drawLcdBox (g, r);
    if (ed.set.modified)
    {
        g.setColour (ed.accent().withAlpha (0.16f));
        g.fillRect (r.reduced (1.0f));
    }
    triangle (g, { r.getRight() - 11.0f, r.getCentreY() }, 7.0f, colours::lcd);
    auto t = r.reduced (8.0f, 0.0f).withTrimmedRight (14.0f);
    if (ed.set.modified)
        drawText (g, "CUSTOM", t.removeFromRight (56.0f), 10.5f, ed.accent(), true, Justification::centredRight);
    drawText (g, String (ed.set.name), t, 14.0f, colours::lcd, true, Justification::centredLeft);
}

void NameDisplay::mouseDown (const MouseEvent&)
{
    showMenu (*this, ed.nameMenu());
}

// ===========================================================================
// WaveView
// ===========================================================================
float WaveView::toX01 (float px) const
{
    const auto r = getLocalBounds().toFloat().reduced (2.0f);
    return jlimit (0.0f, 0.99999f, (px - r.getX()) / r.getWidth());
}

float WaveView::toY (float py) const
{
    const auto r = getLocalBounds().toFloat().reduced (2.0f, 6.0f);
    return jlimit (-1.0f, 1.0f, 1.0f - 2.0f * (py - r.getY()) / r.getHeight());
}

void WaveView::paint (Graphics& g)
{
    const auto box = getLocalBounds().toFloat();
    drawLcdBox (g, box);
    const auto r = box.reduced (2.0f, 6.0f);
    // Grid.
    g.setColour (colours::grid);
    if (ed.gridX > 1 && ed.gridX <= 128)
        for (int i = 1; i < ed.gridX; ++i)
            g.drawVerticalLine (roundToInt (r.getX() + r.getWidth() * (float) i / (float) ed.gridX), r.getY(), r.getBottom());
    if (ed.gridY > 1)
        for (int i = 1; i < ed.gridY; ++i)
            g.drawHorizontalLine (roundToInt (r.getY() + r.getHeight() * (float) i / (float) ed.gridY), r.getX(), r.getRight());
    g.setColour (colours::grid.brighter (0.5f));
    g.drawHorizontalLine (roundToInt (r.getCentreY()), r.getX(), r.getRight());
    if (ed.mirror)
    {
        g.setColour (ed.accent().withAlpha (0.45f));
        const float dash[] { 4.0f, 4.0f };
        g.drawDashedLine ({ r.getCentreX(), r.getY(), r.getCentreX(), r.getBottom() }, dash, 2, 1.0f);
    }

    const auto& f = ed.current();
    auto p = framePath (f.data(), we::kFrameSize, r, (int) r.getWidth() * 2);
    Path fill (p);
    fill.lineTo (r.getRight(), r.getCentreY());
    fill.lineTo (r.getX(), r.getCentreY());
    fill.closeSubPath();
    g.setColour (ed.accent().withAlpha (0.10f));
    g.fillPath (fill);
    g.setColour (ed.accent());
    g.strokePath (p, PathStrokeType (1.6f));

    const auto label = box.reduced (8.0f, 4.0f).removeFromTop (14.0f);
    String left = "FRAME " + String (ed.sel.current + 1) + " / " + String (ed.set.size());
    if (ed.set.morphed()) left << (ed.set.isKeyFrame (ed.sel.current) ? "  KEY" : "  MORPHED");
    const auto lf = panelFont (11.0f);
    g.setColour (colours::black.withAlpha (0.8f));
    g.fillRect (label.withWidth (GlyphArrangement::getStringWidth (lf, left) + 8.0f).translated (-4.0f, 0.0f));
    drawText (g, left, label, 11.0f, colours::lcd, true, Justification::centredLeft);
    String right = String (we::brushName (ed.brush)).toUpperCase();
    right << "   GRID " << (ed.gridX > 0 ? String (ed.gridX) : String ("OFF")) << " x " << (ed.gridY > 0 ? String (ed.gridY) : String ("OFF"));
    if (ed.mirror) right << "   MIRROR";
    const float rw = GlyphArrangement::getStringWidth (lf, right) + 8.0f;
    g.setColour (colours::black.withAlpha (0.8f));
    g.fillRect (label.withLeft (label.getRight() - rw).translated (4.0f, 0.0f));
    drawText (g, right, label, 11.0f, colours::inkDim, true, Justification::centredRight);
}

void WaveView::mouseDown (const MouseEvent& e)
{
    if (e.mods.isPopupMenu()) return;
    stroking = true;
    changed = false;
    last = e.position;
    strokeTo (e.position, true);
}

void WaveView::mouseDrag (const MouseEvent& e)
{
    if (! stroking) return;
    strokeTo (e.position, false);
    last = e.position;
}

void WaveView::mouseUp (const MouseEvent&)
{
    if (! stroking) return;
    stroking = false;
    if (! changed) return;
    if (ed.set.morphed()) we::morph (ed.set, ed.morphMode);
    ed.set.modified = true;
    ed.framesChanged();
    ed.commit ("Draw Wavetable");
}

void WaveView::strokeTo (Point<float> p, bool first)
{
    auto& f = ed.current();
    const auto b = ed.brush;
    const float x0 = toX01 (last.x), x1 = toX01 (p.x);
    bool c = false;
    if (b == we::Brush::Nudge || b == we::Brush::Noise)
    {
        if (first) return;
        const float dy = toY (p.y) - toY (last.y);
        if (! same (dy, 0.0f)) c = we::applyBrush (f, b, x1, b == we::Brush::Noise ? dy * 1.5f : dy, ed.gridX, ed.mirror, rngFor());
    }
    else if (ed.gridX <= 0 && b == we::Brush::FlatLine)
    {
        c = we::drawLine (f, first ? x1 : x0, first ? toY (p.y) : toY (last.y), x1, toY (p.y), ed.mirror);
    }
    else
    {
        const float y0 = we::snapY (toY (last.y), ed.gridY), y1 = we::snapY (toY (p.y), ed.gridY);
        const int c1 = we::gridCell (x1, ed.gridX);
        const int c0 = first ? c1 : we::gridCell (x0, ed.gridX);
        const int dir = c1 >= c0 ? 1 : -1;
        for (int cell = c0;; cell += dir)
        {
            int s = 0, e = 0;
            we::cellRange (cell, ed.gridX, s, e);
            const float t = c1 == c0 ? 1.0f : (float) (cell - c0) / (float) (c1 - c0);
            const float y = y0 + (y1 - y0) * t;
            c |= we::applyBrush (f, b, ((float) s + (float) e) * 0.5f / (float) we::kFrameSize, y, ed.gridX, ed.mirror, rngFor());
            if (cell == c1) break;
        }
    }
    if (c)
    {
        changed = true;
        ed.framesChanged();
    }
}

// ===========================================================================
// FftView
// ===========================================================================
Rectangle<float> FftView::magArea() const   { return getLocalBounds().toFloat().withTrimmedBottom (32.0f); }
Rectangle<float> FftView::phaseArea() const { return getLocalBounds().toFloat().withTrimmedTop ((float) getHeight() - 29.0f).withHeight (20.0f); }
Rectangle<float> FftView::scrollArea() const { return getLocalBounds().toFloat().removeFromBottom (6.0f); }

int FftView::binAt (float x) const
{
    const auto r = magArea().reduced (1.0f);
    const int i = (int) std::floor ((x - r.getX()) * (float) ed.visibleBins() / r.getWidth());
    return jlimit (1, we::kFftBins, ed.fftFirst + 1 + jlimit (0, ed.visibleBins() - 1, i));
}

float FftView::binX (int bin) const
{
    const auto r = magArea().reduced (1.0f);
    return r.getX() + (float) (bin - 1 - ed.fftFirst) * r.getWidth() / (float) ed.visibleBins();
}

void FftView::paint (Graphics& g)
{
    const auto& s = ed.spectrum();
    const auto ma = magArea(), pa = phaseArea();
    drawLcdBox (g, ma);
    drawLcdBox (g, pa);
    const auto mr = ma.reduced (1.0f), pr = pa.reduced (1.0f);
    const int vis = ed.visibleBins();
    const float bw = mr.getWidth() / (float) vis;

    // Octave markers (labels where there is room).
    float lastLabel = -100.0f;
    for (int b = 2; b <= we::kFftBins; b *= 2)
    {
        if (b - 1 < ed.fftFirst || b - 1 >= ed.fftFirst + vis) continue;
        const float x = binX (b);
        g.setColour (colours::grid);
        g.drawVerticalLine (roundToInt (x), mr.getY(), mr.getBottom());
        g.drawVerticalLine (roundToInt (x), pr.getY(), pr.getBottom());
        if (x - lastLabel >= 26.0f)
        {
            drawText (g, String (b), { x + 2.0f, mr.getY() + 2.0f, 30.0f, 12.0f }, 10.5f, colours::inkFaint, true, Justification::centredLeft);
            lastLabel = x;
        }
    }
    if (hoverBin > 0)
    {
        g.setColour (colours::ink.withAlpha (0.10f));
        g.fillRect (Rectangle<float> (binX (hoverBin), mr.getY(), jmax (1.0f, bw), mr.getHeight()));
        g.fillRect (Rectangle<float> (binX (hoverBin), pr.getY(), jmax (1.0f, bw), pr.getHeight()));
    }

    const float gap = bw >= 4.0f ? 1.0f : 0.0f;
    const float mid = pr.getCentreY();
    g.setColour (colours::grid.brighter (0.4f));
    g.drawHorizontalLine (roundToInt (mid), pr.getX(), pr.getRight());
    for (int i = 0; i < vis; ++i)
    {
        const int bin = ed.fftFirst + 1 + i;
        const float x = mr.getX() + (float) i * bw;
        const float bar = jlimit (0.0f, 1.0f, we::magToBar (s.mag[(size_t) bin], bin, ed.fftOpts));
        const bool dim = ! we::binDrawable (bin, ed.fftOpts);
        if (bar > 0.002f)
        {
            g.setColour (dim ? ed.accent().withAlpha (0.4f) : ed.accent());
            const float h = jmax (1.0f, bar * mr.getHeight());
            g.fillRect (Rectangle<float> (x, mr.getBottom() - h, jmax (1.0f, bw - gap), h));
        }
        if (std::abs (s.mag[(size_t) bin]) > 1.0e-4f)
        {
            const float pb = we::phaseToBar (s.phase[(size_t) bin]);
            const float y = pr.getBottom() - pb * pr.getHeight();
            // A short mark at the phase value (bars from the centre turn a saw's
            // +-180 degree phases into a solid block).
            g.setColour (colours::lcd.withAlpha (dim ? 0.4f : 0.9f));
            g.fillRect (Rectangle<float> (x, jlimit (pr.getY(), pr.getBottom() - 3.0f, y - 1.5f), jmax (1.0f, bw - gap), 3.0f));
        }
    }

    // Labels and the readout of the hovered bin.
    auto tag = [&g] (const String& t, Rectangle<float> area) mutable
    {
        const float w = GlyphArrangement::getStringWidth (panelFont (10.5f), t) + 8.0f;
        auto r = area.withLeft (area.getRight() - w);
        g.setColour (colours::black.withAlpha (0.85f));
        g.fillRect (r);
        drawText (g, t, r, 10.5f, colours::inkDim, true);
    };
    tag ("MAG", mr.withTop (mr.getBottom() - 13.0f).withTrimmedRight (2.0f));
    tag ("PHASE", pr.withTrimmedRight (2.0f));
    if (hoverBin > 0)
    {
        String t = "BIN " + String (hoverBin);
        t << "   " << String (s.mag[(size_t) hoverBin], 3) << "   " << String (roundToInt (s.phase[(size_t) hoverBin] * 180.0f / kPi)) << String (CharPointer_UTF8 ("\xc2\xb0"));
        drawText (g, t, mr.reduced (8.0f, 3.0f).removeFromTop (13.0f), 11.0f, colours::lcd, true, Justification::centredRight);
    }

    // Scroll bar.
    const auto sb = scrollArea();
    g.setColour (colours::black);
    g.fillRect (sb);
    const float t0 = (float) ed.fftFirst / (float) we::kFftBins, t1 = (float) (ed.fftFirst + vis) / (float) we::kFftBins;
    g.setColour (ed.zoom > 1 ? colours::inkDim : colours::grid);
    g.fillRect (Rectangle<float> (sb.getX() + t0 * sb.getWidth(), sb.getY() + 1.0f, (t1 - t0) * sb.getWidth(), sb.getHeight() - 2.0f));
}

void FftView::mouseMove (const MouseEvent& e)
{
    const int b = (magArea().contains (e.position) || phaseArea().contains (e.position)) ? binAt (e.position.x) : -1;
    if (b != hoverBin) { hoverBin = b; repaint(); }
}

void FftView::mouseExit (const MouseEvent&)
{
    if (hoverBin != -1) { hoverBin = -1; repaint(); }
}

void FftView::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    if (ed.zoom <= 1) return;
    const float d = ! same (w.deltaX, 0.0f) ? w.deltaX : w.deltaY;
    const int step = jmax (1, ed.visibleBins() / 8);
    ed.fftFirst = jlimit (0, we::kFftBins - ed.visibleBins(), ed.fftFirst + (d < 0 ? step : -step));
    repaint();
}

void FftView::mouseDown (const MouseEvent& e)
{
    drag = Drag::None;
    changed = false;
    const bool inMag = magArea().contains (e.position), inPhase = phaseArea().contains (e.position);
    if (e.mods.isPopupMenu())
    {
        if (inMag || inPhase)
            showContextMenu (*this, e.getPosition(), ed.buildWaveMenu (inPhase ? we::WaveMenu::FftPhases : we::WaveMenu::FftMags, binAt (e.position.x)));
        return;
    }
    if (scrollArea().contains (e.position))
    {
        drag = Drag::Scroll;
        mouseDrag (e);
        return;
    }
    if (inPhase && e.mods.isShiftDown() && (e.mods.isCommandDown() || e.mods.isCtrlDown()))
    {
        if (we::nudgePhase (ed.current(), binAt (e.position.x), kPi / 8.0f))
        {
            ed.set.modified = true;
            if (ed.set.morphed()) we::morph (ed.set, ed.morphMode);
            ed.framesChanged();
            ed.commit ("Nudge Phase");
        }
        return;
    }
    if (! inMag && ! inPhase) return;
    drag = inMag ? Drag::Mag : Drag::Phase;
    work = ed.spectrum();
    lastBin = -1;
    paintTo (e, true);
}

void FftView::mouseDrag (const MouseEvent& e)
{
    if (drag == Drag::Scroll)
    {
        const auto sb = scrollArea();
        const int vis = ed.visibleBins();
        const float t = (e.position.x - sb.getX()) / sb.getWidth();
        ed.fftFirst = jlimit (0, we::kFftBins - vis, roundToInt (t * (float) we::kFftBins - (float) vis * 0.5f));
        repaint();
        return;
    }
    if (drag == Drag::Mag || drag == Drag::Phase) paintTo (e, false);
    mouseMove (e);
}

void FftView::mouseUp (const MouseEvent&)
{
    const auto d = drag;
    drag = Drag::None;
    if (! changed || (d != Drag::Mag && d != Drag::Phase)) return;
    ed.set.modified = true;
    if (ed.set.morphed()) we::morph (ed.set, ed.morphMode);
    ed.framesChanged();
    ed.commit (d == Drag::Mag ? "Draw FFT Magnitudes" : "Draw FFT Phases");
}

void FftView::paintTo (const MouseEvent& e, bool first)
{
    const auto area = (drag == Drag::Mag ? magArea() : phaseArea()).reduced (1.0f);
    const float bar = jlimit (0.0f, 1.0f, 1.0f - (e.position.y - area.getY()) / area.getHeight());
    const int bin = binAt (e.position.x);
    const int from = (first || lastBin < 0) ? bin : lastBin;
    const float fromBar = (first || lastBin < 0) ? bar : lastBar;
    bool c = false;
    const int dir = bin >= from ? 1 : -1;
    for (int b = from;; b += dir)
    {
        const float t = bin == from ? 1.0f : (float) (b - from) / (float) (bin - from);
        const float v = fromBar + (bar - fromBar) * t;
        c |= drag == Drag::Mag ? we::drawMagnitude (work, b, v, ed.fftOpts) : we::drawPhase (work, b, v, ed.fftOpts);
        if (b == bin) break;
    }
    lastBin = bin;
    lastBar = bar;
    if (c)
    {
        changed = true;
        we::synthesise (work, ed.current());
        ed.framesChanged();
        ed.setSpectrum (work);
    }
}

// ===========================================================================
// ThumbStrip
// ===========================================================================
int ThumbStrip::count() const { return ed.set.morphed() ? (int) ed.set.keyFrames.size() : ed.set.size(); }
int ThumbStrip::frameOf (int t) const
{
    if (! ed.set.morphed()) return t;
    return ed.set.keyFrames[(size_t) jlimit (0, (int) ed.set.keyFrames.size() - 1, t)];
}
int ThumbStrip::thumbOf (int frame) const
{
    if (! ed.set.morphed()) return frame;
    int t = 0;
    for (int i = 0; i < (int) ed.set.keyFrames.size(); ++i) if (ed.set.keyFrames[(size_t) i] <= frame) t = i;
    return t;
}
Rectangle<int> ThumbStrip::thumbBounds (int t) const { return { 2 + t * (kThumbW + kGap) - scroll, 1, kThumbW, getHeight() - 2 }; }
Rectangle<int> ThumbStrip::plusBounds() const { return thumbBounds (count()).withWidth (28); }
int ThumbStrip::maxScroll() const { return jmax (0, plusBounds().getRight() + scroll + 2 - getWidth()); }

int ThumbStrip::thumbAt (Point<int> p) const
{
    const int t = (p.x + scroll - 2) / (kThumbW + kGap);
    if (p.x + scroll - 2 < 0 || t >= count()) return -1;
    return thumbBounds (t).contains (p) ? t : -1;
}

void ThumbStrip::ensureVisible (int t)
{
    const auto b = thumbBounds (t);
    if (b.getX() < 2) scroll += b.getX() - 2;
    else if (b.getRight() > getWidth() - 32) scroll += b.getRight() - (getWidth() - 32);
    scroll = jlimit (0, maxScroll(), scroll);
    repaint();
}

void ThumbStrip::paint (Graphics& g)
{
    const int n = count();
    const int curThumb = thumbOf (ed.sel.current);
    const int first = jmax (0, (scroll - 2) / (kThumbW + kGap));
    for (int t = first; t < n; ++t)
    {
        const auto b = thumbBounds (t);
        if (b.getX() > getWidth()) break;
        const int fi = frameOf (t);
        const auto r = b.toFloat();
        drawLcdBox (g, r);
        const bool cur = t == curThumb;
        const bool inSel = ! ed.set.morphed() && ed.sel.count() > 1 && ed.sel.contains (fi);
        g.setColour ((cur ? ed.accent() : colours::ink.withAlpha (0.7f)));
        g.strokePath (framePath (ed.set.frames[(size_t) fi].data(), we::kFrameSize, r.reduced (3.0f, 8.0f).translated (0.0f, 4.0f), kThumbW), PathStrokeType (1.0f));
        const auto lab = Rectangle<float> (r.getX() + 1.0f, r.getY() + 1.0f, 26.0f, 13.0f);
        if (cur)
        {
            g.setColour (kBlue);
            g.fillRect (lab);
        }
        drawText (g, String (fi + 1), lab.withTrimmedLeft (3.0f), 10.5f, cur ? colours::black : colours::inkDim, true, Justification::centredLeft);
        if (inSel || t == dropThumb)
        {
            g.setColour (colours::ledYellow);
            g.drawRect (r, 1.5f);
        }
        else if (cur)
        {
            g.setColour (kBlue);
            g.drawRect (r, 1.5f);
        }
    }
    const auto pb = plusBounds().toFloat();
    if (pb.getX() < (float) getWidth())
    {
        g.setColour (colours::inkDim);
        g.drawRect (pb.reduced (0.5f), 1.0f);
        drawText (g, "+", pb, 16.0f, ed.set.size() < we::kMaxFrames ? colours::ink : colours::inkFaint, true);
    }
    if (dragging && insertAt >= 0)
    {
        const float x = (float) (2 + insertAt * (kThumbW + kGap) - scroll) - (float) kGap * 0.5f - 0.5f;
        g.setColour (colours::ledYellow);
        g.fillRect (Rectangle<float> (x - 1.0f, 0.0f, 3.0f, (float) getHeight()));
    }
    if (scroll > 0 || maxScroll() > scroll)
    {
        // A thin position bar under the strip when it scrolls.
        const float total = (float) (maxScroll() + getWidth());
        g.setColour (colours::inkDim);
        g.fillRect (Rectangle<float> ((float) scroll / total * (float) getWidth(), (float) getHeight() - 1.0f, (float) getWidth() * (float) getWidth() / total, 1.0f));
    }
}

void ThumbStrip::mouseDown (const MouseEvent& e)
{
    dragging = false;
    insertAt = -1;
    pressThumb = -1;
    if (plusBounds().contains (e.getPosition()))
    {
        if (we::appendFrame (ed.set, ed.sel))
        {
            ed.set.modified = true;
            ed.framesChanged();
            ed.commit ("Add Frame");
            ensureVisible (thumbOf (ed.sel.current));
        }
        return;
    }
    const int t = thumbAt (e.getPosition());
    if (t < 0) return;
    pressThumb = t;
    ed.selectFrame (frameOf (t), e.mods.isShiftDown());
}

void ThumbStrip::mouseDrag (const MouseEvent& e)
{
    if (pressThumb < 0 || ed.set.morphed() || e.mods.isShiftDown()) return;
    if (! dragging && e.getDistanceFromDragStart() < 6) return;
    dragging = true;
    insertAt = jlimit (0, count(), (e.x + scroll - 2 + (kThumbW + kGap) / 2) / (kThumbW + kGap));
    if (e.x > getWidth() - 10 && scroll < maxScroll()) scroll = jmin (maxScroll(), scroll + 8);
    if (e.x < 10 && scroll > 0) scroll = jmax (0, scroll - 8);
    repaint();
}

void ThumbStrip::mouseUp (const MouseEvent&)
{
    const bool wasDragging = dragging;
    dragging = false;
    repaint();
    if (! wasDragging || insertAt < 0) return;
    const int from = pressThumb, to = insertAt;
    insertAt = -1;
    if (to == from || to == from + 1) return;
    if (we::moveFrame (ed.set, ed.sel, from, to))
    {
        ed.set.modified = true;
        ed.framesChanged();
        ed.commit ("Move Frame");
    }
}

void ThumbStrip::mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w)
{
    const float d = ! same (w.deltaX, 0.0f) ? w.deltaX : w.deltaY;
    scroll = jlimit (0, maxScroll(), scroll - roundToInt (d * 240.0f));
    repaint();
}

bool ThumbStrip::isInterestedInFileDrag (const StringArray& files)
{
    for (auto& f : files) if (isAudioFile (File (f))) return true;
    return false;
}

void ThumbStrip::fileDragMove (const StringArray&, int x, int y)
{
    const int t = thumbAt ({ x, y });
    if (t != dropThumb) { dropThumb = t; repaint(); }
}

void ThumbStrip::fileDragExit (const StringArray&) { dropThumb = -1; repaint(); }

void ThumbStrip::filesDropped (const StringArray& files, int x, int y)
{
    dropThumb = -1;
    repaint();
    const int t = thumbAt ({ x, y });
    for (auto& path : files)
    {
        const File f (path);
        if (! isAudioFile (f)) continue;
        if (t >= 0) ed.replaceFrameFromFile (frameOf (t), f);
        else ed.importFile (f, we::ImportMethod::DynamicZeroSnap, false);
        return;
    }
}
} // namespace wte

// ===========================================================================
// WavetableEditor
// ===========================================================================
WavetableEditor::WavetableEditor (PanelContext& c, int o, std::function<void()> close)
    : ctx (c), osc (jlimit (0, kNumOscs - 1, o)), onClose (std::move (close))
{
    setOpaque (true);
    for (auto* comp : std::initializer_list<Component*> { &nameBox, &prevTable, &nextTable, &closeBtn, &importBtn, &exportBtn,
                                                          &singleBtn, &allBtn, &morphBtn, &removeBtn, &sortBtn, &formulaBtn,
                                                          &shiftL, &shiftR, &shiftU, &shiftD, &posMinus, &posPlus, &copyBtn,
                                                          &pasteBtn, &zoomStep, &gridXStep, &gridYStep, &fftView, &waveView,
                                                          &thumbs, &formula })
        addAndMakeVisible (comp);
    for (int i = 0; i < (int) we::Brush::NumBrushes; ++i)
        brushButtons.push_back (std::make_unique<wte::BrushButton> (*this, i));
    brushButtons.push_back (std::make_unique<wte::BrushButton> (*this, -1));
    for (auto& b : brushButtons) addAndMakeVisible (*b);

    nameBox.setTooltip ("Wavetable: click for the factory tables and Save Table...");
    prevTable.setTooltip ("Previous table");
    nextTable.setTooltip ("Next table");
    closeBtn.setTooltip ("Close the wavetable editor");
    importBtn.setTooltip ("Import audio as a wavetable");
    exportBtn.setTooltip ("Export the table as audio");
    singleBtn.setTooltip ("Process the current frame");
    allBtn.setTooltip ("Process every frame");
    morphBtn.setTooltip ("Fill the table to 256 frames by interpolating between frames");
    removeBtn.setTooltip ("Init, insert, remove and reduce frames");
    sortBtn.setTooltip ("Sort the frames");
    formulaBtn.setTooltip ("Formula presets");
    shiftL.setTooltip ("Shift the wave left by one grid step");
    shiftR.setTooltip ("Shift the wave right by one grid step");
    shiftU.setTooltip ("Nudge the wave up");
    shiftD.setTooltip ("Nudge the wave down");
    posMinus.setTooltip ("Remove the current frame");
    posPlus.setTooltip ("Insert a copy of the current frame after it");
    copyBtn.setTooltip ("Copy the selected frames");
    pasteBtn.setTooltip ("Paste frames at the current frame");
    for (auto* b : { &prevTable, &nextTable, &shiftL, &shiftR, &shiftU, &shiftD, &posMinus, &posPlus })
        b->setFontHeight (13.0f);

    closeBtn.onClick = [this] (const ModifierKeys&) { if (onClose) onClose(); };
    prevTable.onClick = [this] (const ModifierKeys&) { stepTable (-1); };
    nextTable.onClick = [this] (const ModifierKeys&) { stepTable (1); };
    importBtn.onClick = [this] (const ModifierKeys&) { showMenu (importBtn, importMenu()); };
    exportBtn.onClick = [this] (const ModifierKeys&) { showMenu (exportBtn, exportMenu()); };
    singleBtn.onClick = [this] (const ModifierKeys&) { showMenu (singleBtn, buildWaveMenu (we::WaveMenu::ProcessSingle, 1)); };
    allBtn.onClick = [this] (const ModifierKeys&) { showMenu (allBtn, buildWaveMenu (we::WaveMenu::ProcessAll, 1)); };
    morphBtn.onClick = [this] (const ModifierKeys&) { showMenu (morphBtn, buildWaveMenu (we::WaveMenu::Morph, 1)); };
    removeBtn.onClick = [this] (const ModifierKeys&) { showMenu (removeBtn, buildWaveMenu (we::WaveMenu::Remove, 1)); };
    sortBtn.onClick = [this] (const ModifierKeys&) { showMenu (sortBtn, buildWaveMenu (we::WaveMenu::Sort, 1)); };
    formulaBtn.onClick = [this] (const ModifierKeys&)
    {
        MenuOptions opts;
        opts.matchAnchorWidth = false;
        opts.minWidth = 200;
        showMenu (formulaBtn, formulaMenu(), opts);
    };

    auto shiftOp = [this] (const char* label, std::function<bool (we::Frame&)> fn)
    {
        if (fn (current()))
        {
            set.modified = true;
            if (set.morphed()) we::morph (set, morphMode);
            framesChanged();
            commit (label);
        }
    };
    shiftL.onClick = [this, shiftOp] (const ModifierKeys&) { shiftOp ("Shift Wave Left", [this] (we::Frame& f) { return we::shiftByGrid (f, gridX, -1); }); };
    shiftR.onClick = [this, shiftOp] (const ModifierKeys&) { shiftOp ("Shift Wave Right", [this] (we::Frame& f) { return we::shiftByGrid (f, gridX, 1); }); };
    shiftU.onClick = [this, shiftOp] (const ModifierKeys&) { shiftOp ("Nudge Wave Up", [this] (we::Frame& f) { return we::nudgeVertical (f, gridY > 0 ? 2.0f / (float) gridY : 0.05f); }); };
    shiftD.onClick = [this, shiftOp] (const ModifierKeys&) { shiftOp ("Nudge Wave Down", [this] (we::Frame& f) { return we::nudgeVertical (f, gridY > 0 ? -2.0f / (float) gridY : -0.05f); }); };

    auto structural = [this] (const char* label, std::function<bool()> fn)
    {
        if (fn())
        {
            set.modified = true;
            framesChanged();
            commit (label);
            thumbs.ensureVisible (thumbs.thumbOf (sel.current));
        }
    };
    posMinus.onClick = [this, structural] (const ModifierKeys&) { structural ("Remove Frame", [this] { return we::removeFrame (set, sel); }); };
    posPlus.onClick = [this, structural] (const ModifierKeys&) { structural ("Insert Frame", [this] { return we::insertFrame (set, sel); }); };
    copyBtn.onClick = [this] (const ModifierKeys&)
    {
        if (we::copyFrames (set, sel, clipboard()))
            setMessage (String ((int) clipboard().size()) + (clipboard().size() == 1 ? " frame copied" : " frames copied"), false);
        pasteBtn.setDimmed (clipboard().empty());
    };
    pasteBtn.onClick = [this, structural] (const ModifierKeys&) { structural ("Paste Frames", [this] { return we::pasteFrames (set, sel, clipboard()); }); };

    zoomStep.caption = "ZOOM";
    zoomStep.values = { 1, 2, 4, 8 };
    zoomStep.format = [] (int v) { return String (v) + "x"; };
    zoomStep.get = [this] { return zoom; };
    zoomStep.set = [this] (int v) { zoom = v; fftFirst = jlimit (0, we::kFftBins - visibleBins(), fftFirst); fftView.repaint(); };
    zoomStep.setTooltip ("FFT area zoom (scroll with the bar or the wheel)");
    gridXStep.caption = "X";
    gridXStep.values = { 0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 32, 48, 64, 128 };
    gridXStep.format = [] (int v) { return v > 0 ? String (v) : String ("OFF"); };
    gridXStep.get = [this] { return gridX; };
    gridXStep.set = [this] (int v) { gridX = v; waveView.repaint(); };
    gridXStep.setTooltip ("Horizontal grid: brush cells across the frame (OFF = freehand)");
    gridYStep.caption = "Y";
    gridYStep.values = { 0, 2, 3, 4, 5, 6, 8, 10, 12, 16, 24, 32, 64 };
    gridYStep.format = [] (int v) { return v > 0 ? String (v) : String ("OFF"); };
    gridYStep.get = [this] { return gridY; };
    gridYStep.set = [this] (int v) { gridY = v; waveView.repaint(); };
    gridYStep.setTooltip ("Vertical grid: brush values snap to these steps");

    formula.setFont (panelFont (14.0f, false));
    formula.setIndents (6, 4);
    formula.setTextToShowWhenEmpty ("(enter formula)", colours::lcdDim);
    formula.setSelectAllWhenFocused (false);
    formula.onReturnKey = [this] { applyFormulaText (formula.getText()); };
    formula.setTooltip ("Formula: Enter applies it. Also a sample count (2048) or note (F1) to set the import frame size");

    pasteBtn.setDimmed (clipboard().empty());
    reloadFromPatch();
    ctx.add (this);
}

WavetableEditor::~WavetableEditor()
{
    ctx.remove (this);
}

void WavetableEditor::resized()
{
    nameBox.setBounds (112, kTop, 330, kRowH);
    prevTable.setBounds (446, kTop, 26, kRowH);
    nextTable.setBounds (474, kTop, 26, kRowH);
    zoomStep.setBounds (kMidX + kMidW - 142, kTop, 142, kRowH);
    importBtn.setBounds (kRightX, kTop, 96, kRowH);
    exportBtn.setBounds (kRightX + 100, kTop, 96, kRowH);
    closeBtn.setBounds (kRightX + kRightW - 24, kTop, 24, kRowH);

    fftView.setBounds (kMidX, kFftY, kMidW, kFftH);
    waveView.setBounds (kMidX, kWaveY, kMidW, kWaveH);
    thumbs.setBounds (kBrushX, kThumbY, kRightX + kRightW - kBrushX, kThumbH);
    layoutBrushes();

    // Right column: PROCESS and GRID boxes.
    const int ix = kRightX + 10, iw = kRightW - 20;
    singleBtn.setBounds (ix, 76, iw / 2 - 2, 26);
    allBtn.setBounds (ix + iw / 2 + 2, 76, iw / 2 - 2, 26);
    morphBtn.setBounds (ix, 108, iw, 26);
    gridXStep.setBounds (ix, 174, iw, 26);
    gridYStep.setBounds (ix, 206, iw, 26);
    const int bw = (iw - 12) / 4;
    shiftL.setBounds (ix, 252, bw, 26);
    shiftR.setBounds (ix + (bw + 4), 252, bw, 26);
    shiftU.setBounds (ix + 2 * (bw + 4), 252, bw, 26);
    shiftD.setBounds (ix + 3 * (bw + 4), 252, bw, 26);

    // Bottom bar.
    posArea = { kBrushX + 52, kBarY, 84, kRowH };
    posMinus.setBounds (posArea.getRight() + 4, kBarY, 24, kRowH);
    posPlus.setBounds (posArea.getRight() + 30, kBarY, 24, kRowH);
    copyBtn.setBounds (posArea.getRight() + 66, kBarY, 64, kRowH);
    pasteBtn.setBounds (posArea.getRight() + 134, kBarY, 64, kRowH);
    removeBtn.setBounds (posArea.getRight() + 210, kBarY, 104, kRowH);
    sortBtn.setBounds (posArea.getRight() + 318, kBarY, 92, kRowH);
    const int fx = sortBtn.getRight() + 76;
    formula.setBounds (fx, kBarY, 440, kRowH);
    formulaBtn.setBounds (formula.getRight() + 2, kBarY, 26, kRowH);
    msgArea = { formulaBtn.getRight() + 12, kBarY, kRightX + kRightW - formulaBtn.getRight() - 12, kRowH };
}

void WavetableEditor::layoutBrushes()
{
    const int bw = 46, bh = 28, pitch = 32, y0 = 72;
    for (int i = 0; i < (int) brushButtons.size(); ++i)
    {
        auto& b = *brushButtons[(size_t) i];
        if (b.index < 0) b.setBounds (kBrushX, y0 + 6 * pitch, kBrushW, 26);
        else b.setBounds (kBrushX + (i % 2) * (bw + 4), y0 + (i / 2) * pitch, bw, bh);
    }
}

void WavetableEditor::paint (Graphics& g)
{
    g.fillAll (colours::panel);
    drawSection (g, Rectangle<float> (6.0f, 10.0f, 1388.0f, 374.0f), "WAVETABLE EDITOR", accent());

    drawText (g, oscName(), { (float) kBrushX, (float) kTop, (float) kBrushW, (float) kRowH }, 16.0f, accent(), true, Justification::centredLeft);
    drawText (g, "BRUSH", { (float) kBrushX, 54.0f, (float) kBrushW, 14.0f }, 11.0f, colours::inkDim, true, Justification::centredLeft);
    drawSection (g, Rectangle<float> ((float) kRightX, 64.0f, (float) kRightW, 80.0f), "PROCESS");
    drawSection (g, Rectangle<float> ((float) kRightX, 162.0f, (float) kRightW, (float) (kWaveY + kWaveH - 162)), "GRID");
    drawText (g, "SHIFT", { (float) kRightX + 10.0f, 236.0f, 100.0f, 13.0f }, 10.5f, colours::inkDim, true, Justification::centredLeft);

    // Bottom bar: WT POS readout and captions.
    drawText (g, "WT POS", { (float) kBrushX, (float) kBarY, 50.0f, (float) kRowH }, 11.0f, colours::ink, true, Justification::centredLeft);
    drawLcdBox (g, posArea.toFloat().reduced (0.5f));
    drawText (g, String (sel.current + 1) + " / " + String (set.size()), posArea.toFloat(), 13.0f, colours::lcd, true);
    drawText (g, "FORMULA", { (float) sortBtn.getRight() + 12.0f, (float) kBarY, 62.0f, (float) kRowH }, 11.0f, colours::ink, true, Justification::centredLeft);
    if (message.isNotEmpty())
        drawText (g, message, msgArea.toFloat(), 11.0f, messageIsError ? colours::ledRed.brighter (0.2f) : colours::inkDim, true, Justification::centredLeft);
}

void WavetableEditor::refresh()
{
    const auto& t = ctx.proc.patch().wavetable[(size_t) osc].table;
    if (t != shown) reloadFromPatch();
}

const we::Spectrum& WavetableEditor::spectrum()
{
    if (specVersion != version)
    {
        we::analyse (current(), spec);
        specVersion = version;
    }
    return spec;
}

void WavetableEditor::framesChanged()
{
    ++version;
    sel.clamp (set.size());
    waveView.repaint();
    fftView.repaint();
    thumbs.repaint();
    nameBox.repaint();
    repaint (posArea);
}

void WavetableEditor::selectFrame (int frame, bool extend)
{
    frame = jlimit (0, set.size() - 1, frame);
    if (extend) sel.extendTo (frame); else sel.select (frame);
    framesChanged();
    thumbs.ensureVisible (thumbs.thumbOf (sel.current));
}

void WavetableEditor::commit (const String& label)
{
    std::vector<std::vector<float>> frames;
    auto m = Wavetable::Morph::None;
    if (set.morphed())
    {
        for (int k : set.keyFrames) frames.emplace_back (set.frames[(size_t) k].begin(), set.frames[(size_t) k].end());
        m = (Wavetable::Morph) ((int) morphMode + 1);
    }
    else
    {
        for (auto& f : set.frames) frames.emplace_back (f.begin(), f.end());
    }
    std::shared_ptr<const Wavetable> table = Wavetable::create (set.name, "User", std::move (frames), m, false);
    const int o = osc;
    ctx.proc.editPatch (label, [o, table] (Patch& p) { p.wavetable[(size_t) o].table = table; });
    shown = table;
    auto& c = cacheFor (osc);
    c.table = table;
    c.set = set;
    c.mode = morphMode;
    nameBox.repaint();
}

void WavetableEditor::reloadFromPatch()
{
    const auto t = ctx.proc.patch().wavetable[(size_t) osc].table;
    shown = t;
    auto& c = cacheFor (osc);
    if (t != nullptr && c.table.lock() == t)
    {
        set = c.set;
        morphMode = c.mode;
    }
    else
    {
        set = frameSetFromTable (t.get());
        if (t != nullptr && t->morph() != Wavetable::Morph::None)
            morphMode = (we::MorphMode) jlimit (0, 3, (int) t->morph() - 1);
    }
    if (set.frames.empty()) set = we::FrameSet::initSaw();
    sel.clamp (set.size());
    if (set.morphed() && ! set.isKeyFrame (sel.current)) sel.select (thumbs.frameOf (thumbs.thumbOf (sel.current)));
    framesChanged();
    thumbs.ensureVisible (thumbs.thumbOf (sel.current));
    repaint();
}

// ---------------------------------------------------------------------------
// Table-driven menus (WaveEdit.h)
// ---------------------------------------------------------------------------
Menu WavetableEditor::buildWaveMenu (we::WaveMenu which, int bin)
{
    Menu m;
    const auto list = we::waveMenu (which);
    we::EditContext probe { set, sel };
    probe.fftOptions = &fftOpts;
    const int keys = set.morphed() ? (int) set.keyFrames.size() : set.size();
    const auto other = ctx.proc.patch().wavetable[(size_t) (osc == 0 ? 1 : 0)].table;
    SafePointer<WavetableEditor> safe (this);

    auto enabledFor = [&] (int i, const String& label)
    {
        switch (which)
        {
            case we::WaveMenu::Morph:  return i < 4 ? (keys > 1 && keys < we::kMaxFrames) : set.morphed();
            case we::WaveMenu::Remove:
                if (label.startsWith ("Insert")) return set.size() < we::kMaxFrames;
                if (label.startsWith ("Init")) return true;
                if (label.startsWith ("Every")) return set.size() > 1;
                return set.size() > 1;
            case we::WaveMenu::Sort:   return set.size() > 1;
            case we::WaveMenu::ProcessAll:
                if (label.contains ("Other Osc")) return other != nullptr;
                return true;
            case we::WaveMenu::FftMags: case we::WaveMenu::FftPhases:
            case we::WaveMenu::ProcessSingle: case we::WaveMenu::NumMenus:
                return true;
        }
        return true;
    };

    for (int i = 0; i < list.count; ++i)
    {
        const auto& op = list.items[i];
        if (op.kind == we::WaveOp::Title) { m.header (String (CharPointer_UTF8 (op.label))); continue; }
        if (op.kind == we::WaveOp::Separator) { m.separator(); continue; }
        if (op.submenu != nullptr)
        {
            Menu sub;
            const String subName (CharPointer_UTF8 (op.submenu));
            bool any = false;
            int j = i;
            for (; j < list.count && list.items[j].submenu != nullptr && subName == CharPointer_UTF8 (list.items[j].submenu); ++j)
            {
                const String label (CharPointer_UTF8 (we::menuLabel (list.items[j], bin).c_str()));
                const bool en = enabledFor (j, label);
                any |= en;
                sub.add (label, [safe, which, j, bin] { if (safe != nullptr) safe->runWaveOp (which, j, bin); }, false, en);
            }
            m.sub (subName, std::move (sub), false, any);
            i = j - 1;
            continue;
        }
        const String label (CharPointer_UTF8 (we::menuLabel (op, bin).c_str()));
        if (op.kind == we::WaveOp::Toggle)
        {
            m.toggle (label, we::isChecked (op, probe), [safe, which, i, bin] { if (safe != nullptr) safe->runWaveOp (which, i, bin); });
            continue;
        }
        m.add (label, [safe, which, i, bin] { if (safe != nullptr) safe->runWaveOp (which, i, bin); }, false, enabledFor (i, label));
    }
    return m;
}

void WavetableEditor::runWaveOp (we::WaveMenu which, int index, int bin)
{
    const auto list = we::waveMenu (which);
    if (index < 0 || index >= list.count || list.items[index].fn == nullptr) return;
    const auto& op = list.items[index];
    const String label (CharPointer_UTF8 (op.label));

    we::EditContext c { set, sel };
    c.gridX = gridX;
    c.gridY = gridY;
    c.bin = bin;
    c.fftOptions = &fftOpts;
    we::FrameSet other;
    if (label.contains ("Other Osc"))
    {
        other = frameSetFromTable (ctx.proc.patch().wavetable[(size_t) (osc == 0 ? 1 : 0)].table.get());
        c.otherOsc = &other;
    }
    std::vector<float> noise;
    c.noiseOut = &noise;
    seed = seed * 1664525u + 1013904223u;
    c.seed = seed;

    const bool changed = op.fn (c);
    if (op.kind == we::WaveOp::Toggle) { fftView.repaint(); return; }
    if (! noise.empty())
    {
        sendToNoise (noise);
        setMessage ("Frame " + String (sel.current + 1) + " sent to the noise oscillator", false);
    }
    if (! changed) { framesChanged(); return; }
    if (which == we::WaveMenu::Morph)
    {
        if (index < 4) morphMode = (we::MorphMode) index;
    }
    else if (set.morphed())
    {
        we::morph (set, morphMode);
    }
    set.modified = true;
    framesChanged();
    thumbs.ensureVisible (thumbs.thumbOf (sel.current));
    commit ("Wavetable: " + String (CharPointer_UTF8 (we::menuLabel (op, bin).c_str())));
}

void WavetableEditor::sendToNoise (const std::vector<float>& cycle)
{
    auto sd = std::make_shared<SampleData>();
    sd->name = set.name + " Cycle";
    sd->sampleRate = 44100.0;
    sd->rootNote = 17;                       // 44100 / 2048 = 21.5 Hz ~ F0
    sd->left = cycle;
    sd->loopStart = 0;
    sd->loopEnd = (int) cycle.size();
    sd->embedded = true;
    std::shared_ptr<const SampleData> s = sd;
    ctx.proc.editPatch ("Send to Noise Oscillator", [s] (Patch& p) { p.noise.sample = s; });
}

// ---------------------------------------------------------------------------
// Name menu, factory tables
// ---------------------------------------------------------------------------
Menu WavetableEditor::nameMenu()
{
    Menu m;
    SafePointer<WavetableEditor> safe (this);
    for (const auto& cat : FactoryTables::categories())
    {
        Menu sub;
        bool here = false;
        for (const auto& n : FactoryTables::names (cat))
        {
            const bool tick = shown != nullptr && shown->factory && shown->category == cat && shown->name == n;
            here |= tick;
            const String c (cat), nm (n);
            sub.add (nm, [safe, c, nm] { if (safe != nullptr) safe->loadFactory (c, nm); }, tick);
        }
        m.sub (String (cat), std::move (sub), here);
    }
    const auto userDir = Settings::userTables();
    auto files = userDir.findChildFiles (File::findFiles, false, "*.wav;*.aif;*.aiff");
    if (! files.isEmpty())
    {
        files.sort();
        Menu sub;
        for (auto& f : files)
            sub.add (f.getFileNameWithoutExtension(), [safe, f]
            {
                if (safe == nullptr) return;
                // Saved tables carry 2048 samples per frame.
                const auto keep = safe->fieldOverride;
                safe->fieldOverride.samplesPerCycle = we::kFrameSize;
                safe->importFile (f, we::ImportMethod::FixedFrameSize, false);
                safe->fieldOverride = keep;
            }, shown != nullptr && ! shown->factory && shown->name == f.getFileNameWithoutExtension().toStdString());
        m.sub ("User", std::move (sub));
    }
    m.separator();
    m.add ("Save Table...", [safe]
    {
        if (safe == nullptr) return;
        auto dir = Settings::userTables();
        dir.createDirectory();
        safe->chooseFile (true, false, "Save Table", dir.getChildFile (File::createLegalFileName (String (safe->set.name)) + ".wav"), "*.wav",
                          [safe] (const File& f) { if (safe != nullptr) safe->saveTableTo (f.withFileExtension ("wav")); });
    });
    return m;
}

void WavetableEditor::loadFactory (const String& category, const String& name)
{
    auto t = FactoryTables::get (category.toStdString(), name.toStdString());
    if (t == nullptr) return;
    const int o = osc;
    ctx.proc.editPatch ("Load Wavetable", [o, t] (Patch& p) { p.wavetable[(size_t) o].table = t; });
    sel.select (0);
    reloadFromPatch();
}

void WavetableEditor::stepTable (int dir)
{
    std::string cat = shown != nullptr && shown->factory ? shown->category : std::string ("S2 Tables");
    auto names = FactoryTables::names (cat);
    if (names.empty()) { cat = "S2 Tables"; names = FactoryTables::names (cat); }
    if (names.empty()) return;
    int idx = -1;
    for (int i = 0; i < (int) names.size(); ++i)
        if (shown != nullptr && shown->factory && names[(size_t) i] == shown->name) idx = i;
    const int n = (int) names.size();
    idx = idx < 0 ? (dir > 0 ? 0 : n - 1) : ((idx + dir) % n + n) % n;
    loadFactory (String (cat), String (names[(size_t) idx]));
}

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------
void WavetableEditor::chooseFile (bool save, bool directory, const String& title, const File& start, const String& patterns,
                                  std::function<void (const File&)> then)
{
    chooser = std::make_unique<FileChooser> (title, start, patterns);
    int chooserFlags = save ? (FileBrowserComponent::saveMode | FileBrowserComponent::warnAboutOverwriting) : FileBrowserComponent::openMode;
    chooserFlags |= directory ? FileBrowserComponent::canSelectDirectories : FileBrowserComponent::canSelectFiles;
    chooser->launchAsync (chooserFlags, [done = std::move (then)] (const FileChooser& fc)
    {
        const auto r = fc.getResult();
        if (r != File()) done (r);
    });
}

Menu WavetableEditor::importMenu()
{
    Menu m;
    SafePointer<WavetableEditor> safe (this);
    for (const auto& item : we::importMenu())
    {
        const auto method = item.method;
        const bool single = item.singleCycle;
        m.add (String (CharPointer_UTF8 (item.label)), [safe, method, single]
        {
            if (safe == nullptr) return;
            safe->chooseFile (false, false, "Import Audio", File::getSpecialLocation (File::userHomeDirectory), "*.wav;*.aif;*.aiff;*.flac",
                              [safe, method, single] (const File& f) { if (safe != nullptr) safe->importFile (f, method, single); });
        });
    }
    return m;
}

Menu WavetableEditor::exportMenu()
{
    Menu m;
    SafePointer<WavetableEditor> safe (this);
    for (const auto& item : we::exportMenu())
    {
        const int id = item.id;
        m.add (String (CharPointer_UTF8 (item.label)), [safe, id]
        {
            if (safe == nullptr) return;
            const auto base = File::getSpecialLocation (File::userHomeDirectory);
            const String name = File::createLegalFileName (String (safe->set.name));
            const String ext = id == 0 ? ".256" : ".wav";
            safe->chooseFile (true, id == 3, id == 3 ? "Export Single-Cycle Waves To" : "Export Wavetable",
                              id == 3 ? base : base.getChildFile (name + (id == 4 ? " (selection)" : "") + ext),
                              id == 0 ? "*.256" : "*.wav",
                              [safe, id] (const File& f) { if (safe != nullptr) safe->exportTo (f, id); });
        }, false, id != 4 || set.size() > 0);
    }
    return m;
}

bool WavetableEditor::importFile (const File& file, we::ImportMethod method, bool singleCycle)
{
    const auto s = TinctureProcessor::decodeAudio (file);
    if (s == nullptr || s->frames() < 2)
    {
        setMessage ("Couldn't read " + file.getFileName(), true);
        return false;
    }
    const float* x = s->channel (osc == 1 ? 1 : 0);
    const int len = s->frames();
    if (singleCycle)
    {
        we::Frame f;
        we::importSingleCycle (x, len, f);
        sel.clamp (set.size());
        bool any = false;
        for (int i = sel.first; i <= sel.last; ++i) any |= we::replaceFrame (set, i, f);
        if (! any) return false;
        set.modified = true;
        if (set.morphed()) we::morph (set, morphMode);
        framesChanged();
        commit ("Import Single-Cycle");
        setMessage (file.getFileName() + " imported as a single cycle", false);
        return true;
    }

    we::ImportOverride ov;
    auto txt = file.withFileExtension ("txt");
    if (! txt.existsAsFile()) txt = file.getSiblingFile ("FolderInfo.txt");
    if (txt.existsAsFile()) we::parseTextOverride (txt.loadFileAsString().toStdString(), ov);
    if (fieldOverride.samplesPerCycle > 0) ov.samplesPerCycle = fieldOverride.samplesPerCycle;

    const auto name = file.getFileNameWithoutExtension();
    auto r = we::importAudio (x, len, s->sampleRate, method, ov, name.toStdString());
    if (! r.ok || r.set.frames.empty())
    {
        setMessage ("Import failed: " + String (r.error), true);
        return false;
    }
    set = std::move (r.set);
    set.name = name.toStdString();
    set.modified = false;
    sel.select (0);
    if (! r.fieldText.empty())
    {
        formula.setText (String (r.fieldText), dontSendNotification);
        int samples = 0;
        if (we::parseFieldOverride (r.fieldText, samples)) fieldOverride.samplesPerCycle = samples;
    }
    String msg = String (set.size()) + (set.size() == 1 ? " frame" : " frames") + " from " + file.getFileName();
    if (r.detectedHz > 0.0)
    {
        msg << "  (" << String (r.detectedHz, 1) << " Hz";
        if (r.nearestNote >= 0) msg << ", " << String (we::noteName (r.nearestNote));
        msg << ")";
    }
    framesChanged();
    commit ("Import Wavetable");
    thumbs.ensureVisible (0);
    setMessage (msg, false);
    return true;
}

bool WavetableEditor::importImageFile (const File& file)
{
    const auto img = ImageFileFormat::loadFrom (file);
    if (! img.isValid()) return false;
    const int w = jmin (img.getWidth(), we::kMaxFrames), h = img.getHeight();
    std::vector<float> lum ((size_t) (w * h));
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const auto c = img.getPixelAt (x, y);
            lum[(size_t) (y * w + x)] = 0.2126f * c.getFloatRed() + 0.7152f * c.getFloatGreen() + 0.0722f * c.getFloatBlue();
        }
    set = we::importImage (lum.data(), w, h, we::ImageMode::Waveform, file.getFileNameWithoutExtension().toStdString());
    set.modified = false;
    sel.select (0);
    framesChanged();
    commit ("Import Image");
    return true;
}

bool WavetableEditor::replaceFrameFromFile (int frame, const File& file)
{
    const auto s = TinctureProcessor::decodeAudio (file);
    if (s == nullptr || s->frames() < 2) { setMessage ("Couldn't read " + file.getFileName(), true); return false; }
    we::Frame f;
    we::importSingleCycle (s->channel (osc == 1 ? 1 : 0), s->frames(), f);
    if (! we::replaceFrame (set, frame, f)) return false;
    set.modified = true;
    if (set.morphed()) we::morph (set, morphMode);
    sel.select (frame);
    framesChanged();
    commit ("Replace Frame");
    return true;
}

bool WavetableEditor::exportTo (const File& target, int id)
{
    auto writeBlock = [] (const File& f, const MemoryBlock& b) { return b.getSize() > 0 && f.replaceWithData (b.getData(), b.getSize()); };
    bool ok = false;
    switch (id)
    {
        case 0:
        {
            const auto data = we::export256 (set);
            ok = target.replaceWithData (data.data(), data.size());
            break;
        }
        case 1: ok = writeBlock (target, makeWav (we::exportAudio (set), 16, false)); break;
        case 2: ok = writeBlock (target, makeWav (we::exportAudio (set), 32, false)); break;
        case 3:
        {
            target.createDirectory();
            const auto cycles = we::exportSingleCycles (set);
            ok = true;
            for (int i = 0; i < (int) cycles.size(); ++i)
                ok &= writeBlock (target.getChildFile (String (we::singleCycleFileName (set.name, i))), makeWav (cycles[(size_t) i], 32, false));
            break;
        }
        case 4:
        {
            sel.clamp (set.size());
            ok = writeBlock (target, makeWav (we::exportAudio (set, sel.first, sel.last), 32, true));
            break;
        }
        default: break;
    }
    setMessage (ok ? "Exported " + target.getFileName() : "Export failed: " + target.getFullPathName(), ! ok);
    return ok;
}

bool WavetableEditor::saveTableTo (const File& f)
{
    f.getParentDirectory().createDirectory();
    const auto block = makeWav (we::exportAudio (set), 32, true);
    if (block.getSize() == 0 || ! f.replaceWithData (block.getData(), block.getSize()))
    {
        setMessage ("Couldn't save " + f.getFullPathName(), true);
        return false;
    }
    set.name = f.getFileNameWithoutExtension().toStdString();
    set.modified = false;
    commit ("Save Table");
    setMessage ("Saved " + f.getFileName(), false);
    return true;
}

// ---------------------------------------------------------------------------
// Formula
// ---------------------------------------------------------------------------
File WavetableEditor::formulaFile (bool user, bool multi)
{
    const char* n = user ? (multi ? "FormulaUserMultis.txt" : "FormulaUserSingles.txt")
                         : (multi ? "FormulaFactoryMultis.txt" : "FormulaFactorySingles.txt");
    return Settings::folder().getChildFile ("System").getChildFile (n);
}

Menu WavetableEditor::formulaMenu()
{
    Menu m;
    SafePointer<WavetableEditor> safe (this);
    auto presetMenu = [safe] (const std::vector<we::FormulaPreset>& list)
    {
        Menu sub;
        for (const auto& p : list)
        {
            const String f (p.formula);
            sub.add (String (p.name), [safe, f]
            {
                if (safe == nullptr) return;
                safe->formula.setText (f, dontSendNotification);
                safe->applyFormulaText (f);
            });
        }
        return sub;
    };
    for (const char* item : we::formulaMenuItems())
    {
        const String it (item);
        if (it == "-") { m.separator(); continue; }
        if (it == "Save Formula...")
        {
            m.add (it, [safe] { if (safe != nullptr) safe->showFormulaSave(); }, false, formula.getText().trim().isNotEmpty());
            continue;
        }
        const bool multi = it.contains ("Multis");
        if (it.startsWith ("User"))
        {
            const auto list = we::parseFormulaFile (formulaFile (true, multi).loadFileAsString().toStdString());
            m.sub (it, presetMenu (list), false, ! list.empty());
        }
        else
        {
            m.sub (it, presetMenu (multi ? we::factoryMultis() : we::factorySingles()));
        }
    }
    return m;
}

void WavetableEditor::showFormulaSave()
{
    const auto text = formula.getText().trim();
    if (text.isEmpty() || ! ctx.editValue) return;
    SafePointer<WavetableEditor> safe (this);
    ctx.editValue (formula, text, [safe, text] (const String& name)
    {
        if (safe == nullptr || name.trim().isEmpty()) return;
        const auto file = formulaFile (true, we::isMultiFormula (text.toStdString()));
        file.getParentDirectory().createDirectory();
        file.replaceWithText (String (we::addToFormulaFile (file.loadFileAsString().toStdString(), { name.trim().toStdString(), text.toStdString() })));
        safe->setMessage ("Saved formula \"" + name.trim() + "\"", false);
    });
}

void WavetableEditor::applyFormulaText (const String& raw)
{
    const auto text = raw.trim();
    if (text.isEmpty())
    {
        fieldOverride = {};
        setMessage ({}, false);
        return;
    }
    int samples = 0;
    if (we::parseFieldOverride (text.toStdString(), samples))
    {
        fieldOverride.samplesPerCycle = samples;
        setMessage (String (we::splitEcho (samples)), false);
        return;
    }
    fieldOverride = {};
    seed = seed * 1664525u + 1013904223u;
    const auto r = we::applyFormula (text.toStdString(), set, sel, seed);
    if (! r.ok)
    {
        String e = "Error";
        if (r.error.position >= 0) e << " at " << (r.error.position + 1);
        e << ": " << String (r.error.message);
        setMessage (e, true);
        return;
    }
    if (set.morphed()) we::morph (set, morphMode);
    set.modified = true;
    framesChanged();
    commit ("Wavetable Formula");
    thumbs.ensureVisible (thumbs.thumbOf (sel.current));
    setMessage (r.frames > 1 ? String (r.frames) + " frames written" : "Frame " + String (sel.current + 1) + " written", false);
}

// ===========================================================================
std::unique_ptr<Component> createWavetableEditor (PanelContext& ctx, int osc, std::function<void()> onClose)
{
    auto e = std::make_unique<WavetableEditor> (ctx, osc, std::move (onClose));
    e->setSize (kPageW, kModuleRowH);
    return e;
}
} // namespace tincture::ui
