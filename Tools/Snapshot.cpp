// Snapshot.cpp — dev tool: run Tincture without a host, play a chord through
// it, and render the panel to PNGs.
//   TinctureSnapshot out-dir [page...]        pages: osc mix fx matrix global browser
//   TinctureSnapshot out-dir menus            the main, resize and preset menus open
//   TinctureSnapshot out-dir stress [rounds]  open/use/close the editor while audio runs
//   TinctureSnapshot out-dir wav              render a few seconds of audio to out-dir/out.wav
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "Shots.h"

#include <thread>

namespace
{
void pump (int ms) { juce::MessageManager::getInstance()->runDispatchLoopUntil (ms); }

void play (TinctureProcessor& proc, int blocks, int block, juce::MidiBuffer* first = nullptr)
{
    juce::AudioBuffer<float> buf (2, block);
    for (int i = 0; i < blocks; ++i)
    {
        juce::MidiBuffer midi;
        if (i == 0 && first != nullptr) midi = *first;
        buf.clear();
        proc.processBlock (buf, midi);
    }
}

juce::MidiBuffer chord (bool on)
{
    juce::MidiBuffer m;
    for (int n : { 48, 55, 60, 64 })
        m.addEvent (on ? juce::MidiMessage::noteOn (1, n, (juce::uint8) 100) : juce::MidiMessage::noteOff (1, n), 0);
    return m;
}

void save (juce::Component& c, const juce::File& f)
{
    auto img = c.createComponentSnapshot (c.getLocalBounds(), true, 1.0f);
    f.deleteFile();
    juce::FileOutputStream os (f);
    juce::PNGImageFormat().writeImageToStream (img, os);
    std::printf ("wrote %s\n", f.getFullPathName().toRawUTF8());
}

void press (juce::Component& c, const juce::String& label)
{
    if (auto* b = dynamic_cast<tincture::ui::PanelButton*> (&c))
        if (b->getLabel() == label && b->isVisible() && b->onClick) { b->onClick ({}); return; }
    for (auto* ch : c.getChildren()) press (*ch, label);
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI gui;
    std::setvbuf (stdout, nullptr, _IONBF, 0);
    if (argc < 2) { std::printf ("usage: TinctureSnapshot out-dir [osc|mix|fx|matrix|global|browser|menus|stress|wav]...\n"); return 1; }
    const auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (argv[1]);
    dir.createDirectory();
    const double rate = 48000.0;
    const int block = 512;

    auto proc = std::make_unique<TinctureProcessor>();
    proc->setPlayConfigDetails (0, 2, rate, block);
    proc->prepareToPlay (rate, block);

    juce::StringArray modes;
    for (int i = 2; i < argc; ++i) modes.add (argv[i]);
    if (modes.isEmpty()) modes = { "osc", "mix", "fx", "matrix", "global", "browser", "menus" };

    if (modes.contains ("wav"))
    {
        juce::AudioBuffer<float> all (2, (int) rate * 4);
        all.clear();
        juce::AudioBuffer<float> buf (2, block);
        for (int pos = 0, i = 0; pos + block <= all.getNumSamples(); pos += block, ++i)
        {
            auto midi = i == 0 ? chord (true) : (i == 250 ? chord (false) : juce::MidiBuffer());
            buf.clear();
            proc->processBlock (buf, midi);
            for (int c = 0; c < 2; ++c) all.copyFrom (c, pos, buf, c, 0, block);
        }
        juce::WavAudioFormat wav;
        auto f = dir.getChildFile ("out.wav");
        f.deleteFile();
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (new juce::FileOutputStream (f), rate, 2, 24, {}, 0));
        w->writeFromAudioSampleBuffer (all, 0, all.getNumSamples());
        std::printf ("wrote %s, peak %.3f\n", f.getFullPathName().toRawUTF8(), all.getMagnitude (0, all.getNumSamples()));
        return 0;
    }

    if (modes.contains ("stress"))
    {
        std::atomic<bool> running { true };
        std::thread audio ([&]
        {
            int i = 0;
            while (running.load())
            {
                auto m = (i % 200 == 0) ? chord (true) : (i % 200 == 100 ? chord (false) : juce::MidiBuffer());
                play (*proc, 1, block, &m);
                ++i;
                std::this_thread::sleep_for (std::chrono::microseconds (300));
            }
        });
        const int rounds = modes.size() > 1 ? modes[1].getIntValue() : 4;
        for (int r = 0; r < juce::jmax (1, rounds); ++r)
        {
            std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
            auto& panel = static_cast<TinctureEditor*> (ed.get())->getPanel();
            for (int p = 0; p < TincturePanel::NumPages; ++p) { panel.showPage (p); pump (80); }
            proc->undo();
            proc->redo();
            ed.reset();
            pump (40);
            std::printf ("round %d ok\n", r + 1);
        }
        running.store (false);
        audio.join();
        return 0;
    }

    if (modes[0] == "shot")
    {
        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        ed->setSize (TincturePanel::designW, TincturePanel::designH);
        auto& panel = static_cast<TinctureEditor*> (ed.get())->getPanel();
        pump (60);
        int failures = 0;
        for (int i = 1; i < modes.size(); ++i)
        {
            auto& reg = shotRegistry();
            for (auto& [name, fn] : reg)
                if (modes[i] == "all" || modes[i] == name)
                {
                    std::printf ("shot %s\n", name.toRawUTF8());
                    Shot s { *proc, panel, *ed, dir };
                    fn (s);
                    tincture::ui::dismissMenus (panel);
                    failures += s.summary();
                }
        }
        ed.reset();
        pump (50);
        return failures == 0 ? 0 : 1;
    }

    auto on = chord (true);
    play (*proc, 1, block, &on);
    play (*proc, 60, block);

    std::unique_ptr<juce::AudioProcessorEditor> editor (proc->createEditor());
    editor->setSize (TincturePanel::designW, TincturePanel::designH);
    auto& panel = static_cast<TinctureEditor*> (editor.get())->getPanel();
    for (int i = 0; i < 20; ++i) { play (*proc, 3, block); pump (33); }

    const char* names[] { "osc", "mix", "fx", "matrix", "global" };
    for (int p = 0; p < TincturePanel::NumPages; ++p)
        if (modes.contains (names[p]))
        {
            panel.showPage (p);
            for (int i = 0; i < 10; ++i) { play (*proc, 3, block); pump (33); }
            save (*editor, dir.getChildFile (juce::String ("page_") + names[p] + ".png"));
        }
    if (modes.contains ("browser"))
    {
        panel.toggleBrowser();
        pump (60);
        save (*editor, dir.getChildFile ("browser.png"));
        panel.toggleBrowser();
    }
    if (modes.contains ("menus"))
    {
        panel.showPage (0);
        panel.openMainMenu();   pump (50); save (*editor, dir.getChildFile ("menu_main.png"));
        panel.openResizeMenu(); pump (50); save (*editor, dir.getChildFile ("menu_resize.png"));
        panel.openPresetMenu(); pump (50); save (*editor, dir.getChildFile ("menu_preset.png"));
        tincture::ui::dismissMenus (panel);
    }
    editor.reset();
    pump (50);
    return 0;
}
