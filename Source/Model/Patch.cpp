#include "Patch.h"
#include "../Engine/Wavetable.h"

namespace tincture
{
void PresetInfo::save (Node& n) const
{
    n.set ("name", name).set ("author", author).set ("description", description).set ("category", category)
     .set ("rating", (double) rating);
    std::string t;
    for (const auto& tag : tags) { if (! t.empty()) t += ","; t += tag; }
    n.set ("tags", t);
}

void PresetInfo::load (const Node& n)
{
    name = n.str ("name", "Init");
    author = n.str ("author");
    description = n.str ("description");
    category = n.str ("category");
    rating = n.integer ("rating");
    tags.clear();
    std::string t = n.str ("tags"), cur;
    for (char c : t)
    {
        if (c == ',') { if (! cur.empty()) tags.push_back (cur); cur.clear(); }
        else cur += c;
    }
    if (! cur.empty()) tags.push_back (cur);
}

Patch::Patch()
{
    for (auto& w : wavetable) { w.table = FactoryTables::defaultTable(); w.bakeRemaps(); }
    for (auto& s : sample) s.rebuild();
    noise.load (Node ("noise"));     // the default noise sample, as a loaded preset would have
}

void Patch::save (Node& n) const
{
    info.save (n.add ("info"));
    for (int i = 0; i < kNumOscs; ++i)
    {
        auto& o = n.add ("osc");
        o.set ("index", (double) i);
        wavetable[(size_t) i].save (o.add ("wavetable"));
        sample[(size_t) i].save (o.add ("sample"));
    }
    noise.save (n.add ("noise"));
    for (int i = 0; i < kNumLfos; ++i)
    {
        auto& l = n.add ("lfo");
        l.set ("index", (double) i);
        lfo[(size_t) i].save (l);
    }
    matrix.save (n.add ("matrix"));
    graphs.save (n.add ("graphs"));
    fx.save (n.add ("fx"));
    arp.save (n.add ("arp"));
    clips.save (n.add ("clips"));
    keyboard.save (n.add ("keyboard"));
    auto& m = n.add ("macros");
    for (int i = 0; i < kNumMacros; ++i) m.set ("name" + std::to_string (i + 1), macroNames[(size_t) i]);
    n.set ("limitSameNote", limitSameNote ? 1.0 : 0.0);
}

void Patch::load (const Node& n)
{
    *this = Patch();
    if (auto* c = n.child ("info")) info.load (*c);
    for (auto* o : n.children ("osc"))
    {
        const int i = o->integer ("index", -1);
        if (i < 0 || i >= kNumOscs) continue;
        if (auto* w = o->child ("wavetable")) wavetable[(size_t) i].load (*w);
        if (auto* s = o->child ("sample")) { sample[(size_t) i].load (*s); sample[(size_t) i].rebuild(); }
    }
    if (auto* c = n.child ("noise")) noise.load (*c);
    for (auto* l : n.children ("lfo"))
    {
        const int i = l->integer ("index", -1);
        if (i >= 0 && i < kNumLfos) lfo[(size_t) i].load (*l);
    }
    if (auto* c = n.child ("matrix")) matrix.load (*c);
    if (auto* c = n.child ("graphs")) graphs.load (*c);
    if (auto* c = n.child ("fx")) fx.load (*c);
    if (auto* c = n.child ("arp")) arp.load (*c);
    if (auto* c = n.child ("clips")) clips.load (*c);
    if (auto* c = n.child ("keyboard")) keyboard.load (*c);
    if (auto* c = n.child ("macros"))
        for (int i = 0; i < kNumMacros; ++i) macroNames[(size_t) i] = c->str ("name" + std::to_string (i + 1));
    limitSameNote = n.flag ("limitSameNote");
}
} // namespace tincture
