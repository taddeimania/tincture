// ShotsShell.cpp — the header and page switching.
#include "Shots.h"

TINCTURE_SHOT ("shell", [] (Shot& s)
{
    using namespace tincture;
    for (auto name : { "OSC", "MIX", "FX", "MATRIX", "GLOBAL" })
    {
        auto* b = s.button (name);
        s.check (b != nullptr, juce::String ("tab ") + name);
        if (b != nullptr) s.click (*b);
    }
    s.check (s.panel.currentPage() == TincturePanel::Global, "GLOBAL page shown");
    s.click (*s.button ("OSC"));

    // MENU opens the main menu with Init Preset; choosing it runs it.
    s.click (*s.button ("MENU"));
    s.check (s.menuOpen(), "main menu open");
    s.check (s.menuItems().contains ("Init Modulations"), "main menu items");
    s.save ("shell_menu_main");
    s.choose ("Init Preset");
    s.check (! s.menuOpen(), "menu closed after choosing");

    // Undo / redo of a parameter change.
    const int p = ids().osc[0].level;
    s.proc.setWithUndo (p, 0.3f);
    s.check (std::abs (s.proc.get (p) - 0.3f) < 1e-4f, "level set");
    s.click (*s.button ("UNDO"));
    s.check (std::abs (s.proc.get (p) - 0.75f) < 1e-4f, "undo restores level");
    s.click (*s.button ("REDO"));
    s.check (std::abs (s.proc.get (p) - 0.3f) < 1e-4f, "redo");

    // Preset menu: Factory > Init.
    s.click (s.panel, { 800, 30 });
    s.check (s.menuOpen(), "preset menu open");
    s.check (s.menuHas ("Factory/Init/Init"), "factory init listed");
    s.closeMenu();
    // Clicking outside a menu closes it and nothing else happens.
    s.click (*s.button ("MENU"));
    s.click (s.panel, { 700, 600 });
    s.check (! s.menuOpen(), "click outside closes");
    s.check (s.panel.currentPage() == TincturePanel::Osc, "click outside didn't change page");
});
