// FxLayout.h — the FX racks as patch data: which modules sit in MAIN, BUS 1
// and BUS 2, in what order, which FX unit (parameter set) each one uses, its
// bypass state and any non-parameter data (X-Shaper curves, the Convolve
// impulse). Splitters hold one child list per band (one nesting level only).
//
// All editing happens on the message thread: the UI edits a copy, then hands
// it to FxEngine::setLayout() as a shared_ptr<const FxLayout>.
//
// Units: every module (including those inside splitter bands) owns one of
// the kNumFxUnits parameter units. add() takes the lowest free unit; the
// caller then writes fxDefaults(type) / fxDefaultMix(type) into that unit's
// host parameters. remove() frees units (the caller may leave the values).
#pragma once

#include "FxTypes.h"
#include "../SampleData.h"
#include "../../Model/Curve.h"
#include "../../Model/Tree.h"

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace tincture
{
enum FxRackId { kFxMain = 0, kFxBus1, kFxBus2, kNumFxRacks };

// Parameter values of one unit, for rack presets / pasting / duplicating.
struct FxUnitValues
{
    int unit = -1;
    float mix = 1.0f;
    std::array<float, kFxParams> p {};
};

struct FxUnitRemap { int from = -1, to = -1; };

struct FxModuleSlot
{
    FxType type = FxType::Utility;
    int unit = -1;
    bool bypass = false;
    bool expanded = true;                   // UI: panel open in the rack view

    // Distortion: X-Shaper curves A and B (x = |input|, y = |output|; the
    // Asym mode reads them over -1..1 mapped to 0..1).
    Curve shapeA, shapeB;
    // Convolve: factory IR index, or a user IR (when set it wins).
    int irIndex = 0;
    std::shared_ptr<const SampleData> userIr;

    // Splitters: one module list per band (LOWS/MIDS/HIGHS, or MID/SIDE).
    std::array<std::vector<FxModuleSlot>, 3> bands;
    std::array<bool, 3> bandBypass {};
    int selectedBand = 0;                   // UI

    static FxModuleSlot make (FxType t);    // default extra data for a type, unit -1
    bool isSplitter() const noexcept { return fxInfo (type).splitter; }

    // Saves one module (children too). With `values`, each module also
    // stores its unit's parameter values (rack presets).
    using ValueGetter = std::function<FxUnitValues (int unit)>;
    void save (Node& n, bool withUnits, const ValueGetter* values = nullptr) const;
    // Loads one module (children too). Units are read when `withUnits`;
    // stored values (if any) are appended to `values` depth-first, unit -1.
    static bool load (const Node& n, FxModuleSlot& out, bool withUnits, bool allowSplitter,
                      std::vector<FxUnitValues>* values = nullptr);
};

// Where a module list lives: a rack, or one band of a splitter in a rack.
struct FxList
{
    int rack = kFxMain;
    int splitter = -1;                      // index of the splitter in the rack (-1 = the rack itself)
    int band = -1;
};

// A module: its list and its index there.
struct FxPath
{
    FxList list;
    int index = 0;
};

class FxLayout
{
public:
    struct Rack
    {
        std::vector<FxModuleSlot> modules;
        std::string presetName = "Init";
        bool locked = false;                // "Lock FX Bus": kept when a synth preset loads
    };
    std::array<Rack, kNumFxRacks> racks;

    // Lookup.
    std::vector<FxModuleSlot>* list (const FxList& l);
    const std::vector<FxModuleSlot>* list (const FxList& l) const;
    FxModuleSlot* get (const FxPath& p);
    const FxModuleSlot* get (const FxPath& p) const;
    bool findUnit (int unit, FxPath& out) const;
    std::array<bool, kNumFxUnits> usedUnits() const;
    int numFreeUnits() const;
    int countModules (const FxModuleSlot& m) const;         // 1 + children
    void forEach (const std::function<void (const FxModuleSlot&, const FxPath&)>& fn) const;

    // Editing (message thread). Positions: at < 0 or past the end = append.
    // add* return the new unit, or -1 (no free unit / splitter in a band /
    // bad list). The rack's preset name becomes "Custom".
    int add (const FxList& where, FxType t, int at = -1);
    int add (int rack, FxType t) { return add (FxList { rack, -1, -1 }, t, -1); }
    std::vector<int> remove (const FxPath& p);              // returns the freed units
    bool move (const FxPath& from, const FxList& to, int at);
    // Copy a module (splitters with their bands) to a position, new units.
    // Returns (old unit, new unit) pairs, first = the module itself; empty on
    // failure. The caller copies parameter values old -> new.
    std::vector<std::pair<int, int>> duplicate (const FxPath& from, const FxList& to, int at);
    bool setBypass (const FxPath& p, bool on);
    void setRackBypass (int rack, bool on);                 // Alt-click: all modules of a rack
    bool setBandBypass (int rack, int splitter, int band, bool on);
    std::vector<int> clearRack (int rack);                  // "Clear FX Bus" / Factory > Init
    void setLocked (int rack, bool on)                      { racks[(size_t) rack].locked = on; }
    void setAllLocked (bool on)                             { for (auto& r : racks) r.locked = on; }
    bool allLocked() const                                  { return racks[0].locked && racks[1].locked && racks[2].locked; }

    // Rack presets / FX Bus copy & paste. `values` supplies a unit's current
    // parameter values. loadRack replaces the rack's modules; on success it
    // fills `out` with the new units' values to write to the host and
    // `freed` with the units the old modules used. Fails (layout unchanged)
    // when there aren't enough free units.
    using ValueGetter = FxModuleSlot::ValueGetter;
    Node saveRack (int rack, const ValueGetter& values) const;
    bool loadRack (int rack, const Node& n, std::vector<FxUnitValues>& out, std::vector<int>* freed = nullptr);

    // The whole layout, for the synth preset / host session (unit numbers
    // are saved: the units' values are host parameters). With respectLocks,
    // locked racks keep their current modules; incoming modules whose unit
    // clashes with a kept one get a new unit and are listed in `remap` (the
    // caller moves the preset's values for `from` to `to`).
    void save (Node& n) const;
    bool load (const Node& n, bool respectLocks = false, std::vector<FxUnitRemap>* remap = nullptr);

private:
    int allocUnit() const;
    void markCustom (int rack) { racks[(size_t) rack].presetName = "Custom"; }
    static void collectUnits (const FxModuleSlot& m, std::vector<int>& out);
};
} // namespace tincture
