// OscPage.cpp — the OSC page: the module row (SUB, OSC A-C, NOISE, FILTER
// 1-2), the modulation strip and the bottom strip, plus the overlay slot
// big editors (wavetable, arp, clip, LFO) open into.
#include "OscModules.h"

namespace tincture::ui
{
using namespace juce;

namespace
{
class OscPage final : public Component, public OscPageHost
{
public:
    explicit OscPage (PanelContext& ctx)
    {
        modules = createModuleRow (ctx, *this);
        mods = createModStrip (ctx, *this);
        bottom = createBottomStrip (ctx, *this);
        for (auto* c : { modules.get(), mods.get(), bottom.get() }) addAndMakeVisible (*c);
    }

    ~OscPage() override { overlay.reset(); }

    void resized() override
    {
        modules->setBounds (0, 0, kPageW, kModuleRowH);
        mods->setBounds (0, kModStripY, kPageW, kModStripH);
        bottom->setBounds (0, kBottomY, kPageW, kBottomH);
        placeOverlay();
    }

    void paint (Graphics& g) override { g.fillAll (colours::panel); }

    void showOverlay (std::unique_ptr<Component> c, Area a, const String& id) override
    {
        dismissMenus (*this);
        overlay = std::move (c);
        area = a;
        overlayName = id;
        if (overlay == nullptr) { overlayName.clear(); return; }
        addAndMakeVisible (*overlay);
        placeOverlay();
        overlay->toFront (false);
    }

    void closeOverlay() override
    {
        if (overlay == nullptr) return;
        // The overlay may be closing itself from one of its own handlers.
        overlay->setVisible (false);
        retired = std::move (overlay);
        overlayName.clear();
        SafePointer<OscPage> safe (this);
        MessageManager::callAsync ([safe] { if (safe != nullptr) safe->retired.reset(); });
    }

    String overlayId() const override { return overlayName; }

private:
    void placeOverlay()
    {
        if (overlay != nullptr)
            overlay->setBounds (0, 0, kPageW, area == Area::Modules ? kModuleRowH : kModuleRowH + kModStripH);
    }

    std::unique_ptr<Component> modules, mods, bottom, overlay, retired;
    Area area = Area::Modules;
    String overlayName;
};
}

std::unique_ptr<Component> createOscPage (PanelContext& ctx)
{
    return std::make_unique<OscPage> (ctx);
}
} // namespace tincture::ui
