#include "Curve.h"

#include <algorithm>
#include <cmath>

namespace tincture
{
Curve::Curve() { points = { { 0.0f, 0.0f, 0.0f, false }, { 1.0f, 1.0f, 0.0f, false } }; }

Curve Curve::line (float y0, float y1)
{
    Curve c;
    c.points = { { 0.0f, y0, 0.0f, false }, { 1.0f, y1, 0.0f, false } };
    return c;
}

Curve Curve::triangle()
{
    Curve c;
    c.points = { { 0.0f, 0.0f, 0.0f, false }, { 0.5f, 1.0f, 0.0f, false }, { 1.0f, 0.0f, 0.0f, false } };
    return c;
}

float Curve::bend (float t, float c) noexcept
{
    if (std::abs (c) < 1.0e-4f) return t;
    // Exponential bend; |c| up to 1 gives a strong but finite curvature.
    const float k = c * 9.0f;
    return (1.0f - std::exp (-k * t)) / (1.0f - std::exp (-k));
}

float Curve::evaluate (float x) const noexcept
{
    if (points.empty()) return 0.0f;
    if (x <= points.front().x) return points.front().y;
    if (x >= points.back().x) return points.back().y;
    // Binary search for the segment.
    size_t lo = 0, hi = points.size() - 1;
    while (hi - lo > 1)
    {
        const size_t mid = (lo + hi) / 2;
        if (points[mid].x <= x) lo = mid; else hi = mid;
    }
    const auto& a = points[lo];
    const auto& b = points[hi];
    if (b.flat) return a.y;
    const float w = b.x - a.x;
    if (w <= 1.0e-7f) return b.y;
    const float t = (x - a.x) / w;
    return a.y + (b.y - a.y) * bend (t, b.curve);
}

void Curve::sortAndClamp()
{
    for (auto& p : points)
    {
        p.x = std::clamp (p.x, 0.0f, 1.0f);
        p.y = std::clamp (p.y, 0.0f, 1.0f);
        p.curve = std::clamp (p.curve, -1.0f, 1.0f);
    }
    std::stable_sort (points.begin(), points.end(), [] (const CurvePoint& a, const CurvePoint& b) { return a.x < b.x; });
    if (points.empty()) *this = Curve();
}

int Curve::insert (float x, float y)
{
    CurvePoint p { std::clamp (x, 0.0f, 1.0f), std::clamp (y, 0.0f, 1.0f), 0.0f, false };
    auto it = std::upper_bound (points.begin(), points.end(), p.x, [] (float v, const CurvePoint& q) { return v < q.x; });
    const int index = (int) (it - points.begin());
    points.insert (it, p);
    return index;
}

bool Curve::remove (int i)
{
    // The end points stay.
    if (i <= 0 || i >= (int) points.size() - 1) return false;
    points.erase (points.begin() + i);
    return true;
}

void Curve::bake (std::array<float, kLutSize + 1>& lut) const
{
    for (int i = 0; i <= kLutSize; ++i)
        lut[(size_t) i] = evaluate ((float) i / (float) kLutSize);
}

void Curve::save (Node& n) const
{
    std::vector<float> d;
    d.reserve (points.size() * 4);
    for (const auto& p : points) { d.push_back (p.x); d.push_back (p.y); d.push_back (p.curve); d.push_back (p.flat ? 1.0f : 0.0f); }
    n.set ("points", std::move (d));
}

void Curve::load (const Node& n)
{
    if (auto* d = n.floats ("points"); d != nullptr && d->size() >= 8 && d->size() % 4 == 0)
    {
        points.clear();
        for (size_t i = 0; i + 3 < d->size(); i += 4)
            points.push_back ({ (*d)[i], (*d)[i + 1], (*d)[i + 2], (*d)[i + 3] > 0.5f });
        sortAndClamp();
    }
}

bool Curve::operator== (const Curve& o) const
{
    if (points.size() != o.points.size()) return false;
    for (size_t i = 0; i < points.size(); ++i)
        if (points[i].x != o.points[i].x || points[i].y != o.points[i].y || points[i].curve != o.points[i].curve || points[i].flat != o.points[i].flat)
            return false;
    return true;
}
} // namespace tincture
