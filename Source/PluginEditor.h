// PluginEditor.h — the Tincture panel: header (logo/resize menu, page tabs,
// preset bar, browser, MENU, undo/redo, MAIN volume and meter), the page
// area, and a status line. Drawn in a fixed design space scaled to the
// window; menus live in the panel's menu layer (UI/Menu.h).
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "PluginProcessor.h"
#include "UI/Bind.h"
#include "UI/Pages.h"

class TincturePanel final : public tincture::ui::MenuHost,
                            public tincture::ui::PanelContext,
                            public juce::DragAndDropContainer,
                            private juce::Timer
{
public:
    static constexpr int designW = tincture::ui::kPanelW;
    static constexpr int designH = tincture::ui::kPanelH;
    enum Page { Osc = 0, Mix, Fx, Matrix, Global, NumPages };

    explicit TincturePanel (TinctureProcessor&);
    ~TincturePanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;

    void showPage (int);
    int currentPage() const noexcept { return page; }
    void toggleBrowser();
    std::function<void (float scale)> onScale;   // the editor resizes the window

    // Test / snapshot access.
    juce::Component* pageComponent (int p) { return pages[(size_t) p].get(); }
    void openMainMenu();
    void openResizeMenu();
    void openPresetMenu();

private:
    void timerCallback() override;
    void editValueBox (juce::Component& target, const juce::String& initial, std::function<void (const juce::String&)> onDone);
    void savePresetDialog();
    void loadPresetDialog();
    void importSerumDialog();
    void importSerumFolderDialog();
    void serumFolderDialog();
    void showReport (const juce::String& title, const juce::String& text);
    juce::Rectangle<int> logoArea() const { return { 14, 8, 170, 46 }; }

    using PanelButton = tincture::ui::PanelButton;
    tincture::ui::PanelLookAndFeel lnf;
    std::array<std::unique_ptr<juce::Component>, NumPages> pages;
    std::unique_ptr<juce::Component> browser;
    int page = Osc;

    PanelButton tabs[NumPages] { PanelButton ("OSC"), PanelButton ("MIX"), PanelButton ("FX"), PanelButton ("MATRIX"), PanelButton ("GLOBAL") };
    PanelButton saveBtn { "SAVE" }, prevBtn { "<" }, nextBtn { ">" }, browseBtn { "BROWSE" }, menuBtn { "MENU" },
                undoBtn { "UNDO" }, redoBtn { "REDO" };
    tincture::ui::ChoiceDisplay presetName;
    std::unique_ptr<tincture::ui::ParamKnob> masterKnob;
    tincture::ui::Meter meterL, meterR;
    juce::String status, shownStatus;
    int unison = -1;
    std::unique_ptr<juce::TextEditor> valueEditor;
    std::function<void (const juce::String&)> valueDone;
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<juce::AlertWindow> saveWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TincturePanel)
};

class TinctureEditor final : public juce::AudioProcessorEditor
{
public:
    explicit TinctureEditor (TinctureProcessor&);
    void paint (juce::Graphics&) override;
    void resized() override;
    TincturePanel& getPanel() { return panel; }

private:
    TincturePanel panel;
    std::unique_ptr<juce::TooltipWindow> tooltips;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (TinctureEditor)
};
