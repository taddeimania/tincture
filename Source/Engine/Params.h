// Params.h — the parameter registry.
//
// Every host-automatable, modulatable control is a parameter here. Each
// module declares its own parameters in a register function in its own file
// (registerOscParams, registerFilterParams, ...); Params.cpp calls them in a
// fixed order. Ids are strings ("A.level", "F1.cutoff", "L3.rate") and are
// what the host stores, so never rename an id once released; append new ones.
//
// Values everywhere are PLAIN (in the parameter's own units: Hz, dB, %, an
// index for choices). Modulation works in NORMALISED space (0..1 over the
// control's travel, as the knob is drawn), so a +50% modulation always moves
// a knob half its travel whatever its units. toNorm/fromNorm convert.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace tincture
{
enum class ParamScale : uint8_t
{
    Linear,     // min..max
    Power,      // norm^skew mapped onto min..max (skew > 1 puts more travel at the low end)
    Log,        // exponential: min * (max/min)^norm; min must be > 0
    Int,        // integers min..max, linear
    Bool,       // 0 / 1
    Choice      // index into `choices`
};

enum ParamFlags : uint32_t
{
    kModulatable = 1u << 0,     // can be a mod-matrix destination
    kPerVoice    = 1u << 1,     // modulation is evaluated per voice (else globally, from the newest voice)
    kBipolar     = 1u << 2,     // drawn from the centre; modulation centre is the middle
    kNoAutomate  = 1u << 3,     // not exposed to the host
};

enum class Unit : uint8_t { None, Percent, Db, Hz, Ms, Seconds, Semitones, Cents, Degrees, Ratio, Note, Octaves, Bpm };

struct ParamInfo
{
    std::string id;             // stable, e.g. "A.level"
    std::string name;           // for the host and tooltips, e.g. "A Level"
    std::string shortName;      // knob caption, e.g. "LEVEL"
    std::string group;          // module, e.g. "OSC A" (for menus)
    float min = 0.0f, max = 1.0f, def = 0.0f;
    ParamScale scale = ParamScale::Linear;
    float skew = 1.0f;          // Power only
    Unit unit = Unit::None;
    float displayScale = 1.0f;  // e.g. 100 to show 0..1 as 0..100 %
    int decimals = 2;
    std::vector<std::string> choices;
    uint32_t flags = kModulatable | kPerVoice;

    float toNorm (float plain) const noexcept;
    float fromNorm (float norm) const noexcept;
    float snap (float plain) const noexcept;      // integers/choices/bools rounded
    bool isDiscrete() const noexcept { return scale == ParamScale::Int || scale == ParamScale::Bool || scale == ParamScale::Choice; }
    bool modulatable() const noexcept { return (flags & kModulatable) != 0; }
    bool perVoice() const noexcept { return (flags & kPerVoice) != 0; }
    bool bipolar() const noexcept { return (flags & kBipolar) != 0; }
    std::string format (float plain, bool withUnit = true) const;
    bool parse (const std::string& text, float& plainOut) const;
};

class ParamRegistry
{
public:
    // Builders return the new parameter's index.
    int add (ParamInfo info);
    int linear (const std::string& id, const std::string& name, const std::string& shortName, float min, float max, float def,
                Unit unit = Unit::None, uint32_t flags = kModulatable | kPerVoice);
    int percent (const std::string& id, const std::string& name, const std::string& shortName, float def01,
                 uint32_t flags = kModulatable | kPerVoice);                   // 0..1 shown as 0..100 %
    int bipolar (const std::string& id, const std::string& name, const std::string& shortName, float def = 0.0f,
                 uint32_t flags = kModulatable | kPerVoice);                   // -1..1 shown as -100..100
    int logHz (const std::string& id, const std::string& name, const std::string& shortName, float min, float max, float def,
               uint32_t flags = kModulatable | kPerVoice);
    int time (const std::string& id, const std::string& name, const std::string& shortName, float maxSeconds, float defSeconds,
              uint32_t flags = kModulatable | kPerVoice);                      // 0..max seconds, skewed, shown in ms/s
    int integer (const std::string& id, const std::string& name, const std::string& shortName, int min, int max, int def,
                 Unit unit = Unit::None, uint32_t flags = kModulatable | kPerVoice);
    int toggle (const std::string& id, const std::string& name, const std::string& shortName, bool def,
                uint32_t flags = kNoAutomate);
    int choice (const std::string& id, const std::string& name, const std::string& shortName,
                std::vector<std::string> choices, int def, uint32_t flags = kNoAutomate);

    // Sets the group (module name) for parameters added from now on.
    void setGroup (const std::string& g) { group = g; }

    int size() const noexcept { return (int) list.size(); }
    const ParamInfo& operator[] (int i) const noexcept { return list[(size_t) i]; }
    ParamInfo& edit (int i) noexcept { return list[(size_t) i]; }
    int find (const std::string& id) const;          // -1 if missing
    const std::vector<ParamInfo>& all() const noexcept { return list; }

private:
    std::vector<ParamInfo> list;
    std::string group;
};

// The one registry, built on first use. Thread-safe to read after that.
const ParamRegistry& params();

// Plain parameter values, indexed like the registry. The processor fills
// one each block from the host's parameters; the engine reads it.
struct ParamValues
{
    std::vector<float> v;
    void resetToDefaults();
    float operator[] (int i) const noexcept { return v[(size_t) i]; }
    float& operator[] (int i) noexcept { return v[(size_t) i]; }
};
} // namespace tincture
