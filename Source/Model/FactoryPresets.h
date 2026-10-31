// FactoryPresets.h — the built-in sounds, made in code (parameters + patch).
#pragma once

#include "Patch.h"
#include "../Engine/Params.h"

#include <functional>
#include <string>
#include <vector>

namespace tincture
{
struct FactoryPreset
{
    std::string name, category, description;
    std::vector<std::string> tags;
    // Starts from the init patch and default parameters.
    std::function<void (Patch&, ParamValues&)> build;
};

const std::vector<FactoryPreset>& factoryPresets();

// Preset files and host state share one format: a Node with "params" (every
// parameter by id) and "patch" children.
Node encodePreset (const Patch&, const ParamValues&);
bool decodePreset (const Node&, Patch&, ParamValues&);
} // namespace tincture
