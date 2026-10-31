// CurveEditor.cpp — see CurveEditor.h.
#include "CurveEditor.h"
#include "Bind.h"

namespace tincture::ui
{
using namespace juce;

namespace
{
constexpr float kEps = 1.0e-5f;
constexpr float kPointR = 4.0f, kHandleR = 3.2f;

// Drop zero-width segments that don't jump (same x and y as the previous point)
// and keep at most two points at one x (the left limit and the right value).
void tidy (std::vector<CurvePoint>& p)
{
    std::vector<CurvePoint> out;
    for (const auto& q : p)
    {
        if (! out.empty() && std::abs (q.x - out.back().x) <= kEps)
        {
            if (std::abs (q.y - out.back().y) <= kEps) continue;
            if (out.size() >= 2 && std::abs (out[out.size() - 2].x - q.x) <= kEps) { out.back() = q; out.back().x = out[out.size() - 2].x; continue; }
        }
        out.push_back (q);
    }
    p = std::move (out);
}
} // namespace

CurveEditor::CurveEditor()
{
    setRepaintsOnMouseActivity (false);
}

Rectangle<float> CurveEditor::plotArea() const
{
    return getLocalBounds().toFloat().reduced (padding);
}

Point<float> CurveEditor::toScreen (float x, float y) const
{
    const auto r = plotArea();
    return { r.getX() + x * r.getWidth(), r.getBottom() - y * r.getHeight() };
}

Point<float> CurveEditor::fromScreen (Point<float> p) const
{
    const auto r = plotArea();
    return { jlimit (0.0f, 1.0f, (p.x - r.getX()) / jmax (1.0f, r.getWidth())),
             jlimit (0.0f, 1.0f, (r.getBottom() - p.y) / jmax (1.0f, r.getHeight())) };
}

Point<float> CurveEditor::pointPos (int i) const
{
    if (! isPositiveAndBelow (i, (int) cur.points.size())) return {};
    const auto& q = cur.points[(size_t) i];
    return toScreen (q.x, q.y);
}

Point<float> CurveEditor::handlePos (int i) const
{
    if (i < 1 || i >= (int) cur.points.size()) return {};
    const auto& a = cur.points[(size_t) i - 1];
    const auto& b = cur.points[(size_t) i];
    return toScreen ((a.x + b.x) * 0.5f, a.y + (b.y - a.y) * Curve::bend (0.5f, b.curve));
}

int CurveEditor::hitPoint (Point<float> p) const
{
    int best = -1;
    float bestD = 7.5f;
    for (int i = 0; i < (int) cur.points.size(); ++i)
    {
        const float d = pointPos (i).getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

int CurveEditor::hitHandle (Point<float> p) const
{
    int best = -1;
    float bestD = 6.5f;
    for (int i = 1; i < (int) cur.points.size(); ++i)
    {
        const auto& a = cur.points[(size_t) i - 1];
        const auto& b = cur.points[(size_t) i];
        if (b.flat || b.x - a.x < kEps || std::abs (b.y - a.y) < 1.0e-3f) continue;
        const float d = handlePos (i).getDistanceFrom (p);
        if (d < bestD) { bestD = d; best = i; }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Edits
// ---------------------------------------------------------------------------
Curve CurveEditor::drawCell (const Curve& c, float x0, float x1, float y, Tool t)
{
    const auto& P = c.points;
    std::vector<CurvePoint> out;
    // Left part, up to and including the left limit at x0.
    for (const auto& q : P) if (q.x < x0 - kEps) out.push_back (q);
    if (x0 > kEps)
    {
        bool at = false;
        for (const auto& q : P)
            if (std::abs (q.x - x0) <= kEps) { auto k = q; k.x = x0; out.push_back (k); at = true; break; }
        if (! at)
        {
            // Split the segment that crosses x0 (keep its bend / step type).
            CurvePoint k { x0, c.evaluate (x0), 0.0f, false };
            for (const auto& q : P) if (q.x > x0) { k.curve = q.curve; k.flat = q.flat; break; }
            out.push_back (k);
        }
    }
    const float ys = t == Tool::RampUp ? 0.0f : y;
    const float ye = t == Tool::RampDown ? 0.0f : y;
    out.push_back ({ x0 <= kEps ? 0.0f : x0, ys, 0.0f, false });
    out.push_back ({ x1, ye, 0.0f, t == Tool::Flat });
    // Right part: the right value at x1, then the rest.
    if (x1 < 1.0f - kEps)
    {
        const CurvePoint* lastAt = nullptr;
        for (const auto& q : P) if (std::abs (q.x - x1) <= kEps) lastAt = &q;
        CurvePoint k = lastAt != nullptr ? *lastAt : CurvePoint { x1, c.evaluate (x1), 0.0f, false };
        k.x = x1;
        k.curve = 0.0f;
        k.flat = false;
        out.push_back (k);
        for (const auto& q : P) if (q.x > x1 + kEps) out.push_back (q);
    }
    tidy (out);
    Curve r;
    r.points = out;
    if (r.points.front().x > 0.0f) r.points.front().x = 0.0f;
    if (r.points.back().x < 1.0f) r.points.back().x = 1.0f;
    return r;
}

Curve CurveEditor::flipVertical (const Curve& c, const std::set<int>& sel)
{
    Curve r = c;
    for (int i = 0; i < (int) r.points.size(); ++i)
        if (sel.empty() || sel.count (i) > 0)
            r.points[(size_t) i].y = 1.0f - r.points[(size_t) i].y;
    return r;
}

Curve CurveEditor::flipHorizontal (const Curve& c, const std::set<int>& sel)
{
    const auto& P = c.points;
    if (P.size() < 2) return c;
    if (! sel.empty() && sel.size() < P.size())
    {
        // Mirror the selected points' x within their own span.
        float lo = 1.0f, hi = 0.0f;
        for (int i : sel) { lo = jmin (lo, P[(size_t) i].x); hi = jmax (hi, P[(size_t) i].x); }
        Curve r = c;
        for (int i : sel)
            if (i > 0 && i < (int) P.size() - 1)
                r.points[(size_t) i].x = lo + hi - P[(size_t) i].x;
        r.sortAndClamp();
        return r;
    }
    std::vector<CurvePoint> out;
    out.push_back ({ 0.0f, P.back().y, 0.0f, false });
    for (int i = (int) P.size() - 1; i >= 1; --i)
    {
        const auto& b = P[(size_t) i];
        const auto& a = P[(size_t) i - 1];
        if (b.flat)
        {
            out.push_back ({ 1.0f - b.x, a.y, 0.0f, false });          // jump to the held value
            out.push_back ({ 1.0f - a.x, a.y, 0.0f, true });
        }
        else
            out.push_back ({ 1.0f - a.x, a.y, -b.curve, false });
    }
    tidy (out);
    Curve r;
    r.points = out;
    r.points.front().x = 0.0f;
    r.points.back().x = 1.0f;
    return r;
}

void CurveEditor::edit (const Curve& c)
{
    cur = c;
    changed = true;
    if (onEdit) onEdit (c);
    repaint();
}

void CurveEditor::drawAt (Point<float> p, Tool t)
{
    const auto v = fromScreen (p);
    const int gx = jmax (1, gridX);
    const int cell = jlimit (0, gx - 1, (int) (v.x * (float) gx));
    const float y = snapTo (v.y, jmax (1, gridY));
    if (cell == lastCell && std::abs (y - lastY) < 1.0e-5f) return;
    lastCell = cell;
    lastY = y;
    edit (drawCell (cur, (float) cell / (float) gx, (float) (cell + 1) / (float) gx, y, t));
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void CurveEditor::mouseDown (const MouseEvent& e)
{
    if (getCurve) cur = getCurve();
    start = cur;
    changed = false;
    gesture = Gesture::None;
    downPos = e.position;
    const int hp = hitPoint (e.position);
    const int hh = hp < 0 ? hitHandle (e.position) : -1;

    if (e.mods.isPopupMenu())
    {
        if (! pointMenu) return;
        if (hp >= 0 && selection.count (hp) == 0) { selection.clear(); }
        showContextMenu (*this, e.getPosition(), pointMenu (hp >= 0 ? hp : hh, hp < 0 && hh >= 0));
        return;
    }
    if (! editable) return;

    if (e.mods.isShiftDown() && e.mods.isCommandDown() && hp >= 0)
    {
        if (onSetLoopback) onSetLoopback (hp);
        return;
    }
    if (e.mods.isShiftDown() || tool != Tool::Point)
    {
        drawTool = e.mods.isShiftDown() ? shiftTool : tool;
        if (drawTool == Tool::Point) drawTool = Tool::Flat;
        lastCell = -1;
        gesture = Gesture::Draw;
        drawAt (e.position, drawTool);
        return;
    }
    if (hp >= 0)
    {
        gesture = Gesture::Point;
        dragIndex = hp;
        relative = e.mods.isCommandDown();
        if (selection.count (hp) == 0) selection = { hp };
        dragSet = selection;
        repaint();
        return;
    }
    if (hh >= 0)
    {
        gesture = Gesture::Handle;
        dragIndex = hh;
        bendAll = e.mods.isAltDown();
        return;
    }
    gesture = Gesture::Marquee;
    marqueeEnd = e.position;
    selection.clear();
    repaint();
}

void CurveEditor::mouseDrag (const MouseEvent& e)
{
    if (e.mods.isPopupMenu() || ! editable) return;
    switch (gesture)
    {
        case Gesture::Draw:
            drawAt (e.position, drawTool);
            break;

        case Gesture::Point:
        {
            const auto r = plotArea();
            float dx = (e.position.x - downPos.x) / jmax (1.0f, r.getWidth());
            float dy = (downPos.y - e.position.y) / jmax (1.0f, r.getHeight());
            Curve c = start;
            auto& P = c.points;
            const int n = (int) P.size();
            const auto& g = start.points[(size_t) dragIndex];
            if (e.mods.isAltDown())
            {
                // Snap the grabbed point to the grid; the rest follow by the same delta.
                dx = snapTo (g.x + dx, jmax (1, gridX)) - g.x;
                dy = snapTo (g.y + dy, jmax (1, gridY)) - g.y;
            }
            if (relative)
            {
                // Distance-weighted move of every point (y), the grabbed one also in x.
                for (int i = 0; i < n; ++i)
                {
                    const float w = jmax (0.0f, 1.0f - std::abs (P[(size_t) i].x - g.x) * 2.0f);
                    P[(size_t) i].y = jlimit (0.0f, 1.0f, start.points[(size_t) i].y + dy * w);
                }
                dragSet = { dragIndex };
                dy = 0.0f;
            }
            for (int i : dragSet)
            {
                if (! isPositiveAndBelow (i, n)) continue;
                auto& q = P[(size_t) i];
                const auto& o = start.points[(size_t) i];
                if (! relative) q.y = jlimit (0.0f, 1.0f, o.y + dy);
                if (i == 0 || i == n - 1) continue;              // end points keep x
                float lo = 0.0f, hi = 1.0f;
                for (int k = i - 1; k >= 0; --k) if (dragSet.count (k) == 0) { lo = start.points[(size_t) k].x; break; }
                for (int k = i + 1; k < n; ++k) if (dragSet.count (k) == 0) { hi = start.points[(size_t) k].x; break; }
                q.x = jlimit (lo, hi, o.x + dx);
            }
            std::stable_sort (P.begin(), P.end(), [] (const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
            edit (c);
            break;
        }

        case Gesture::Handle:
        {
            Curve c = start;
            auto& P = c.points;
            const float dy = (downPos.y - e.position.y) / 70.0f;
            auto bendOne = [&] (int i)
            {
                const auto& a = P[(size_t) i - 1];
                auto& b = P[(size_t) i];
                // Dragging up bows the segment up whichever way it goes.
                const float dir = b.y >= a.y ? 1.0f : -1.0f;
                b.curve = jlimit (-1.0f, 1.0f, start.points[(size_t) i].curve + dy * dir);
            };
            if (bendAll) { for (int i = 1; i < (int) P.size(); ++i) bendOne (i); }
            else bendOne (dragIndex);
            edit (c);
            break;
        }

        case Gesture::Marquee:
        {
            marqueeEnd = e.position;
            const auto box = Rectangle<float> (downPos, marqueeEnd);
            selection.clear();
            for (int i = 0; i < (int) cur.points.size(); ++i)
                if (box.contains (pointPos (i))) selection.insert (i);
            repaint();
            break;
        }

        case Gesture::None: break;
    }
}

void CurveEditor::mouseUp (const MouseEvent&)
{
    const auto g = gesture;
    gesture = Gesture::None;
    relative = false;
    if (changed && onCommit)
        onCommit (g == Gesture::Handle ? "Bend Curve" : g == Gesture::Draw ? "Draw Steps" : "Move Point");
    changed = false;
    repaint();
}

void CurveEditor::mouseDoubleClick (const MouseEvent& e)
{
    if (! editable || tool != Tool::Point) return;
    if (getCurve) cur = getCurve();
    const int hp = hitPoint (e.position);
    Curve c = cur;
    if (hp >= 0)
    {
        if (! c.remove (hp)) return;
        selection.clear();
        edit (c);
        if (onCommit) onCommit ("Remove Point");
    }
    else
    {
        const auto v = fromScreen (e.position);
        const int i = c.insert (v.x, v.y);
        selection = { i };
        edit (c);
        if (onCommit) onCommit ("Add Point");
    }
    changed = false;
}

void CurveEditor::mouseMove (const MouseEvent& e)
{
    const int hp = hitPoint (e.position);
    const int hh = hp < 0 ? hitHandle (e.position) : -1;
    if (hp != hoverPoint || hh != hoverHandle) { hoverPoint = hp; hoverHandle = hh; repaint(); }
    setMouseCursor (hp >= 0 || hh >= 0 ? MouseCursor::PointingHandCursor : MouseCursor::NormalCursor);
}

void CurveEditor::mouseExit (const MouseEvent&)
{
    if (hoverPoint >= 0 || hoverHandle >= 0) { hoverPoint = hoverHandle = -1; repaint(); }
}

// ---------------------------------------------------------------------------
// Mod-source drops onto points (§2.6: X / Y drop buttons)
// ---------------------------------------------------------------------------
Rectangle<float> CurveEditor::dropBox (int point, int axis) const
{
    const auto p = pointPos (point);
    const float x = p.x + (p.x > plotArea().getRight() - 30.0f ? -26.0f : 8.0f);
    return { x, p.y - 17.0f + (float) axis * 17.0f, 18.0f, 16.0f };
}

bool CurveEditor::isInterestedInDragSource (const SourceDetails& d)
{
    return editable && onPointDrop && modSourceFromDrag (d.description) != ModSource::None;
}

void CurveEditor::itemDragMove (const SourceDetails& d)
{
    const auto p = d.localPosition.toFloat();
    int pt = dropPoint, axis = -1;
    if (pt >= 0)
        for (int a = 0; a < 2; ++a) if (dropBox (pt, a).expanded (2.0f).contains (p)) axis = a;
    if (axis < 0)
    {
        // Nearest point within reach.
        pt = -1;
        float best = 40.0f;
        for (int i = 0; i < (int) cur.points.size(); ++i)
        {
            const float dd = pointPos (i).getDistanceFrom (p);
            if (dd < best) { best = dd; pt = i; }
        }
    }
    if (pt != dropPoint || axis != dropAxis) { dropPoint = pt; dropAxis = axis; repaint(); }
}

void CurveEditor::itemDragExit (const SourceDetails&)
{
    dropPoint = dropAxis = -1;
    repaint();
}

void CurveEditor::itemDropped (const SourceDetails& d)
{
    itemDragMove (d);
    const int pt = dropPoint, axis = dropAxis;
    dropPoint = dropAxis = -1;
    repaint();
    if (pt >= 0 && axis >= 0 && onPointDrop) onPointDrop (pt, axis, modSourceFromDrag (d.description));
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void CurveEditor::update()
{
    if (gesture != Gesture::None) return;
    Curve c = getCurve ? getCurve() : cur;
    bool dirty = false;
    if (! (c == cur)) { cur = c; dirty = true; }
    for (auto it = selection.begin(); it != selection.end();)
        it = *it >= (int) cur.points.size() ? selection.erase (it) : std::next (it);
    if (std::abs (playhead - shownPlayhead) > 1.0e-4f) dirty = true;
    if (loopback != shownLoop) dirty = true;
    if (! trace.empty() || shownTrace != 0) dirty = true;
    if (dirty) repaint();
}

void CurveEditor::rebuildPath (const Curve& c)
{
    curvePath.clear();
    fillPath.clear();
    const auto& P = c.points;
    if (P.empty()) return;
    const auto r = plotArea();
    curvePath.startNewSubPath (toScreen (P[0].x, P[0].y));
    for (size_t i = 1; i < P.size(); ++i)
    {
        const auto& a = P[i - 1];
        const auto& b = P[i];
        if (b.flat)
        {
            curvePath.lineTo (toScreen (b.x, a.y));
            curvePath.lineTo (toScreen (b.x, b.y));
            continue;
        }
        const float w = (b.x - a.x) * r.getWidth();
        const int steps = std::abs (b.curve) < 1.0e-4f ? 1 : jlimit (2, 64, (int) (w / 3.0f));
        for (int s = 1; s <= steps; ++s)
        {
            const float t = (float) s / (float) steps;
            curvePath.lineTo (toScreen (a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * Curve::bend (t, b.curve)));
        }
    }
    fillPath = curvePath;
    fillPath.lineTo (toScreen (P.back().x, 0.0f));
    fillPath.lineTo (toScreen (P.front().x, 0.0f));
    fillPath.closeSubPath();
    pathCurve = c;
    pathValid = true;
}

void CurveEditor::paint (Graphics& g)
{
    const auto bounds = getLocalBounds().toFloat();
    drawLcdBox (g, bounds);
    const auto r = plotArea();

    if (showGrid)
    {
        g.setColour (colours::grid);
        for (int i = 1; i < gridX; ++i)
            g.drawVerticalLine (roundToInt (r.getX() + r.getWidth() * (float) i / (float) gridX), r.getY(), r.getBottom());
        for (int i = 1; i < gridY; ++i)
            g.drawHorizontalLine (roundToInt (r.getY() + r.getHeight() * (float) i / (float) gridY), r.getX(), r.getRight());
        g.setColour (colours::grid.brighter (0.25f));
        g.drawRect (r, 1.0f);
    }

    shownPlayhead = playhead;
    shownLoop = loopback;
    shownTrace = trace.size();

    if (! trace.empty())
    {
        Path p;
        for (size_t i = 0; i < trace.size(); ++i)
        {
            const auto q = toScreen ((float) i / (float) jmax<size_t> (1, trace.size() - 1), jlimit (0.0f, 1.0f, trace[i]));
            if (i == 0) p.startNewSubPath (q); else p.lineTo (q);
        }
        g.setColour (colour);
        g.strokePath (p, PathStrokeType (1.6f));
        if (overlayText.isNotEmpty())
            drawText (g, overlayText, r.withHeight (16.0f), 10.5f, colours::inkDim, true, Justification::centredRight);
        return;
    }

    if (! pathValid || ! (pathCurve == cur)) rebuildPath (cur);
    if (fill)
    {
        g.setColour (colour.withAlpha (0.14f));
        g.fillPath (fillPath);
    }
    g.setColour (editable ? colour : colour.withAlpha (0.6f));
    g.strokePath (curvePath, PathStrokeType (1.8f, PathStrokeType::curved, PathStrokeType::rounded));

    // Loopback marker.
    if (isPositiveAndBelow (loopback, (int) cur.points.size() - 1))
    {
        const float x = pointPos (loopback).x;
        g.setColour (colours::ink.withAlpha (0.7f));
        const float dashes[] { 3.0f, 3.0f };
        g.drawDashedLine (Line<float> (x, r.getY(), x, r.getBottom()), dashes, 2, 1.0f);
        drawText (g, "LOOP", { x + 2.0f, r.getY(), 32.0f, 11.0f }, 9.0f, colours::ink.withAlpha (0.8f), true, Justification::centredLeft);
    }

    // Point modulation ranges.
    for (const auto& m : modBars)
    {
        if (! isPositiveAndBelow (m.point, (int) cur.points.size())) continue;
        const auto p = pointPos (m.point);
        g.setColour (m.colour.withAlpha (0.45f));
        if (std::abs (m.dy) > 1.0e-4f)
        {
            const float y2 = toScreen (0.0f, jlimit (0.0f, 1.0f, cur.points[(size_t) m.point].y + m.dy)).y;
            g.fillRect (Rectangle<float> (p.x - 2.5f, jmin (p.y, y2), 5.0f, std::abs (y2 - p.y)));
        }
        if (std::abs (m.dx) > 1.0e-4f)
        {
            const float x2 = toScreen (jlimit (0.0f, 1.0f, cur.points[(size_t) m.point].x + m.dx), 0.0f).x;
            g.fillRect (Rectangle<float> (jmin (p.x, x2), p.y - 2.5f, std::abs (x2 - p.x), 5.0f));
        }
        if (std::abs (m.dc) > 1.0e-4f && m.point >= 1)
        {
            const auto h = handlePos (m.point);
            g.drawEllipse (Rectangle<float> (12.0f, 12.0f).withCentre (h), 2.0f);
        }
    }

    if (editable)
    {
        // Curve handles.
        for (int i = 1; i < (int) cur.points.size(); ++i)
        {
            const auto& a = cur.points[(size_t) i - 1];
            const auto& b = cur.points[(size_t) i];
            if (b.flat || b.x - a.x < kEps || std::abs (b.y - a.y) < 1.0e-3f) continue;
            if ((b.x - a.x) * r.getWidth() < 9.0f) continue;
            const auto h = handlePos (i);
            g.setColour (i == hoverHandle ? colours::ink : colour.withAlpha (0.8f));
            g.drawEllipse (Rectangle<float> (kHandleR * 2.0f, kHandleR * 2.0f).withCentre (h), 1.2f);
        }
        // Points.
        const bool rainbow = gesture == Gesture::Point && relative;
        for (int i = 0; i < (int) cur.points.size(); ++i)
        {
            const auto p = pointPos (i);
            const bool sel = selection.count (i) > 0;
            const auto box = Rectangle<float> (kPointR * 2.0f, kPointR * 2.0f).withCentre (p);
            Colour c = sel ? colours::cream : colours::black;
            if (rainbow) c = Colour::fromHSV (jlimit (0.0f, 1.0f, std::abs (cur.points[(size_t) i].x - cur.points[(size_t) dragIndex].x)) * 0.8f, 0.7f, 1.0f, 1.0f);
            g.setColour (c);
            g.fillEllipse (box);
            g.setColour (i == hoverPoint ? colours::ink : colour);
            g.drawEllipse (box, 1.4f);
        }
    }

    // Playhead.
    if (playhead >= 0.0f)
    {
        const auto p = toScreen (playhead, cur.evaluate (playhead));
        g.setColour (colours::ink.withAlpha (0.35f));
        g.drawVerticalLine (roundToInt (p.x), r.getY(), r.getBottom());
        g.setColour (colours::ink);
        g.fillEllipse (Rectangle<float> (6.0f, 6.0f).withCentre (p));
    }

    if (gesture == Gesture::Marquee)
    {
        const auto box = Rectangle<float> (downPos, marqueeEnd);
        g.setColour (colours::ink.withAlpha (0.12f));
        g.fillRect (box);
        g.setColour (colours::ink.withAlpha (0.6f));
        g.drawRect (box, 1.0f);
    }

    if (dropPoint >= 0)
    {
        for (int a = 0; a < 2; ++a)
        {
            const auto b = dropBox (dropPoint, a);
            g.setColour (a == dropAxis ? colours::cream : colours::black);
            g.fillRect (b);
            g.setColour (colours::ink);
            g.drawRect (b, 1.0f);
            drawText (g, a == 0 ? "X" : "Y", b, 11.0f, a == dropAxis ? colours::black : colours::ink, true);
        }
    }

    if (overlayText.isNotEmpty())
        drawText (g, overlayText, r.reduced (4.0f, 2.0f).removeFromTop (14.0f), 10.5f, colours::inkDim, true, Justification::centredRight);
}
} // namespace tincture::ui
