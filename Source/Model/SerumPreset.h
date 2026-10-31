// SerumPreset.h — reading Xfer Serum 2 preset files (.SerumPreset).
//
// The container (little-endian):
//     "XferJson\0"
//     uint64   length of a JSON metadata header
//     bytes    JSON (presetName, presetAuthor, presetDescription, tags, ...)
//     uint32   size of the payload once decompressed
//     uint32   payload encoding (2 = zstd)
//     bytes    a zstd frame holding a CBOR document
//
// The CBOR document is a map of module name -> module state ("Oscillator0",
// "Env1", "VoiceFilter0", "ModSlot12", "FXRack0", ...). Each module has a
// "plainParams" entry: either the string "default" or a map holding only
// the parameters that differ from Serum's defaults, in real units.
//
// This file only decodes; SerumImport.h turns the result into a Tincture
// patch.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace tincture::serum
{
// A decoded CBOR value. Maps keep their order; keys that aren't text are
// stored as their text form.
struct Value
{
    struct Entry;
    enum class Type : uint8_t { Null, Bool, Number, String, Bytes, Array, Map };
    Type type = Type::Null;
    double number = 0.0;                 // Number, Bool (0/1)
    std::string text;                    // String, Bytes
    std::vector<Value> items;            // Array
    std::vector<Entry> entries;          // Map

    bool isNumber() const noexcept { return type == Type::Number || type == Type::Bool; }
    bool isString() const noexcept { return type == Type::String; }
    bool isMap() const noexcept    { return type == Type::Map; }
    bool isArray() const noexcept  { return type == Type::Array; }

    // Map lookup; a shared null value when missing (so lookups chain).
    const Value& operator[] (const std::string& key) const noexcept;
    const Value* find (const std::string& key) const noexcept;
    // Array element; null when out of range.
    const Value& operator[] (size_t i) const noexcept;
    size_t size() const noexcept;

    double num (double fallback) const noexcept { return isNumber() ? number : fallback; }
    const std::string& str() const noexcept { return text; }
};

struct Value::Entry
{
    std::string first;                   // key
    Value second;
};

// Decodes one CBOR item. Returns false on malformed input (depth > 64,
// truncation, unsupported major types).
bool decodeCbor (const uint8_t* data, size_t size, Value& out);

// A minimal JSON reader for the header (objects, arrays, strings, numbers,
// true/false/null), into the same Value tree.
bool decodeJson (const std::string& text, Value& out);

struct File
{
    Value header;                        // the JSON metadata
    Value state;                         // the CBOR module map
    int encoding = 0;
};

// True if the bytes start with the Serum 2 container magic.
bool looksLikeSerumPreset (const uint8_t* data, size_t size) noexcept;

// Reads a whole .SerumPreset. On failure returns false and sets `error`.
bool readFile (const uint8_t* data, size_t size, File& out, std::string& error);
} // namespace tincture::serum
