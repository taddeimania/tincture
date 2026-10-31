// Pages.h — the panel's pages. Each is built by a factory in its own file so
// the shell (PluginEditor) doesn't depend on their insides.
//
// Design space: every page fills kPageW x kPageH (the panel below the header)
// and lays itself out in those fixed units; the whole panel is scaled to the
// window. Pages refresh through Bound/Refreshable (PanelContext::refreshAll
// runs at 30 Hz), never their own timers unless they animate.
#pragma once

#include "Bind.h"

namespace tincture::ui
{
constexpr int kPanelW = 1400, kPanelH = 880;
constexpr int kHeaderH = 62;
constexpr int kStatusH = 18;
constexpr int kPageW = kPanelW, kPageH = kPanelH - kHeaderH - kStatusH;

std::unique_ptr<juce::Component> createOscPage (PanelContext&);       // OscPage.cpp
std::unique_ptr<juce::Component> createMixPage (PanelContext&);       // MixPage.cpp
std::unique_ptr<juce::Component> createFxPage (PanelContext&);        // FxPage.cpp
std::unique_ptr<juce::Component> createMatrixPage (PanelContext&);    // MatrixPage.cpp
std::unique_ptr<juce::Component> createGlobalPage (PanelContext&);    // GlobalPage.cpp
// ---------------------------------------------------------------------------
// The OSC page is three bands: the module row (SUB, OSC A-C, NOISE, FILTER 1-2),
// the modulation strip (macros, envelopes, LFOs, VELO/NOTE, voicing), and the
// bottom strip (wheels, CLIP/ARP, keyboard bar, portamento). Big editors
// (wavetable, arp, clip, LFO) open as overlays over the module row (or over
// the module row and the mod strip together).
// ---------------------------------------------------------------------------
constexpr int kModuleRowH = 392;
constexpr int kModStripY = kModuleRowH, kModStripH = 240;
constexpr int kBottomY = kModStripY + kModStripH, kBottomH = kPageH - kBottomY;   // 168

class OscPageHost
{
public:
    virtual ~OscPageHost() = default;
    enum class Area { Modules, ModulesAndMods };
    // Shows `c` over that area (replacing any other overlay). `id` names it
    // so a second click on whatever opened it can close it (toggle).
    virtual void showOverlay (std::unique_ptr<juce::Component> c, Area, const juce::String& id) = 0;
    virtual void closeOverlay() = 0;
    virtual juce::String overlayId() const = 0;
};

std::unique_ptr<juce::Component> createModStrip (PanelContext&, OscPageHost&);      // ModStrip.cpp, kPageW x kModStripH
std::unique_ptr<juce::Component> createBottomStrip (PanelContext&, OscPageHost&);   // BottomStrip.cpp, kPageW x kBottomH
std::unique_ptr<juce::Component> createArpEditor (PanelContext&, std::function<void()> onClose);    // ArpEditor.cpp
std::unique_ptr<juce::Component> createClipEditor (PanelContext&, std::function<void()> onClose);   // ClipEditor.cpp
std::unique_ptr<juce::Component> createWavetableEditor (PanelContext&, int osc, std::function<void()> onClose);   // WavetableEditor.cpp
std::unique_ptr<juce::Component> createOscMappingEditor (PanelContext&, std::function<void()> onClose);         // OscMapping.cpp

// The preset browser, shown over the page area. `onClose` hides it.
std::unique_ptr<juce::Component> createPresetBrowser (PanelContext&, std::function<void()> onClose);   // PresetBrowser.cpp
} // namespace tincture::ui
