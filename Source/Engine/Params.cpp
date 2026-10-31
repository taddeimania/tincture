#include "Params.h"
#include "ParamIds.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace tincture
{
// ---------------------------------------------------------------------------
// ParamInfo
// ---------------------------------------------------------------------------
float ParamInfo::toNorm (float x) const noexcept
{
    if (max <= min) return 0.0f;
    switch (scale)
    {
        case ParamScale::Log:
        {
            const float lo = min > 0.0f ? min : 1.0e-4f;
            x = x < lo ? lo : (x > max ? max : x);
            return std::log (x / lo) / std::log (max / lo);
        }
        case ParamScale::Power:
        {
            float t = (x - min) / (max - min);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            return std::pow (t, 1.0f / skew);
        }
        default:
        {
            const float t = (x - min) / (max - min);
            return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
        }
    }
}

float ParamInfo::fromNorm (float t) const noexcept
{
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    switch (scale)
    {
        case ParamScale::Log:
        {
            const float lo = min > 0.0f ? min : 1.0e-4f;
            return lo * std::pow (max / lo, t);
        }
        case ParamScale::Power:  return min + (max - min) * std::pow (t, skew);
        case ParamScale::Int:
        case ParamScale::Bool:
        case ParamScale::Choice: return std::round (min + (max - min) * t);
        default:                 return min + (max - min) * t;
    }
}

float ParamInfo::snap (float x) const noexcept
{
    x = x < min ? min : (x > max ? max : x);
    return isDiscrete() ? std::round (x) : x;
}

static std::string fmt (const char* f, double v)
{
    char b[64];
    std::snprintf (b, sizeof (b), f, v);
    return b;
}

std::string ParamInfo::format (float x, bool withUnit) const
{
    if (scale == ParamScale::Choice)
    {
        const int i = (int) std::lround (x);
        return i >= 0 && i < (int) choices.size() ? choices[(size_t) i] : std::string ("?");
    }
    if (scale == ParamScale::Bool) return x >= 0.5f ? "On" : "Off";
    const double v = (double) x * displayScale;
    const char* f = decimals <= 0 ? "%.0f" : decimals == 1 ? "%.1f" : decimals == 2 ? "%.2f" : "%.3f";
    switch (unit)
    {
        case Unit::Percent:   return fmt (std::abs (v) >= 10.0 ? "%.0f" : "%.1f", v) + (withUnit ? " %" : "");
        case Unit::Db:        return x <= -119.0f ? std::string ("-inf dB") : fmt ("%.1f", v) + (withUnit ? " dB" : "");
        case Unit::Hz:        return v >= 1000.0 ? fmt (v >= 10000.0 ? "%.1f" : "%.2f", v / 1000.0) + (withUnit ? " kHz" : "")
                                                 : fmt (v >= 100.0 ? "%.0f" : v >= 10.0 ? "%.1f" : "%.2f", v) + (withUnit ? " Hz" : "");
        case Unit::Seconds:   return v < 1.0 ? fmt (v < 0.01 ? "%.2f" : "%.1f", v * 1000.0) + (withUnit ? " ms" : "")
                                             : fmt ("%.2f", v) + (withUnit ? " s" : "");
        case Unit::Ms:        return fmt (v >= 100.0 ? "%.0f" : "%.1f", v) + (withUnit ? " ms" : "");
        case Unit::Semitones: return fmt (scale == ParamScale::Int ? "%+.0f" : "%+.2f", v) + (withUnit ? " st" : "");
        case Unit::Cents:     return fmt ("%+.0f", v) + (withUnit ? " ct" : "");
        case Unit::Degrees:   return fmt ("%.0f", v) + (withUnit ? "\xc2\xb0" : "");
        case Unit::Octaves:   return fmt ("%+.2f", v) + (withUnit ? " oct" : "");
        case Unit::Bpm:       return fmt ("%.1f", v) + (withUnit ? " BPM" : "");
        case Unit::Note:
        {
            static const char* n[12] { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
            const int m = (int) std::lround (v);
            return std::string (n[((m % 12) + 12) % 12]) + std::to_string (m / 12 - 2);
        }
        case Unit::Ratio:     return fmt ("%.2f", v) + (withUnit ? "x" : "");
        default:              return fmt (scale == ParamScale::Int ? "%.0f" : f, v);
    }
}

bool ParamInfo::parse (const std::string& text, float& out) const
{
    if (scale == ParamScale::Choice)
    {
        for (size_t i = 0; i < choices.size(); ++i)
            if (choices[i] == text) { out = (float) i; return true; }
    }
    char* end = nullptr;
    double v = std::strtod (text.c_str(), &end);
    if (end == text.c_str()) return false;
    std::string rest (end);
    for (auto& c : rest) c = (char) std::tolower ((unsigned char) c);
    if (unit == Unit::Seconds)
    {
        const bool ms = rest.find ("ms") != std::string::npos;
        const bool s = ! ms && rest.find ('s') != std::string::npos;
        if (! s) v /= 1000.0;            // "250" and "250 ms" are milliseconds
    }
    else
    {
        if (unit == Unit::Hz && rest.find ('k') != std::string::npos) v *= 1000.0;
        v /= (double) displayScale;
    }
    out = snap ((float) v);
    return true;
}

// ---------------------------------------------------------------------------
// Registry
// ---------------------------------------------------------------------------
int ParamRegistry::add (ParamInfo info)
{
    if (info.group.empty()) info.group = group;
    if (info.shortName.empty()) info.shortName = info.name;
    list.push_back (std::move (info));
    return (int) list.size() - 1;
}

int ParamRegistry::linear (const std::string& id, const std::string& name, const std::string& sn, float mn, float mx, float def, Unit u, uint32_t flags)
{
    ParamInfo p;
    p.id = id; p.name = name; p.shortName = sn; p.min = mn; p.max = mx; p.def = def; p.unit = u; p.flags = flags;
    if (mn < 0.0f && mx > 0.0f && std::abs (mn + mx) < 1.0e-6f) p.flags |= kBipolar;
    return add (std::move (p));
}

int ParamRegistry::percent (const std::string& id, const std::string& name, const std::string& sn, float def, uint32_t flags)
{
    ParamInfo p;
    p.id = id; p.name = name; p.shortName = sn; p.min = 0.0f; p.max = 1.0f; p.def = def;
    p.unit = Unit::Percent; p.displayScale = 100.0f; p.flags = flags;
    return add (std::move (p));
}

int ParamRegistry::bipolar (const std::string& id, const std::string& name, const std::string& sn, float def, uint32_t flags)
{
    ParamInfo p;
    p.id = id; p.name = name; p.shortName = sn; p.min = -1.0f; p.max = 1.0f; p.def = def;
    p.unit = Unit::Percent; p.displayScale = 100.0f; p.flags = flags | kBipolar;
    return add (std::move (p));
}

int ParamRegistry::logHz (const std::string& id, const std::string& name, const std::string& sn, float mn, float mx, float def, uint32_t flags)
{
    ParamInfo p;
    p.id = id; p.name = name; p.shortName = sn; p.min = mn; p.max = mx; p.def = def;
    p.scale = ParamScale::Log; p.unit = Unit::Hz; p.flags = flags;
    return add (std::move (p));
}

int ParamRegistry::time (const std::string& id, const std::string& name, const std::string& sn, float maxS, float defS, uint32_t flags)
{
    ParamInfo p;
    p.id = id; p.name = name; p.shortName = sn; p.min = 0.0f; p.max = maxS; p.def = defS;
    p.scale = ParamScale::Power; p.skew = 4.0f; p.unit = Unit::Seconds; p.flags = flags;
    return add (std::move (p));
}

int ParamRegistry::integer (const std::string& id, const std::string& name, const std::string& sn, int mn, int mx, int def, Unit u, uint32_t flags)
{
    ParamInfo p;
    p.id = id; p.name = name; p.shortName = sn; p.min = (float) mn; p.max = (float) mx; p.def = (float) def;
    p.scale = ParamScale::Int; p.unit = u; p.decimals = 0; p.flags = flags;
    if (mn < 0 && mx > 0 && mn == -mx) p.flags |= kBipolar;
    return add (std::move (p));
}

int ParamRegistry::toggle (const std::string& id, const std::string& name, const std::string& sn, bool def, uint32_t flags)
{
    ParamInfo p;
    p.id = id; p.name = name; p.shortName = sn; p.min = 0.0f; p.max = 1.0f; p.def = def ? 1.0f : 0.0f;
    p.scale = ParamScale::Bool; p.decimals = 0; p.flags = flags;
    return add (std::move (p));
}

int ParamRegistry::choice (const std::string& id, const std::string& name, const std::string& sn, std::vector<std::string> c, int def, uint32_t flags)
{
    ParamInfo p;
    p.id = id; p.name = name; p.shortName = sn; p.min = 0.0f; p.max = (float) std::max<size_t> (1, c.size()) - 1.0f; p.def = (float) def;
    p.scale = ParamScale::Choice; p.decimals = 0; p.choices = std::move (c); p.flags = flags;
    return add (std::move (p));
}

int ParamRegistry::find (const std::string& id) const
{
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].id == id) return (int) i;
    return -1;
}

const ParamRegistry& params()
{
    static const ParamRegistry reg = []
    {
        ParamRegistry r;
        registerAllParams (r);
        return r;
    }();
    return reg;
}

void ParamValues::resetToDefaults()
{
    const auto& reg = params();
    v.resize ((size_t) reg.size());
    for (int i = 0; i < reg.size(); ++i) v[(size_t) i] = reg[i].def;
}
} // namespace tincture
