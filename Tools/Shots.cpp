#include "Shots.h"

using namespace tincture::ui;

std::map<juce::String, ShotFn>& shotRegistry()
{
    static std::map<juce::String, ShotFn> r;
    return r;
}

void Shot::pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

void Shot::play (int blocks)
{
    juce::AudioBuffer<float> buf (2, 512);
    for (int i = 0; i < blocks; ++i)
    {
        juce::MidiBuffer m;
        buf.clear();
        proc.processBlock (buf, m);
    }
    pump (35);
}

void Shot::noteOn (int note, int vel) { proc.uiNoteOn (note, (float) vel / 127.0f); play (2); }
void Shot::noteOff (int note) { proc.uiNoteOff (note); play (2); }

void Shot::save (const juce::String& name)
{
    pump (40);
    auto img = editor.createComponentSnapshot (editor.getLocalBounds(), true, 1.0f);
    auto f = dir.getChildFile (name + ".png");
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("  wrote %s\n", f.getFileName().toRawUTF8());
}

void Shot::check (bool ok, const juce::String& what)
{
    ++checks;
    if (! ok) { ++failures; std::printf ("  FAIL %s\n", what.toRawUTF8()); }
}

int Shot::summary() const
{
    std::printf ("%d checks, %d failures\n", checks, failures);
    return failures;
}

static bool shown (juce::Component& c, juce::Component& root)
{
    for (auto* p = &c; p != nullptr && p != &root; p = p->getParentComponent())
        if (! p->isVisible()) return false;
    return true;
}

juce::Component* Shot::findByTooltip (const juce::String& s)
{
    return find<juce::Component> ([&] (juce::Component& c)
    {
        auto* t = dynamic_cast<juce::TooltipClient*> (&c);
        return t != nullptr && shown (c, panel) && t->getTooltip().containsIgnoreCase (s);
    });
}

PanelButton* Shot::button (const juce::String& label)
{
    return find<PanelButton> ([&] (PanelButton& b) { return b.getLabel() == label && shown (b, panel); });
}

namespace
{
juce::MouseEvent ev (juce::Component& c, juce::Point<int> p, juce::ModifierKeys mods, int clicks = 1, juce::Point<int> downAt = { -100000, 0 })
{
    if (downAt.x == -100000) downAt = p;
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), mods,
                             juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                             juce::Time::getCurrentTime(), downAt.toFloat(), juce::Time::getCurrentTime(), clicks, p != downAt);
}

// Deliver a mouse event to the deepest child under `p` (like the OS would).
juce::Component* target (juce::Component& c, juce::Point<int>& p)
{
    auto* t = c.getComponentAt (p);
    if (t == nullptr) return &c;
    p = t->getLocalPoint (&c, p);
    return t;
}

MenuLayer* layer (TincturePanel& panel) { return panel.hasMenuLayer() ? &panel.getMenuLayer() : nullptr; }
} // namespace

void Shot::click (juce::Component& c, juce::Point<int> at, bool right)
{
    if (at.x < 0) at = c.getLocalBounds().getCentre();
    // Route through the panel so the menu layer (if open) gets clicks outside menus.
    auto p = panel.getLocalPoint (&c, at);
    auto* t = target (panel, p);
    const juce::ModifierKeys down (right ? juce::ModifierKeys::rightButtonModifier : juce::ModifierKeys::leftButtonModifier);
    juce::Component::SafePointer<juce::Component> safe (t);
    t->mouseDown (ev (*t, p, down));
    if (safe != nullptr) t->mouseUp (ev (*t, p, right ? down : juce::ModifierKeys()));
    pump (30);
}

void Shot::doubleClick (juce::Component& c, juce::Point<int> at)
{
    if (at.x < 0) at = c.getLocalBounds().getCentre();
    auto p = panel.getLocalPoint (&c, at);
    auto* t = target (panel, p);
    t->mouseDoubleClick (ev (*t, p, {}, 2));
    pump (30);
}

void Shot::drag (juce::Component& c, juce::Point<int> from, juce::Point<int> to, int steps, bool shift)
{
    auto p0 = panel.getLocalPoint (&c, from);
    auto* t = target (panel, p0);
    const auto origin = t->getLocalPoint (&panel, panel.getLocalPoint (&c, from));
    juce::ModifierKeys mods (juce::ModifierKeys::leftButtonModifier | (shift ? juce::ModifierKeys::shiftModifier : 0));
    juce::Component::SafePointer<juce::Component> safe (t);
    t->mouseDown (ev (*t, origin, mods));
    for (int i = 1; i <= steps && safe != nullptr; ++i)
    {
        const auto q = from + (to - from) * i / steps;
        const auto lp = t->getLocalPoint (&c, q);
        t->mouseDrag (ev (*t, lp, mods, 1, origin));
    }
    if (safe != nullptr) t->mouseUp (ev (*t, t->getLocalPoint (&c, to), juce::ModifierKeys(), 1, origin));
    pump (30);
}

void Shot::hover (juce::Component& c, juce::Point<int> at)
{
    auto p = panel.getLocalPoint (&c, at);
    auto* t = target (panel, p);
    t->mouseMove (ev (*t, p, {}));
}

void Shot::wheel (juce::Component& c, float dy, juce::Point<int> at)
{
    if (at.x < 0) at = c.getLocalBounds().getCentre();
    auto p = panel.getLocalPoint (&c, at);
    auto* t = target (panel, p);
    juce::MouseWheelDetails w { 0.0f, dy, false, false, false };
    t->mouseWheelMove (ev (*t, p, {}), w);
    pump (20);
}

bool Shot::menuOpen() { auto* L = layer (panel); return L != nullptr && L->isOpen(); }
int Shot::menuDepth() { auto* L = layer (panel); return L == nullptr ? 0 : L->paneCount(); }

juce::StringArray Shot::menuItems (int depth)
{
    juce::StringArray out;
    auto* L = layer (panel);
    if (L == nullptr || L->pane (depth) == nullptr) return out;
    for (const auto& r : L->pane (depth)->getRows())
    {
        using K = Menu::Item::Kind;
        if (r.item->kind == K::Separator) out.add ("---");
        else if (r.item->kind == K::Header) out.add ("#" + r.item->text);
        else if (r.item->kind != K::ColumnBreak) out.add (r.item->text);
    }
    return out;
}

// Split "Folder/Item" into parts; "\\/" is a literal slash ("Swap A\\/B", "1\\/8").
static juce::StringArray splitPath (const juce::String& path)
{
    juce::StringArray parts;
    juce::String cur;
    for (int i = 0; i < path.length(); ++i)
    {
        if (path[i] == '\\' && i + 1 < path.length() && path[i + 1] == '/') { cur << '/'; ++i; }
        else if (path[i] == '/') { parts.add (cur); cur.clear(); }
        else cur << path[i];
    }
    parts.add (cur);
    return parts;
}

static bool walkTo (Shot& s, MenuLayer& L, const juce::StringArray& parts, bool chooseIt)
{
    for (int d = 0; d < parts.size(); ++d)
    {
        auto* pane = L.pane (d);
        if (pane == nullptr) return false;
        const int row = pane->findRow (parts[d]);
        if (row < 0) return false;
        const auto centre = pane->rowBoundsOnScreen (row).getCentre();
        const auto* item = pane->getRows()[(size_t) row].item;
        if (d < parts.size() - 1)
        {
            if (item->kind != Menu::Item::Kind::Submenu) return false;
            // Hover, then click to open the sub-menu right away.
            pane->mouseMove (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), centre.toFloat(), {}, 1.0f, 0, 0, 0, 0,
                                               pane, pane, juce::Time::getCurrentTime(), centre.toFloat(), juce::Time::getCurrentTime(), 1, false));
            pane->mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), centre.toFloat(), juce::ModifierKeys::leftButtonModifier, 1.0f, 0, 0, 0, 0,
                                               pane, pane, juce::Time::getCurrentTime(), centre.toFloat(), juce::Time::getCurrentTime(), 1, false));
            s.pump (10);
            continue;
        }
        if (! chooseIt) return true;
        juce::Component::SafePointer<MenuPane> safe (pane);
        pane->mouseMove (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), centre.toFloat(), {}, 1.0f, 0, 0, 0, 0,
                                           pane, pane, juce::Time::getCurrentTime(), centre.toFloat(), juce::Time::getCurrentTime(), 1, false));
        pane->mouseDown (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), centre.toFloat(), juce::ModifierKeys::leftButtonModifier, 1.0f, 0, 0, 0, 0,
                                           pane, pane, juce::Time::getCurrentTime(), centre.toFloat(), juce::Time::getCurrentTime(), 1, false));
        if (safe != nullptr)
            pane->mouseUp (juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), centre.toFloat(), {}, 1.0f, 0, 0, 0, 0,
                                             pane, pane, juce::Time::getCurrentTime(), centre.toFloat(), juce::Time::getCurrentTime(), 1, false));
        s.pump (40);
        return true;
    }
    return false;
}

bool Shot::menuHas (const juce::String& path)
{
    auto* L = layer (panel);
    if (L == nullptr || ! L->isOpen()) return false;
    return walkTo (*this, *L, splitPath (path), false) || walkTo (*this, *L, juce::StringArray (path), false);
}

bool Shot::choose (const juce::String& path)
{
    auto* L = layer (panel);
    bool ok = L != nullptr && L->isOpen() && walkTo (*this, *L, splitPath (path), true);
    // An item whose own name contains "/" ("Up/Down"): try the whole path as one item.
    if (! ok && L != nullptr && L->isOpen() && path.containsChar ('/'))
        ok = walkTo (*this, *L, juce::StringArray (path), true);
    if (! ok) std::printf ("  (menu path not found: %s; top items: %s)\n", path.toRawUTF8(), menuItems (0).joinIntoString (" | ").toRawUTF8());
    return ok;
}

void Shot::closeMenu() { dismissMenus (panel); pump (10); }

void Shot::key (int code, juce::juce_wchar ch)
{
    const juce::KeyPress k (code, juce::ModifierKeys(), ch);
    if (menuOpen()) panel.getMenuLayer().keyPressed (k);
    else panel.keyPressed (k);
    pump (20);
}
