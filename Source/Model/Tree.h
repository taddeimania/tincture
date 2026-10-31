// Tree.h — a small JUCE-free property tree for saving state (presets, the
// host session, banks). Every non-parameter piece of the patch (wavetables,
// LFO shapes, mod matrix, FX racks, arp and clip banks, ...) saves itself
// into a Node and loads from one. Nodes serialise to a compact binary blob.
//
// Rules for loaders: missing properties mean "use the default", unknown ones
// are ignored, so old presets load in new versions and vice versa.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace tincture
{
class Node
{
public:
    using Value = std::variant<double, std::string, std::vector<float>>;

    explicit Node (std::string type = {}) : nodeType (std::move (type)) {}

    const std::string& type() const noexcept { return nodeType; }

    // Properties.
    Node& set (const std::string& key, double v)                { props[key] = v; return *this; }
    Node& set (const std::string& key, const std::string& v)    { props[key] = v; return *this; }
    Node& set (const std::string& key, const char* v)           { props[key] = std::string (v); return *this; }
    Node& set (const std::string& key, std::vector<float> v)    { props[key] = std::move (v); return *this; }
    bool has (const std::string& key) const                     { return props.count (key) > 0; }
    double num (const std::string& key, double def = 0.0) const;
    int integer (const std::string& key, int def = 0) const     { return (int) num (key, def); }
    bool flag (const std::string& key, bool def = false) const  { return num (key, def ? 1.0 : 0.0) > 0.5 || num (key, def ? 1.0 : 0.0) < -0.5; }
    std::string str (const std::string& key, const std::string& def = {}) const;
    const std::vector<float>* floats (const std::string& key) const;
    const std::map<std::string, Value>& properties() const noexcept { return props; }

    // Children.
    Node& add (Node child);                              // returns the stored child
    Node& add (const std::string& type)                  { return add (Node (type)); }
    const Node* child (const std::string& type) const;   // first child of that type
    std::vector<const Node*> children (const std::string& type) const;
    const std::vector<Node>& allChildren() const noexcept { return kids; }
    std::vector<Node>& allChildren() noexcept { return kids; }

    // Serialisation.
    std::vector<uint8_t> toBinary() const;
    static bool fromBinary (const uint8_t* data, size_t size, Node& out);

private:
    std::string nodeType;
    std::map<std::string, Value> props;
    std::vector<Node> kids;
};
} // namespace tincture
