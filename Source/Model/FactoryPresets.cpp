#include "FactoryPresets.h"
#include "../Engine/ParamIds.h"

namespace tincture
{
Node encodePreset (const Patch& patch, const ParamValues& pv)
{
    Node root ("tincture-preset");
    root.set ("version", 1.0);
    auto& p = root.add ("params");
    const auto& reg = params();
    for (int i = 0; i < reg.size() && i < (int) pv.v.size(); ++i)
        p.set (reg[i].id, (double) pv[i]);
    patch.save (root.add ("patch"));
    return root;
}

bool decodePreset (const Node& root, Patch& patch, ParamValues& pv)
{
    if (root.type() != "tincture-preset") return false;
    pv.resetToDefaults();
    if (auto* p = root.child ("params"))
    {
        const auto& reg = params();
        for (int i = 0; i < reg.size(); ++i)
            if (p->has (reg[i].id)) pv[i] = reg[i].snap ((float) p->num (reg[i].id));
    }
    if (auto* c = root.child ("patch")) patch.load (*c);
    else patch = Patch();
    return true;
}

// The sound library itself is in FactorySounds.cpp.
std::vector<FactoryPreset> buildFactorySounds();

const std::vector<FactoryPreset>& factoryPresets()
{
    static const std::vector<FactoryPreset> list = buildFactorySounds();
    return list;
}
} // namespace tincture
