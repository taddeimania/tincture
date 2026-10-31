// BottomStrip.cpp — the OSC page's bottom strip: pitch and mod wheels with
// the bend ranges, the CLIP and ARP tabs (power, label, 12-key launcher),
// the keyboard bar (TRANSPOSE, KEY, SCALE, SWING, OSC MAPPING) over the
// on-screen keyboard, and the MIDI OUT box at the right.
#include "SeqWidgets.h"
#include "../Engine/Scales.h"

namespace tincture::ui
{
using namespace juce;
using namespace seq;


namespace
{
const Colour kKeyAccent = colours::brand;

// ---------------------------------------------------------------------------
// Pitch / mod wheel.
// ---------------------------------------------------------------------------
class Wheel final : public Component, public SettableTooltipClient
{
public:
    Wheel (TinctureProcessor& p, bool pitch) : proc (p), isPitch (pitch)
    {
        setTooltip (pitch ? "Pitch wheel: drag up or down; springs back on release" : "Mod wheel (CC 1): drag up or down");
    }

    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        const auto cap = r.removeFromBottom (14.0f);
        drawText (g, isPitch ? "PITCH" : "MOD", cap, 10.5f, colours::inkDim, true);
        r = r.reduced (4.0f, 0.0f);
        g.setColour (colours::black);
        g.fillRect (r);
        g.setColour (colours::inkDim);
        g.drawRect (r, 1.0f);
        const auto in = r.reduced (3.0f);
        // Ridges.
        g.setColour (colours::grid);
        for (float y = in.getY() + 4.0f; y < in.getBottom(); y += 6.0f)
            g.fillRect (in.getX() + 2.0f, y, in.getWidth() - 4.0f, 1.0f);
        const float norm = isPitch ? (value + 1.0f) * 0.5f : value;
        const float y = in.getBottom() - norm * in.getHeight();
        if (isPitch)
        {
            g.setColour (colours::inkFaint);
            g.fillRect (in.getX(), in.getCentreY() - 0.5f, in.getWidth(), 1.0f);
        }
        else
        {
            g.setColour (kKeyAccent.withAlpha (0.35f));
            g.fillRect (in.withTop (y));
        }
        g.setColour (colours::cream);
        g.fillRect (Rectangle<float> (in.getX(), y - 4.0f, in.getWidth(), 8.0f).constrainedWithin (in));
        g.setColour (colours::panel);
        g.fillRect (Rectangle<float> (in.getX() + 2.0f, y - 0.5f, in.getWidth() - 4.0f, 1.0f).constrainedWithin (in));
    }

    void mouseDown (const MouseEvent&) override { start = value; }

    void mouseDrag (const MouseEvent& e) override
    {
        const float range = (float) getHeight() - 20.0f;
        const float d = (float) -e.getDistanceFromDragStartY() / range * (isPitch ? 2.0f : 1.0f);
        set (jlimit (isPitch ? -1.0f : 0.0f, 1.0f, start + d));
    }

    void mouseUp (const MouseEvent&) override { if (isPitch) set (0.0f); }

    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) override
    {
        if (! isPitch) set (jlimit (0.0f, 1.0f, value + w.deltaY * 0.1f));
    }

    void set (float v)
    {
        if (same (v, value)) return;
        value = v;
        if (isPitch) proc.uiPitchBend (v);
        else proc.uiController (1, roundToInt (v * 127.0f));
        repaint();
    }

    float value = 0.0f;

private:
    TinctureProcessor& proc;
    bool isPitch;
    float start = 0.0f;
};

// ---------------------------------------------------------------------------
// The on-screen keyboard, C-2 .. C8.
// ---------------------------------------------------------------------------
class Keyboard final : public Component, public Refreshable, public SettableTooltipClient
{
public:
    static constexpr int kLow = 0, kHigh = 120;

    explicit Keyboard (PanelContext& c) : ctx (c)
    {
        ctx.add (this);
        setTooltip ("Click or drag to play; click lower on a key for a louder note");
    }
    ~Keyboard() override { releaseNote(); ctx.remove (this); }

    bool isBlack (int n) const { const int k = n % 12; return k == 1 || k == 3 || k == 6 || k == 8 || k == 10; }

    Rectangle<float> keyRect (int n) const { return rects[(size_t) n]; }

    void resized() override
    {
        int whites = 0;
        for (int n = kLow; n <= kHigh; ++n) whites += isBlack (n) ? 0 : 1;
        const float ww = (float) getWidth() / (float) whites;
        const float h = (float) getHeight();
        int wi = 0;
        for (int n = kLow; n <= kHigh; ++n)
        {
            if (! isBlack (n)) { rects[(size_t) n] = { ww * (float) wi, 0.0f, ww, h }; ++wi; }
            else
            {
                const float bw = ww * 0.62f;
                rects[(size_t) n] = { ww * (float) wi - bw * 0.5f, 0.0f, bw, h * 0.6f };
            }
        }
        background = {};
    }

    int noteAt (Point<float> p) const
    {
        for (int n = kLow; n <= kHigh; ++n) if (isBlack (n) && rects[(size_t) n].contains (p)) return n;
        for (int n = kLow; n <= kHigh; ++n) if (! isBlack (n) && rects[(size_t) n].contains (p)) return n;
        return -1;
    }

    void paint (Graphics& g) override
    {
        if (background.isNull())
        {
            background = Image (Image::ARGB, jmax (1, getWidth()), jmax (1, getHeight()), true);
            Graphics bg (background);
            for (int pass = 0; pass < 2; ++pass)
                for (int n = kLow; n <= kHigh; ++n)
                {
                    if (isBlack (n) != (pass == 1)) continue;
                    const auto r = rects[(size_t) n];
                    bg.setColour (isBlack (n) ? colours::black : colours::cream);
                    bg.fillRect (r);
                    bg.setColour (colours::panelEdge);
                    bg.drawRect (r, 1.0f);
                    if (n % 12 == 0)
                        drawText (bg, noteName (n), r.withTop (r.getBottom() - 16.0f).expanded (6.0f, 0.0f), 10.5f,
                                  n == 60 ? colours::black : colours::inkFaint, n == 60, Justification::centred);
                }
        }
        g.drawImageAt (background, 0, 0);
        for (int pass = 0; pass < 2; ++pass)
            for (int n = kLow; n <= kHigh; ++n)
            {
                if (isBlack (n) != (pass == 1) || ! down[(size_t) n]) continue;
                auto r = rects[(size_t) n].reduced (1.5f, 0.0f).withTrimmedBottom (1.5f);
                g.setColour (kKeyAccent.withAlpha (isBlack (n) ? 1.0f : 0.85f));
                g.fillRect (r);
                if (isBlack (n)) continue;
                // Keep the white key's black neighbours on top.
                for (int b : { n - 1, n + 1 })
                    if (b >= kLow && b <= kHigh && isBlack (b))
                    {
                        g.setColour (down[(size_t) b] ? kKeyAccent : colours::black);
                        g.fillRect (rects[(size_t) b]);
                        g.setColour (colours::panelEdge);
                        g.drawRect (rects[(size_t) b], 1.0f);
                    }
                if (n % 12 == 0)
                    drawText (g, noteName (n), r.withTop (r.getBottom() - 16.0f).expanded (6.0f, 0.0f), 10.0f, colours::black, true);
            }
    }

    void refresh() override
    {
        const auto& t = ctx.proc.telemetry();
        const uint64_t lo = t.keysLo.load (std::memory_order_relaxed), hi = t.keysHi.load (std::memory_order_relaxed);
        std::array<bool, 128> now {};
        for (int n = 0; n < 64; ++n) now[(size_t) n] = (lo >> n) & 1u;
        for (int n = 0; n < 64; ++n) now[(size_t) (n + 64)] = (hi >> n) & 1u;
        if (playing >= 0) now[(size_t) playing] = true;
        if (now != down) { down = now; repaint(); }
    }

    void mouseDown (const MouseEvent& e) override { play (e.position); }
    void mouseDrag (const MouseEvent& e) override
    {
        const int n = noteAt (e.position);
        if (n != playing && n >= 0) play (e.position);
    }
    void mouseUp (const MouseEvent&) override { releaseNote(); refresh(); }

    int playing = -1;

private:
    void play (Point<float> p)
    {
        const int n = noteAt (p);
        if (n < 0) return;
        releaseNote();
        const auto r = rects[(size_t) n];
        const float vel = jlimit (0.08f, 1.0f, 0.25f + 0.75f * (p.y - r.getY()) / jmax (1.0f, r.getHeight() * 0.9f));
        ctx.proc.uiNoteOn (n, vel);
        playing = n;
        refresh();
    }
    void releaseNote()
    {
        if (playing >= 0) ctx.proc.uiNoteOff (playing);
        playing = -1;
    }

    PanelContext& ctx;
    std::array<Rectangle<float>, 128> rects {};
    std::array<bool, 128> down {};
    Image background;
};

// TRANSPOSE: mouse wheel = +-12 semitones.
class TransposeField final : public ParamField
{
public:
    using ParamField::ParamField;
    void mouseWheelMove (const MouseEvent&, const MouseWheelDetails& w) override
    {
        if (same (w.deltaY, 0.0f)) return;
        const int p = paramId;
        ctx.proc.setWithUndo (p, jlimit (-48.0f, 48.0f, ctx.proc.get (p) + (w.deltaY > 0 ? 12.0f : -12.0f)));
        refresh();
    }
};

// A tiny up/down arrow button.
class ArrowButton final : public Component
{
public:
    explicit ArrowButton (bool upArrow) : up (upArrow) {}
    std::function<void()> onClick;
    void paint (Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (0.5f);
        g.setColour (colours::black);
        g.fillRect (r);
        g.setColour (colours::inkDim);
        g.drawRect (r, 1.0f);
        drawArrow (g, r, up ? 2 : 3, colours::ink);
    }
    void mouseDown (const MouseEvent& e) override { if (! e.mods.isPopupMenu() && onClick) onClick(); }
private:
    bool up;
};

// ---------------------------------------------------------------------------
class BottomStrip final : public Component, public Refreshable
{
public:
    BottomStrip (PanelContext& c, OscPageHost& h)
        : ctx (c), host (h),
          pitch (c.proc, true), mod (c.proc, false),
          bendUp (c, ids().global.bendUp), bendDown (c, ids().global.bendDown),
          clipPower (c, ids().global.clipOn, clipColour), arpPower (c, ids().global.arpOn, arpColour),
          clipTab ("CLIP"), arpTab ("ARP"),
          transpose (c, ids().global.transpose), up (true), down (false),
          key (c, ids().global.key), scale (c, ids().global.scale),
          swing (c, ids().global.swing), swingDiv (c),
          oscMapping ("OSC MAPPING"), keyboard (c), midiOut (c)
    {
        ctx.add (this);
        const auto& G = ids().global;
        for (auto* w : { &pitch, &mod }) addAndMakeVisible (*w);
        for (auto* f : { &bendUp, &bendDown })
        {
            f->fontH = 12.0f;
            f->pixelsPerStep = 10.0f;
            addAndMakeVisible (*f);
        }
        bendUp.setTooltip ("Pitch bend range up (semitones)");
        bendDown.setTooltip ("Pitch bend range down (semitones)");
        bendUp.format = [] (float v) { return String (roundToInt (v)); };
        bendDown.format = [] (float v) { return String (roundToInt (v)); };

        // CLIP and ARP tabs.
        auto setupTab = [this] (PanelButton& tab, bool clip)
        {
            addAndMakeVisible (tab);
            tab.setFontHeight (13.0f);
            tab.setLitColour (clip ? clipColour : arpColour);
            tab.setTooltip (clip ? "Show the CLIP sequencer (right-click: Lock Module, trigger octave)"
                                 : "Show the arpeggiator (right-click: Lock Module, trigger octave)");
            tab.onClick = [this, clip, &tab] (const ModifierKeys& m)
            {
                if (m.isPopupMenu()) { showContextMenu (tab, tab.getMouseXYRelative(), tabMenu (ctx, clip)); return; }
                toggleEditor (clip);
            };
        };
        setupTab (clipTab, true);
        setupTab (arpTab, false);
        clipPower.setTooltip ("Clip playback on/off");
        arpPower.setTooltip ("Arpeggiator on/off");
        addAndMakeVisible (clipPower);
        addAndMakeVisible (arpPower);

        clipKeys.accent = clipColour;
        clipKeys.setTooltip ("Launch clips 1-12 (click again to stop)");
        clipKeys.state = [this] (int k)
        {
            const auto& t = ctx.proc.seqTelemetry();
            if ((t.clipPlaying.load() >> k) & 1u) return 1;
            if ((t.clipPending.load() >> k) & 1u) return 2;
            return 0;
        };
        clipKeys.hasContent = [this] (int k) { return ! ctx.proc.patch().clips.clips[(size_t) k].empty(); };
        clipKeys.onKey = [this] (int k) { ctx.proc.post ({ SeqCommand::ClipToggle, (int8_t) k }); };
        addAndMakeVisible (clipKeys);

        arpKeys.accent = arpColour;
        arpKeys.setTooltip ("Launch arp slots 1-12 (plays while keys are held or latched)");
        arpKeys.state = [this] (int k)
        {
            if (ctx.proc.seqTelemetry().arpSlot.load() == k) return 1;
            return ctx.proc.patch().arp.selected == k ? 3 : 0;
        };
        arpKeys.onKey = [this] (int k)
        {
            ctx.proc.editPatch ({}, [k] (Patch& p) { p.arp.selected = k; });
            ctx.proc.post ({ SeqCommand::ArpLaunch, (int8_t) k });
        };
        addAndMakeVisible (arpKeys);

        // Keyboard bar.
        transpose.fontH = 12.5f;
        transpose.pixelsPerStep = 8.0f;
        transpose.format = [] (float v) { const int i = roundToInt (v); return i == 0 ? String ("0") : (i > 0 ? "+" : "") + String (i); };
        addAndMakeVisible (transpose);
        up.onClick = [this] { nudgeTranspose (1); };
        down.onClick = [this] { nudgeTranspose (-1); };
        addAndMakeVisible (up);
        addAndMakeVisible (down);

        key.fontH = 12.5f;
        key.getText = [this] { const int k = ctx.proc.params.geti (ids().global.key); return k == 0 ? String ("-") : String (choices::keys()[(size_t) k]); };
        key.setTooltip ("KEY: quantise MIDI input and CLIP/ARP output to a key and scale");
        addAndMakeVisible (key);

        scale.fontH = 12.5f;
        scale.getText = [this] { return String (choices::scales()[(size_t) ctx.proc.params.geti (ids().global.scale)]); };
        scale.buildMenu = [this] { return scaleMenu(); };
        scale.setTooltip ("SCALE (active when a KEY is chosen)");
        addAndMakeVisible (scale);

        swing.fontH = 12.5f;
        swing.format = [] (float v) { return v <= 0.0005f ? String ("OFF") : String (50.0f + 25.0f * v, 1) + "%"; };
        swing.setTooltip ("SWING for CLIP and ARP timing (50% = none)");
        addAndMakeVisible (swing);

        swingDiv.fontH = 12.5f;
        swingDiv.dropArrow = true;
        swingDiv.setTooltip ("Swing division");
        swingDiv.text = [this] { return String (seqchoices::swingDivisions()[(size_t) jlimit (0, 3, ctx.proc.patch().keyboard.swingDivision)]); };
        swingDiv.onStep = [this] (int d)
        {
            const int v = jlimit (0, 3, ctx.proc.patch().keyboard.swingDivision + d);
            seq::edit (ctx, "Swing Division", [v] (Patch& p) { p.keyboard.swingDivision = v; });
        };
        swingDiv.menu = [this]
        {
            Menu m;
            const auto& l = seqchoices::swingDivisions();
            const int cur = ctx.proc.patch().keyboard.swingDivision;
            auto& proc = ctx.proc;
            for (int i = 0; i < (int) l.size(); ++i)
                m.add (l[(size_t) i], [&proc, i] { proc.editPatch ("Swing Division", [i] (Patch& p) { p.keyboard.swingDivision = i; }); }, i == cur);
            return m;
        };
        addChildComponent (swingDiv);

        oscMapping.setFontHeight (12.0f);
        oscMapping.setTooltip ("Edit the key and velocity ranges of SUB, OSC A-C, NOISE and ARP");
        oscMapping.onClick = [this] (const ModifierKeys& m)
        {
            if (m.isPopupMenu()) return;
            if (host.overlayId() == "oscmap") { host.closeOverlay(); return; }
            auto& pageHost = host;
            host.showOverlay (createOscMappingEditor (ctx, [&pageHost] { pageHost.closeOverlay(); }), OscPageHost::Area::Modules, "oscmap");
        };
        addAndMakeVisible (oscMapping);

        addAndMakeVisible (keyboard);
        addAndMakeVisible (midiOut);
        (void) G;
        refresh();
    }

    ~BottomStrip() override { ctx.remove (this); }

    void toggleEditor (bool clip)
    {
        const String id = clip ? "clip" : "arp";
        if (host.overlayId() == id) { host.closeOverlay(); refresh(); return; }
        auto& h = host;
        auto onClose = [&h] { h.closeOverlay(); };
        host.showOverlay (clip ? createClipEditor (ctx, onClose) : createArpEditor (ctx, onClose), OscPageHost::Area::ModulesAndMods, id);
        refresh();
    }

    void nudgeTranspose (int d)
    {
        const int p = ids().global.transpose;
        ctx.proc.setWithUndo (p, jlimit (-48.0f, 48.0f, ctx.proc.get (p) + (float) d));
    }

    Menu scaleMenu()
    {
        auto& proc = ctx.proc;
        const int p = ids().global.scale;
        const int cur = proc.params.geti (p);
        const auto& l = choices::scales();
        Menu m;
        for (int i = 0; i < (int) l.size(); ++i)
        {
            if (i == 23 || i == 46) m.columnBreak();
            m.add (l[(size_t) i], [&proc, p, i] { proc.setWithUndo (p, (float) i); }, i == cur);
        }
        return m;
    }

    void refresh() override
    {
        const auto id = host.overlayId();
        clipTab.setLit (id == "clip");
        arpTab.setLit (id == "arp");
        oscMapping.setLit (id == "oscmap");
        clipKeys.update();
        arpKeys.update();
        const bool keyOn = ctx.proc.params.geti (ids().global.key) != 0;
        scale.setAlpha (keyOn ? 1.0f : 0.45f);
        swingDiv.setVisible (ctx.proc.get (ids().global.swing) > 0.0005f);
        if (keyOn != shownKeyOn) { shownKeyOn = keyOn; repaint (628, 14, 48, 28); }
    }

    void resized() override
    {
        // Wheels.
        pitch.setBounds (9, 20, 32, 136);
        mod.setBounds (39, 20, 32, 136);
        bendUp.setBounds (73, 36, 28, 20);
        bendDown.setBounds (73, 80, 28, 20);
        // CLIP / ARP tabs.
        clipPower.setBounds (118, 18, 22, 22);
        clipTab.setBounds (144, 18, 86, 22);
        clipKeys.setBounds (118, 48, 112, 108);
        arpPower.setBounds (252, 18, 22, 22);
        arpTab.setBounds (278, 18, 86, 22);
        arpKeys.setBounds (252, 48, 112, 108);
        // Keyboard bar.
        transpose.setBounds (452, 17, 48, 22);
        up.setBounds (502, 17, 14, 11);
        down.setBounds (502, 28, 14, 11);
        key.setBounds (560, 17, 58, 22);
        scale.setBounds (676, 17, 168, 22);
        swing.setBounds (902, 17, 62, 22);
        swingDiv.setBounds (968, 17, 58, 22);
        oscMapping.setBounds (1078, 17, 108, 22);
        keyboard.setBounds (386, 46, 802, 110);
        midiOut.setBounds (1210, 18, 176, 138);
    }

    void paint (Graphics& g) override
    {
        g.fillAll (colours::panel);
        g.setColour (colours::panelEdge);
        g.fillRect (0, 0, getWidth(), 1);
        drawSection (g, { 6.0f, 10.0f, 100.0f, 152.0f }, {});
        drawSection (g, { 110.0f, 10.0f, 128.0f, 152.0f }, {});
        drawSection (g, { 244.0f, 10.0f, 128.0f, 152.0f }, {});
        drawSection (g, { 378.0f, 10.0f, 818.0f, 152.0f }, {});
        drawSection (g, { 1202.0f, 10.0f, 192.0f, 152.0f }, {});
        drawText (g, "BEND", { 70.0f, 18.0f, 34.0f, 14.0f }, 10.5f, colours::inkDim, true);
        drawArrow (g, { 80.0f, 58.0f, 14.0f, 10.0f }, 2, colours::inkFaint);
        drawArrow (g, { 80.0f, 102.0f, 14.0f, 10.0f }, 3, colours::inkFaint);
        drawText (g, "TRANSPOSE", { 388.0f, 17.0f, 62.0f, 22.0f }, 11.0f, colours::ink, true, Justification::centredLeft);
        drawText (g, "KEY", { 530.0f, 17.0f, 28.0f, 22.0f }, 11.0f, colours::ink, true, Justification::centredLeft);
        const bool keyOn = ctx.proc.params.geti (ids().global.key) != 0;
        drawText (g, "SCALE", { 632.0f, 17.0f, 42.0f, 22.0f }, 11.0f, keyOn ? colours::ink : colours::inkDim, true, Justification::centredLeft);
        drawText (g, "SWING", { 858.0f, 17.0f, 42.0f, 22.0f }, 11.0f, colours::ink, true, Justification::centredLeft);
    }

private:
    PanelContext& ctx;
    OscPageHost& host;
    Wheel pitch, mod;
    ParamField bendUp, bendDown;
    PowerButton clipPower, arpPower;
    PanelButton clipTab, arpTab;
    MiniKeys clipKeys, arpKeys;
    TransposeField transpose;
    ArrowButton up, down;
    ParamChoice key, scale;
    ParamField swing;
    seq::Field swingDiv;
    PanelButton oscMapping;
    Keyboard keyboard;
    MidiOutBox midiOut;
    bool shownKeyOn = false;
};
} // namespace

std::unique_ptr<Component> createBottomStrip (PanelContext& ctx, OscPageHost& host)
{
    return std::make_unique<BottomStrip> (ctx, host);
}
} // namespace tincture::ui
