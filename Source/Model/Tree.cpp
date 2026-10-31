#include "Tree.h"

#include <cstring>

namespace tincture
{
double Node::num (const std::string& key, double def) const
{
    auto it = props.find (key);
    if (it == props.end()) return def;
    if (auto* d = std::get_if<double> (&it->second)) return *d;
    return def;
}

std::string Node::str (const std::string& key, const std::string& def) const
{
    auto it = props.find (key);
    if (it == props.end()) return def;
    if (auto* s = std::get_if<std::string> (&it->second)) return *s;
    return def;
}

const std::vector<float>* Node::floats (const std::string& key) const
{
    auto it = props.find (key);
    if (it == props.end()) return nullptr;
    return std::get_if<std::vector<float>> (&it->second);
}

Node& Node::add (Node c)
{
    kids.push_back (std::move (c));
    return kids.back();
}

const Node* Node::child (const std::string& t) const
{
    for (const auto& k : kids)
        if (k.nodeType == t) return &k;
    return nullptr;
}

std::vector<const Node*> Node::children (const std::string& t) const
{
    std::vector<const Node*> out;
    for (const auto& k : kids)
        if (k.nodeType == t) out.push_back (&k);
    return out;
}

// ---------------------------------------------------------------------------
// Binary format (little-endian):
//   magic "TNC1"  node
//   node   := str type, u32 nprops, { str key, u8 tag, value }, u32 nkids, { node }
//   str    := u32 len, bytes
//   value  := tag 0: f64 | tag 1: str | tag 2: u32 n, n*f32
// ---------------------------------------------------------------------------
namespace
{
struct Writer
{
    std::vector<uint8_t>& b;
    void raw (const void* p, size_t n) { auto* c = (const uint8_t*) p; b.insert (b.end(), c, c + n); }
    void u32 (uint32_t v) { raw (&v, 4); }
    void str (const std::string& s) { u32 ((uint32_t) s.size()); raw (s.data(), s.size()); }
    void node (const Node& n)
    {
        str (n.type());
        u32 ((uint32_t) n.properties().size());
        for (const auto& [k, v] : n.properties())
        {
            str (k);
            if (auto* d = std::get_if<double> (&v)) { uint8_t t = 0; raw (&t, 1); raw (d, 8); }
            else if (auto* s = std::get_if<std::string> (&v)) { uint8_t t = 1; raw (&t, 1); str (*s); }
            else if (auto* f = std::get_if<std::vector<float>> (&v)) { uint8_t t = 2; raw (&t, 1); u32 ((uint32_t) f->size()); raw (f->data(), f->size() * 4); }
        }
        u32 ((uint32_t) n.allChildren().size());
        for (const auto& k : n.allChildren()) node (k);
    }
};

struct Reader
{
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    bool need (size_t n) { if ((size_t) (end - p) < n) ok = false; return ok; }
    uint32_t u32() { uint32_t v = 0; if (need (4)) { std::memcpy (&v, p, 4); p += 4; } return v; }
    std::string str() { const uint32_t n = u32(); std::string s; if (n < (1u << 28) && need (n)) { s.assign ((const char*) p, n); p += n; } else ok = false; return s; }
    bool node (Node& out, int depth)
    {
        if (depth > 64) return ok = false;
        out = Node (str());
        const uint32_t np = u32();
        for (uint32_t i = 0; ok && i < np; ++i)
        {
            const auto key = str();
            if (! need (1)) break;
            const uint8_t tag = *p++;
            if (tag == 0) { double d = 0; if (need (8)) { std::memcpy (&d, p, 8); p += 8; out.set (key, d); } }
            else if (tag == 1) out.set (key, str());
            else if (tag == 2)
            {
                const uint32_t n = u32();
                if (n < (1u << 26) && need ((size_t) n * 4))
                {
                    std::vector<float> f (n);
                    std::memcpy (f.data(), p, (size_t) n * 4);
                    p += (size_t) n * 4;
                    out.set (key, std::move (f));
                }
                else ok = false;
            }
            else ok = false;
        }
        const uint32_t nk = u32();
        for (uint32_t i = 0; ok && i < nk; ++i)
        {
            Node c;
            if (! node (c, depth + 1)) break;
            out.add (std::move (c));
        }
        return ok;
    }
};
} // namespace

std::vector<uint8_t> Node::toBinary() const
{
    std::vector<uint8_t> b;
    b.reserve (4096);
    Writer w { b };
    w.raw ("TNC1", 4);
    w.node (*this);
    return b;
}

bool Node::fromBinary (const uint8_t* data, size_t size, Node& out)
{
    if (data == nullptr || size < 4 || std::memcmp (data, "TNC1", 4) != 0) return false;
    Reader r { data + 4, data + size };
    return r.node (out, 0) && r.ok;
}
} // namespace tincture
