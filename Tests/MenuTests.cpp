// MenuTests.cpp — drives Tincture's menu system with synthetic mouse and
// keyboard events and checks the rules listed at the top of Menu.h. Also
// writes PNGs of menus in typical states for a visual check.
//   TinctureMenuTests [out-dir]
#include "UI/Menu.h"
#include "UI/Widgets.h"

using namespace juce;
using namespace tincture::ui;

namespace
{
int failures = 0, checks = 0;
#define CHECK(cond) do { ++checks; if (! (cond)) { ++failures; std::printf ("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

void pump (int ms) { MessageManager::getInstance()->runDispatchLoopUntil (ms); }

struct Host final : public MenuHost
{
    Host() { setSize (1400, 900); setLookAndFeel (&lnf); }
    ~Host() override { setLookAndFeel (nullptr); }
    void paint (Graphics& g) override
    {
        g.fillAll (colours::panel);
        drawSection (g, getLocalBounds().toFloat().reduced (20.0f), "TEST PANEL");
    }
    void resized() override { resizeMenuLayer(); }
    PanelLookAndFeel lnf;
};

struct Anchor final : public Component
{
    void paint (Graphics& g) override { drawLcdBox (g, getLocalBounds().toFloat()); drawText (g, "ANCHOR", getLocalBounds().toFloat(), 13.0f, colours::lcd); }
};

MouseEvent ev (Component& c, Point<int> p, bool down = false)
{
    return MouseEvent (Desktop::getInstance().getMainMouseSource(), p.toFloat(),
                       down ? ModifierKeys (ModifierKeys::leftButtonModifier) : ModifierKeys(),
                       MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c, Time::getCurrentTime(),
                       p.toFloat(), Time::getCurrentTime(), 1, false);
}

MenuLayer& layerOf (Host& h) { return h.getMenuLayer(); }

// Mouse helpers in layer coordinates: route to the pane under the point, like JUCE would.
MenuPane* paneAt (MenuLayer& L, Point<int> p)
{
    for (int i = L.paneCount() - 1; i >= 0; --i)
        if (L.pane (i)->getBounds().contains (p)) return L.pane (i);
    return nullptr;
}
MenuPane* lastHovered = nullptr;
void moveTo (MenuLayer& L, Point<int> p)
{
    auto* pane = paneAt (L, p);
    if (lastHovered != nullptr && lastHovered != pane)
    {
        for (int i = 0; i < L.paneCount(); ++i)
            if (L.pane (i) == lastHovered) { lastHovered->mouseExit (ev (*lastHovered, p - lastHovered->getPosition())); break; }
    }
    lastHovered = pane;
    if (pane != nullptr) pane->mouseMove (ev (*pane, p - pane->getPosition()));
}
void click (MenuLayer& L, Point<int> p)
{
    moveTo (L, p);
    if (auto* pane = paneAt (L, p))
    {
        Component::SafePointer<MenuPane> safe (pane);
        pane->mouseDown (ev (*pane, p - pane->getPosition(), true));
        if (safe != nullptr) pane->mouseUp (ev (*pane, p - pane->getPosition()));
    }
    else
        L.mouseDown (ev (L, p, true));
    lastHovered = nullptr;
}
Point<int> rowCentre (MenuLayer& L, int paneIdx, const String& text)
{
    auto* pane = L.pane (paneIdx);
    if (pane == nullptr) return { -1, -1 };
    const int r = pane->findRow (text);
    if (r < 0) return { -1, -1 };
    return pane->rowBoundsOnScreen (r).getCentre() + pane->getPosition();
}
void key (MenuLayer& L, int code, juce_wchar ch = 0) { L.keyPressed (KeyPress (code, ModifierKeys(), ch)); }
void typeText (MenuLayer& L, const String& s) { for (auto c : s) key (L, (int) c, c); }

void snapshot (Host& h, const File& dir, const String& name)
{
    if (dir == File()) return;
    auto img = h.createComponentSnapshot (h.getLocalBounds(), true, 1.0f);
    auto f = dir.getChildFile (name + ".png");
    f.deleteFile();
    FileOutputStream os (f);
    PNGImageFormat().writeImageToStream (img, os);
}

bool inside (Host& h, MenuLayer& L)
{
    for (int i = 0; i < L.paneCount(); ++i)
        if (! h.getLocalBounds().contains (L.pane (i)->getBounds())) return false;
    return true;
}

Menu filterTypes (String& chosen)
{
    // Shaped like the filter type menu: categories, each a sub-menu.
    auto leaf = [&chosen] (const String& n) { return [&chosen, n] { chosen = n; }; };
    Menu normal, multi, flanges, misc;
    for (auto n : { "MG Low 6", "MG Low 12", "MG Low 18", "MG Low 24", "Low 6", "Low 12", "Low 18", "Low 24", "High 6", "High 12", "High 18", "High 24", "Band 12", "Band 24", "Peak 12", "Peak 24", "Notch 12", "Notch 24" })
        normal.add (n, leaf (n), String (n) == "MG Low 12");
    for (auto n : { "LH 6", "LH 12", "LB 12", "LP 12", "LN 12", "HB 12", "HP 12", "HN 12", "BP 12", "PP 12", "PN 12", "NN 12" })
        multi.add (n, leaf (n));
    for (auto n : { "Cmb +", "Cmb -", "Cmb BL6 +", "Flg +", "Flg -", "Phs 12+", "Phs 48+" })
        flanges.add (n, leaf (n));
    for (auto n : { "Low EQ 6", "Band EQ 12", "Ring Mod", "French LP", "German LP", "Comb L6+", "Reverb", "Sample & Hold", "Formant-I", "Formant-II" })
        misc.add (n, leaf (n));
    Menu m;
    m.sub ("Normal", normal, true).sub ("Multi", multi).sub ("Flanges", flanges).sub ("Misc", misc)
     .separator().add ("Off", leaf ("Off"));
    return m;
}
} // namespace

int main (int argc, char** argv)
{
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    ScopedJuceInitialiser_GUI gui;
    const File out = argc > 1 ? File::getCurrentWorkingDirectory().getChildFile (argv[1]) : File();
    if (out != File()) out.createDirectory();

    Host host;
    auto& L = layerOf (host);
    host.resized();

    auto anchor = std::make_unique<Anchor>();
    host.addAndMakeVisible (*anchor);
    anchor->setBounds (200, 120, 160, 24);

    String chosen;
    int fired = 0;

    std::printf ("1. dropdown opens below, starts on the ticked item, nothing fires on open\n");
    {
        Menu m;
        m.add ("Wavetable", [&] { chosen = "Wavetable"; ++fired; }, true)
         .add ("Multisample", [&] { chosen = "Multisample"; ++fired; })
         .add ("Sample", [&] { chosen = "Sample"; ++fired; })
         .add ("Granular", [&] { chosen = "Granular"; ++fired; })
         .add ("Spectral", [&] { chosen = "Spectral"; ++fired; });
        showMenu (*anchor, m);
        CHECK (L.isOpen());
        CHECK (L.paneCount() == 1);
        auto* p = L.pane (0);
        CHECK (p->getY() == anchor->getBottom());
        CHECK (p->getX() == anchor->getX());
        CHECK (p->getWidth() >= anchor->getWidth());
        CHECK (p->hotRow() == p->findRow ("Wavetable"));
        // The release of the click that opened the menu lands on an item: must not fire.
        const auto c = rowCentre (L, 0, "Sample");
        p->mouseUp (ev (*p, c - p->getPosition()));
        pump (30);
        CHECK (fired == 0);
        CHECK (L.isOpen());
        snapshot (host, out, "01_dropdown");

        std::printf ("2. click fires after close, asynchronously\n");
        click (L, c);
        CHECK (! L.isOpen());
        CHECK (fired == 0);                 // not yet: it runs after the menu has gone
        pump (30);
        CHECK (fired == 1 && chosen == "Sample");
    }

    std::printf ("3. click outside closes and fires nothing; clicking the anchor area closes too\n");
    {
        Menu m;
        m.add ("One", [&] { ++fired; }).add ("Two", [&] { ++fired; });
        fired = 0;
        showMenu (*anchor, m);
        click (L, { 1000, 700 });
        pump (30);
        CHECK (! L.isOpen() && fired == 0);
        showMenu (*anchor, m);
        click (L, anchor->getBounds().getCentre());   // the layer sits over the anchor
        pump (30);
        CHECK (! L.isOpen() && fired == 0);
    }

    std::printf ("4. anchor deleted while open: closes quietly\n");
    {
        auto temp = std::make_unique<Anchor>();
        host.addAndMakeVisible (*temp);
        temp->setBounds (600, 300, 120, 24);
        Menu m;
        m.add ("Boom", [&] { ++fired; });
        fired = 0;
        showMenu (*temp, m);
        CHECK (L.isOpen());
        temp.reset();
        pump (150);
        CHECK (! L.isOpen());
        CHECK (fired == 0);
    }

    std::printf ("5. no room below: opens above\n");
    {
        anchor->setBounds (200, 860, 160, 24);
        Menu m;
        for (int i = 0; i < 12; ++i) m.add ("Item " + String (i), [] {});
        showMenu (*anchor, m);
        CHECK (L.pane (0)->getBottom() == anchor->getY());
        CHECK (inside (host, L));
        snapshot (host, out, "05_above");
        L.dismiss();
        anchor->setBounds (200, 120, 160, 24);
    }

    std::printf ("6. very long menu: columns, all inside the panel, ticked item visible\n");
    {
        Menu m;
        for (int i = 0; i < 160; ++i) m.add ("Wavetable " + String (i + 1).paddedLeft ('0', 3), [] {}, i == 140);
        showMenu (*anchor, m);
        auto* p = L.pane (0);
        CHECK (inside (host, L));
        CHECK (p->columnCount() > 1 || p->scrollOffset() > 0);
        const auto tick = p->rowBoundsOnScreen (p->hotRow());
        CHECK (p->getLocalBounds().contains (tick));
        CHECK (p->filterText().isEmpty());
        snapshot (host, out, "06_long");

        std::printf ("7. searchable: typing filters, Return picks the first match\n");
        int picked = -1;
        Menu s;
        for (int i = 0; i < 160; ++i) s.add ("Wavetable " + String (i + 1).paddedLeft ('0', 3), [&picked, i] { picked = i; });
        showMenu (*anchor, s);
        typeText (L, "07");
        CHECK (L.pane (0)->filterText() == "07");
        CHECK (L.pane (0)->getRows().size() == 12);   // 007, 070..079 and 107
        snapshot (host, out, "07_search");
        key (L, KeyPress::returnKey);
        pump (30);
        CHECK (picked == 6);
        L.dismiss();
    }

    std::printf ("8. sub-menus: hover opens after a delay, diagonal moves keep it, straight moves switch\n");
    {
        chosen.clear();
        showMenu (*anchor, filterTypes (chosen));
        moveTo (L, rowCentre (L, 0, "Normal"));
        CHECK (L.paneCount() == 1);
        pump (220);
        CHECK (L.paneCount() == 2);
        snapshot (host, out, "08_submenu");
        // Diagonal: from Normal towards the sub-menu, crossing Multi.
        const auto normal = rowCentre (L, 0, "Normal");
        const auto multi = rowCentre (L, 0, "Multi");
        const auto childLeft = L.pane (1)->getX();
        moveTo (L, { normal.x + 30, normal.y + 4 });
        moveTo (L, { (multi.x + childLeft) / 2 + 40, multi.y });
        pump (100);
        CHECK (L.paneCount() == 2 && L.pane (0)->openRow() == L.pane (0)->findRow ("Normal"));
        // Into the sub-menu and pick something.
        click (L, rowCentre (L, 1, "Low 24"));
        pump (30);
        CHECK (chosen == "Low 24");
        CHECK (! L.isOpen());

        // Straight down onto another category switches after the delay.
        showMenu (*anchor, filterTypes (chosen));
        moveTo (L, rowCentre (L, 0, "Normal"));
        pump (220);
        moveTo (L, rowCentre (L, 0, "Normal") + Point<int> (-20, 0));
        moveTo (L, rowCentre (L, 0, "Misc") + Point<int> (-20, 0));
        pump (400);
        CHECK (L.paneCount() == 2 && L.pane (0)->openRow() == L.pane (0)->findRow ("Misc"));
        // Hovering a plain item closes the sub-menu.
        moveTo (L, rowCentre (L, 0, "Off") + Point<int> (-20, 0));
        pump (400);
        CHECK (L.paneCount() == 1);
        L.dismiss();
    }

    std::printf ("9. keyboard: arrows, right into a sub-menu, left back, Return, Escape\n");
    {
        chosen.clear();
        showMenu (*anchor, filterTypes (chosen));
        auto* p = L.pane (0);
        CHECK (p->hotRow() == -1);          // the ticked item is a sub-menu, so nothing is hot yet
        key (L, KeyPress::downKey);
        CHECK (p->hotRow() == p->findRow ("Normal"));
        key (L, KeyPress::downKey);
        key (L, KeyPress::downKey);
        CHECK (p->hotRow() == p->findRow ("Flanges"));
        key (L, KeyPress::rightKey);
        CHECK (L.paneCount() == 2);
        CHECK (L.pane (1)->hotRow() == 0);
        key (L, KeyPress::downKey);
        key (L, KeyPress::leftKey);
        CHECK (L.paneCount() == 1);
        key (L, KeyPress::rightKey);
        key (L, KeyPress::downKey);
        key (L, KeyPress::returnKey);
        pump (30);
        CHECK (chosen == "Cmb -");
        showMenu (*anchor, filterTypes (chosen));
        key (L, KeyPress::escapeKey);
        CHECK (! L.isOpen());
        // Letter jump in a non-searchable menu.
        Menu small;
        small.add ("Normal", [] {}).add ("Multi", [] {}).add ("Flanges", [] {}).add ("Misc", [] {});
        showMenu (*anchor, small);
        key (L, 'm', 'm');
        CHECK (L.pane (0)->hotRow() == L.pane (0)->findRow ("Multi"));
        key (L, 'm', 'm');
        CHECK (L.pane (0)->hotRow() == L.pane (0)->findRow ("Misc"));
        L.dismiss();
    }

    std::printf ("10. context menu at the right/bottom edges flips to stay inside\n");
    {
        Menu m;
        m.add ("Enter Value", [] {}).add ("Reset to Default", [] {}).separator()
         .add ("MIDI Learn", [] {}).add ("Clear MIDI Learn", [] {}, false, false).separator()
         .sub ("Copy", Menu().add ("Value", [] {}).add ("Modulation", [] {}));
        showContextMenu (host, { 1395, 895 }, m);
        CHECK (inside (host, L));
        CHECK (L.pane (0)->getRight() <= 1395 && L.pane (0)->getBottom() <= 895);
        snapshot (host, out, "10_context");
        // Disabled items can't be hovered or chosen.
        const auto dis = rowCentre (L, 0, "Clear MIDI Learn");
        moveTo (L, dis);
        CHECK (L.pane (0)->hotRow() == -1);
        click (L, dis);
        CHECK (L.isOpen());
        L.dismiss();
    }

    std::printf ("11. an action that deletes its own anchor and opens another menu\n");
    {
        auto temp = std::make_unique<Anchor>();
        host.addAndMakeVisible (*temp);
        temp->setBounds (500, 500, 140, 24);
        bool second = false;
        Menu m;
        m.add ("Rebuild", [&]
        {
            temp.reset();                  // the anchor goes away inside its own action
            Menu n;
            n.add ("Again", [&] { second = true; });
            showMenu (*anchor, n);
        });
        showMenu (*temp, m);
        click (L, rowCentre (L, 0, "Rebuild"));
        pump (30);
        CHECK (temp == nullptr);
        CHECK (L.isOpen());
        click (L, rowCentre (L, 0, "Again"));
        pump (30);
        CHECK (second);
    }

    std::printf ("12. keep-open toggles\n");
    {
        bool a = false, b = true;
        Menu m;
        m.add ("Tag A", [&] { a = ! a; }).keepingOpen ([&] { return a; })
         .add ("Tag B", [&] { b = ! b; }).keepingOpen ([&] { return b; });
        showMenu (*anchor, m);
        click (L, rowCentre (L, 0, "Tag A"));
        CHECK (L.isOpen() && a);
        click (L, rowCentre (L, 0, "Tag B"));
        CHECK (L.isOpen() && ! b);
        L.dismiss();
    }

    std::printf ("13. reopening replaces the open menu; dismiss is idempotent\n");
    {
        Menu m; m.add ("X", [] {});
        showMenu (*anchor, m);
        showMenu (*anchor, m);
        CHECK (L.paneCount() == 1);
        L.dismiss(); L.dismiss();
        CHECK (! L.isOpen() && ! L.isVisible());
    }

    std::printf ("14. scaled panel: the layer is a child of the host, so it scales with it\n");
    {
        host.setTransform (AffineTransform::scale (0.6f));
        Menu m; m.add ("Scaled", [] {});
        showMenu (*anchor, m);
        CHECK (L.getParentComponent() == &host);
        CHECK (L.getBounds() == host.getLocalBounds());
        L.dismiss();
        host.setTransform ({});
    }

    std::printf ("15. scroll when columns can't fit (narrow panel)\n");
    {
        host.setSize (300, 400);
        host.resized();
        anchor->setBounds (20, 40, 120, 24);
        Menu m;
        for (int i = 0; i < 80; ++i) m.add ("A long item name number " + String (i), [] {}, i == 70);
        showMenu (*anchor, m);
        auto* p = L.pane (0);
        CHECK (inside (host, L));
        CHECK (p->scrollOffset() > 0);   // scrolled to show the ticked item
        CHECK (p->getLocalBounds().contains (p->rowBoundsOnScreen (p->hotRow()).getCentre()));
        p->mouseWheelMove (ev (*p, { 20, 60 }), MouseWheelDetails { 0.0f, 1.0f, false, false, false });
        CHECK (p->scrollOffset() == 0 || p->scrollOffset() < 1000);
        for (int i = 0; i < 100; ++i) key (L, KeyPress::downKey);
        CHECK (p->hotRow() == (int) p->getRows().size() - 1);
        CHECK (p->getLocalBounds().contains (p->rowBoundsOnScreen (p->hotRow()).getCentre()));
        snapshot (host, out, "15_scroll");
        L.dismiss();
        host.setSize (1400, 900);
        host.resized();
    }

    std::printf ("\n%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
