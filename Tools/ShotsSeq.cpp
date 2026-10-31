// ShotsSeq.cpp — the bottom strip (wheels, CLIP/ARP tabs, keyboard bar,
// on-screen keyboard), the CLIP editor (piano roll, slots, menus) and the
// ARP editor (panes, slots, pattern editor), through real clicks and drags.
#include "Shots.h"
#include "UI/SeqWidgets.h"

using namespace tincture;
using namespace tincture::ui;

namespace
{
struct SeqHost final : OscPageHost
{
    explicit SeqHost (juce::Component& p) : page (p) {}
    void showOverlay (std::unique_ptr<juce::Component> c, Area a, const juce::String& i) override
    {
        closeOverlay();
        ov = std::move (c);
        id = i;
        page.addAndMakeVisible (*ov);
        ov->setBounds (0, 0, kPageW, a == Area::Modules ? kModuleRowH : kModuleRowH + kModStripH);
    }
    void closeOverlay() override
    {
        // Overlays close from their own buttons: delete after the click returns.
        if (ov != nullptr) { graveyard.push_back (std::move (ov)); graveyard.back()->setVisible (false); }
        id.clear();
    }
    juce::String overlayId() const override { return id; }
    juce::Component& page;
    std::unique_ptr<juce::Component> ov;
    std::vector<std::unique_ptr<juce::Component>> graveyard;
    juce::String id;
};

juce::MouseEvent mouseEv (juce::Component& c, juce::Point<int> p, juce::ModifierKeys mods)
{
    return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), p.toFloat(), mods, juce::MouseInputSource::defaultPressure,
                             0.0f, 0.0f, 0.0f, 0.0f, &c, &c, juce::Time::getCurrentTime(), p.toFloat(), juce::Time::getCurrentTime(), 1, false);
}

bool keyDown (Shot& s, int note)
{
    const auto& t = s.proc.telemetry();
    const uint64_t bits = note < 64 ? t.keysLo.load() : t.keysHi.load();
    return ((bits >> (note % 64)) & 1u) != 0;
}

const Clip& clip (Shot& s, int i) { return s.proc.patch().clips.clips[(size_t) i]; }
const ArpSlot& arpSlot (Shot& s, int i) { return s.proc.patch().arp.slots[(size_t) i]; }

template <typename T> T* byTip (Shot& s, const juce::String& tip) { return dynamic_cast<T*> (s.findByTooltip (tip)); }

// Shot::choose splits its path on '/', so items like "1/8" or "Up/Down" are
// chosen here: one string per menu level, same hover/click sequence.
bool pick (Shot& s, juce::StringArray path)
{
    auto& L = s.panel.getMenuLayer();
    auto ev = [] (MenuPane* pane, juce::Point<int> c, juce::ModifierKeys m)
    {
        return juce::MouseEvent (juce::Desktop::getInstance().getMainMouseSource(), c.toFloat(), m, 1.0f, 0, 0, 0, 0,
                                 pane, pane, juce::Time::getCurrentTime(), c.toFloat(), juce::Time::getCurrentTime(), 1, false);
    };
    for (int d = 0; d < path.size(); ++d)
    {
        auto* pane = L.pane (d);
        if (pane == nullptr) break;
        const int row = pane->findRow (path[d]);
        if (row < 0) break;
        const auto c = pane->rowBoundsOnScreen (row).getCentre();
        juce::Component::SafePointer<MenuPane> safe (pane);
        pane->mouseMove (ev (pane, c, {}));
        pane->mouseDown (ev (pane, c, juce::ModifierKeys::leftButtonModifier));
        if (d < path.size() - 1) { s.pump (10); continue; }
        if (safe != nullptr) pane->mouseUp (ev (pane, c, {}));
        s.pump (40);
        return true;
    }
    std::printf ("  (pick failed: %s; items: %s)\n", path.joinIntoString (" > ").toRawUTF8(), s.menuItems().joinIntoString (" | ").toRawUTF8());
    s.closeMenu();
    return false;
}

bool shownIn (juce::Component& c, juce::Component& root)
{
    for (auto* p = &c; p != nullptr && p != &root; p = p->getParentComponent())
        if (! p->isVisible()) return false;
    return true;
}
} // namespace

static void runSeqShot (Shot& s)
{
    const auto& G = ids().global;
    s.panel.showPage (TincturePanel::Osc);
    auto* page = s.panel.pageComponent (TincturePanel::Osc);
    s.check (page != nullptr, "osc page");
    if (page == nullptr) return;

    std::unique_ptr<SeqHost> host;
    std::unique_ptr<juce::Component> owned;
    if (s.findByTooltip ("Pitch wheel") == nullptr)
    {
        host = std::make_unique<SeqHost> (*page);
        owned = createBottomStrip (s.panel, *host);
        page->addAndMakeVisible (*owned);
        owned->setBounds (0, kBottomY, kPageW, kBottomH);
        std::printf ("  (mounted the bottom strip with a test host)\n");
    }
    s.pump (60);
    s.save ("seq_strip");

    // ---- keyboard bar --------------------------------------------------------
    auto* key = s.find<ParamChoice> ([&] (ParamChoice& c) { return c.param() == G.key; });
    auto* scale = s.find<ParamChoice> ([&] (ParamChoice& c) { return c.param() == G.scale; });
    s.check (key != nullptr && scale != nullptr, "key/scale fields");
    if (key == nullptr || scale == nullptr) return;
    s.click (*key);
    s.check (s.menuOpen() && s.menuItems().contains ("C#") && s.menuItems().contains ("-"), "key menu");
    s.save ("seq_menu_key");
    s.choose ("D");
    s.check (s.proc.params.geti (G.key) == 3, "key D chosen");
    s.click (*scale);
    {
        auto items = s.menuItems();
        s.check (items.contains ("Major") && items.contains ("Iwato") && items.contains ("Insen") && items.contains ("Chromatic"), "scale menu items");
        auto* pane = s.panel.getMenuLayer().pane (0);
        s.check (pane != nullptr && pane->columnCount() == 3, "scale menu in 3 columns (" + juce::String (pane ? pane->columnCount() : 0) + ")");
        s.check (items.indexOf ("Insen") == items.indexOf ("Iwato") + 1, "column order");
    }
    s.save ("seq_menu_scale");
    s.choose ("Dorian");
    s.check (s.proc.params.geti (G.scale) == 1, "scale Dorian chosen");

    // TRANSPOSE: wheel = 12, arrows = 1.
    if (auto* tr = s.find<ParamField> ([&] (ParamField& f) { return f.param() == G.transpose; }))
    {
        s.wheel (*tr, 1.0f);
        s.check (std::abs (s.proc.get (G.transpose) - 12.0f) < 0.01f, "transpose wheel +12");
        s.wheel (*tr, -1.0f);
        s.drag (*tr, { 20, 15 }, { 20, -1 });
        s.check (s.proc.get (G.transpose) >= 1.0f, "transpose drag up");
        s.proc.setWithUndo (G.transpose, 0.0f);
    }
    // SWING: division field appears once swing is on.
    if (auto* sw = s.find<ParamField> ([&] (ParamField& f) { return f.param() == G.swing; }))
    {
        s.drag (*sw, { 30, 15 }, { 30, -25 });
        s.check (s.proc.get (G.swing) > 0.0f, "swing dragged");
        s.pump (60);
        auto* div = byTip<seq::Field> (s, "Swing division");
        s.check (div != nullptr, "swing division visible");
        if (div != nullptr)
        {
            s.click (*div);
            s.check (s.menuItems().contains ("1/8"), "swing division menu");
            pick (s, { "1/8" });
            s.check (s.proc.patch().keyboard.swingDivision == 2, "swing division 1/8");
        }
    }

    // On-screen keyboard: press C3 and hold.
    auto* kb = s.findByTooltip ("Click or drag to play");
    s.check (kb != nullptr, "keyboard");
    if (kb != nullptr)
    {
        // C3 = note 60: white key index 35 of 71.
        const float ww = (float) kb->getWidth() / 71.0f;
        const juce::Point<int> c3 ((int) (ww * 35.5f), kb->getHeight() - 12);
        kb->mouseDown (mouseEv (*kb, c3, juce::ModifierKeys::leftButtonModifier));
        s.play (6);
        s.check (keyDown (s, 60), "C3 held reaches the engine");
        s.check (s.proc.telemetry().activeVoices.load() > 0, "a voice sounds");
        s.save ("seq_strip_playing");
        kb->mouseUp (mouseEv (*kb, c3, {}));
        s.play (6);
        s.check (! keyDown (s, 60), "C3 released");
    }
    // Mod wheel.
    if (auto* mw = s.findByTooltip ("Mod wheel"))
    {
        s.drag (*mw, { 10, 120 }, { 10, 40 });
        s.play (2);
    }

    // ---- CLIP editor ---------------------------------------------------------
    auto* clipTab = s.button ("CLIP");
    s.check (clipTab != nullptr, "CLIP tab");
    if (clipTab == nullptr) return;
    s.click (*clipTab, {}, true);
    s.check (s.menuItems().contains ("Lock Module") && s.menuHas ("MIDI Input Trigger Octave/C-1"), "CLIP tab menu");
    s.save ("seq_menu_cliptab");
    s.choose ("MIDI Input Trigger Octave/C-1");
    s.check (s.proc.patch().clips.triggerOctave == 1, "trigger octave C-1");
    s.click (*clipTab);
    s.pump (40);
    auto* roll = s.findByTooltip ("Double-click: add/delete a note");
    s.check (roll != nullptr, "piano roll open");
    if (roll == nullptr) return;
    s.save ("seq_clip_empty");

    // Fit the view to 1 bar, C3 region; draw notes with double-clicks.
    // Grid area: x from 46, ruler 22 high. With 1 bar over the width: beat b at 46 + b * (w-46)/4.
    const int gw = roll->getWidth() - 46;
    auto xAt = [&] (double beat) { return 46 + (int) (beat * gw / 4.0) + 3; };
    // Rows are 12 px; after FIT the top row is one above the highest note.
    const int rowsVisibleTop = 32;   // row of pitch 76; rows are 12 px
    const int before = (int) clip (s, 0).notes.size();
    const int pitches[] { 0, 2, 4, 1 };   // rows below the top: E5-ish, going down
    for (int i = 0; i < 4; ++i)
        s.doubleClick (*roll, { xAt (i), rowsVisibleTop + 12 * (6 + pitches[i]) + 6 });
    s.check ((int) clip (s, 0).notes.size() == before + 4, "4 notes drawn (" + juce::String ((int) clip (s, 0).notes.size()) + ")");
    if (clip (s, 0).notes.size() >= 4)
    {
        const auto first = clip (s, 0).notes[0];
        // Drag the first note right by one beat and up two rows.
        const int y0 = rowsVisibleTop + 12 * 6 + 6;
        s.drag (*roll, { xAt (0) + 6, y0 }, { xAt (1) + 6, y0 - 24 });
        bool moved = false;
        for (auto& n : clip (s, 0).notes) moved |= std::abs (n.start - (first.start + 1.0)) < 1.0e-6 && n.pitch == first.pitch + 2;
        s.check (moved, "note moved by drag (+1 beat, +2 semitones)");
        // Resize: drag the right edge of the note now at beat 1.
        int idx = -1;
        for (int i = 0; i < (int) clip (s, 0).notes.size(); ++i)
            if (std::abs (clip (s, 0).notes[(size_t) i].start - 1.0) < 1.0e-6 && clip (s, 0).notes[(size_t) i].pitch == first.pitch + 2) idx = i;
        if (idx >= 0)
        {
            const int xr = 46 + (int) (1.25 * gw / 4.0) - 3;
            s.drag (*roll, { xr, y0 - 24 }, { xr + gw / 4, y0 - 24 });
            s.check (clip (s, 0).notes[(size_t) idx].length > 0.4, "note resized (" + juce::String (clip (s, 0).notes[(size_t) idx].length) + ")");
        }
    }
    // Undo groups a drag into one step.
    s.check (s.proc.undoLabel() == "Resize Notes" || s.proc.undoLabel() == "Move Notes", "drag is one undo step (" + s.proc.undoLabel() + ")");
    s.save ("seq_clip_notes");

    // Marquee select everything, then the note menu.
    s.drag (*roll, { 50, 36 }, { roll->getWidth() - 10, roll->getHeight() - 70 });
    s.check (clip (s, 0).numSelected() == (int) clip (s, 0).notes.size(), "marquee selects all");
    {
        const auto& n = clip (s, 0).notes[0];
        juce::ignoreUnused (n);
        s.click (*roll, { xAt (2), rowsVisibleTop + 12 * 10 + 6 }, true);
        auto items = s.menuItems();
        s.check (items.contains ("Cut") && items.contains ("Conform to Scale") && items.contains ("Double Entire clip") && items.contains ("Select All"), "note menu items");
        s.check (items.size() >= 20, "note menu complete (" + juce::String (items.size()) + ")");
        s.save ("seq_menu_note");
        const int count = (int) clip (s, 0).notes.size();
        s.choose ("Duplicate");
        s.check ((int) clip (s, 0).notes.size() == count * 2, "duplicate doubled the notes");
        s.proc.undo();
        s.click (*roll, { xAt (2), rowsVisibleTop + 12 * 10 + 6 }, true);
        s.choose ("Double Entire clip");
        s.check (clip (s, 0).length >= 8.0, "double entire clip");
        s.proc.undo();
    }
    // Keyboard shortcut: select all + up arrow.
    if (auto* pr = dynamic_cast<juce::Component*> (roll))
    {
        const int p0 = clip (s, 0).notes[0].pitch;
        pr->keyPressed (juce::KeyPress ('A', juce::ModifierKeys::commandModifier, 'a'));
        pr->keyPressed (juce::KeyPress (juce::KeyPress::upKey));
        s.pump (20);
        s.check (clip (s, 0).notes[0].pitch == p0 + 1, "up arrow transposes");
        pr->keyPressed (juce::KeyPress (juce::KeyPress::downKey));
    }

    // Grid menu.
    if (auto* gf = byTip<seq::Field> (s, "Grid: drag to change"))
    {
        s.click (*gf);
        auto items = s.menuItems();
        s.check (items.size() == 5 && items[0] == "Double Grid" && items[4] == "Auto Adjust Grid", "grid menu (" + items.joinIntoString ("|") + ")");
        s.save ("seq_menu_grid");
        s.choose ("Half Grid");
        s.check (std::abs (s.proc.patch().clips.grid - 0.125) < 1.0e-9, "half grid");
        s.click (*gf, {}, true);
        s.choose ("Triplets");
        s.check (std::abs (s.proc.patch().clips.grid - 0.125 * 2.0 / 3.0) < 1.0e-9, "triplets");
        s.click (*gf, {}, true);
        s.choose ("Triplets");
        s.click (*gf);
        s.choose ("Double Grid");
        s.check (std::abs (s.proc.patch().clips.grid - 0.25) < 1.0e-9, "double grid");
    }
    // Start-offset marker menu (right-click the marker at beat 0 in the ruler).
    s.click (*roll, { 47, 28 }, true);
    s.check (s.menuItems().contains ("#Clip 1 Offset") && s.menuItems().contains ("Wrap Clip to Begin Here"), "offset marker menu");
    s.save ("seq_menu_offset");
    pick (s, { "Quantize", "1/8" });
    s.check (clip (s, 0).offsetQuant == 3, "offset quantize 1/8");
    // End flag drag: length to 2 bars needs zoom; drag to 3 beats instead.
    s.drag (*roll, { 46 + gw - 2, 18 }, { xAt (3) - 3, 18 });
    s.check (std::abs (clip (s, 0).length - 3.0) < 1.0e-6, "end flag sets length (" + juce::String (clip (s, 0).length) + ")");
    s.proc.undo();

    // CLIP SETTINGS menus.
    if (auto* kbs = byTip<seq::Field> (s, "KB SPAN"))
    {
        s.click (*kbs);
        s.check (s.menuItems() == juce::StringArray ({ "Off", "Mono", "Poly", "Offset" }), "KB SPAN menu");
        s.choose ("Poly");
        s.check (clip (s, 0).settings.kbSpan == 2, "KB SPAN Poly");
    }
    if (auto* lq = byTip<seq::Field> (s, "LAUNCH QUANT: grid on which a launched clip"))
    {
        s.click (*lq);
        pick (s, { "1/4" });
        s.check (clip (s, 0).settings.launchQuant == 5, "clip launch quant 1/4");
    }
    // Clip name menu.
    if (auto* cn = byTip<seq::Field> (s, "The selected clip"))
    {
        s.click (*cn);
        s.check (s.menuHas ("Factory/Four Chords") && s.menuItems().contains ("Save Clip...") && s.menuItems().contains ("Rename Clip"), "clip menu");
        s.closeMenu();
    }
    // Slots: copy clip 1, paste into 2, preview clip.
    auto* midiOut = s.find<ParamChoice> ([&] (ParamChoice& c) { return c.param() == G.midiOut && shownIn (c, s.panel); });
    juce::Component* slotsComp = nullptr;
    if (midiOut != nullptr)
    {
        // The slots sit left of the MIDI OUT box, same row.
        auto* ed = roll->getParentComponent();
        for (auto* ch : ed->getChildren())
            if (ch->getY() > roll->getBottom() && ch->getWidth() > 600) slotsComp = ch;
    }
    s.check (slotsComp != nullptr, "clip slots");
    if (slotsComp != nullptr)
    {
        const float sw = ((float) slotsComp->getWidth() - 66.0f) / 12.0f;
        auto slotPt = [&] (int i, int y) { return juce::Point<int> ((int) ((sw + 6.0f) * (float) i + sw * 0.5f), y); };
        s.click (*slotsComp, slotPt (0, 40), true);
        s.check (s.menuItems() == juce::StringArray ({ "Rename Clip", "Copy Clip", "Paste Clip", "Erase Clip", "Set as Preview Clip" }), "slot menu");
        s.save ("seq_menu_clipslot");
        s.choose ("Copy Clip");
        s.click (*slotsComp, slotPt (1, 40), true);
        s.choose ("Paste Clip");
        s.check (clip (s, 1).notes.size() == clip (s, 0).notes.size(), "paste clip");
        s.click (*slotsComp, slotPt (0, 40), true);
        s.choose ("Set as Preview Clip");
        s.check (s.proc.patch().clips.previewClip == 0, "preview clip");
        // Launch slot 1 with CLIP on.
        s.proc.setWithUndo (G.clipOn, 1.0f);
        s.click (*slotsComp, slotPt (0, slotsComp->getHeight() - 10));
        s.play (60);
        s.check ((s.proc.seqTelemetry().clipPlaying.load() & 1u) != 0, "clip 1 plays");
        s.save ("seq_clip_playing");
        s.check (s.proc.seqTelemetry().noteCount.load() > 0, "clip notes reach the engine");
        // Select slot 3 (empty) then back.
        s.click (*slotsComp, slotPt (2, 40));
        s.check (s.proc.patch().clips.selected == 2, "slot 3 selected");
        s.click (*slotsComp, slotPt (0, 40));
    }
    if (midiOut != nullptr)
    {
        s.click (*midiOut);
        s.check (s.menuItems() == juce::StringArray ({ "Off", "Clip Player", "On" }), "MIDI OUT menu");
        s.save ("seq_menu_midiout");
        s.choose ("Clip Player");
        s.check (s.proc.params.geti (G.midiOut) == 1, "MIDI OUT Clip Player");
    }
    // Bank menu.
    if (auto* bank = byTip<seq::Field> (s, "Clip bank"))
    {
        s.click (*bank);
        s.check (s.menuHas ("Factory/Chords/Basic Triads") && s.menuHas ("Factory/Init") && s.menuItems().contains ("Save Clip Bank..."), "clip bank menu");
        s.menuHas ("Factory/Chords");
        s.save ("seq_menu_clipbank");
        s.closeMenu();
        s.click (*bank);
        s.choose ("Factory/Chords/Basic Triads");
        s.check (s.proc.patch().clips.name == "Basic Triads" && clip (s, 0).notes.size() == 3, "bank loaded");
        s.pump (40);
        s.check (s.findByTooltip ("Clip bank") != nullptr, "bank field still there");
        s.save ("seq_clip_bank");
        // Edit -> modified mark.
        s.doubleClick (*roll, { xAt (3), rowsVisibleTop + 12 * 3 + 6 });
        s.pump (40);
        s.check (dynamic_cast<seq::Field*> (s.findByTooltip ("Clip bank"))->text().endsWith ("*"), "modified bank shows *");
    }
    // Recording: start, then DONE.
    if (auto* rec = byTip<seq::Check> (s, "Record into the selected clip"))
    {
        s.click (*rec);
        s.play (10);
        s.check (s.proc.seqTelemetry().recordingSlot.load() == 0, "recording started");
        s.noteOn (64); s.play (20); s.noteOff (64); s.play (10);
        s.pump (80);
        s.save ("seq_clip_recording");
        if (auto* done = byTip<seq::Check> (s, "Commit everything"))
        {
            s.click (*done);
            s.play (6);
            s.check (s.proc.seqTelemetry().recordingSlot.load() < 0, "recording stopped");
            s.check (! clip (s, 0).hasUncommitted(), "takes committed");
        }
    }
    s.proc.post ({ SeqCommand::ClipStopAll, 0 });
    s.play (6);

    // ---- ARP editor ----------------------------------------------------------
    auto* arpTab = s.button ("ARP");
    s.check (arpTab != nullptr, "ARP tab");
    if (arpTab == nullptr) return;
    s.click (*arpTab, {}, true);
    s.check (s.menuItems().contains ("Lock Module"), "ARP tab menu");
    s.choose ("Lock Module");
    s.check (s.proc.patch().arp.lockModule, "arp lock module");
    s.click (*arpTab);
    s.pump (40);
    s.check (s.findByTooltip ("Close the ARP editor") != nullptr, "arp editor open");
    s.save ("seq_arp_default");

    if (auto* shape = byTip<seq::Field> (s, "SHAPE: the order"))
    {
        s.click (*shape, { 100, 12 });
        auto items = s.menuItems();
        s.check (items.size() == 21 && items[0] == "Up" && items[20] == "Pattern" && items[19] == "---", "shape menu (" + juce::String (items.size()) + ")");
        s.save ("seq_menu_shape");
        pick (s, { "Up/Down" });
        s.check (arpSlot (s, 0).shape == ArpShape::UpDown, "shape Up/Down");
    }
    // RANGE right-click: Shape submenu.
    {
        auto* range = s.find<seq::SeqKnob> ([&] (seq::SeqKnob& k) { return k.knob.caption == "RANGE" && shownIn (k, s.panel); });
        s.check (range != nullptr, "RANGE knob");
        if (range != nullptr)
        {
            s.click (range->knob, {}, true);
            auto items = s.menuItems();
            s.check (items.contains ("#Arp 1 Range") && items.contains ("Shape") && items.contains ("Reset Control") && items.contains ("Mod Source"), "RANGE menu");
            s.menuHas ("Shape/Down");
            s.save ("seq_menu_range");
            s.choose ("Shape/Down");
            s.check (arpSlot (s, 0).rangeShape == 1, "range shape Down");
            s.drag (range->knob, { 30, 30 }, { 30, -10 });
            s.check (arpSlot (s, 0).range > 0, "RANGE dragged (" + juce::String (arpSlot (s, 0).range) + ")");
        }
        auto* chance = s.find<seq::SeqKnob> ([&] (seq::SeqKnob& k) { return k.knob.caption == "CHANCE" && shownIn (k, s.panel); });
        if (chance != nullptr)
        {
            s.click (chance->knob, {}, true);
            s.check (s.menuItems().contains ("Pre"), "CHANCE menu has Pre");
            s.choose ("Pre");
            s.check (arpSlot (s, 0).chancePre, "chance Pre on");
        }
    }
    // EDIT ALL with a toggle: LATCH applies to every slot.
    if (auto* ea = byTip<seq::Check> (s, "EDIT ALL: edits apply to all 12"))
    {
        s.click (*ea);
        if (auto* latch = byTip<seq::Check> (s, "LATCH:")) s.click (*latch);
        bool all = true;
        for (auto& sl : s.proc.patch().arp.slots) all &= sl.latch;
        s.check (all, "EDIT ALL latch on every slot");
        s.click (*ea);
    }
    if (auto* lq = byTip<seq::Field> (s, "LAUNCH QUANT: grid on which a launched arp"))
    {
        s.click (*lq);
        s.check (s.menuItems().size() == 10, "arp launch quant menu");
        pick (s, { "1/8" });
        s.check (s.proc.patch().arp.launchQuant == 4, "arp launch quant 1/8");
    }
    // Slots: right-click menu, then launch slot 1 with notes held.
    juce::Component* arpSlots = nullptr;
    if (auto* ed = s.findByTooltip ("Close the ARP editor")->getParentComponent())
        for (auto* ch : ed->getChildren())
            if (ch->getY() > 470 && ch->getWidth() > 600) arpSlots = ch;
    s.check (arpSlots != nullptr, "arp slots");
    if (arpSlots != nullptr)
    {
        const float sw = ((float) arpSlots->getWidth() - 66.0f) / 12.0f;
        auto slotPt = [&] (int i, int y) { return juce::Point<int> ((int) ((sw + 6.0f) * (float) i + sw * 0.5f), y); };
        s.click (*arpSlots, slotPt (3, 40), true);
        s.check (s.menuItems() == juce::StringArray ({ "Rename Pattern", "Copy Pattern", "Paste Pattern", "Erase Pattern", "Show Pattern Names" }), "arp slot menu");
        s.save ("seq_menu_arpslot");
        s.choose ("Copy Pattern");
        s.click (*arpSlots, slotPt (4, 40), true);
        s.choose ("Paste Pattern");
        s.check (arpSlot (s, 4).shape == arpSlot (s, 3).shape && arpSlot (s, 4).latch == arpSlot (s, 3).latch, "paste pattern");

        s.proc.setWithUndo (G.arpOn, 1.0f);
        // Launch slot 2 from its play bar, then hold a chord.
        s.click (*arpSlots, slotPt (1, arpSlots->getHeight() - 10));
        s.noteOn (60); s.noteOn (64); s.noteOn (67);
        s.play (80);
        s.check (s.proc.seqTelemetry().arpSlot.load() == 1, "arp slot 2 playing (" + juce::String (s.proc.seqTelemetry().arpSlot.load()) + ")");
        s.save ("seq_arp_playing");
        s.noteOff (60); s.noteOff (64); s.noteOff (67);
        s.proc.post ({ SeqCommand::ArpStop, 0 });
        s.play (10);
    }
    // MIDI OUT menu from the arp editor.
    if (auto* mo = s.find<ParamChoice> ([&] (ParamChoice& c) { return c.param() == G.midiOut && shownIn (c, s.panel); }))
    {
        s.click (*mo);
        s.choose ("On");
        s.check (s.proc.params.geti (G.midiOut) == 2, "MIDI OUT On");
    }
    // Bank menu.
    if (auto* bank = byTip<seq::Field> (s, "Arp bank"))
    {
        s.click (*bank);
        s.check (s.menuHas ("Factory/Basic Shapes") && s.menuItems().contains ("Save Arp Bank..."), "arp bank menu");
        s.menuHas ("Factory/Init");
        s.save ("seq_menu_arpbank");
        s.choose ("Factory/Basic Shapes");
        s.check (s.proc.patch().arp.name == "Basic Shapes" && arpSlot (s, 11).shape == ArpShape::Pattern, "arp bank loaded");
    }
    // Pattern editor: slot 12 is a Pattern.
    if (arpSlots != nullptr)
    {
        const float sw = ((float) arpSlots->getWidth() - 66.0f) / 12.0f;
        s.click (*arpSlots, { (int) ((sw + 6.0f) * 11.0f + sw * 0.5f), 40 });
        s.pump (40);
        auto* graph = s.findByTooltip ("Double-click to add or delete a step");
        s.check (graph != nullptr, "pattern editor shown");
        if (graph != nullptr)
        {
            const int n0 = (int) arpSlot (s, 11).pattern.steps.size();
            // Row 5 is the second row from the top (rows 6..-2).
            const float rh = (float) (graph->getHeight() - 18 - 14 - 44 - 4) / 9.0f;
            const int y5 = 18 + (int) (rh * 1.5f);
            const float gwp = (float) graph->getWidth() - 34.0f;
            s.doubleClick (*graph, { 34 + (int) (gwp * 0.53f), y5 });
            s.check ((int) arpSlot (s, 11).pattern.steps.size() == n0 + 1, "step added");
            // Drag a step from row 1 (first step) to row 2.
            const int y1 = 18 + (int) (rh * 5.5f);
            const auto first = arpSlot (s, 11).pattern.steps[0];
            s.drag (*graph, { 34 + 4, y1 }, { 34 + 4, y1 - (int) rh });
            s.check (arpSlot (s, 11).pattern.steps[0].row == first.row + 1 || arpSlot (s, 11).pattern.steps[1].row == first.row + 1, "step moved up a row");
            s.save ("seq_arp_pattern");
            if (auto* pn = s.find<seq::Field> ([&] (seq::Field& f) { return shownIn (f, s.panel) && f.text && f.text().startsWith ("Init"); }))
            {
                s.click (*pn, { 100, 12 });
                s.check (s.menuHas ("Factory/Bounce") && s.menuItems().contains ("Rename Pattern..."), "pattern menu");
                s.choose ("Factory/Bounce");
                s.check (arpSlot (s, 11).pattern.name == "Bounce", "pattern Bounce loaded");
            }
            s.pump (40);
            s.save ("seq_arp_pattern_bounce");
        }
    }

    // ---- OSC MAPPING ---------------------------------------------------------
    if (auto* om = s.button ("OSC MAPPING"))
    {
        s.click (*om);
        s.pump (40);
        auto* area = s.findByTooltip ("Drag an edge to resize");
        s.check (area != nullptr, "mapping editor");
        if (area != nullptr)
        {
            // Drag OSC A's left edge to about C1 (note 36).
            const int y = 38 + 19;
            const float cw = (float) area->getWidth() / 128.0f;
            s.drag (*area, { 2, y }, { (int) (cw * 36.5f), y });
            const auto& m = s.proc.patch().keyboard.mapping[1];
            s.check (m.keyLo >= 34 && m.keyLo <= 38, "OSC A range dragged (" + juce::String (m.keyLo) + ")");
            s.save ("seq_oscmap");
            // FOLD on for OSC A.
            auto* fold = s.find<seq::Check> ([&] (seq::Check& c) { return shownIn (c, s.panel) && c.getTooltip().startsWith ("FOLD") && c.getY() > 80 && c.getY() < 110; });
            if (fold != nullptr) { s.click (*fold); s.check (s.proc.patch().keyboard.mapping[1].fold, "OSC A fold on"); }
        }
        s.click (*om);
    }
    s.check (! s.menuOpen(), "no menu left open");
    s.pump (40);
}

TINCTURE_SHOT ("seq", runSeqShot);
