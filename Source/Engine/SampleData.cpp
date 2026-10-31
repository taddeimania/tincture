#include "SampleData.h"

#include <cctype>
#include <cstdlib>

namespace tincture
{
void SampleData::save (Node& n) const
{
    n.set ("name", name).set ("path", path).set ("category", category)
     .set ("rate", sampleRate).set ("root", (double) rootNote)
     .set ("loopStart", (double) loopStart).set ("loopEnd", (double) loopEnd)
     .set ("embedded", embedded ? 1.0 : 0.0);
    if (! slices.empty())
    {
        std::vector<float> s;
        for (int x : slices) s.push_back ((float) x);
        n.set ("slices", std::move (s));
    }
    if (embedded)
    {
        n.set ("left", left);
        if (stereo()) n.set ("right", right);
    }
}

std::shared_ptr<const SampleData> SampleData::load (const Node& n)
{
    auto s = std::make_shared<SampleData>();
    s->name = n.str ("name");
    s->path = n.str ("path");
    s->category = n.str ("category");
    s->sampleRate = n.num ("rate", 48000.0);
    s->rootNote = n.integer ("root", 60);
    s->loopStart = n.integer ("loopStart", -1);
    s->loopEnd = n.integer ("loopEnd", -1);
    s->embedded = n.flag ("embedded");
    if (auto* sl = n.floats ("slices"))
        for (float f : *sl) s->slices.push_back ((int) f);
    if (auto* l = n.floats ("left")) s->left = *l;
    if (auto* r = n.floats ("right")) s->right = *r;
    // Not embedded: the plugin layer reloads the audio from `path`.
    return s;
}

int rootNoteFromName (const std::string& fileName)
{
    // Strip the extension.
    std::string s = fileName;
    if (auto dot = s.find_last_of ('.'); dot != std::string::npos) s = s.substr (0, dot);
    // Scan from the end for a note name like C3, F#2, Bb-1, or a bare MIDI number 0..127.
    static const int semis[7] { 9, 11, 0, 2, 4, 5, 7 };   // A B C D E F G
    for (int i = (int) s.size() - 1; i >= 0; --i)
    {
        const char c = (char) std::toupper ((unsigned char) s[(size_t) i]);
        if (c < 'A' || c > 'G') continue;
        if (i > 0 && std::isalpha ((unsigned char) s[(size_t) i - 1])) continue;   // part of a word
        size_t j = (size_t) i + 1;
        int note = semis[c - 'A'];
        if (j < s.size() && (s[j] == '#' || s[j] == 's')) { ++note; ++j; }
        else if (j < s.size() && s[j] == 'b') { --note; ++j; }
        bool neg = false;
        if (j < s.size() && s[j] == '-') { neg = true; ++j; }
        if (j >= s.size() || ! std::isdigit ((unsigned char) s[j])) continue;
        int oct = 0;
        while (j < s.size() && std::isdigit ((unsigned char) s[j])) oct = oct * 10 + (s[j++] - '0');
        if (j < s.size() && std::isalnum ((unsigned char) s[j])) continue;
        if (neg) oct = -oct;
        const int midi = (oct + 2) * 12 + note;
        if (midi >= 0 && midi <= 127) return midi;
    }
    // A trailing MIDI number after a separator: "lead-60".
    size_t k = s.size();
    while (k > 0 && std::isdigit ((unsigned char) s[k - 1])) --k;
    if (k < s.size() && k > 0 && (s[k - 1] == '-' || s[k - 1] == '_' || s[k - 1] == ' '))
    {
        const int v = std::atoi (s.c_str() + k);
        if (v >= 0 && v <= 127) return v;
    }
    return -1;
}
} // namespace tincture
