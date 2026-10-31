// SerumPreset.cpp — the .SerumPreset container, a CBOR decoder and a small
// JSON reader. See SerumPreset.h.
#include "SerumPreset.h"

#include <cmath>
#include <cstring>

extern "C"
{
size_t ZSTD_decompress (void* dst, size_t dstCapacity, const void* src, size_t compressedSize);
unsigned ZSTD_isError (size_t code);
}

namespace tincture::serum
{
namespace
{
const Value& nullValue()
{
    static const Value v;
    return v;
}
} // namespace

const Value* Value::find (const std::string& key) const noexcept
{
    if (type != Type::Map) return nullptr;
    for (const auto& e : entries)
        if (e.first == key) return &e.second;
    return nullptr;
}

const Value& Value::operator[] (const std::string& key) const noexcept
{
    const Value* v = find (key);
    return v != nullptr ? *v : nullValue();
}

size_t Value::size() const noexcept { return type == Type::Map ? entries.size() : items.size(); }

const Value& Value::operator[] (size_t i) const noexcept
{
    return type == Type::Array && i < items.size() ? items[i] : nullValue();
}

// ---------------------------------------------------------------------------
// CBOR (RFC 8949): the subset Serum writes, plus indefinite lengths and tags.
// ---------------------------------------------------------------------------
namespace
{
struct CborReader
{
    const uint8_t* p;
    const uint8_t* end;

    bool byte (uint8_t& b)
    {
        if (p >= end) return false;
        b = *p++;
        return true;
    }

    bool uintN (int bytes, uint64_t& v)
    {
        if (end - p < bytes) return false;
        v = 0;
        for (int i = 0; i < bytes; ++i) v = (v << 8) | *p++;
        return true;
    }

    // The argument of an initial byte; `indefinite` for additional info 31.
    bool argument (uint8_t info, uint64_t& v, bool& indefinite)
    {
        indefinite = false;
        if (info < 24) { v = info; return true; }
        switch (info)
        {
            case 24: return uintN (1, v);
            case 25: return uintN (2, v);
            case 26: return uintN (4, v);
            case 27: return uintN (8, v);
            case 31: indefinite = true; v = 0; return true;
            default: return false;
        }
    }

    static double halfToDouble (uint16_t h)
    {
        const int e = (h >> 10) & 0x1f;
        const int m = h & 0x3ff;
        double v;
        if (e == 0)       v = std::ldexp ((double) m, -24);
        else if (e != 31) v = std::ldexp ((double) (m + 1024), e - 25);
        else              v = m == 0 ? INFINITY : NAN;
        return (h & 0x8000) != 0 ? -v : v;
    }

    bool chunkedString (uint8_t major, std::string& out, int depth)
    {
        // Indefinite-length string: definite chunks of the same major type until 0xff.
        for (;;)
        {
            if (p >= end) return false;
            if (*p == 0xff) { ++p; return true; }
            Value chunk;
            if ((*p >> 5) != major || ! item (chunk, depth + 1)) return false;
            out += chunk.text;
        }
    }

    bool item (Value& v, int depth)
    {
        if (depth > 64) return false;
        uint8_t ib;
        if (! byte (ib)) return false;
        const uint8_t major = (uint8_t) (ib >> 5), info = (uint8_t) (ib & 0x1f);

        if (major == 7)
        {
            switch (info)
            {
                case 20: v.type = Value::Type::Bool; v.number = 0.0; return true;
                case 21: v.type = Value::Type::Bool; v.number = 1.0; return true;
                case 22: case 23: v.type = Value::Type::Null; return true;
                case 25: { uint64_t h; if (! uintN (2, h)) return false; v.type = Value::Type::Number; v.number = halfToDouble ((uint16_t) h); return true; }
                case 26:
                {
                    uint64_t b; if (! uintN (4, b)) return false;
                    const auto bits = (uint32_t) b;
                    float f; std::memcpy (&f, &bits, 4);
                    v.type = Value::Type::Number; v.number = (double) f;
                    return true;
                }
                case 27:
                {
                    uint64_t bits; if (! uintN (8, bits)) return false;
                    double d; std::memcpy (&d, &bits, 8);
                    v.type = Value::Type::Number; v.number = d;
                    return true;
                }
                default:
                    if (info < 20) { v.type = Value::Type::Null; return true; }   // unassigned simple values
                    if (info == 24) { uint8_t skip; return byte (skip); }
                    return false;
            }
        }

        uint64_t arg;
        bool indefinite;
        if (! argument (info, arg, indefinite)) return false;

        switch (major)
        {
            case 0:
                if (indefinite) return false;
                v.type = Value::Type::Number; v.number = (double) arg;
                return true;
            case 1:
                if (indefinite) return false;
                v.type = Value::Type::Number; v.number = -1.0 - (double) arg;
                return true;
            case 2:
            case 3:
                v.type = major == 2 ? Value::Type::Bytes : Value::Type::String;
                if (indefinite) return chunkedString (major, v.text, depth);
                if (arg > (uint64_t) (end - p)) return false;
                v.text.assign ((const char*) p, (size_t) arg);
                p += arg;
                return true;
            case 4:
                v.type = Value::Type::Array;
                if (! indefinite && arg > (uint64_t) (end - p)) return false;    // each item needs a byte
                for (uint64_t i = 0; indefinite || i < arg; ++i)
                {
                    if (indefinite)
                    {
                        if (p >= end) return false;
                        if (*p == 0xff) { ++p; break; }
                    }
                    v.items.emplace_back();
                    if (! item (v.items.back(), depth + 1)) return false;
                }
                return true;
            case 5:
                v.type = Value::Type::Map;
                if (! indefinite && arg > (uint64_t) (end - p)) return false;
                for (uint64_t i = 0; indefinite || i < arg; ++i)
                {
                    if (indefinite)
                    {
                        if (p >= end) return false;
                        if (*p == 0xff) { ++p; break; }
                    }
                    Value key;
                    if (! item (key, depth + 1)) return false;
                    std::string k = key.text;
                    if (key.type == Value::Type::Number || key.type == Value::Type::Bool)
                    {
                        const double r = std::round (key.number);
                        k = std::abs (r - key.number) < 1.0e-9 ? std::to_string ((long long) r) : std::to_string (key.number);
                    }
                    v.entries.push_back ({ std::move (k), Value {} });
                    if (! item (v.entries.back().second, depth + 1)) return false;
                }
                return true;
            case 6:
                return item (v, depth + 1);       // tag: keep the tagged item
            default:
                return false;
        }
    }
};

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
struct JsonReader
{
    const std::string& s;
    size_t i = 0;

    void ws() { while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i; }

    static void utf8 (std::string& out, uint32_t c)
    {
        if (c < 0x80) out += (char) c;
        else if (c < 0x800) { out += (char) (0xc0 | (c >> 6)); out += (char) (0x80 | (c & 0x3f)); }
        else if (c < 0x10000) { out += (char) (0xe0 | (c >> 12)); out += (char) (0x80 | ((c >> 6) & 0x3f)); out += (char) (0x80 | (c & 0x3f)); }
        else { out += (char) (0xf0 | (c >> 18)); out += (char) (0x80 | ((c >> 12) & 0x3f)); out += (char) (0x80 | ((c >> 6) & 0x3f)); out += (char) (0x80 | (c & 0x3f)); }
    }

    bool hex4 (uint32_t& c)
    {
        if (i + 4 > s.size()) return false;
        c = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char h = s[i++];
            c <<= 4;
            if (h >= '0' && h <= '9') c |= (uint32_t) (h - '0');
            else if (h >= 'a' && h <= 'f') c |= (uint32_t) (h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') c |= (uint32_t) (h - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string (std::string& out)
    {
        if (i >= s.size() || s[i] != '"') return false;
        ++i;
        while (i < s.size())
        {
            const char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') { out += c; continue; }
            if (i >= s.size()) return false;
            const char e = s[i++];
            switch (e)
            {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u':
                {
                    uint32_t c1;
                    if (! hex4 (c1)) return false;
                    if (c1 >= 0xd800 && c1 < 0xdc00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u')
                    {
                        i += 2;
                        uint32_t c2;
                        if (! hex4 (c2)) return false;
                        c1 = 0x10000 + ((c1 - 0xd800) << 10) + (c2 - 0xdc00);
                    }
                    utf8 (out, c1);
                    break;
                }
                default: out += e; break;
            }
        }
        return false;
    }

    bool value (Value& v, int depth)
    {
        if (depth > 64) return false;
        ws();
        if (i >= s.size()) return false;
        const char c = s[i];
        if (c == '{')
        {
            ++i;
            v.type = Value::Type::Map;
            ws();
            if (i < s.size() && s[i] == '}') { ++i; return true; }
            for (;;)
            {
                ws();
                std::string key;
                if (! string (key)) return false;
                ws();
                if (i >= s.size() || s[i] != ':') return false;
                ++i;
                v.entries.push_back ({ std::move (key), Value {} });
                if (! value (v.entries.back().second, depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[')
        {
            ++i;
            v.type = Value::Type::Array;
            ws();
            if (i < s.size() && s[i] == ']') { ++i; return true; }
            for (;;)
            {
                v.items.emplace_back();
                if (! value (v.items.back(), depth + 1)) return false;
                ws();
                if (i < s.size() && s[i] == ',') { ++i; continue; }
                if (i < s.size() && s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        if (c == '"') { v.type = Value::Type::String; return string (v.text); }
        if (s.compare (i, 4, "true") == 0)  { i += 4; v.type = Value::Type::Bool; v.number = 1.0; return true; }
        if (s.compare (i, 5, "false") == 0) { i += 5; v.type = Value::Type::Bool; v.number = 0.0; return true; }
        if (s.compare (i, 4, "null") == 0)  { i += 4; v.type = Value::Type::Null; return true; }
        const size_t start = i;
        while (i < s.size() && (std::strchr ("+-0123456789.eE", s[i]) != nullptr)) ++i;
        if (i == start) return false;
        v.type = Value::Type::Number;
        v.number = std::strtod (s.substr (start, i - start).c_str(), nullptr);
        return true;
    }
};

uint64_t readLe (const uint8_t* p, int bytes)
{
    uint64_t v = 0;
    for (int i = bytes - 1; i >= 0; --i) v = (v << 8) | p[i];
    return v;
}

constexpr char kMagic[] = "XferJson";       // followed by a 0 byte
constexpr size_t kMagicSize = 9;
constexpr int kEncodingZstd = 2;
constexpr uint64_t kMaxPayload = 64u * 1024u * 1024u;
} // namespace

bool decodeCbor (const uint8_t* data, size_t size, Value& out)
{
    out = Value {};
    CborReader r { data, data + size };
    return r.item (out, 0);
}

bool decodeJson (const std::string& text, Value& out)
{
    out = Value {};
    JsonReader r { text };
    return r.value (out, 0);
}

bool looksLikeSerumPreset (const uint8_t* data, size_t size) noexcept
{
    return size >= kMagicSize && std::memcmp (data, kMagic, kMagicSize) == 0;     // includes the 0 byte
}

bool readFile (const uint8_t* data, size_t size, File& out, std::string& error)
{
    out = File {};
    if (! looksLikeSerumPreset (data, size)) { error = "not a Serum 2 preset"; return false; }
    if (size < kMagicSize + 8) { error = "file is truncated"; return false; }
    const uint64_t jsonLen = readLe (data + kMagicSize, 8);
    const size_t headerEnd = kMagicSize + 8;
    if (jsonLen > size - headerEnd || size - headerEnd - (size_t) jsonLen < 8) { error = "file is truncated"; return false; }

    const std::string json ((const char*) data + headerEnd, (size_t) jsonLen);
    if (! decodeJson (json, out.header) || ! out.header.isMap()) { error = "bad metadata header"; return false; }

    const size_t at = headerEnd + (size_t) jsonLen;
    const uint64_t rawSize = readLe (data + at, 4);
    out.encoding = (int) readLe (data + at + 4, 4);
    if (out.encoding != kEncodingZstd) { error = "unsupported payload encoding " + std::to_string (out.encoding); return false; }
    if (rawSize == 0 || rawSize > kMaxPayload) { error = "bad payload size"; return false; }

    std::vector<uint8_t> raw ((size_t) rawSize);
    const size_t got = ZSTD_decompress (raw.data(), raw.size(), data + at + 8, size - at - 8);
    if (ZSTD_isError (got) != 0 || got == 0) { error = "payload does not decompress"; return false; }
    raw.resize (got);

    if (! decodeCbor (raw.data(), raw.size(), out.state) || ! out.state.isMap()) { error = "payload is not a module map"; return false; }
    return true;
}
} // namespace tincture::serum
