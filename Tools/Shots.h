// Shots.h — a harness for UI checks inside TinctureSnapshot.
//
// Each UI area registers a shot function in its own Tools/Shots<Area>.cpp:
//
//     TINCTURE_SHOT ("osc", [] (Shot& s) { ... });
//
// and runs it with:   TinctureSnapshot out-dir shot osc
//
// A shot gets a live processor (audio blocks run on demand, a chord can be
// held) and the open editor panel. Helpers simulate real mouse input on real
// components (so menus open through the same handlers as in a host), inspect
// and choose menu items through the menu layer, and save PNGs.
#pragma once

#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "UI/Menu.h"

#include <functional>
#include <map>

struct Shot
{
    TinctureProcessor& proc;
    TincturePanel& panel;
    juce::Component& editor;
    juce::File dir;
    int failures = 0, checks = 0;

    // Time and audio.
    void pump (int ms);                         // run the message loop (timers, async menu actions)
    void play (int blocks = 20);                // audio blocks (512 samples) + 1 UI frame
    void noteOn (int note, int vel = 100);
    void noteOff (int note);

    // Output.
    void save (const juce::String& name);       // out-dir/<name>.png of the whole panel
    void check (bool ok, const juce::String& what);

    // Finding components.
    template <typename T> T* find (std::function<bool (T&)> pred = {})
    {
        T* found = nullptr;
        std::function<void (juce::Component&)> walk = [&] (juce::Component& c)
        {
            if (found != nullptr) return;
            if (auto* t = dynamic_cast<T*> (&c)) if (! pred || pred (*t)) { found = t; return; }
            for (auto* ch : c.getChildren()) walk (*ch);
        };
        walk (panel);
        return found;
    }
    juce::Component* findByTooltip (const juce::String& contains);   // visible components only
    tincture::ui::PanelButton* button (const juce::String& label);   // a visible PanelButton by label

    // Mouse (component-local coordinates; default = centre).
    void click (juce::Component&, juce::Point<int> at = { -1, -1 }, bool right = false);
    void doubleClick (juce::Component&, juce::Point<int> at = { -1, -1 });
    void drag (juce::Component&, juce::Point<int> from, juce::Point<int> to, int steps = 8, bool shift = false);
    void hover (juce::Component&, juce::Point<int> at);
    void wheel (juce::Component&, float deltaY, juce::Point<int> at = { -1, -1 });

    // Menus.
    bool menuOpen();
    int menuDepth();
    juce::StringArray menuItems (int depth = 0);           // visible rows' text ("---" separators, "#" headers)
    // Paths are "Sub/Item"; write "\\/" for a slash inside an item ("Swap A\\/B").
    bool menuHas (const juce::String& slashPath);           // "Analog/Default Shapes" (opens sub-menus as needed)
    bool choose (const juce::String& slashPath);           // navigates with real hovers/clicks, then pumps
    void closeMenu();
    void key (int keyCode, juce::juce_wchar ch = 0);       // to the open menu (or the panel)

    int summary() const;                       // prints and returns failures
};

using ShotFn = std::function<void (Shot&)>;
std::map<juce::String, ShotFn>& shotRegistry();

struct ShotRegistrar
{
    ShotRegistrar (const char* name, ShotFn fn) { shotRegistry()[name] = std::move (fn); }
};
#define TINCTURE_SHOT_CAT2(a, b) a##b
#define TINCTURE_SHOT_CAT(a, b) TINCTURE_SHOT_CAT2 (a, b)
#define TINCTURE_SHOT(name, fn) static ShotRegistrar TINCTURE_SHOT_CAT (shotReg_, __LINE__) (name, fn)
