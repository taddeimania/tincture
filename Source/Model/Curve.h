// Curve.h — a breakpoint curve: points joined by bendable segments.
// Used by LFO shapes, the VELO / NOTE graphs, warp remap graphs, the mod
// matrix CRV editors and the envelope display.
//
// x runs 0..1 (strictly increasing; first point at 0, last at 1 for shapes),
// y runs 0..1. Each point's `curve` bends the segment that ENDS at it:
// 0 = straight, >0 bows up (fast start), <0 bows down. `flat` makes the
// segment a step (hold the previous y). Evaluation for the audio thread
// goes through a baked lookup table (bake()).
#pragma once

#include "Tree.h"

#include <array>
#include <vector>

namespace tincture
{
struct CurvePoint
{
    float x = 0.0f, y = 0.0f, curve = 0.0f;
    bool flat = false;
};

class Curve
{
public:
    static constexpr int kLutSize = 1024;

    std::vector<CurvePoint> points;

    Curve();                                           // a straight line 0,0 -> 1,1
    static Curve line (float y0, float y1);
    static Curve triangle();                           // 0 -> 1 at 0.5 -> 0 (the LFO default)

    // Exact evaluation (UI, baking).
    float evaluate (float x) const noexcept;
    // Shape of one segment, t 0..1 -> 0..1, for a given bend.
    static float bend (float t, float curve) noexcept;

    void sortAndClamp();
    // Insert/remove points (UI editing); returns new index / success.
    int insert (float x, float y);
    bool remove (int index);

    void bake (std::array<float, kLutSize + 1>& lut) const;

    void save (Node& n) const;
    void load (const Node& n);
    bool operator== (const Curve& o) const;
};

// A curve baked for the audio thread: linear lookup in a 1025-entry table.
struct BakedCurve
{
    std::array<float, Curve::kLutSize + 1> lut {};
    BakedCurve() { Curve().bake (lut); }
    explicit BakedCurve (const Curve& c) { c.bake (lut); }
    float operator() (float x) const noexcept
    {
        x = x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x);
        const float f = x * (float) Curve::kLutSize;
        const int i = (int) f;
        if (i >= Curve::kLutSize) return lut[Curve::kLutSize];
        return lut[(size_t) i] + (lut[(size_t) i + 1] - lut[(size_t) i]) * (f - (float) i);
    }
};
} // namespace tincture
