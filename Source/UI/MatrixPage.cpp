// MatrixPage.cpp — the modulation matrix (spec D §5): 64 rows with
//   [grip] SOURCE  CRV  AMOUNT  POL  DESTINATION  OUT  AUX SOURCE  INV  CRV  OUTPUT  [bypass] [x]
// the ▼ matrix menu, row drag-reorder and the CRV curve editor (custom
// curve + RISE / FALL slew).
#include "FxModules.h"

namespace tincture::ui
{
using namespace juce;

namespace
{
enum Col { cGrip, cSource, cCrv, cAmount, cPol, cDest, cOut, cAux, cInv, cAuxCrv, cOutput, cBypass, cRemove, cNumCols };
const int colW[cNumCols] { 34, 150, 44, 200, 40, 260, 84, 150, 40, 44, 150, 30, 30 };
const char* colName[cNumCols] { "", "SOURCE", "CRV", "AMOUNT", "POL", "DESTINATION", "OUT", "AUX SOURCE", "INV", "CRV", "OUTPUT", "", "" };
constexpr int kRowH = 30;
constexpr int kGap = 6;
const Colour kAmountBlue { 0xff5c9cf2 };

int colX (int c)
{
    int x = 4;
    for (int i = 0; i < c; ++i) x += colW[i] + kGap;
    return x;
}

// Draws a CRV thumbnail.
void drawCurveThumb (Graphics& g, Rectangle<float> r, float bend, bool custom, const Curve& curve, Colour col)
{
    drawLcdBox (g, r);
    const bool linear = ! custom && std::abs (bend - 0.5f) < 0.005f;
    const auto p = r.reduced (4.0f);
    Path path;
    for (int i = 0; i <= 20; ++i)
    {
        const float x = (float) i / 20.0f;
        const float y = jlimit (0.0f, 1.0f, custom ? curve.evaluate (x) : modSimpleBend (x, bend));
        const auto pt = Point<float> (p.getX() + x * p.getWidth(), p.getBottom() - y * p.getHeight());
        if (i == 0) path.startNewSubPath (pt); else path.lineTo (pt);
    }
    g.setColour (linear ? colours::inkFaint : col);
    g.strokePath (path, PathStrokeType (1.6f));
}

// A small drag field for a non-parameter value (RISE / FALL).
class DragValue final : public Component
{
public:
    String caption;
    std::function<float()> get;
    std::function<void (float, bool commit)> set;
    float maxV = 1.0f;
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        drawText (g, caption, r.removeFromLeft (44.0f), 11.0f, colours::ink, true, Justification::centredLeft);
        drawLcdBox (g, r);
        const float v = get ? get() : 0.0f;
        drawText (g, v <= 0.0f ? String ("OFF") : String (roundToInt (v * 1000.0f)) + " ms", r, 12.5f, colours::lcd, true);
    }
    void mouseDown (const MouseEvent&) override { start = get ? get() : 0.0f; }
    void mouseDrag (const MouseEvent& e) override
    {
        if (set) set (jlimit (0.0f, maxV, start - (float) e.getDistanceFromDragStartY() / 200.0f * maxV), false);
        repaint();
    }
    void mouseUp (const MouseEvent& e) override { if (set && e.getDistanceFromDragStart() > 0) set (get(), true); }
    void mouseDoubleClick (const MouseEvent&) override { if (set) set (0.0f, true); repaint(); }
private:
    float start = 0.0f;
};

class MatrixPage;

// ---------------------------------------------------------------------------
class Table final : public Component
{
public:
    Table (PanelContext& c, MatrixPage& p) : ctx (c), page (p) { setRepaintsOnMouseActivity (false); }

    PanelContext& ctx;
    MatrixPage& page;
    float out[kMaxModRows] {};
    std::array<std::array<float, 24>, kMaxModRows> hist {};
    int histPos = 0;
    int dropRow = -1;

    Rectangle<int> cell (int row, int c) const { return { colX (c), row * kRowH + 3, colW[c], kRowH - 6 }; }
    int rowAt (int y) const { return jlimit (0, kMaxModRows - 1, y / kRowH); }
    int colAt (int x) const
    {
        for (int c = 0; c < cNumCols; ++c) if (x >= colX (c) - kGap / 2 && x < colX (c) + colW[c] + kGap / 2) return c;
        return -1;
    }
    const ModRow* rowData (int r) const
    {
        const auto& rows = ctx.proc.patch().matrix.rows;
        return r < (int) rows.size() ? &rows[(size_t) r] : nullptr;
    }

    void tick()
    {
        const auto& rows = ctx.proc.patch().matrix.rows;
        histPos = (histPos + 1) % 24;
        for (int r = 0; r < (int) rows.size() && r < kMaxModRows; ++r)
        {
            const float v = rows[(size_t) r].active() ? ctx.proc.matrixTelemetry().rowOut[r].load() : 0.0f;
            hist[(size_t) r][(size_t) histPos] = v;
            if (! same (v, out[r]) || rows[(size_t) r].active()) { out[r] = v; repaint (cell (r, cOut)); }
        }
    }

    void paint (Graphics& g) override;
    void mouseDown (const MouseEvent&) override;
    void mouseDrag (const MouseEvent&) override;
    void mouseUp (const MouseEvent&) override;
    void mouseDoubleClick (const MouseEvent&) override;
    void mouseMove (const MouseEvent&) override;

private:
    void editRow (int r, const String& label, std::function<void (ModRow&)> fn);
    float sliderValue (int c, const MouseEvent&) const;
    int downRow = -1, downCol = -1;
    bool dragging = false;
    float startVal = 0.0f;
};

// ---------------------------------------------------------------------------
class CurvePopover final : public Component
{
public:
    CurvePopover (PanelContext& c) : ctx (c)
    {
        addAndMakeVisible (editor);
        addAndMakeVisible (close);
        addAndMakeVisible (rise);
        addAndMakeVisible (fall);
        close.onClick = [this] (const ModifierKeys&) { setVisible (false); };
        rise.caption = "RISE";
        fall.caption = "FALL";
    }
    void open (int r, bool aux)
    {
        row = r;
        isAux = aux;
        auto& proc = ctx.proc;
        auto getRow = [&proc, r] () -> const ModRow* { const auto& rows = proc.patch().matrix.rows; return r < (int) rows.size() ? &rows[(size_t) r] : nullptr; };
        editor.colour = colours::brand;
        editor.get = [getRow, aux]
        {
            const auto* m = getRow();
            if (m == nullptr) return Curve();
            if (aux) return m->auxCustom ? m->auxCurveData : Curve();
            return m->customCurve ? m->srcCurve : Curve();
        };
        editor.set = [&proc, r, aux] (const Curve& c, bool commit)
        {
            proc.editPatch ("", [&] (Patch& p)
            {
                if (r >= (int) p.matrix.rows.size()) return;
                auto& m = p.matrix.rows[(size_t) r];
                if (aux) { m.auxCurveData = c; m.auxCustom = true; } else { m.srcCurve = c; m.customCurve = true; }
            });
            if (commit) proc.commitHistory ("Edit Curve");
        };
        auto slew = [&proc, r, getRow] (bool isRise, DragValue& d)
        {
            d.get = [getRow, isRise] { const auto* m = getRow(); return m == nullptr ? 0.0f : (isRise ? m->rise : m->fall); };
            d.set = [&proc, r, isRise] (float v, bool commit)
            {
                proc.editPatch ("", [&] (Patch& p) { if (r < (int) p.matrix.rows.size()) (isRise ? p.matrix.rows[(size_t) r].rise : p.matrix.rows[(size_t) r].fall) = v; });
                if (commit) proc.commitHistory (isRise ? "Curve Rise" : "Curve Fall");
            };
        };
        slew (true, rise);
        slew (false, fall);
        rise.setVisible (! aux);
        fall.setVisible (! aux);
        setVisible (true);
        toFront (false);
        repaint();
    }
    void paint (Graphics& g) override
    {
        g.fillAll (colours::panel);
        g.setColour (colours::ink);
        g.drawRect (getLocalBounds(), 2);
        drawText (g, String ("ROW ") + String (row + 1) + (isAux ? "  AUX CURVE" : "  SOURCE CURVE"), { 14.0f, 8.0f, 300.0f, 18.0f }, 14.0f, colours::brand, true, Justification::centredLeft);
        drawText (g, "Double-click: add/remove point. Drag a segment to bend; Alt bends all.", { 14.0f, (float) getHeight() - 24.0f, 440.0f, 16.0f },
                  11.0f, colours::inkDim, false, Justification::centredLeft);
    }
    void resized() override
    {
        close.setBounds (getWidth() - 74, 6, 64, 24);
        editor.setBounds (14, 36, getWidth() - 28, getHeight() - 104);
        rise.setBounds (14, getHeight() - 60, 150, 26);
        fall.setBounds (180, getHeight() - 60, 150, 26);
    }
    void refreshView() { editor.repaint(); rise.repaint(); fall.repaint(); }
    fxui::CurveEditor editor;
    int row = -1;
    bool isAux = false;
private:
    PanelContext& ctx;
    PanelButton close { "CLOSE" };
    DragValue rise, fall;
};

// The ▼ button.
class ArrowButton final : public Component, public SettableTooltipClient
{
public:
    std::function<void()> onClick;
    void paint (Graphics& g) override
    {
        const auto r = getLocalBounds().toFloat().reduced (1.0f);
        g.setColour (colours::black);
        g.fillRect (r);
        g.setColour (colours::ink);
        g.drawRect (r, 1.5f);
        Path tri;
        const auto c = r.getCentre();
        tri.addTriangle (c.x - 6, c.y - 3, c.x + 6, c.y - 3, c.x, c.y + 4);
        g.fillPath (tri);
    }
    void mouseDown (const MouseEvent& e) override { if (! e.mods.isPopupMenu() && onClick) onClick(); }
};

// ---------------------------------------------------------------------------
class MatrixPage final : public Component, public Refreshable
{
public:
    explicit MatrixPage (PanelContext& c) : ctx (c), table (c, *this), popover (c)
    {
        ctx.add (this);
        table.setSize (colX (cNumCols) + 4, kMaxModRows * kRowH);
        view.setViewedComponent (&table, false);
        view.setScrollBarsShown (true, false);
        view.setScrollBarThickness (10);
        addAndMakeVisible (view);
        menuBtn.onClick = [this] { showMenu (menuBtn, matrixMenu(), MenuOptions { MenuOptions::Place::Below, {}, 380, false }); };
        menuBtn.setTooltip ("Matrix menu");
        addAndMakeVisible (menuBtn);
        addChildComponent (popover);
    }
    ~MatrixPage() override { ctx.remove (this); }

    Menu matrixMenu()
    {
        auto& proc = ctx.proc;
        Menu m;
        m.add ("Sort by Source", [&proc] { proc.editPatch ("Sort by Source", [] (Patch& p) { p.matrix.sortBySource(); }); });
        m.add ("Sort by Destination", [&proc] { proc.editPatch ("Sort by Destination", [] (Patch& p) { p.matrix.sortByDestination(); }); });
        const bool locked = proc.patch().matrix.locked;
        m.toggle ("Lock Matrix (Keep Assignments on Preset Change)", locked, [&proc, locked]
        {
            proc.editPatch ("Lock Matrix", [locked] (Patch& p) { p.matrix.locked = ! locked; });
        });
        m.add ("Create Vibrato (Unused LFO->Pitch via Wheel)", [&proc]
        {
            proc.editAll ("Create Vibrato", [] (Patch& p, ParamValues& v) { p.matrix.createVibrato (&v, p.lfo.data()); });
        });
        m.add ("Create Velo->Amp Assignment", [&proc]
        {
            proc.editAll ("Create Velo->Amp", [] (Patch& p, ParamValues& v) { p.matrix.createVeloAmp (v); });
        });
        m.add ("Apply and Delete Macros", [&proc]
        {
            proc.editAll ("Apply and Delete Macros", [] (Patch& p, ParamValues& v) { p.matrix.applyAndDeleteMacros (v); });
        });
        return m;
    }

    void openCurve (int row, bool aux) { popover.open (row, aux); }

    void refresh() override
    {
        if (! fxui::panelVisible (*this)) return;
        // Repaint the table when the matrix data changes.
        String sig;
        for (auto& r : ctx.proc.patch().matrix.rows)
            sig << (int) r.source << "," << r.curve << "," << (int) r.customCurve << "," << r.amount << "," << (int) r.bipolar << "," << r.dest << ","
                << (int) r.aux << "," << (int) r.auxInvert << "," << r.auxCurve << "," << (int) r.auxCustom << "," << r.output << "," << (int) r.bypass
                << "," << (int) r.srcCurve.points.size() << "," << (int) r.auxCurveData.points.size() << ";";
        if (sig != shownSig) { shownSig = sig; table.repaint(); if (popover.isVisible()) popover.refreshView(); }
        table.tick();
    }

    void paint (Graphics& g) override
    {
        g.fillAll (colours::panel);
        const auto vb = view.getBounds();
        g.setColour (colours::black);
        g.fillRect (vb.getX(), 12, vb.getWidth(), 28);
        for (int c = 0; c < cNumCols; ++c)
            drawText (g, colName[c], { (float) (vb.getX() + colX (c)), 12.0f, (float) colW[c], 28.0f }, 11.5f, colours::ink, true,
                      c == cSource || c == cDest || c == cAux ? Justification::centredLeft : Justification::centred);
        drawSection (g, vb.toFloat().expanded (3.0f), {});
    }

    void resized() override
    {
        const int w = colX (cNumCols) + 16;
        const int x = (getWidth() - w) / 2;
        view.setBounds (x, 42, w, getHeight() - 54);
        menuBtn.setBounds (x + w - 40, 14, 36, 24);
        popover.setBounds (getLocalBounds().withSizeKeepingCentre (560, 420));
    }

    PanelContext& ctx;
    Table table;
    Viewport view;
    ArrowButton menuBtn;
    CurvePopover popover;
    String shownSig;
};

// ---------------------------------------------------------------------------
void Table::paint (Graphics& g)
{
    const auto& rows = ctx.proc.patch().matrix.rows;
    for (int r = 0; r < kMaxModRows; ++r)
    {
        const auto rr = Rectangle<float> (0.0f, (float) (r * kRowH), (float) getWidth(), (float) kRowH);
        g.setColour (r % 2 ? colours::panel : colours::panel.brighter (0.035f));
        g.fillRect (rr);
        const ModRow* m = r < (int) rows.size() ? &rows[(size_t) r] : nullptr;
        const float dim = m != nullptr && m->bypass ? 0.45f : 1.0f;
        auto c = [&] (int col) { return cell (r, col).toFloat(); };
        // Grip + number.
        drawText (g, String (r + 1), c (cGrip).withTrimmedLeft (14.0f), 10.5f, colours::inkDim, false, Justification::centredLeft);
        if (m != nullptr)
        {
            g.setColour (colours::inkFaint);
            for (int i = 0; i < 3; ++i) g.drawVerticalLine ((int) c (cGrip).getX() + 2 + i * 3, c (cGrip).getY() + 5, c (cGrip).getBottom() - 5);
        }
        // SOURCE / DESTINATION / AUX boxes.
        auto box = [&] (int col, const String& text, Colour sw)
        {
            const auto b = c (col);
            drawLcdBox (g, b);
            if (! sw.isTransparent())
            {
                g.setColour (sw.withMultipliedAlpha (dim));
                g.fillRect (b.withWidth (4.0f).reduced (0.0f, 3.0f).translated (3.0f, 0.0f));
            }
            drawText (g, text, b.withTrimmedLeft (11.0f).withTrimmedRight (14.0f), 12.0f, text == "---" ? colours::lcdDim : colours::lcd.withMultipliedAlpha (dim), true, Justification::centredLeft);
            Path tri;
            const float ax = b.getRight() - 8.0f, ay = b.getCentreY();
            tri.addTriangle (ax - 3.5f, ay - 2.0f, ax + 3.5f, ay - 2.0f, ax, ay + 2.5f);
            g.setColour (colours::inkDim);
            g.fillPath (tri);
        };
        const ModSource src = m ? m->source : ModSource::None;
        const ModSource aux = m ? m->aux : ModSource::None;
        box (cSource, modSourceInfo (src).shortName, src == ModSource::None ? Colour() : PanelContext::sourceColour (src));
        box (cDest, m && m->dest >= 0 ? fxui::paramDisplayName (ctx.proc, m->dest) : String ("---"), Colour());
        box (cAux, modSourceInfo (aux).shortName, aux == ModSource::None ? Colour() : PanelContext::sourceColour (aux));
        if (m == nullptr) continue;     // empty rows: just the three pop-ups
        // Curves.
        static const Curve lin;
        drawCurveThumb (g, c (cCrv), m ? m->curve : 0.5f, m && m->customCurve, m ? m->srcCurve : lin, colours::brand);
        drawCurveThumb (g, c (cAuxCrv), m ? m->auxCurve : 0.5f, m && m->auxCustom, m ? m->auxCurveData : lin, colours::brand);
        // AMOUNT (bipolar) / OUTPUT sliders.
        auto slider = [&] (int col, float v, bool bip)
        {
            const auto b = c (col).reduced (2.0f, 6.0f);
            g.setColour (colours::black);
            g.fillRect (b);
            g.setColour (colours::inkDim);
            g.drawRect (b, 1.0f);
            const float zero = bip ? b.getCentreX() : b.getX();
            const float x = bip ? b.getCentreX() + v * b.getWidth() * 0.5f : b.getX() + v * b.getWidth();
            g.setColour (kAmountBlue.withMultipliedAlpha (dim));
            g.fillRect (Rectangle<float> (jmin (zero, x), b.getY() + 2, std::abs (x - zero), b.getHeight() - 4));
            if (bip)
            {
                g.setColour (colours::ink);
                g.drawVerticalLine ((int) zero, b.getY() - 4, b.getY());
            }
            g.setColour (colours::cream);
            g.fillRect (Rectangle<float> (4.0f, b.getHeight() + 6).withCentre ({ x, b.getCentreY() }));
            drawText (g, String (roundToInt (v * 100.0f)) + "%", b.withWidth (44.0f).translated (bip ? (v < 0 ? b.getWidth() - 46.0f : 2.0f) : b.getWidth() - 46.0f, 0.0f),
                      10.5f, colours::ink.withAlpha (0.8f), true);
        };
        slider (cAmount, m ? m->amount : 0.0f, true);
        slider (cOutput, m ? m->output : 1.0f, false);
        // POL / INV / bypass / remove.
        auto toggleBox = [&] (int col, bool on, const String& t, Colour litCol)
        {
            const auto b = c (col).reduced (4.0f, 1.0f);
            g.setColour (on ? litCol : colours::black);
            g.fillRect (b);
            g.setColour (colours::inkDim);
            g.drawRect (b, 1.0f);
            drawText (g, t, b, 10.5f, on ? colours::black : colours::inkDim, true);
        };
        toggleBox (cPol, m && m->bipolar, m && m->bipolar ? "BI" : "UNI", colours::cream);
        toggleBox (cInv, m && m->auxInvert, "INV", colours::cream);
        if (m != nullptr)
        {
            fxui::BypassButton bb;
            bb.bypassed = m->bypass;
            bb.setBounds (cell (r, cBypass).reduced (2));
            Graphics::ScopedSaveState ss (g);
            g.setOrigin (bb.getPosition());
            bb.paint (g);
        }
        if (m != nullptr) drawText (g, "x", c (cRemove), 14.0f, colours::inkDim, true);
        // OUT: the live output as a short trace.
        {
            const auto b = c (cOut);
            drawLcdBox (g, b);
            g.setColour (colours::grid);
            g.drawHorizontalLine ((int) b.getCentreY(), b.getX() + 1, b.getRight() - 1);
            if (m != nullptr && m->active())
            {
                Path p;
                for (int i = 0; i < 24; ++i)
                {
                    const float v = jlimit (-1.0f, 1.0f, hist[(size_t) r][(size_t) ((histPos + 1 + i) % 24)]);
                    const auto pt = Point<float> (b.getX() + 2 + (float) i / 23.0f * (b.getWidth() - 4), b.getCentreY() - v * (b.getHeight() * 0.5f - 3));
                    if (i == 0) p.startNewSubPath (pt); else p.lineTo (pt);
                }
                g.setColour (PanelContext::sourceColour (m->source));
                g.strokePath (p, PathStrokeType (1.4f));
            }
        }
    }
    if (dropRow >= 0)
    {
        g.setColour (colours::ledYellow);
        g.fillRect (0, dropRow * kRowH - 1, getWidth(), 3);
    }
}

void Table::editRow (int r, const String& label, std::function<void (ModRow&)> fn)
{
    ctx.proc.editPatch (label, [r, fn] (Patch& p)
    {
        auto& rows = p.matrix.rows;
        if (r < (int) rows.size()) { fn (rows[(size_t) r]); return; }
        ModRow row;
        fn (row);
        p.matrix.add (row);
    });
}

float Table::sliderValue (int c, const MouseEvent& e) const
{
    const auto b = cell (downRow, c).toFloat().reduced (2.0f, 6.0f);
    const float dx = (float) e.getDistanceFromDragStartX() / b.getWidth() * (c == cAmount ? 2.0f : 1.0f) * (e.mods.isShiftDown() ? 0.2f : 1.0f);
    return jlimit (c == cAmount ? -1.0f : 0.0f, 1.0f, startVal + dx);
}

void Table::mouseDown (const MouseEvent& e)
{
    downRow = rowAt (e.y);
    downCol = colAt (e.x);
    dragging = false;
    auto& proc = ctx.proc;
    const int r = downRow;
    const ModRow* m = rowData (r);
    const auto anchorPt = cell (r, jmax (0, downCol)).getBottomLeft();
    MenuOptions at { MenuOptions::Place::AtPoint, anchorPt, 0, false };
    switch (downCol)
    {
        case cSource:
        {
            // Empty rows below the last used row fill the next free slot.
            showMenu (*this, sourceMenu ([this, r] (ModSource s) { editRow (r, "Matrix Source", [s] (ModRow& row) { row.source = s; }); },
                                         m ? m->source : ModSource::None), at);
            break;
        }
        case cDest:
            showMenu (*this, destinationMenu ([this, r] (int d)
            {
                editRow (r, "Matrix Destination", [d] (ModRow& row) { row.dest = d; row.bipolar = ModMatrixData::defaultBipolar (d); });
            }, m ? m->dest : -1), at);
            break;
        case cAux:
        {
            Menu menu;
            menu.add ("---", [this, r] { editRow (r, "Matrix Aux", [] (ModRow& row) { row.aux = ModSource::None; }); }, m == nullptr || m->aux == ModSource::None);
            const auto src = sourceMenu ([this, r] (ModSource s) { editRow (r, "Matrix Aux", [s] (ModRow& row) { row.aux = s; }); }, m ? m->aux : ModSource::None);
            for (auto& it : src.items()) menu.add (it);
            showMenu (*this, menu, at);
            break;
        }
        case cCrv: case cAuxCrv:
        {
            if (m == nullptr) break;
            const bool aux = downCol == cAuxCrv;
            if (e.mods.isPopupMenu())
            {
                Menu menu;
                const bool custom = aux ? m->auxCustom : m->customCurve;
                auto* pg = &page;
                menu.toggle ("Editable Curve", custom, [this, r, aux, custom, pg]
                {
                    editRow (r, "Editable Curve", [aux, custom] (ModRow& row) { (aux ? row.auxCustom : row.customCurve) = ! custom; });
                    if (! custom) pg->openCurve (r, aux);
                });
                menu.add ("Edit Custom Curve", [r, aux, pg] { pg->openCurve (r, aux); });
                menu.add ("Reset Curve", [this, r, aux]
                {
                    editRow (r, "Reset Curve", [aux] (ModRow& row)
                    {
                        if (aux) { row.auxCurve = 0.5f; row.auxCustom = false; row.auxCurveData = Curve(); }
                        else { row.curve = 0.5f; row.customCurve = false; row.srcCurve = Curve(); row.rise = row.fall = 0.0f; }
                    });
                });
                showContextMenu (*this, e.getPosition(), menu);
                downCol = -1;
                break;
            }
            startVal = aux ? m->auxCurve : m->curve;
            break;
        }
        case cAmount:
            if (m == nullptr) break;
            if (e.mods.isCommandDown()) { editRow (r, "Matrix Amount", [] (ModRow& row) { row.amount = 0.0f; }); downCol = -1; break; }
            startVal = m->amount;
            break;
        case cOutput:
            if (m == nullptr) break;
            if (e.mods.isCommandDown()) { editRow (r, "Matrix Output", [] (ModRow& row) { row.output = 1.0f; }); downCol = -1; break; }
            startVal = m->output;
            break;
        case cPol:
            if (m) editRow (r, "Matrix Polarity", [] (ModRow& row) { row.bipolar = ! row.bipolar; });
            break;
        case cInv:
            if (m) editRow (r, "Matrix Aux Invert", [] (ModRow& row) { row.auxInvert = ! row.auxInvert; });
            break;
        case cBypass:
            if (m) editRow (r, "Matrix Bypass", [] (ModRow& row) { row.bypass = ! row.bypass; });
            break;
        case cRemove:
            if (m) proc.editPatch ("Remove Modulation", [r] (Patch& p) { p.matrix.remove (r); });
            break;
        default: break;
    }
}

void Table::mouseDrag (const MouseEvent& e)
{
    const ModRow* m = rowData (downRow);
    if (m == nullptr || e.mods.isPopupMenu()) return;
    if (! dragging && e.getDistanceFromDragStart() < 3) return;
    dragging = true;
    const int r = downRow;
    auto& proc = ctx.proc;
    switch (downCol)
    {
        case cGrip:
        {
            const int n = (int) proc.patch().matrix.rows.size();
            dropRow = jlimit (0, n, (e.y + kRowH / 2) / kRowH);
            repaint();
            break;
        }
        case cAmount: case cOutput:
        {
            const float v = sliderValue (downCol, e);
            const bool amt = downCol == cAmount;
            proc.editPatch ("", [r, v, amt] (Patch& p) { if (r < (int) p.matrix.rows.size()) (amt ? p.matrix.rows[(size_t) r].amount : p.matrix.rows[(size_t) r].output) = v; });
            if (ctx.setStatus) ctx.setStatus ((amt ? "Amount: " : "Output: ") + String (roundToInt (v * 100.0f)) + "%");
            repaint (cell (r, downCol));
            break;
        }
        case cCrv: case cAuxCrv:
        {
            const float v = jlimit (0.0f, 1.0f, startVal - (float) e.getDistanceFromDragStartY() / 150.0f);
            const bool aux = downCol == cAuxCrv;
            proc.editPatch ("", [r, v, aux] (Patch& p) { if (r < (int) p.matrix.rows.size()) (aux ? p.matrix.rows[(size_t) r].auxCurve : p.matrix.rows[(size_t) r].curve) = v; });
            repaint (cell (r, downCol));
            break;
        }
        default: break;
    }
}

void Table::mouseUp (const MouseEvent& e)
{
    if (! dragging) return;
    dragging = false;
    auto& proc = ctx.proc;
    switch (downCol)
    {
        case cGrip:
        {
            const int from = downRow;
            int to = dropRow;
            dropRow = -1;
            if (to > from) --to;
            const int n = (int) proc.patch().matrix.rows.size();
            to = jlimit (0, jmax (0, n - 1), to);
            if (to != from) proc.editPatch ("Move Modulation", [from, to] (Patch& p) { p.matrix.move (from, to); });
            repaint();
            break;
        }
        case cAmount: proc.commitHistory ("Matrix Amount"); break;
        case cOutput: proc.commitHistory ("Matrix Output"); break;
        case cCrv: case cAuxCrv: proc.commitHistory ("Matrix Curve"); break;
        default: break;
    }
    ignoreUnused (e);
}

void Table::mouseDoubleClick (const MouseEvent& e)
{
    const int r = rowAt (e.y), c = colAt (e.x);
    if (rowData (r) == nullptr) return;
    if (c == cCrv || c == cAuxCrv)
    {
        const bool aux = c == cAuxCrv;
        editRow (r, "Editable Curve", [aux] (ModRow& row) { (aux ? row.auxCustom : row.customCurve) = true; });
        page.openCurve (r, aux);
    }
}

void Table::mouseMove (const MouseEvent& e)
{
    static const char* tips[cNumCols] {
        "Drag to reorder", "Modulation source", "Source curve: drag to bend, double-click to edit", "Depth (Cmd/Ctrl-click resets)",
        "Unipolar / bipolar", "Destination parameter", "Live output", "Aux source scales the depth", "Invert the aux",
        "Aux curve: drag to bend, double-click to edit", "Output scaling", "Bypass this row", "Remove this row" };
    const int c = colAt (e.x);
    if (c >= 0 && ctx.setStatus) ctx.setStatus (tips[c]);
}
} // namespace

std::unique_ptr<juce::Component> createMatrixPage (PanelContext& ctx)
{
    return std::make_unique<MatrixPage> (ctx);
}
} // namespace tincture::ui
